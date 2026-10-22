#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/core/transfer/TransferPayload.h"
#include "relayer/core/transfer/TransferPipeline.h"
#include "relayer/core/transfer/WorkerPool.h"
#include "relayer/filestore/IFileStore.h"

// 接收侧传输引擎（PROTOCOL.md §9.4 - §9.8）。
//
// 与 TransferSender 对称，但有个根本区别：
//
//   发送侧可以随便重读（只读，幂等）。
//   接收侧写错了就是写错了——**落盘是不可逆的**。
//
// 这个区别决定了三条设计：
//
// 1. **先落盘后 ACK**。只有 write() 返回成功、且数据真的进了文件，才把这一
//    位算作"已收"。反过来（收到就 ACK、异步慢慢写）在网络中断时表现是
//    "发送方以为传完了，接收方文件缺一块"，而校验要等到最后才发现。
//
// 2. **每个写盘任务自己 open 写流**。共用一条 IWriteStream 会让并发任务
//    互相踩乱文件偏移——写进去的是别人的数据、长度却完全正确，只有最终
//    哈希能发现。发送侧踩过同一个坑（见 TransferSender.cpp 的注释）。
//
// 3. **残片路径与成品路径分离**。数据先写 `<目标>.relaypart`，校验通过才
//    atomicReplace 成正式文件名。中途失败留下的是残片，不会让用户以为
//    "文件已经到手了"。
//
// 流水线形态：
//
//   收块（strand 上）        写盘（WorkerPool）           回 ACK
//   ─────────────────        ──────────────────           ─────────
//   acceptChunk()     ──►    write at offset      ──►     takeAck()
//   （判重、判边界）          （各自 open/seek）           编码 + 发送
//
// 接收方**不做窗口调度**：块什么时候到由发送方决定，接收方只能接受。所以
// 这里没有 ReceiveWindow 那种"该发哪几块"的东西，只有：
//
//   - 收到位图（判重 + 生成 ACK 用）
//   - 落盘位图（判重 + 完成判定用）
//
// 两张图必须分开：收到但还没写成功的块不能参与 ACK，否则就违反了第 1 条。
//
// 内存保护：hasCapacity() / maxBufferedChunks 限制"已收未落盘"的块数。
// 到上限时 acceptChunk 返回 kBusy，上层应当暂停从连接读数据（背压的
// 逻辑层落点，物理层仍在 WorkerPool 队列上）。

namespace relayer::core::transfer
{

// 接收方要写出去的一块数据。
//
// data 按值持有：这块数据从网络缓冲区拷出来之后，网络层那块缓冲马上就能
// 复用/释放。不拷的话写盘任务是异步的，等它跑起来时原缓冲可能已经被
// 下一次读覆盖——这类 bug 表现为"偶发文件损坏"，几乎无法复现。
struct ReceivedChunk
{
    std::uint32_t chunkIndex = 0;
    std::vector<char> data;
};

class TransferReceiver
{
public:
    struct Config
    {
        // "已收到但还没写盘"的块数上限。它同时是内存上限与背压着力点。
        //
        // 按块数而不是字节数：块大小是传输开始时就定好的常量，一个块数
        // 上限换算成内存是确定的（maxBufferedChunks × chunkSize）。
        // 默认 16 × 256 KiB = 4 MiB 未落盘缓冲；块大小调到 4 MiB 时会变成
        // 64 MiB，调大块之前先想想这个数。
        std::uint32_t maxBufferedChunks = 16;

        // 同时在写盘的任务数上限。与上面那个是独立约束：
        // 上面管"内存里堆着多少块"，这里管"磁盘上同时排着多少请求"。
        //
        // 慢盘时这里先满，快盘慢网时上面那个先满。分开才能看出瓶颈在哪。
        std::uint32_t maxWriteInFlight = 4;

        // 落盘时是否 fsync。默认关：逐块 fsync 在机械盘上能把吞吐压到
        // 1/10，而它的收益只是"断电时不丢已 ACK 的块"。
        //
        // 需要强一致时打开它，或接受"断电后残片需要重传几块"。
        // 无论哪种，**正确性都不依赖它**——最终校验会把坏块查出来。
        bool syncOnWrite = false;
    };

    // 不拥有任何东西；store 与 pool 的生命周期必须覆盖本对象。
    TransferReceiver(filestore::IFileStore &store, WorkerPool &pool);
    TransferReceiver(filestore::IFileStore &store, WorkerPool &pool, Config config);

    ~TransferReceiver();

    TransferReceiver(const TransferReceiver &) = delete;
    TransferReceiver &operator=(const TransferReceiver &) = delete;

    // 接受传输描述，在 targetPath 上准备接收。
    //
    // 行为：
    //   - 校验 desc 自洽（块大小、总长与块数）；
    //   - 若 targetPath 已存在且大小与 desc 一致，返回 kAlreadyExists——
    //     这是**正常响应**而非失败：上层据此回 AnswerStatus::kTargetExists，
    //     由发送方（或用户）决定是否覆盖。绝不在这里偷偷覆盖用户的文件；
    //   - 否则创建残片 `targetPath + ".relaypart"`，若已存在且长度能整除
    //     块大小就续传，否则从 0 开始（截断残片）。
    //
    // resumeFrom 出参：告诉调用方从哪一块开始要。它一定是整块对齐的。
    //
    // 这里**不预分配**整个文件：1GB 的传输一上来就 resize 到 1GB，在文件
    // 系统上是瞬间的，但会让"磁盘满"报在一个和实际写入无关的时刻。逐块
    // 写入让 kNoSpace 出现在它真正发生的时候。
    [[nodiscard]] Status open(const TransferDesc &desc, std::string_view targetPath, std::uint32_t &resumeFrom);

    [[nodiscard]] std::uint32_t totalChunks() const noexcept;
    [[nodiscard]] std::uint32_t chunkSize() const noexcept;
    [[nodiscard]] std::uint64_t totalSize() const noexcept;

    // 残片路径（正式输出是 targetPath）。
    [[nodiscard]] const std::string &partPath() const noexcept;
    [[nodiscard]] const std::string &targetPath() const noexcept;

    // 判重：这一块是否已经"收到且落盘"。
    //
    // 上层在 acceptChunk 之前问它，可以省掉一次数据拷贝。但**不判也不算错**
    // ——acceptChunk 自己会再判一次（并发场景下这里返回 false、到那边变成
    // true 是正常的，那时它返回 kDuplicate 而不是写两遍）。
    [[nodiscard]] bool hasChunk(std::uint32_t chunkIndex) const;

    // 还能不能再塞一块。false 表示到背压上限了，上层应暂停读连接。
    [[nodiscard]] bool hasCapacity() const;

    // 收下一块，提交写盘。
    //
    // 返回值：
    //   kOk        已受理（写盘是异步的，此刻还没落盘）
    //   kDuplicate 这一块已经收到并落盘了，丢弃即可——**不是错误**，
    //              重传场景下必然发生
    //   kBusy      背压上限到了，调用方应暂停并稍后重试（不要当失败处理，
    //              更不要断开连接）
    //   其他       chunkIndex 越界、描述非法、已取消等真错误
    [[nodiscard]] Status acceptChunk(std::uint32_t chunkIndex, const void *data, std::size_t len, const PostFn &post);

    // 写盘完成后由投递回调调用。public 的理由同 TransferSender::onChunkReady。
    void onChunkWritten(ReceivedChunk chunk);
    void onChunkWriteFailed(std::uint32_t chunkIndex, Status error);

    // 生成当前 ACK：连续前缀 + 其后的位图。
    //
    // **只统计已落盘的块**。这是"先落盘后 ACK"的落点——把在写的块算进来，
    // 就是在告诉对端"这块我要了"而实际还没写成功。
    [[nodiscard]] ChunkAck buildCurrentAck() const;

    // 全部块都已落盘。
    [[nodiscard]] bool allChunksReceived() const;

    // 已落盘的块数。
    [[nodiscard]] std::uint32_t writtenChunks() const;

    // 写盘失败时记住的第一个错误。非 Ok 时调用方应取消整个传输。
    [[nodiscard]] Status writeError() const;

    // 是否还有"在写但没回来"的任务。
    [[nodiscard]] bool hasPendingWrites() const;

    // ---- 收尾 ----

    struct FinalizeResult
    {
        CompleteStatus status = CompleteStatus::kOk;
        std::string actualRootHex;  // 实际算出的根，失败时用来说明差在哪
        Status error;               // status != kOk 时的底层原因（可为空）
    };

    // 校验并落成正式文件。
    //
    // expectedRootHex 来自 kFileRequestComplete（§9.8）——**不来自描述**：
    // §9.4 的描述里只有整文件 sha256 与 fingerprint，Merkle 根是完成握手
    // 时才传的。这一点在实现时撞过：按直觉从描述里取根，取不到任何字段。
    //
    // 步骤：把残片刷盘 → 重读残片算 Merkle 根 → 与 expectedRootHex 比对 →
    //      一致则 atomicReplace 成正式文件名。
    //
    // 必须等 hasPendingWrites() 为 false 之后调用，否则读到的是半截数据。
    // 本函数会自己检查这一点，不会静默计算出错误的根。
    //
    // 为什么重读残片而不是"写的时候顺手攒叶子"：写盘是乱序的，攒叶子需要
    // 按块号顺序拼起来，那等于在内存里再留一份完整文件。重读一遍代价是
    // 一次顺序读，换来"内存占用与文件大小无关"。
    [[nodiscard]] FinalizeResult finalize(std::string_view expectedRootHex);

    // 放弃接收并删除残片。
    //
    // 不删残片的话，用户会在目标目录里看到一个永远传不完的 `.relaypart`，
    // 而界面上"传输已取消"——没人会把两者联系起来。
    void abort();

private:
    [[nodiscard]] std::uint64_t offsetOf(std::uint32_t chunkIndex) const noexcept;
    [[nodiscard]] std::uint32_t lengthOf(std::uint32_t chunkIndex) const noexcept;

    // 扫描已有残片，判断能续传到哪一块。
    //
    // 只按长度判断，不逐块校验内容：逐块校验要读一遍整个残片，而
    // 续传的价值就在于"别重传已有的部分"。内容正确性由最终 Merkle 根
    // 兜底——真对不上会退回 kRootMismatch，上层丢弃残片重来。
    [[nodiscard]] std::uint32_t scanResumePoint() const;

    // 关闭残片写流。finalize / abort 前必须调用，析构时由成员自动完成。
    //
    // 幂等，且不会与正在进行的写盘任务并发破坏状态：内部持 writeMu_，
    // 拿到锁时那些任务要么已完成、要么还没开始。
    void closeWriteStream() noexcept;

    filestore::IFileStore &store_;
    WorkerPool &pool_;
    Config config_;

    std::string targetPath_;
    std::string partPath_;
    std::uint32_t chunkSize_ = 0;
    std::uint64_t totalSize_ = 0;
    std::uint32_t totalChunks_ = 0;
    TransferDesc desc_;

    // ---- 跨线程共享 ----
    //
    // written_ / received_ / writeError_ / inFlight_ 都会被 worker 线程碰：
    // worker 写盘完成后通过 post 投回 onChunkWritten，但那个投递**可能**
    // 配置成直接调用（测试用的 inlinePost），所以不能假设"一定在 strand 上"。
    // 全部由 mu_ 保护。
    mutable std::mutex mu_;

    std::vector<bool> written_;   // 已落盘的块
    std::vector<bool> received_;  // 已受理、还在写的块（判重用）
    Status writeError_;

    // 在写但没回来的任务数。它和 written_.size() 是两件事：
    // 前者是本类自己的在飞量（受 maxWriteInFlight 约束），
    // 后者是"总共收了多少块"。
    std::uint32_t inFlight_ = 0;

    // 析构时必须等它归零：写盘任务体捕获了 this。
    std::atomic<std::uint32_t> runningTasks_{0};

    // 残片的写流，整个传输期间复用。
    //
    // 为什么不每块 open 一次：一次 open 在 Windows 上是 CreateFile 加安全
    // 求值，实测约 9ms。256 KiB 块下 256 MB 的文件有 1024 块，光开关文件
    // 就是 9 秒——把 4 MiB 块时代 14 秒的总时长拖到 44 秒，还不算由此引发
    // 的超时重传（实测 144 块）。复用之后每块只剩 seek/write/flush。
    //
    // 为什么用锁而不是"每块一个流"：IWriteStream 的读写位置是流内状态，
    // seek 与 write 必须成对串行，否则一个线程 seek 完、另一个线程插进来
    // seek 到别处，数据就写错块了。而磁盘本身就是串行的，多流并行写同一
    // 文件只会让磁头来回跑。
    std::unique_ptr<filestore::IWriteStream> writeStream_;
    std::mutex writeMu_;

    bool opened_ = false;
    bool aborted_ = false;
};

}  // namespace relayer::core::transfer
