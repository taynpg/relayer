#pragma once

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "relayer/common/Status.h"
#include "relayer/core/transfer/TransferPeer.h"
#include "relayer/core/transfer/WorkerPool.h"
#include "relayer/filestore/IFileStore.h"

namespace relayer::core::session {
class Session;
}

namespace relayer::core::net {

// 数据连接（v1 `FileSession` 的现代版）。
//
// 专门跑文件传输：一条到中继的独立 TCP，只承载传输帧（请求 / 分块 / ACK /
// 收尾）。控制类交互走 ControlLink，两者互不排队——这是"传输期间还能浏览、
// 还能取消"的另一半（另一半是 ControlLink 的并发请求）。
//
// 一条连接同时具备两种能力：
//
//   - **发送**：`sendFile()` 建一个 sender，把本地文件推给某个对端；
//   - **接收**：常驻一个 receiver 等对端推过来，收完自动重挂。
//
// 这是 v1 的形态：文件会话既发也收。分成两条连接（一条发、一条收）会多一次
// 握手、多一个 id，换不到任何东西。
//
// ## 接收是串行的
//
// 同一时刻只有一个接收在跑（引擎的 receiver 一次只认一个 sessionId）。
// 两个对端同时推文件过来时，第二个的请求会被正在跑的那一路拒绝——与 v1 一致，
// 也是刻意的：并发接收会让落盘 IO 互相踩，收益远小于复杂度。
//
// ## 落点
//
// 接收目录固定在 `Config::receiveDir`，具体文件路径由发送方描述里的
// `desc.root` 决定，经 `IFileStore::resolveWithin` 校验（挡住 `../../` 这类
// 逃逸）。当前**不支持**"按请求改落点"（那种要 TransferPeer 提供一个
// "接不接"的钩子），有需要时先改 receiveDir 再重建 receiver。
class TransferLink : public std::enable_shared_from_this<TransferLink>
{
public:
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 9527;
        std::string password;
        bool noAuth = false;
        std::string requestedId;

        // 对端推来的文件落在这下面。
        std::string receiveDir;

        // 要不要挂常驻接收。默认挂——数据连接的意义之一就是"随时能收"。
        //
        // 关掉它的场景：只发不收的客户端（CLI 的 send）。挂着接收意味着
        // 任何知道本机数据连接 id 的对端都能往 receiveDir 里写文件，而
        // 纯发送方没有理由接受这件事。
        bool acceptIncoming = true;

        // 发送方的分块大小。0 = 引擎默认（256 KiB）。
        std::uint32_t chunkSize = 0;

        // 接收方目标已存在时覆盖。默认拒绝——"要不要覆盖用户的文件"是
        // 上层策略（GUI 的发送前检查已经把这件事问完了）。
        bool overwriteExisting = false;

        // 传输开始后的空闲上限：多久没有任何进展就判对端失联。
        //
        // 语义是"空闲"而不是"总时长"——1GB 在慢盘上可能跑很久，用总时长
        // 会误杀正常传输。默认 60 秒与旧的单连接实现一致。
        std::chrono::milliseconds idleTimeout{60000};

        // 请求帧发出后等 kAnswerSend 的时限。
        //
        // 与 idleTimeout 分开：这一段只覆盖"请求发出到对方接受"。目标不在线
        // 时中继**静默丢帧**（协议没有投递失败这一类），所以发错 id 的唯一
        // 现象就是干等。AnswerSend 在对端在线时是毫秒级，10 秒已经很宽裕。
        std::chrono::milliseconds answerTimeout{10000};

        // 推进引擎的间隔。10ms 与单连接时代一致：重传判定是秒级，
        // ACK 是 20ms 级，再密也不会有额外收益。
        std::chrono::milliseconds tickInterval{10};

        // 进度上报间隔。比 tick 稀疏得多——每次上报都要跨线程投递回 GUI，
        // 10ms 一次既无意义又浪费。
        std::chrono::milliseconds progressInterval{100};
    };

    // 一路传输的对外视图。
    struct JobInfo {
        std::uint64_t id = 0;
        bool sending = false;
        // 发送：本地源文件；接收：对端描述里的落地路径（完成前为空）。
        std::string path;
        std::uint64_t bytesDone = 0;
        std::uint64_t bytesTotal = 0;

        // 传输层计数，在 onFinished 时是最终值（进度回调里是过程值）。
        //
        // 为什么带在这里而不是让上层再去问：job 在 onFinished 之后就可能
        // 被销毁，那时读不到计数了。而这些数字是"这次传得怎么样"的唯一
        // 证据（重传了几块、有没有重复块），诊断丢包时必须能看到。
        std::uint64_t chunksSent = 0;
        std::uint64_t chunksReceived = 0;
        std::uint64_t chunksRetransmitted = 0;
        std::uint64_t duplicateChunks = 0;
        std::uint64_t acksSent = 0;
        std::uint64_t acksReceived = 0;
    };

    struct Handler {
        // 连接可用（认证完成）。与 ControlLink 一样可能被调用多次。
        std::function<void(std::string assignedId)> onReady;
        // 连接结束（断开 / 认证失败 / 主动关闭）。
        std::function<void(Status why)> onClosed;
        // 进度（按 progressInterval 节流）。接收方空闲时不会触发。
        std::function<void(const JobInfo&)> onProgress;
        // 一路传输的结论。result.targetPath 对接收方是实际落点。
        std::function<void(const JobInfo&, const transfer::TransferResult&)> onFinished;
    };

    // store 由调用方注入：core 只能看见 IFileStore 接口，不许直接依赖具体
    // 实现（docs/DESIGN.md §2.2 的 R2，由 tools/check_arch.py 强制）。这也让
    // 测试可以直接塞 MemFileStore，不必碰磁盘。
    static std::shared_ptr<TransferLink> connect(asio::io_context& ctx, Config config, filestore::IFileStore& store,
                                                 Handler handler);
    ~TransferLink();

    TransferLink(const TransferLink&) = delete;
    TransferLink& operator=(const TransferLink&) = delete;

    // 把本地文件推给 toPeerId（**对端的数据连接 id**，见 PROTOCOL.md §11.13）。
    //
    // remoteName 为空时用源文件名。返回 job id；0 表示没发出去
    // （未就绪 / 引擎拒绝，例如同一个源文件已经有一个在飞）。
    std::uint64_t sendFile(std::string sourcePath, std::string remoteName, std::string toPeerId);

    // 取消一路传输。结论仍会经 onFinished 回来（outcome = kLocalCancelled），
    // 不是静默消失。
    bool cancel(std::uint64_t jobId, std::string_view reason = "用户取消");

    void cancelAll();

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    [[nodiscard]] const std::string& assignedId() const noexcept
    {
        return assignedId_;
    }
    [[nodiscard]] std::size_t activeJobs() const noexcept
    {
        return jobs_.size();
    }

    // 改接收目录。只影响**下一次**接收：正在跑的那一路落点已经定了。
    void setReceiveDir(std::string dir);

    void close();

private:
    TransferLink(asio::io_context& ctx, Config config, filestore::IFileStore& store, Handler handler);

    struct Job {
        std::uint64_t id = 0;
        bool sending = false;
        std::string path;
        std::shared_ptr<transfer::TransferPeer> peer;
    };

    transfer::TransferPeer::Config peerConfig() const;

    void onSessionReady(std::string assignedId);
    void onSessionFrame(const protocol::Frame& f);
    void onSessionClosed(Status why);

    void ensureReceiver();
    void onJobDone(std::uint64_t id, const transfer::TransferResult& result);

    void scheduleTick();
    void onTick(const std::error_code& ec);
    void reportProgress();

    [[nodiscard]] JobInfo infoOf(const Job& job) const;

    asio::io_context& ctx_;
    Config config_;
    Handler handler_;

    std::shared_ptr<session::Session> session_;
    // 传输引擎要的两样东西。store 是外部注入的（见 connect 的注释）；
    // pool 是本类独占的——两个数据连接共用一个线程池会让"谁在等我"变得含糊。
    filestore::IFileStore& store_;
    transfer::WorkerPool pool_{2};

    std::map<std::uint64_t, Job> jobs_;

    // 常驻接收。空闲时它也挂在会话上等 kFileRequestSend。
    std::shared_ptr<transfer::TransferPeer> receiver_;
    std::uint64_t receiverJobId_ = 0;

    asio::steady_timer tick_;
    std::chrono::steady_clock::time_point lastProgress_{};

    std::uint64_t nextJobId_ = 1;
    bool ready_ = false;
    bool closing_ = false;
    bool claimSent_ = false;
    std::string assignedId_;
};

}   // namespace relayer::core::net
