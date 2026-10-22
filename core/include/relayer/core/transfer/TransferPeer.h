#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <asio/io_context.hpp>

#include "relayer/common/Status.h"
#include "relayer/core/protocol/Frame.h"
#include "relayer/core/session/Session.h"
#include "relayer/core/transfer/TransferReceiver.h"
#include "relayer/core/transfer/TransferSender.h"
#include "relayer/core/transfer/WorkerPool.h"
#include "relayer/filestore/IFileStore.h"

// 把传输引擎接到会话上。
//
// TransferSender / TransferReceiver 本身不认识帧：它们只管"哪一块该读/写"，
// 不碰 socket、不碰编码。本层补上中间那段——把引擎的动作翻成帧、把收到的
// 帧翻回引擎的调用。
//
// 为什么单独一层而不是让 app 直接操作两个引擎：
//
//   帧类型与引擎动作的对应关系（§9.4 - §9.8）是**线上契约**，写散在 app 的
//   事件处理里之后，它就没法被单测覆盖了——测它得先造一个 GUI。收在这里
//   之后，"发一个分块帧会发生什么"可以用一对内存 socket 完整验证。
//
// 一次传输的帧序列（发送方视角）：
//
//   -> kFileRequestSend(desc)          请求
//   <- kFileAnswerSend(status, resume) 应答（带续传点）
//   -> kChunkRequestData × N           数据块
//   <- kChunkRequestAck × N            累积 ACK
//   -> kFileRequestComplete(root)      收尾
//   <- kFileAnswerComplete(status)     校验结果
//
// app 要做的事只有三件：创建它、把 Session 的 onFrame 转发给 onFrame()、
// 在事件循环上定时调 pump()。

namespace relayer::core::transfer
{

// 一次传输的进度事件。上层用它更新界面。
struct TransferProgress
{
    std::uint32_t chunksDone = 0;
    std::uint32_t chunksTotal = 0;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
};

// 传输结束的原因。
enum class TransferOutcome
{
    kCompleted,       // 校验通过，文件已落盘
    kPeerCancelled,   // 对端发了 cancel
    kLocalCancelled,  // 本端取消
    kFailed,          // 出错（磁盘、协议、校验）
    kPeerGone,        // 连接断了
};

struct TransferResult
{
    TransferOutcome outcome = TransferOutcome::kFailed;
    Status status;
    std::string targetPath;  // 接收方落点（发送方为空）
};

class TransferPeer : public std::enable_shared_from_this<TransferPeer>
{
public:
    // 发送方与接收方共用一个实现，靠 role 分派。
    //
    // 分成两个工厂函数而不是让调用方传 role：这个字段填错会走完全错误的
    // 帧序列（发送方去等 kFileRequestSend），而错误要到对端回帧时才暴露，
    // 中间隔着一次网络往返。
    enum class Role
    {
        kSender,
        kReceiver,
    };

    struct Config
    {
        // 发送方切块大小。接收方从描述里读，本字段对它无意义。
        //
        // 默认取协议上限（4 MiB）：大块能摊薄每块的帧头与 ACK 开销，
        // 在千兆以上的链路上明显更快。测试与小文件场景可以调小——
        // 4 MiB 意味着 2 MB 的文件只有 1 块，测不出乱序、续传、背压。
        std::uint32_t chunkSize = kDefaultChunkSize;

        // 分块重传的超时。超过这么久没收到某块的 ACK，就当它丢了。
        //
        // 4 MiB 块在慢链路上可能真要几秒才传完，所以不能设得太短；
        // 但太长又会让丢包恢复变钝。3 秒是个经验值。
        std::chrono::milliseconds retransmitTimeout{3000};

        // 发出 kFileRequestSend 后等对端应答的上限。
        //
        // 没有这个上限时，对端进程没起、请求帧丢了、或者对端压根不理这条
        // 会话，发送方会永远停在"等应答"上——重传超时管不到（此时一块都
        // 没发过），用户看到的是一个永远 0% 的进度条。
        //
        // 30 秒是给"对端正在校验旧残片"留的余量：接收方 open() 里可能要
        // 扫一遍残片算续传点，大文件上不算快。
        std::chrono::milliseconds answerTimeout{30000};

        // 收尾握手的超时。对端算 Merkle 根可能较慢（要整读一遍文件）。
        std::chrono::milliseconds finalizeTimeout{60000};

        TransferSender::Config sender;
        TransferReceiver::Config receiver;
    };

    // 发送方：打开 sourcePath 并开始。
    [[nodiscard]] static std::shared_ptr<TransferPeer>
    createSender(asio::io_context &ctx, std::shared_ptr<session::Session> session, filestore::IFileStore &store,
                 WorkerPool &pool, std::string sourcePath, std::string remoteName, std::string toPeerId,
                 std::int64_t sessionId, Config config, std::function<void(const TransferResult &)> onDone);

    // 接收方：登记要收的目标路径并等对端发描述。
    //
    // sessionId 传 0 表示**通配**：本端接受第一个 kFileRequestSend 带来的
    // sessionId，之后按它过滤。
    //
    // 为什么需要这个：sessionId 由发送方选，而协议里没有任何一帧用于协商
    // （请求帧自带它，发送方不需要别人告诉）。两个独立进程要对接时，
    // 接收方**无从知道**对方用了什么数字——传 0 是唯一的可行约定。
    //
    // 传非 0 则严格按它过滤，与发送方一侧对称。已有调用点（测试、同进程
    // 双方）都显式给出同一个号，行为不变。
    [[nodiscard]] static std::shared_ptr<TransferPeer>
    createReceiver(asio::io_context &ctx, std::shared_ptr<session::Session> session, filestore::IFileStore &store,
                   WorkerPool &pool, std::string targetPath, std::int64_t sessionId, Config config,
                   std::function<void(const TransferResult &)> onDone);

    ~TransferPeer();

    TransferPeer(const TransferPeer &) = delete;
    TransferPeer &operator=(const TransferPeer &) = delete;

    // 把一帧交给本层。已认证的会话把 onFrame 转进来即可。
    //
    // 不属于本会话（sessionId 不符）或本层不处理的帧会被静默忽略——
    // 返回 false 只是给上层做统计，不代表错误。
    bool onFrame(const protocol::Frame &frame);

    // 推进一次。由调用方在事件循环上定时（比如每 10ms）调用。
    //
    // 为什么不用内部定时器：本类会被放进 app 与测试两种环境，而"多久
    // 推进一次"是调度策略，不该硬编码在传输层。测试里可以手工调它来
    // 精确控制节奏，不必等真实时间。
    void pump();

    void cancel(std::string_view reason);

    [[nodiscard]] TransferProgress progress() const;

    // 传输真正跑起来了（可以收发数据块）。
    //
    // 发送方：收到对端的 kFileAnswerSend 之后才为真，在此之前它可能在
    // **算摘要**——那段时间网络上是完全沉默的，看门狗不该把它当成
    // "对端失联"。接收方：open 成功之后为真。
    //
    // 上层需要这个信号来判断"沉默是不是正常"，见 cli/Client.cpp 的
    // checkIdle。
    [[nodiscard]] bool started() const noexcept
    {
        return started_;
    }

    [[nodiscard]] std::int64_t sessionId() const noexcept
    {
        return sessionId_;
    }

    [[nodiscard]] bool finished() const noexcept
    {
        return finished_;
    }

    // 诊断计数。
    struct Stats
    {
        std::uint64_t chunksSent = 0;
        std::uint64_t chunksReceived = 0;
        std::uint64_t acksSent = 0;
        std::uint64_t acksReceived = 0;
        std::uint64_t duplicateChunks = 0;
        std::uint64_t protocolErrors = 0;
    };
    [[nodiscard]] const Stats &stats() const noexcept
    {
        return stats_;
    }

private:
    TransferPeer(asio::io_context &ctx, std::shared_ptr<session::Session> session, filestore::IFileStore &store,
                 WorkerPool &pool, Role role, std::int64_t sessionId, Config config,
                 std::function<void(const TransferResult &)> onDone);

    // 发送方
    void startSending();

    // 摘要算完（在 strand 上）。异步计算的回投点，见 startSending()。
    void onDigestReady(TransferSender::DigestResult digest);
    void handleAnswerSend(const protocol::Frame &frame);
    void handleChunkAck(const protocol::Frame &frame);
    void handleAnswerComplete(const protocol::Frame &frame);
    void sendReadyChunks();
    void sendCompleteRequest();
    void checkRetransmit();

    // 接收方
    void handleRequestSend(const protocol::Frame &frame);
    void handleChunkData(const protocol::Frame &frame);
    void handleCompleteRequest(const protocol::Frame &frame);
    void sendAck();
    void sendAnswerSend(AnswerStatus status, std::uint32_t resumeFrom, std::string_view reason);

    // 两边共用
    void sendFrame(protocol::Frame frame);
    void finish(TransferOutcome outcome, Status status, std::string targetPath = {});

    asio::io_context &ctx_;
    std::shared_ptr<session::Session> session_;
    filestore::IFileStore &store_;
    WorkerPool &pool_;

    Role role_;
    std::int64_t sessionId_ = 0;
    Config config_;
    std::function<void(const TransferResult &)> onDone_;

    std::unique_ptr<TransferSender> sender_;
    std::unique_ptr<TransferReceiver> receiver_;

    std::string sourcePath_;
    std::string remoteName_;
    std::string targetPath_;
    std::string toPeerId_;

    // 发送方的 Merkle 根。startSending 时算一次，收尾握手时用。
    //
    // 存在这里而不是算完就丢：收尾时重算要再整读一遍文件，而文件在传输
    // 过程中完全可能被改动——那时算出来的根和发出去的描述对不上，接收方
    // 会报 kRootMismatch，而真正的错误（源文件被改了）谁也看不出来。
    std::string merkleRootHex_;

    // 每块的发送时刻，用于重传判定。
    //
    // SendWindow 只记状态不记时间（它刻意不认识时间，才可能用纯逻辑测试），
    // 所以"发了多久没回音"这件事只能由本层维护。
    std::map<std::uint32_t, std::chrono::steady_clock::time_point> chunkSentAt_;

    // 接收方上次发 ACK 的时间。ACK 不能每收一块就发——4 MiB 块下那等于
    // 每个块都多一个往返。
    std::chrono::steady_clock::time_point lastAckSent_{};
    bool ackPending_ = false;

    // 传输已经真正跑起来了（可以收发数据块）。
    //
    // 发送方：**收到 kFileAnswerSend 之后**才置位，不是"发起"就置位。
    // 描述帧（含 sha256、块大小）必须先到对端，对端才有能力解读数据块。
    // 早先这里是"startSending 里就置 true"，结果摘要算完前 pump() 就会
    // 往出发数据块，而对端此时连块大小都不知道。
    //
    // 接收方：open 成功（或确认可续传）后置位。
    bool started_ = false;

    // 发送方：摘要计算已经启动。
    //
    // 与 requestSent_ 严格区分。摘要是异步的（要整读一遍文件），期间
    // 请求帧**还没发出去**，对端什么都收不到。用一个标记表示"已经安排
    // 好了、别重复安排"，另一个表示"真的发出去了、可以开始等应答"。
    //
    // 合成一个标记的后果我们踩过：pump 里按"已发出"去算等待时长，
    // 而计时起点还没被赋值（epoch），于是摘要阶段就被判成对端超时。
    bool digestStarted_ = false;

    // 发送方的描述帧已发出（摘要算完、kFileRequestSend 已投递）。
    //
    // 与 started_ 分开是因为两者之间隔着一个网络往返：置了 started_ 就
    // 意味着"可以发块了"，而这里只表示"可以开始等应答"。
    bool requestSent_ = false;

    // 收尾请求（kFileRequestComplete）已发出。
    //
    // 需要单独一个标记，因为触发点是"某条 ACK 让 done() 变真"——而对端
    // 会持续发 ACK（哪怕只是重复确认）。不挡重复发送的话，对端会收到
    // 多个 complete 请求，而每个都要跑一遍整文件校验。
    bool completeRequested_ = false;

    // 发出 complete 请求的时刻。配合 finalizeTimeout 判"对端不响应收尾"。
    //
    // 需要它是因为收尾阶段**没有块在飞**：重传超时机制此时已经无事可做，
    // 而收尾请求本身没有重发。对端若因为进程被杀、卡在校验等原因不回，
    // 这条传输会永远停在"等一个不会来的应答"上——UI 上是永远转圈的进度条。
    std::chrono::steady_clock::time_point completeRequestedAt_{};

    // 发出 kFileRequestSend 的时刻。配合 answerTimeout 判"对端不理会请求"。
    std::chrono::steady_clock::time_point requestSentAt_{};

    // 收到了对端的 complete 请求，但本端还有块没落盘，暂时压着不处理。
    //
    // 存 frame 副本而不是只存一个"待办"标记：处理时要用请求里的 Merkle 根
    // 与总长，重发请求不一定带着同样的内容（对端可能已经放弃）。
    bool completeDeferred_ = false;
    protocol::Frame deferredComplete_;

    bool finished_ = false;

    Stats stats_;
};

}  // namespace relayer::core::transfer
