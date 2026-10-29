#include "relayer/core/net/TransferLink.h"

#include <utility>
#include <vector>

#include "relayer/core/session/Session.h"

namespace relayer::core::net {

namespace {
using protocol::Frame;

session::SessionConfig makeSessionConfig(const TransferLink::Config& cfg)
{
    session::SessionConfig sc;
    sc.role = session::Role::kClient;
    sc.clientPassword = cfg.password;
    sc.server.allowNoAuth = cfg.noAuth;
    return sc;
}
}   // namespace

TransferLink::TransferLink(asio::io_context& ctx, Config config, filestore::IFileStore& store, Handler handler)
    : ctx_(ctx), config_(std::move(config)), handler_(std::move(handler)), store_(store), tick_(ctx)
{
}

TransferLink::~TransferLink() = default;

transfer::TransferPeer::Config TransferLink::peerConfig() const
{
    transfer::TransferPeer::Config c;
    if (config_.chunkSize > 0) {
        c.chunkSize = config_.chunkSize;
    }
    c.overwriteExisting = config_.overwriteExisting;
    c.finalizeTimeout = config_.idleTimeout;
    c.answerTimeout = config_.answerTimeout;
    return c;
}

std::shared_ptr<TransferLink> TransferLink::connect(asio::io_context& ctx, Config config, filestore::IFileStore& store,
                                                    Handler handler)
{
    std::shared_ptr<TransferLink> self(new TransferLink(ctx, std::move(config), store, std::move(handler)));

    session::SessionHandler sh;
    std::weak_ptr<TransferLink> weak = self;
    sh.onAuthenticated = [weak](session::Session&, std::string id) {
        if (auto link = weak.lock()) {
            link->onSessionReady(std::move(id));
        }
    };
    sh.onFrame = [weak](session::Session&, const Frame& f) {
        if (auto link = weak.lock()) {
            link->onSessionFrame(f);
        }
    };
    sh.onClosed = [weak](std::string, Status s) {
        if (auto link = weak.lock()) {
            link->onSessionClosed(std::move(s));
        }
    };

    auto cfg = self->config_;
    self->session_ = session::Session::connect(ctx, cfg.host, cfg.port, makeSessionConfig(cfg), std::move(sh));

    if (self->session_ != nullptr && !cfg.requestedId.empty()) {
        // 与 ControlLink 同一条时序约定：声明身份必须在认证之后。
        std::weak_ptr<TransferLink> w2 = self;
        asio::post(ctx, [w2] {
            auto link = w2.lock();
            if (link == nullptr || link->session_ == nullptr || !link->session_->authenticated()) {
                return;
            }
            (void)link->session_->claimPeerId(link->config_.requestedId);
        });
    }
    return self;
}

void TransferLink::onSessionReady(std::string assignedId)
{
    ready_ = true;
    if (!assignedId.empty()) {
        assignedId_ = std::move(assignedId);
    }

    // 声明身份。数据连接的 id 由上层指定（通常是"控制连接的 id + 后缀"），
    // 为什么必须自己声明：中继按帧头 `to` 里的 id 找连接转发，而双方交换
    // 数据连接 id 的时机晚于认证——用服务端分配的 client-N 也能工作，但
    // 那个号在每次重连后都变，调用方没法预判；声明成确定的名字让"重连后
    // 数据连接仍然叫同一个名字"成立。
    if (!config_.requestedId.empty() && !claimSent_ && session_ != nullptr && session_->authenticated() &&
        assignedId_ != config_.requestedId) {
        claimSent_ = true;
        (void)session_->claimPeerId(config_.requestedId);
    }

    if (handler_.onReady) {
        handler_.onReady(assignedId_);
    }

    // 认证一过就把接收挂上：数据连接的意义之一是"随时能收"，等到有人要发
    // 才挂，对端那一下请求就会落空（它会收到拒绝，然后退避重试）。
    if (config_.acceptIncoming) {
        ensureReceiver();
    }
    scheduleTick();
}

void TransferLink::onSessionFrame(const Frame& f)
{
    // 数据连接上只有传输帧。全部交给接收 peer；发送 peer 的应答也由它转。
    //
    // 这里不区分"这是给哪个 job 的"：TransferPeer::onFrame 自己按 sessionId
    // 过滤，返回 false 只是"不是我的"，不是错误。
    if (receiver_ && receiver_->onFrame(f)) {
        return;
    }
    for (auto& [id, job] : jobs_) {
        if (job.peer && job.peer->onFrame(f)) {
            return;
        }
    }
    // 谁都不认的帧（迟到的 ACK、别的会话的残帧）：丢掉。数据连接上没有
    // 通知类帧，留个日志反而会被正常重传刷屏。
}

void TransferLink::onSessionClosed(Status why)
{
    ready_ = false;
    const Status reason = why.ok() ? Status::Fail(ErrorCode::kCancelled, "数据连接已关闭") : why;

    // 先把在跑的传输收掉（它们的结论必须回来，否则界面会停在"传输中"），
    // 再通知上层。
    if (receiver_ && !receiver_->finished()) {
        receiver_->cancel("数据连接断开");
    }
    std::vector<std::shared_ptr<transfer::TransferPeer>> peers;
    peers.reserve(jobs_.size());
    for (auto& [id, job] : jobs_) {
        if (job.peer && !job.peer->finished()) {
            peers.push_back(job.peer);
        }
    }
    for (auto& p : peers) {
        p->cancel("数据连接断开");
    }

    if (handler_.onClosed) {
        handler_.onClosed(reason);
    }
}

void TransferLink::ensureReceiver()
{
    if (closing_ || session_ == nullptr || !ready_) {
        return;
    }
    if (receiver_ && !receiver_->finished()) {
        return;
    }
    if (receiverJobId_ == 0) {
        receiverJobId_ = nextJobId_++;
    }

    // sessionId 传 0 = 通配：本端接受第一个 kFileRequestSend 带来的号。
    //
    // 双连接下这一点更关键：发送方的数据连接和接收方的数据连接是两条不同的
    // 连接，接收方**无从知道**对方用了什么 sessionId。传 0 是唯一可行约定。
    const std::uint64_t id = receiverJobId_;
    receiver_ = transfer::TransferPeer::createReceiver(ctx_, session_, store_, pool_, config_.receiveDir, 0, peerConfig(),
                                                       [this, id](const transfer::TransferResult& r) { onJobDone(id, r); });
}

std::uint64_t TransferLink::sendFile(std::string sourcePath, std::string remoteName, std::string toPeerId)
{
    if (!ready_ || closing_ || session_ == nullptr) {
        return 0;
    }
    if (sourcePath.empty() || toPeerId.empty()) {
        return 0;
    }

    const std::uint64_t id = nextJobId_++;

    // sessionId 用 job id：同一条数据连接上可能同时有两个发送（发往不同
    // 对端），引擎靠它区分各自的帧。
    const auto sessionId = static_cast<std::int64_t>(id);

    Job job;
    job.id = id;
    job.sending = true;
    job.path = sourcePath;
    job.peer = transfer::TransferPeer::createSender(ctx_, session_, store_, pool_, std::move(sourcePath), std::move(remoteName),
                                                    std::move(toPeerId), sessionId, peerConfig(),
                                                    [this, id](const transfer::TransferResult& r) { onJobDone(id, r); });

    if (!job.peer) {
        return 0;
    }
    jobs_.emplace(id, std::move(job));
    return id;
}

bool TransferLink::cancel(std::uint64_t jobId, std::string_view reason)
{
    if (jobId == receiverJobId_ && receiver_) {
        if (receiver_->finished()) {
            return false;
        }
        receiver_->cancel(reason);
        return true;
    }
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end() || !it->second.peer || it->second.peer->finished()) {
        return false;
    }
    it->second.peer->cancel(reason);
    return true;
}

void TransferLink::cancelAll()
{
    if (receiver_ && !receiver_->finished()) {
        receiver_->cancel("全部取消");
    }
    // 先收集再取消：cancel 可能同步触发 onDone，那会改 jobs_。
    std::vector<std::shared_ptr<transfer::TransferPeer>> peers;
    peers.reserve(jobs_.size());
    for (auto& [id, job] : jobs_) {
        if (job.peer && !job.peer->finished()) {
            peers.push_back(job.peer);
        }
    }
    for (auto& p : peers) {
        p->cancel("全部取消");
    }
}

void TransferLink::onJobDone(std::uint64_t id, const transfer::TransferResult& result)
{
    if (id == receiverJobId_) {
        // 接收：先把 job 描述留给上层（落点/结果），再重挂。
        JobInfo info;
        info.id = id;
        info.sending = false;
        info.path = result.targetPath;
        if (receiver_) {
            // receiver_ 在这一刻还活着（reset 在回调之后），计数读得到。
            const auto p = receiver_->progress();
            info.bytesDone = p.bytesDone;
            info.bytesTotal = p.bytesTotal;
            const auto st = receiver_->stats();
            info.chunksSent = st.chunksSent;
            info.chunksReceived = st.chunksReceived;
            info.chunksRetransmitted = st.chunksRetransmitted;
            info.duplicateChunks = st.duplicateChunks;
            info.acksSent = st.acksSent;
            info.acksReceived = st.acksReceived;
        }
        if (handler_.onFinished) {
            handler_.onFinished(info, result);
        }
        // 重挂接收。失败的那一次也要重挂——否则一个坏请求（路径越界、
        // 磁盘满）就把整条数据连接的接收能力废掉了。
        receiver_.reset();
        ensureReceiver();
        return;
    }

    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return;
    }
    // 摘出来再回调：回调里可能立刻发下一个文件（"传完就传下一个"是常态），
    // 那时 jobs_ 必须已经干净。
    Job job = std::move(it->second);
    jobs_.erase(it);

    if (handler_.onFinished) {
        handler_.onFinished(infoOf(job), result);
    }
}

TransferLink::JobInfo TransferLink::infoOf(const Job& job) const
{
    JobInfo info;
    info.id = job.id;
    info.sending = job.sending;
    info.path = job.path;
    if (job.peer) {
        const auto p = job.peer->progress();
        info.bytesDone = p.bytesDone;
        info.bytesTotal = p.bytesTotal;
        const auto st = job.peer->stats();
        info.chunksSent = st.chunksSent;
        info.chunksReceived = st.chunksReceived;
        info.chunksRetransmitted = st.chunksRetransmitted;
        info.duplicateChunks = st.duplicateChunks;
        info.acksSent = st.acksSent;
        info.acksReceived = st.acksReceived;
    }
    return info;
}

void TransferLink::scheduleTick()
{
    if (closing_) {
        return;
    }
    tick_.expires_after(config_.tickInterval);
    std::weak_ptr<TransferLink> weak = weak_from_this();
    tick_.async_wait([weak](const std::error_code& ec) {
        if (auto link = weak.lock()) {
            link->onTick(ec);
        }
    });
}

void TransferLink::onTick(const std::error_code& ec)
{
    if (ec || closing_) {
        return;
    }

    // 先把 peer 拷出来再 pump：pump 内部可能触发 onDone → onJobDone →
    // 从 jobs_ 里删除，直接遍历 map 会在中途失效。
    std::vector<std::shared_ptr<transfer::TransferPeer>> peers;
    peers.reserve(jobs_.size() + 1);
    if (receiver_) {
        peers.push_back(receiver_);
    }
    for (auto& [id, job] : jobs_) {
        if (job.peer) {
            peers.push_back(job.peer);
        }
    }
    for (auto& p : peers) {
        p->pump();
    }

    reportProgress();
    scheduleTick();
}

void TransferLink::reportProgress()
{
    if (!handler_.onProgress) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastProgress_ < config_.progressInterval) {
        return;
    }
    lastProgress_ = now;

    for (const auto& [id, job] : jobs_) {
        if (job.peer && !job.peer->finished()) {
            handler_.onProgress(infoOf(job));
        }
    }
    // 接收进度也报：连续收两个文件时，界面上的"正在接收"要有进度可看。
    // 空闲的 receiver（没收到请求）全都为 0，跳过它避免刷无意义的 0%。
    if (receiver_ && !receiver_->finished()) {
        const auto p = receiver_->progress();
        if (p.chunksTotal > 0) {
            JobInfo info;
            info.id = receiverJobId_;
            info.sending = false;
            info.bytesDone = p.bytesDone;
            info.bytesTotal = p.bytesTotal;
            handler_.onProgress(info);
        }
    }
}

void TransferLink::setReceiveDir(std::string dir)
{
    config_.receiveDir = std::move(dir);
    if (receiver_ && !receiver_->finished()) {
        // 正在跑的那一路落点已经定了，不能改。重挂会把它丢掉——所以只在
        // 空闲时重建：空闲的 receiver 没有任何状态需要保留。
        const auto p = receiver_->progress();
        if (p.chunksTotal == 0) {
            receiver_.reset();
            ensureReceiver();
        }
    }
}

void TransferLink::close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    ready_ = false;
    tick_.cancel();
    if (session_ != nullptr) {
        session_->close();
    }
    // 不等 onClosed 投递：调用方调完 close 之后不该再收到"传输成功"。
    if (receiver_ && !receiver_->finished()) {
        receiver_->cancel("数据连接已关闭");
    }
    std::vector<std::shared_ptr<transfer::TransferPeer>> peers;
    peers.reserve(jobs_.size());
    for (auto& [id, job] : jobs_) {
        if (job.peer && !job.peer->finished()) {
            peers.push_back(job.peer);
        }
    }
    for (auto& p : peers) {
        p->cancel("数据连接已关闭");
    }
}

}   // namespace relayer::core::net
