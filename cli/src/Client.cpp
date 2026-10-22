#include "relayer/cli/Client.h"

#include <algorithm>
#include <utility>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "relayer/core/session/PeerTable.h"
#include "relayer/core/transfer/TransferPeer.h"

// Client 的实现。
//
// 整个类只有一个状态机，三条路（send / receive / list）共用骨架：
//
//   connect ──> authenticated ──┬─> (send) 建 TransferPeer 发请求
//                               ├─> (receive) 建 TransferPeer 等请求
//                               └─> (list)  问列表、打印、结束
//                                        │
//                                        └─> finished
//
// 所有状态都只在事件循环线程上被读写（io_context 单线程跑），所以没有锁。
// 这不是"暂时偷懒"，而是本类被设计成必须单线程 run 的：pump 定时器、
// 帧回调、结束回调都在同一个 strand 语义下顺序执行。

namespace relayer::cli
{

namespace
{

using protocol::Frame;
using protocol::FrameType;

// 进度上报的最小间隔。
//
// 每个块都报一次在 4 MiB 块下是"每块一行"，看起来像日志刷屏；而在小
// 块测试里又会变成几千行。100ms 是"人眼觉得在动、输出又不至于淹掉终端"
// 的折中。
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

// pump 间隔。TransferPeer 不自带定时器（见它的 pump 注释），由调用方驱动。
// 10ms 对它够用：重传判定是秒级，ACK 是 20ms 级。
constexpr auto kPumpInterval = std::chrono::milliseconds(10);

std::string fileNameOf(const std::string &path)
{
    filestore::PathPolicy policy;
    return policy.fileName(path);
}

}  // namespace

struct Client::Impl
{
    Impl(asio::io_context &ctxIn, ClientConfig cfg, std::function<void(const ProgressReport &)> prog,
         std::function<void(const Outcome &)> done)
        : ctx(ctxIn)
        , config(std::move(cfg))
        , onProgress(std::move(prog))
        , onDone(std::move(done))
        , pumpTimer(ctxIn)
        , watchdog(ctxIn)
        , progressTimer(ctxIn)
    {
    }

    asio::io_context &ctx;
    ClientConfig config;
    std::function<void(const ProgressReport &)> onProgress;
    std::function<void(const Outcome &)> onDone;

    filestore::StdFileStore store;
    core::transfer::WorkerPool pool{2};

    std::shared_ptr<core::session::Session> session;
    std::shared_ptr<core::transfer::TransferPeer> peer;

    asio::steady_timer pumpTimer;
    asio::steady_timer watchdog;      // 空闲 / 总时长
    asio::steady_timer progressTimer;

    Action action = Action::kList;
    std::string toPeerId;
    std::string targetPath;

    bool authenticated = false;
    bool finished = false;

    // onAuthenticated 可能被回调两次（先空身份、后真实 id），而它触发的
    // 动作一旦做两遍就会把已建立的 TransferPeer 覆盖掉。见 onAuthenticated
    // 的注释。
    bool startedOnce = false;

    std::string assignedId;

    // 进度速率的基线。用"两次上报之间走了多少字节"算瞬时速度，比
    // "总字节 / 总耗时"更早反映"卡住了"。
    std::uint64_t lastBytes = 0;
    std::chrono::steady_clock::time_point lastReportAt = std::chrono::steady_clock::now();
    double lastSpeed = 0.0;

    // 空闲看门狗的"最后有进展"时刻。任何帧、任何字节变化都刷新它。
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
    std::uint64_t lastActivityBytes = 0;

    // 服务端列表（kAnswerClientList 的内容）。
    std::vector<core::session::PeerInfo> peers;

    // ---------------------------------------------------------------------
    // 收尾
    // ---------------------------------------------------------------------

    // 结束这件事只能发生一次。所有路径（成功、失败、超时、连接断）都
    // 走这里，否则会出现"先因为超时报了错，两秒后又因为对端回帧报了成功"。
    void finish(bool ok, std::string summary, Status status = Status{})
    {
        if (finished)
        {
            return;
        }
        finished = true;

        pumpTimer.cancel();
        watchdog.cancel();
        progressTimer.cancel();

        if (!ok)
        {
            // 失败时**不**调 session->close()：先让调用方看到结论，关闭
            // 交给析构。反过来（先关再报）会让 main 拿到结论时连接已经
            // 没了，日志里看不出是哪一步断的。
        }

        if (onDone)
        {
            Outcome o;
            o.ok = ok;
            o.summary = std::move(summary);
            o.status = std::move(status);
            onDone(o);
        }
    }

    // ---------------------------------------------------------------------
    // 定时器
    // ---------------------------------------------------------------------

    void startPump()
    {
        if (finished)
        {
            return;
        }
        pumpTimer.expires_after(kPumpInterval);
        pumpTimer.async_wait(
            [this](std::error_code ec)
            {
                if (ec || finished)
                {
                    return;
                }
                if (peer)
                {
                    peer->pump();
                }
                // 空闲判定放在这里而不是单独一个定时器：pump 本来就在
                // 固定节拍上跑，再起一个只会多一条要 cancel 的生命周期。
                checkIdle();
                startPump();
            });
    }

    void startProgressTimer()
    {
        if (finished)
        {
            return;
        }
        progressTimer.expires_after(kProgressInterval);
        progressTimer.async_wait(
            [this](std::error_code ec)
            {
                if (ec || finished)
                {
                    return;
                }
                reportProgress();
                startProgressTimer();
            });
    }

    void armHardTimeout()
    {
        if (config.hardTimeout.count() <= 0 || finished)
        {
            return;
        }
        watchdog.expires_after(config.hardTimeout);
        watchdog.async_wait(
            [this](std::error_code ec)
            {
                if (ec || finished)
                {
                    return;
                }
                finish(false, "超时：操作总时长超过上限", Status::Fail(ErrorCode::kTimeout, "hard timeout"));
            });
    }

    // 空闲判定：多久没有任何**看得见的进展**。
    //
    // 什么算进展：
    //   - 收到任何一帧（见 dispatch / touch）
    //   - 传输的已完成字节数变化
    //   - 本地还在忙（正在算摘要、正在读盘）——这一点最容易被漏掉
    //
    // 早先的版本只看 bytesDone，结果发送方在算摘要的那十几秒里 bytesDone
    // 一直是 0，看门狗判定"对端无响应"，在请求帧还没发出去时就把传输
    // 掐了。用户看到的是"对端没应答"，而真实原因是我们自己在读盘。
    //
    // 所以判据不是"有没有字节流过"，而是"这件事到底还有没有人在推进"：
    // 本地还在干活，就说明不是对端的问题。
    void checkIdle()
    {
        if (finished || !authenticated)
        {
            return;
        }

        if (peer)
        {
            const auto p = peer->progress();
            if (p.bytesDone != lastActivityBytes)
            {
                lastActivityBytes = p.bytesDone;
                lastActivity = std::chrono::steady_clock::now();
                return;
            }

            // 传输还没开始（发送方等摘要，或接收方等请求帧）时不算空闲。
            //
            // 发送方的摘要阶段没有上限可用：1GB 文件在慢盘上可能要几十秒，
            // 而这段时间它对网络是完全沉默的。用 idleTimeout 卡它等于
            // "文件越大越容易失败"，而那与用户感知的"网速慢"毫无关系。
            // 真正的兜底是 hardTimeout（如果调用方给了）——那是另一个语义。
            if (action == Action::kSend && !peer->started())
            {
                lastActivity = std::chrono::steady_clock::now();
                return;
            }
        }

        const auto idle = std::chrono::steady_clock::now() - lastActivity;
        if (idle < config.idleTimeout)
        {
            return;
        }

        // 注意措辞：这里的结论是"对端没动静"，不是"传输失败"。对端进程
        // 被 kill 之后，本地能观察到的**唯一**事实就是没有进展——把这点
        // 说清楚，测试失败时人才不会往错的方向查。
        finish(false, "对端长时间无响应（" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                          config.idleTimeout)
                                                              .count()) +
                          " 秒无进展）",
               Status::Fail(ErrorCode::kTimeout, "peer idle"));
    }

    void touch()
    {
        lastActivity = std::chrono::steady_clock::now();
    }

    void reportProgress()
    {
        if (!onProgress || !peer)
        {
            return;
        }
        const auto p = peer->progress();

        const auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<double>(now - lastReportAt).count();
        if (dt > 0.0 && p.bytesDone >= lastBytes)
        {
            lastSpeed = static_cast<double>(p.bytesDone - lastBytes) / dt;
        }
        lastBytes = p.bytesDone;
        lastReportAt = now;

        ProgressReport r;
        r.bytesDone = p.bytesDone;
        r.bytesTotal = p.bytesTotal;
        r.bytesPerSecond = lastSpeed;
        onProgress(r);
    }

    // ---------------------------------------------------------------------
    // 入口
    // ---------------------------------------------------------------------

    void connect()
    {
        core::session::SessionConfig sc;
        sc.role = core::session::Role::kClient;
        // 客户端侧的字段名是 clientPassword，不是 server.password——
        // 后者是"服务端要求的口令"，填错了表现为认证失败，很难查。
        sc.clientPassword = config.password;
        sc.handshakeTimeout = config.connectTimeout;

        core::session::SessionHandler h;

        h.onAuthenticated = [this](core::session::Session &self, std::string id)
        {
            const std::string previous = assignedId;
            assignedId = std::move(id);
            authenticated = true;
            touch();

            // expectId 只在自测时给。对不上时直接失败，而不是"将就用"：
            // 拿着错的身份去传输，发出去的帧会被中继当成第三方投递，
            // 表现是"对端一直没反应"，与网络问题无法区分。
            //
            // **必须等 id 非空再比**。onAuthenticated 首次回调时身份还是空
            // （见下面打印处的说明），此时比较必然不等——那样 --expect-id
            // 会把每次正常连接都判成"身份不符"，而报错里"实际 "后面是空的，
            // 看上去像服务端没给身份，排查方向直接跑偏。
            if (!config.expectId.empty() && !assignedId.empty() && assignedId != config.expectId)
            {
                finish(false, "身份不符：期望 " + config.expectId + "，实际 " + assignedId,
                       Status::Fail(ErrorCode::kProtocol, "unexpected assigned id"));
                return;
            }

            (void)self;

            // 把分配到的身份打到 stdout，供集成测试解析。
            //
            // 为什么这是"契约输出"而不是调试打印：id 由服务端分配且**递增**，
            // 测试无法预知接收端会拿到 client-几。没有这一行，测试就只能
            // 靠解析 list 的输出猜——而每次 list 又建一条新连接、消耗一个
            // 新 id，猜的过程本身会改变答案。
            //
            // 与 relayerd 的 LISTEN_PORT= 是同一类东西：进程对外宣告自己的
            // 某个动态事实，格式固定，供机器读取。
            //
            // **只打非空、且只打一次**。这里踩过一次坑：onAuthenticated 会在
            // "认证成功但还不知道自己叫什么"时先调一次（id 为空），随后
            // 收到 kAnswerId 再调一次（真实 id）。两次都打印时，第一行是
            // `ASSIGNED_ID=`，而脚本用 head -1 取第一条——拿到空串，于是
            // "接收端没拿到 id"这个报错出现在完全正常的连接上。
            //
            // 判据用"非空且与上次不同"而不是单纯的"非空"：服务端理论上可以
            // 改名再通知一次，那时该打新值；而重复通知同一个 id 时重复打印
            // 会让"第一行"这个契约失效。
            if (!assignedId.empty() && assignedId != previous)
            {
                std::printf("ASSIGNED_ID=%s\n", assignedId.c_str());
                std::fflush(stdout);
            }

            onAuthenticated();
        };

        h.onFrame = [this](core::session::Session &, const Frame &f)
        {
            touch();
            dispatch(f);
        };

        h.onClosed = [this](std::string, Status st)
        {
            if (finished)
            {
                return;
            }
            finish(false, "与中继的连接断开", std::move(st));
        };

        session = core::session::Session::connect(ctx, config.host, config.port, std::move(sc), std::move(h));

        // 连接失败（拒绝、DNS）也会走到 onClosed，所以这里不重复判定。
        startPump();
        startProgressTimer();
        armHardTimeout();
    }

    void onAuthenticated()
    {
        // **必须幂等**。Session 会在两处回调 onAuthenticated：
        //   1. 握手刚过，此时还不知道自己叫什么（id 为空）
        //   2. 收到服务端的 kAnswerId，拿到真实 id
        //
        // 踩过的坑：这里不做防护时，两次都会走下面的 startSending()，于是
        // 发送方**建了两个 TransferPeer**——第二个把第一个覆盖掉。而第二个
        // 用的是新选的 sessionId（时间戳，每次不同），所以第一个 peer 发出
        // 的请求即使收到应答，也已经没有对象能接收它了。表现是：
        //
        //   服务端日志显示 500 发出、501 回来了、然后发送方**又发了一次
        //   500（换了个 sessionId）**，最后报"对端没应答"。
        //
        // 而接收方同理：它的 peer 也被重建，`.relaypart` 建了却收不到数据。
        //
        // 这份工作只需要做一次。用 peer/请求已发起作为判据，而不是"第几次
        // 回调"——回调次数是 Session 的实现细节，不该被上层依赖。
        if (startedOnce)
        {
            return;
        }
        startedOnce = true;

        switch (action)
        {
        case Action::kList:
            requestPeerList();
            return;

        case Action::kSend:
            startSending();
            return;

        case Action::kReceive:
            // 接收方什么都不做：等对端的 kFileRequestSend。
            // 但要把 peer 建起来，否则帧来了没人接。
            startReceiving();
            return;
        }
    }

    // ---------------------------------------------------------------------
    // list
    // ---------------------------------------------------------------------

    void requestPeerList()
    {
        // 空 body。encodeAskId 那套编码在这里用不上——问列表不需要参数。
        Frame f;
        f.type = FrameType::kAskClientList;
        (void)session->send(f);
    }

    // ---------------------------------------------------------------------
    // 传输
    // ---------------------------------------------------------------------

    core::transfer::TransferPeer::Config peerConfig() const
    {
        core::transfer::TransferPeer::Config c;
        if (config.chunkSize > 0)
        {
            c.chunkSize = config.chunkSize;
        }
        // 空闲超时接进传输层：让 TransferPeer 自己也会因为"等不到应答"
        // 而失败，而不是只靠外面的看门狗。两层都设不是重复——外层的
        // 看门狗看整体有没有字节进展，能覆盖"连 peer 都没建起来"的阶段。
        c.answerTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.idleTimeout);
        c.finalizeTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.idleTimeout);
        return c;
    }

    void startSending()
    {
        if (toPeerId.empty())
        {
            finish(false, "内部错误：发送时缺少目标对端", Status::Fail(ErrorCode::kUnknown, "empty toPeerId"));
            return;
        }

        auto cfg = peerConfig();

        // sessionId 由发送方选。这里用时间戳而不是随机数：多进程测试里
        // 两边都从同一个种子起算，撞车的概率与"进程启动在同一毫秒"相当，
        // 而时间戳在日志里能直接看出"哪一次"。
        const auto sid = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

        peer = core::transfer::TransferPeer::createSender(
            ctx, session, store, pool, peerPathSource, peerPathRemoteName, toPeerId, sid, std::move(cfg),
            [this](const core::transfer::TransferResult &r) { onTransferDone(r); });

        // createSender 内部可能已经同步失败（源文件打不开），此时 onDone
        // 已经在构造函数里被调用过，peer 处于 finished 状态。
        if (peer && peer->finished())
        {
            return;
        }

        touch();
    }

    void startReceiving()
    {
        auto cfg = peerConfig();

        // 接收方用自己的 sessionId 是错的——sessionId 是**发送方**选的，
        // 接收方要接受任意一个。这里传 0 并依赖 TransferPeer 的
        // "接收方以收到的帧里的 sessionId 为准"的语义。
        //
        // 但当前实现里 onFrame 会按 sessionId 过滤（!= sessionId_ 就丢），
        // 所以接收方的 sessionId 必须与发送方一致。命令行工具没法协商，
        // 于是接收方采用 **0 表示通配**——见下面对 dispatch 的处理。
        peer = core::transfer::TransferPeer::createReceiver(
            ctx, session, store, pool, targetPath, 0, std::move(cfg),
            [this](const core::transfer::TransferResult &r) { onTransferDone(r); });

        touch();
    }

    std::string peerPathSource;
    std::string peerPathRemoteName;

    void onTransferDone(const core::transfer::TransferResult &r)
    {
        if (finished)
        {
            return;
        }

        switch (r.outcome)
        {
        case core::transfer::TransferOutcome::kCompleted:
        {
            // 最终进度报一次满的。否则最后一行停在"99%"，用户会怀疑
            // 是不是没走完——虽然 onDone 已经说了成功。
            if (onProgress && peer)
            {
                const auto p = peer->progress();
                ProgressReport rep;
                rep.bytesDone = p.bytesTotal == 0 ? p.bytesDone : p.bytesTotal;
                rep.bytesTotal = p.bytesTotal;
                rep.bytesPerSecond = lastSpeed;
                onProgress(rep);
            }

            if (action == Action::kReceive)
            {
                finish(true, "接收完成：" + r.targetPath);
            }
            else
            {
                finish(true, "发送完成");
            }
            return;
        }

        case core::transfer::TransferOutcome::kPeerCancelled:
            finish(false, "对端取消了传输", r.status);
            return;

        case core::transfer::TransferOutcome::kLocalCancelled:
            finish(false, "已取消", r.status);
            return;

        case core::transfer::TransferOutcome::kPeerGone:
            // 这是 kill 测试里**必然**走到的分支：发送端进程消失，
            // 接收端在 idleTimeout 后得到这个结论。目标文件是否完好
            // 由测试断言，这里的措辞不该暗示"文件坏了"。
            finish(false, "对端已断开（传输未完成）", r.status);
            return;

        case core::transfer::TransferOutcome::kFailed:
        default:
            finish(false, "传输失败：" + r.status.message, r.status);
            return;
        }
    }

    // ---------------------------------------------------------------------
    // 帧分派
    // ---------------------------------------------------------------------

    void dispatch(const Frame &f)
    {
        // 先给 peer。它认不认是它的事（sessionId 不符会返回 false）。
        if (peer && peer->onFrame(f))
        {
            return;
        }

        if (f.type == FrameType::kAnswerClientList)
        {
            std::string err;
            std::vector<core::session::PeerInfo> list;
            if (!core::session::decodeClientList(f.body, list, err))
            {
                finish(false, "在线列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                return;
            }
            peers = std::move(list);

            std::string text = "在线对端 " + std::to_string(peers.size()) + " 个：";
            for (const auto &p : peers)
            {
                text += "\n  " + p.id + "  " + p.address;
            }
            finish(true, std::move(text));
            return;
        }

        // 其余帧不处理。**不报错**：中继可能发来与本操作无关的信令
        // （心跳应答之类），把"不认识"当成失败会让工具在正常网络里
        // 无故退出。
    }

    void abortNow()
    {
        if (finished)
        {
            return;
        }
        finished = true;
        pumpTimer.cancel();
        watchdog.cancel();
        progressTimer.cancel();
        if (session)
        {
            session->close();
        }
        if (onDone)
        {
            Outcome o;
            o.ok = false;
            o.summary = "中止";
            o.status = Status::Fail(ErrorCode::kCancelled, "aborted");
            onDone(o);
        }
    }
};

// ---------------------------------------------------------------------------
// 门面
// ---------------------------------------------------------------------------

Client::Client(asio::io_context &ctx, ClientConfig config, std::function<void(const ProgressReport &)> onProgress,
               std::function<void(const Outcome &)> onDone)
    : impl_(std::make_unique<Impl>(ctx, std::move(config), std::move(onProgress), std::move(onDone)))
{
}

Client::~Client()
{
    if (impl_ && impl_->session)
    {
        impl_->session->close();
    }
}

void Client::sendFile(std::string sourcePath, std::string remoteName, std::string toPeerId)
{
    impl_->action = Action::kSend;
    impl_->peerPathSource = std::move(sourcePath);
    impl_->peerPathRemoteName = remoteName.empty() ? fileNameOf(impl_->peerPathSource) : std::move(remoteName);
    impl_->toPeerId = std::move(toPeerId);
    impl_->connect();
}

void Client::receiveFile(std::string targetPath)
{
    impl_->action = Action::kReceive;
    impl_->targetPath = std::move(targetPath);
    impl_->connect();
}

void Client::listPeers()
{
    impl_->action = Action::kList;
    impl_->connect();
}

void Client::abort()
{
    impl_->abortNow();
}

const std::string &Client::assignedId() const noexcept
{
    return impl_->assignedId;
}

}  // namespace relayer::cli
