#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/core/transfer/SendWindow.h"
#include "relayer/core/transfer/TransferPayload.h"
#include "relayer/core/transfer/TransferPipeline.h"
#include "relayer/core/transfer/WorkerPool.h"
#include "relayer/filestore/IFileStore.h"

// 发送侧传输引擎（PROTOCOL.md §9.4 - §9.8）。
//
// 职责边界：**只管"哪一块该在什么时候被读出来"**，不碰 socket、不碰帧。
// 帧的编码与发送由上层（Session / app）做——它才知道帧头该填什么 sessionId、
// 走哪条连接、要不要加密。
//
// 这样切的好处是：这一层的所有状态都能用"一个内存文件 + 手动驱动"来测。
// 真正难测的部分（网络时序、重连、半开连接）留在上层，而那一层本来就该用
// 集成测试覆盖。
//
// 流水线形态（异步三阶段）：
//
//   调度（strand 上）        读盘（WorkerPool）           上层发帧
//   ─────────────────        ──────────────────           ─────────
//   pump()            ──►    read at offset       ──►     takeReadyChunks()
//   applyAck() 腾窗口  ◄──   onChunkReady()               编码 + 发送
//
// 关键点：**读盘不占 strand**。一次 256 KiB 的 read 在慢盘上也要几毫秒，
// 而 strand 串行执行——占着它会把心跳、收帧、超时判定全部卡住（见 WorkerPool.h）。

namespace relayer::core::transfer
{

// 一块读出来的数据，连同它的块号。
//
// chunkIndex 必须跟着缓冲区走：读盘是异步的，完成顺序和提交顺序无关。
// 只传 buffer 的话，完成回调里根本不知道这是第几块——而这个信息猜不出来。
struct ReadChunk
{
    std::uint32_t chunkIndex = 0;
    std::vector<char> data;
};

// PostFn（投递函数）定义在 TransferPipeline.h——它与接收侧共用，
// 不该由本文件拥有，否则 TransferReceiver 要用它就得引入整个发送侧。

class TransferSender
{
public:
    struct Config
    {
        // 在飞数据的字节配额。窗口的块数由它在 open() 时换算：
        // windowChunks = max(1, windowBytes / chunkSize)。
        //
        // 为什么按字节而不是按块数：吞吐取决于"管道里有多少字节"，
        // 而取消延迟取决于"最后一帧什么时候能发出去"，后者由块大小决定。
        // 把这两件事分开，才能既保持吞吐又缩短控制帧的等待——块从 4 MiB
        // 降到 256 KiB 时，同样是 4 MiB 在飞，块数自动从 4 变成 16。
        //
        // 4 MiB 是"一个 RTT 内能被填满、又不至于吃太多内存"的折中。
        std::size_t windowBytes = 4u * 1024 * 1024;

        // 显式指定窗口块数。非 0 时**覆盖** windowBytes 的换算结果。
        //
        // 测试需要精确控制"同时有几块在飞"（比如验证背压、乱序、续传），
        // 按字节换算会让块数随 chunkSize 漂移，用例就不可复现了。
        std::uint32_t windowChunks = 0;

        // 同时在读盘的任务上限。它与窗口是两个独立约束：
        // 窗口管"允许多少块没被确认"，这里管"允许多少块同时在磁盘上排队"。
        //
        // 分开是因为两者瓶颈不同：网络慢时窗口先满，磁盘慢时这里先满。
        // 合成一个数的话，慢盘场景下窗口会被读盘任务占满，而窗口本该描述
        // 网络在飞量。
        std::uint32_t maxReadInFlight = 8;
    };

    // 不拥有任何东西；store 与 pool 的生命周期必须覆盖本对象。
    //
    // 分成两个重载而不是 `Config config = {}`：后者在 GCC 上对"带默认成员
    // 初始化器的聚合类型"会推导失败（could not convert from init-list）。
    TransferSender(filestore::IFileStore &store, WorkerPool &pool);
    TransferSender(filestore::IFileStore &store, WorkerPool &pool, Config config);

    ~TransferSender();

    TransferSender(const TransferSender &) = delete;
    TransferSender &operator=(const TransferSender &) = delete;

    // 打开源文件并计算块数。
    //
    // 这里**不**计算整文件 SHA-256 或 Merkle 根：那是 O(文件大小) 的活，
    // 见 computeDigest()。在 open() 里同步算的话，一个 10GB 的文件会让
    // 构造过程假死。
    [[nodiscard]] Status open(std::string_view sourcePath, std::uint32_t chunkSize);

    [[nodiscard]] std::uint32_t totalChunks() const noexcept;
    [[nodiscard]] std::uint32_t chunkSize() const noexcept;
    [[nodiscard]] std::uint64_t totalSize() const noexcept;

    // 把窗口里能发的块都提交去读盘，读完通过 post 投回。
    //
    // 返回本次提交的任务数。为 0 有三种可能，调用方不该用它判断"是否该等"：
    //   - 窗口满了（等 ACK）
    //   - 没有待发的块（全在飞或全已确认）
    //   - 读盘队列满了（WorkerPool 背压）
    // 该等的时候用 hasWork() 判断。
    //
    // 读完的块进入内部队列，由 takeReadyChunks() 取走。
    std::uint32_t pump(const PostFn &post);

    // 取走已读好、还没交给上层的块。
    //
    // 之所以要"取走"这一步而不是在读盘完成时直接回调上层：上层发帧是异步的
    // （async_write 也可能排队），直接回调会让它的节奏被读盘节奏牵着走。
    // 给一个队列，上层可以按自己的节奏消费。
    [[nodiscard]] std::vector<ReadChunk> takeReadyChunks();

    // 读盘完成的内部入口（由 pump 提交的任务通过 PostFn 投回后调用）。
    //
    // public 是因为投递的是裸回调，测试需要手动驱动它来避免依赖线程时序。
    // 正常使用中不该直接调用。
    void onChunkReady(ReadChunk chunk);

    // 是否还有活要干：有块待发、有块在读，或有块读好了没被取走。
    [[nodiscard]] bool hasWork() const;

    // 收到 ACK。
    void applyAck(const ChunkAck &ack);

    // 所有**在飞**的块号（升序）。用于"过一遍重传定时器"。
    //
    // 转发到 SendWindow 的同名方法：本层不重新定义语义。之所以不直接把
    // window_ 暴露出去，是因为窗口里还有 pending / acked 那些状态，
    // 上层不该看见——它只该知道"哪些块我已经发出去在等回音"。
    [[nodiscard]] std::vector<std::uint32_t> outstandingChunks() const;

    // 把在飞超时的块标记为待重传。返回被标记的块数。
    //
    // 时间判定在上层：SendWindow 刻意不认识时间，而"多久算超时"取决于
    // 链路质量，那是上层才知道的事。
    std::uint32_t markRetransmit(const std::vector<std::uint32_t> &chunks);

    // 取消：把所有在读和在飞的块归还窗口，清空就绪队列。取消后不可复用。
    void cancelAll();

    // 全部块已确认。
    [[nodiscard]] bool done() const noexcept;

    [[nodiscard]] std::uint32_t ackedChunks() const noexcept;

    // 累计取出用于重传的块次。转发 SendWindow 的同名计数。
    //
    // 丢包场景下它应当约等于"真正丢掉的块数"。若它约等于总块数，说明重传
    // 退化成了全量重发——那时内容仍然正确，只有这个数字能看出来。
    [[nodiscard]] std::uint32_t retransmittedChunks() const noexcept;

    // 读盘失败时记住的错误。非 Ok 时调用方应当取消整个传输。
    // 返回副本而不是引用：内部有锁，返回引用会让调用方拿到一个随时
    // 可能被 worker 改写的对象。
    [[nodiscard]] Status readError() const;

    // ---- 完成握手需要的信息 ----

    struct DigestResult
    {
        std::string merkleRootHex;  // 空表示失败
        std::string sha256Hex;
        Status status;
    };

    // 异步计算 Merkle 根与整文件 SHA-256（单遍读，两个摘要一起算）。
    //
    // 完成回调在 WorkerPool 线程上执行，**调用方负责投回 strand**——本类
    // 不知道 io_context 在哪，乱投会把回调扔到没有同步保护的地方。
    [[nodiscard]] Status computeDigest(std::function<void(DigestResult)> onDone);

    // 同步算一遍摘要，仅用于测试与小文件。大文件请走 computeDigest。
    [[nodiscard]] DigestResult computeDigestSync();

private:
    [[nodiscard]] std::uint64_t offsetOf(std::uint32_t chunkIndex) const noexcept;
    [[nodiscard]] std::uint32_t lengthOf(std::uint32_t chunkIndex) const noexcept;

    // 读盘失败时，把这一块从 reading_ 摘掉并归还窗口。
    void onChunkReadyInternalFailure(std::uint32_t chunkIndex);

    filestore::IFileStore &store_;
    WorkerPool &pool_;
    Config config_;

    std::string sourcePath_;
    std::uint32_t chunkSize_ = 0;
    std::uint64_t totalSize_ = 0;
    std::uint32_t totalChunks_ = 0;

    std::unique_ptr<SendWindow> window_;

    // ---- 跨线程共享的部分 ----
    //
    // worker 线程上跑的任务会碰这几个成员，所以它们必须加锁：
    //
    //   mu_         保护下面全部字段
    //   reading_    提交出去、还没回来的块
    //   ready_      读好了、还没被取走的块
    //   readError_  第一个读盘错误（worker 写、strand 读）
    //
    // 早先这里没锁，症状是偶发堆损坏：worker 在 push_back 时另一个线程
    // 正在 swap，vector 的内部指针被写坏。而它只在"传输正好跑到一半"
    // 时出现，单跑某条用例几乎撞不到。
    mutable std::mutex mu_;
    std::vector<std::uint32_t> reading_;
    std::vector<ReadChunk> ready_;
    Status readError_;

    // 还在执行（或排队等待执行）的任务数。
    //
    // 析构时必须等它归零：任务体里捕获了 this，越过本对象生命周期之后就
    // 是 use-after-free。这与 WorkerPool 的"等正在跑的、丢排队的"语义
    // 叠加，才能保证安全——池子保证不等排队任务，所以这里只等已经开始的。
    std::atomic<std::uint32_t> runningTasks_{0};

    bool opened_ = false;
};

}  // namespace relayer::core::transfer
