#include "relayer/cli/Client.h"

#include <algorithm>
#include <utility>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "relayer/core/session/FileList.h"
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
// 每块都报一次的话，默认 256 KiB 块下一个 256 MB 的文件会刷出 1024 行，
// 终端直接淹掉。100ms 是"人眼觉得在动、输出又不至于刷屏"的折中。
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

    // ---- 常驻会话上的"搭车发送" ----
    //
    // 常驻接收会话（keepReceiving）活着时，发送**不新建连接**，而是把第二个
    // TransferPeer 挂到同一条 session 上。两边的帧靠 sessionId 区分：接收方
    // 空闲时以 0 通配、只认领 kFileRequestSend；发送方的应答/ACK 帧类型
    // 不同，不会被接收 peer 误吞（见 TransferPeer::onFrame 的 sessionMismatch
    // 判定）。服务端只按 to 字段转发，不关心一条连接上跑几个传输。
    //
    // 这样做的根因：每新建一条连接，服务端就分配一个新 id —— GUI"点一次
    // 发送，自己的 id 就涨一号"，对方表里的我们也随之过期。
    std::shared_ptr<core::transfer::TransferPeer> sendPeer;
    std::string sendSourcePath;
    std::string sendRemoteName;
    std::string sendToPeerId;
    std::function<void(const Outcome &)> onSendDone;
    bool sendActive = false;

    asio::steady_timer pumpTimer;
    asio::steady_timer watchdog;  // 空闲 / 总时长
    asio::steady_timer progressTimer;

    Action action = Action::kList;
    std::string toPeerId;

    // kBrowse 的目标与相对路径（PROTOCOL.md §11.8）。
    std::string browsePeerId;
    std::string browseRelPath;
    std::string targetPath;

    bool authenticated = false;
    bool finished = false;

    // onAuthenticated 可能被回调两次（先空身份、后真实 id），而它触发的
    // 动作一旦做两遍就会把已建立的 TransferPeer 覆盖掉。见 onAuthenticated
    // 的注释。
    bool startedOnce = false;

    // 已经发过 kClaimId。声明成功后服务端会再回调一次 onAuthenticated
    // （带新 id），没有这个标志就会无限声明。
    bool claimSent = false;

    // 自检发送已启动（见 ClientConfig::selfSendPath）。
    bool selfSendStarted = false;

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

    // 远端列举的结果（kAnswerFileList 的内容）。由 finish() 搬进 Outcome。
    std::vector<core::session::RemoteEntry> remoteEntries;
    core::session::FileListStatus listStatus = core::session::FileListStatus::kOk;

    // ---------------------------------------------------------------------
    // 收尾
    // ---------------------------------------------------------------------

    // 结束这件事只能发生一次。所有路径（成功、失败、超时、连接断）都
    // 走这里，否则会出现"先因为超时报了错，两秒后又因为对端回帧报了成功"。
    void finish(bool ok, std::string summary, Status status = Status{}, bool connectionLost = false)
    {
        if (finished)
        {
            return;
        }
        finished = true;

        pumpTimer.cancel();
        watchdog.cancel();
        progressTimer.cancel();

        // 会话要结束了：正在搭车发送的那一次不再单独回调——上层能收到的
        // 结论只能有一个，就是本次 onDone（会话级）。sendPeer 随后随
        // Impl 一起释放。
        sendActive = false;
        onSendDone = nullptr;

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
            o.connectionLost = connectionLost;
            if (peer)
            {
                // 只在这里读一次：finish 之后 peer 会被上层释放，之后再取
                // 就是悬空访问。
                const auto s = peer->stats();
                o.stats.chunksSent = s.chunksSent;
                o.stats.chunksReceived = s.chunksReceived;
                o.stats.chunksRetransmitted = s.chunksRetransmitted;
                o.stats.duplicateChunks = s.duplicateChunks;
                o.stats.acksSent = s.acksSent;
                o.stats.acksReceived = s.acksReceived;
            }
            if (action == Action::kList)
            {
                for (const auto &p : peers)
                {
                    o.peerList.push_back(PeerEntry{p.id, p.address});
                }
            }
            else if (action == Action::kBrowse)
            {
                o.remoteEntries = remoteEntries;
                o.listStatus = listStatus;
            }
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
                if (sendPeer)
                {
                    sendPeer->pump();
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

        // 看门狗盯"当前在动的那件事"：有会话内发送时看发送，否则看接收。
        // 两者同时在跑时只看发送——接收那边的沉默由它自己的收尾/空闲
        // 判定负责，而批次场景下用户关心的是发送是否卡住。
        auto *active = (sendActive && sendPeer) ? sendPeer.get() : peer.get();
        if (active)
        {
            const auto p = active->progress();
            if (p.bytesDone != lastActivityBytes)
            {
                lastActivityBytes = p.bytesDone;
                lastActivity = std::chrono::steady_clock::now();
                return;
            }

            // 传输还没开始（发送方等摘要，或接收方等请求帧）时不算空闲。
            //
            // 两边的沉默都是**正常**的：
            //   - 发送方的摘要阶段没有上限可用：1GB 文件在慢盘上可能要几十秒，
            //     而这段时间它对网络是完全沉默的。用 idleTimeout 卡它等于
            //     "文件越大越容易失败"，而那与用户感知的"网速慢"毫无关系。
            //   - 接收方在第一个 kFileRequestSend 到达之前只是挂着等别人发，
            //     这段时间长不代表对端失联——对端可能还没点"发送"。
            //
            // 所以判据交给传输层：started() 就是"现在沉默是否正常"的答案
            // （见 TransferPeer.h 的注释）。**不要按角色分别判**——早先只写了
            // kSend，结果接收方用 `--idle-timeout 8` 时，发送端还在算摘要、
            // 请求帧还没发出，接收端就把自己掐了；发送方随后拿到的是
            // "peer did not answer"，排查方向完全错（看着像中继丢包）。
            //
            // 真正的兜底是 hardTimeout（如果调用方给了）——那是另一个语义：
            // 不管有没有进展，总共只许花这么久。
            if (!active->started())
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
        finish(false,
               "对端长时间无响应（" +
                   std::to_string(std::chrono::duration_cast<std::chrono::seconds>(config.idleTimeout).count()) +
                   " 秒无进展）",
               Status::Fail(ErrorCode::kTimeout, "peer idle"),
               // 发送方视角的"对端失联"就是连接级失败：可能是对端进程死了，
               // 也可能是中继链路断了。批次执行者凭它决定"剩余项不再发"。
               true);
    }

    void touch()
    {
        lastActivity = std::chrono::steady_clock::now();
    }

    void reportProgress()
    {
        auto *active = (sendActive && sendPeer) ? sendPeer.get() : peer.get();
        if (!onProgress || !active)
        {
            return;
        }
        const auto p = active->progress();

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
        // server.allowNoAuth 在客户端侧的语义是"我知道对端关了认证"：
        // 决定 ClientHandshake 走 kNoAuth（不期待 Challenge）还是
        // kRequireAuth。不设置它时，无认证中继发来的 answer_id 会被
        // 当成"未完成握手册收到了业务帧"而拒绝连接——症状是"连上去
        // 立刻被断开"，与中继不可达的表现完全不同，极难排查。
        sc.server.allowNoAuth = config.serverAllowNoAuth;
        sc.handshakeTimeout = config.connectTimeout;

        core::session::SessionHandler h;

        h.onAuthenticated = [this](core::session::Session &self, std::string id)
        {
            const std::string previous = assignedId;
            assignedId = std::move(id);
            authenticated = true;
            touch();

            // 声明标识：认证一过就发，只发一次。
            //
            // 发在这一刻而不是"拿到 id 之后"：服务端在认证完成时就分配了
            // 临时号并发 kAnswerId，两者到达顺序不确定；而 kClaimId 只要
            // 在认证之后发就有效（服务端会做改名）。
            //
            // claimSent 防重入：声明成功后 assignPeerId 会再回调一次
            // onAuthenticated（新身份），不设防就会无限声明。
            if (!config.requestedId.empty() && !claimSent)
            {
                claimSent = true;
                if (const auto st = self.claimPeerId(config.requestedId); !st.ok())
                {
                    finish(false, "声明标识失败：" + config.requestedId, st, true);
                    return;
                }
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
            //
            // 声明了标识时还要多一条：**临时号不算数**。认证后服务端先发
            // client-N，改名成功后再发真正的名字；中间那个 N 一旦打出去，
            // e2e 的 head -1 就会拿到它（"接收端 id 不是我要的那个"），
            // GUI 也会闪一下错误的身份。所以只认"等于声明的那个"。
            const bool identityFinal = config.requestedId.empty() || assignedId == config.requestedId;
            if (!assignedId.empty() && assignedId != previous && identityFinal)
            {
                std::printf("ASSIGNED_ID=%s\n", assignedId.c_str());
                std::fflush(stdout);
                if (config.onIdAssigned)
                {
                    config.onIdAssigned(assignedId);
                }

                // askListOnStart 放在"真实 id 到手"这一刻而不是 startReceiving：
                // 首次 onAuthenticated 回调时身份还是空的，此刻问列表，服务端
                // 返回的名单里未必有自己（id 都还没发），GUI 的自标红就会失效。
                if (action == Action::kReceive && config.askListOnStart)
                {
                    touch();
                    requestPeerList();
                }
            }

            onAuthenticated();

            // 自检（PROTOCOL.md §11.3.1）：接收端已经建好，把自己的文件
            // 发给自己，帧由中继绕回本连接。
            //
            // 必须等身份**最终确定**（identityFinal）再发：声明了标识时，
            // 第一次回调的身份还是空的，那时发出去 to 会是空串（被当成
            // "发给服务端"），请求直接进黑洞。
            if (action == Action::kReceive && identityFinal && !config.selfSendPath.empty() && !selfSendStarted)
            {
                selfSendStarted = true;
                const bool started =
                    startSendOnSession(config.selfSendPath, std::string(), assignedId,
                                       [this](const Outcome &o)
                                       {
                                           // 成功不做额外动作：接收侧收完会自己 finish（退出码
                                           // 由它决定）。失败要提前结束，否则进程挂在"等文件"
                                           // 上直到硬超时，看不出是自检发送出了问题。
                                           if (!o.ok)
                                           {
                                               finish(false, "自检发送失败：" + o.summary, o.status, o.connectionLost);
                                           }
                                       });
                if (!started)
                {
                    finish(false, "自检发送未能启动（接收会话不可用）",
                           Status::Fail(ErrorCode::kUnknown, "self-send unavailable"), false);
                }
            }
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
            // 未认证状态收到业务帧，几乎总是"客户端按认证模式连了一个
            // 无认证中继"：服务端跳过握手直接发 answer_id，被客户端按
            // 协议拒绝。这个场景足够常见（服务端 --allow-no-auth 部署），
            // 给一条可照做的提示，而不是让用户去猜协议细节。
            if (!config.serverAllowNoAuth && st.message.find("non-auth frame") != std::string::npos)
            {
                st.message += "（中继可能以 --allow-no-auth 启动，客户端请加 --no-auth 重试）";
            }
            // 声明标识被拒：这不是"网络断了"，措辞必须让用户知道该改什么。
            // 两种原因的处置完全不同——被占用是要么换名字、要么关掉占用的
            // 那台；不合法是名字本身不能用。
            if (st.code == ErrorCode::kIdTaken)
            {
                finish(false, "标识「" + config.requestedId + "」已被占用，该标识同时只能有一条连接", std::move(st),
                       true);
                return;
            }
            if (st.code == ErrorCode::kIdInvalid)
            {
                finish(false, "标识「" + config.requestedId + "」不合法（不能为空/超长/含非法字符）", std::move(st),
                       true);
                return;
            }
            finish(false, "与中继的连接断开", std::move(st), true);
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

        case Action::kBrowse:
            // 只要 `to` 对，不需要等自己的 id——服务端在认证时已经登记了
            // 这条连接，转发目标查得到。
            requestFileList();
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

    void requestFileList()
    {
        Frame f;
        f.type = FrameType::kAskFileList;
        f.to = browsePeerId;
        f.body = core::session::encodeAskFileList(browseRelPath);
        if (!session->send(f).ok())
        {
            finish(false, "远端列举请求发送失败", Status::Fail(ErrorCode::kProtocol, "send failed"));
        }
    }

    // 对端来问目录：按共享根解析、列一层、回一条。**只服务这一个根**。
    //
    // 目录列举是同步做的：一次 readdir 在该规模下是毫秒级，而引入工作线程
    // 会把"谁保证 store 生命周期"的问题带进来。真遇到网络盘这类慢列举，
    // 再按传输路径那样丢进 WorkerPool。
    void serveFileList(const Frame &req)
    {
        Frame answer;
        answer.type = FrameType::kAnswerFileList;
        // 请求者的身份由服务端在转发时绑定（不是自称），可以直接当回信地址。
        answer.to = req.from;

        std::vector<core::session::RemoteEntry> out;
        auto status = core::session::FileListStatus::kOk;

        std::string rel;
        std::string err;
        if (!core::session::decodeAskFileList(req.body, rel, err))
        {
            status = core::session::FileListStatus::kRejected;
        }
        else if (config.shareRoot.empty())
        {
            // 没开共享：明确拒绝。静默不回会让对方等满应答超时，
            // 看起来像网络问题。
            status = core::session::FileListStatus::kRejected;
        }
        else
        {
            const std::string target = rel.empty() ? config.shareRoot : store.resolveWithin(config.shareRoot, rel);
            if (target.empty())
            {
                status = core::session::FileListStatus::kRejected;  // 绝对路径 / 越界
            }
            else
            {
                const auto st = store.stat(target);
                if (!st.exists || !st.isDir)
                {
                    status = core::session::FileListStatus::kNotFound;
                }
                else
                {
                    std::vector<filestore::DirEntry> raw;
                    if (!store.list(target, false, raw).ok())
                    {
                        status = core::session::FileListStatus::kNotFound;
                    }
                    else
                    {
                        // 目录在前、同类型按名字排：列表是给人看的，顺序稳定
                        // 比"文件系统返回什么算什么"重要（也方便测试断言）。
                        std::sort(raw.begin(), raw.end(),
                                  [](const filestore::DirEntry &a, const filestore::DirEntry &b)
                                  {
                                      if (a.isDir != b.isDir)
                                      {
                                          return a.isDir;
                                      }
                                      return a.name < b.name;
                                  });
                        if (raw.size() > core::session::kMaxListEntries)
                        {
                            raw.resize(core::session::kMaxListEntries);
                            status = core::session::FileListStatus::kTruncated;
                        }
                        out.reserve(raw.size());
                        for (const auto &e : raw)
                        {
                            core::session::RemoteEntry r;
                            r.name = e.name;
                            r.isDir = e.isDir;
                            r.size = e.size;
                            r.mtime = e.mtime;
                            out.push_back(std::move(r));
                        }
                    }
                }
            }
        }

        if (const auto st = core::session::encodeAnswerFileList(status, out, answer.body); !st.ok())
        {
            return;  // 编码失败只可能是内部 bug，没必要再回一个坏帧
        }
        (void)session->send(answer);
    }

    void refreshPeers()
    {
        // 在现有连接上问列表，不新建连接（见 Client::refreshPeers 的注释）。
        // touch 刷新空闲基线：等响应的往返不该被看门狗当成"对端无响应"。
        if (finished || action != Action::kReceive || !authenticated || !session)
        {
            return;
        }
        touch();
        requestPeerList();
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
        c.finalizeTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.idleTimeout);

        // answerTimeout 与 idleTimeout 解耦：它只管"请求帧发出后等
        // AnswerSend"这一段（见 TransferPeer pump 里 started_ 之前的分支）。
        // 目标不在线时中继静默丢帧（协议没有投递失败帧，§9 靠超时发现），
        // 用 60s 的 idleTimeout 意味着"发错 id 要干等一分钟才报错"。
        // AnswerSend 在对端在线时是毫秒级（收到请求即回），10s 已经很宽裕；
        // 传输开始后的超时仍由 finalizeTimeout/重传逻辑负责，不受影响。
        c.answerTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min(config.idleTimeout, std::chrono::seconds(10)));

        // 覆盖策略透传。默认 false（拒绝覆盖）是 core 层的设计；CLI 的
        // --overwrite 与 GUI 的接收开关在这里汇入。
        c.overwriteExisting = config.overwriteExisting;
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
        const auto sid =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
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
        // 接收方用自己的 sessionId 是错的——sessionId 是**发送方**选的，
        // 接收方要接受任意一个。这里传 0 并依赖 TransferPeer 的
        // "接收方以收到的帧里的 sessionId 为准"的语义。
        //
        // 但当前实现里 onFrame 会按 sessionId 过滤（!= sessionId_ 就丢），
        // 所以接收方的 sessionId 必须与发送方一致。命令行工具没法协商，
        // 于是接收方采用 **0 表示通配**——见下面对 dispatch 的处理。
        auto cfg = peerConfig();
        peer = core::transfer::TransferPeer::createReceiver(ctx, session, store, pool, targetPath, 0, std::move(cfg),
                                                            [this](const core::transfer::TransferResult &r)
                                                            { onTransferDone(r); });

        touch();
    }

    std::string peerPathSource;
    std::string peerPathRemoteName;

    // ---------------------------------------------------------------------
    // 常驻会话上的发送
    // ---------------------------------------------------------------------

    // 只有"常驻接收会话活着"时才能搭车发送：别的 action 是一次性操作，
    // 连接生命周期与操作绑定，发完就该结束。
    bool canSendOnSession() const noexcept
    {
        return !finished && authenticated && session && action == Action::kReceive;
    }

    bool startSendOnSession(std::string source, std::string remoteName, std::string targetPeerId,
                            std::function<void(const Outcome &)> done)
    {
        if (!canSendOnSession() || sendActive)
        {
            return false;
        }
        sendActive = true;
        onSendDone = std::move(done);
        sendSourcePath = std::move(source);
        sendRemoteName = remoteName.empty() ? fileNameOf(sendSourcePath) : std::move(remoteName);
        sendToPeerId = std::move(targetPeerId);

        auto cfg = peerConfig();
        const auto sid =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();

        auto created = core::transfer::TransferPeer::createSender(
            ctx, session, store, pool, sendSourcePath, sendRemoteName, sendToPeerId, sid, std::move(cfg),
            [this](const core::transfer::TransferResult &r) { onSendResult(r); });
        sendPeer = std::move(created);

        // 源文件打不开时 createSender 会同步回调 onSendResult（sendActive
        // 已被清、结论已上报），这里的 touch 无害。
        touch();
        return true;
    }

    // 一次"会话内发送"的结论。**不是**整条会话的结论：会话继续活着。
    void finishSend(bool ok, std::string summary, Status status = Status{}, bool connectionLost = false)
    {
        if (!sendActive)
        {
            return;
        }
        sendActive = false;

        Outcome o;
        o.ok = ok;
        o.summary = std::move(summary);
        o.status = std::move(status);
        o.connectionLost = connectionLost;
        if (sendPeer)
        {
            const auto s = sendPeer->stats();
            o.stats.chunksSent = s.chunksSent;
            o.stats.chunksReceived = s.chunksReceived;
            o.stats.chunksRetransmitted = s.chunksRetransmitted;
            o.stats.duplicateChunks = s.duplicateChunks;
            o.stats.acksSent = s.acksSent;
            o.stats.acksReceived = s.acksReceived;
        }

        auto cb = std::move(onSendDone);
        onSendDone = nullptr;
        // 不在 TransferPeer 的完成回调栈上释放它（use-after-free 老坑），
        // 交给下一轮事件循环。
        asio::post(ctx,
                   [this]
                   {
                       if (!sendActive)
                       {
                           sendPeer.reset();
                       }
                   });
        if (cb)
        {
            cb(o);
        }
    }

    void onSendResult(const core::transfer::TransferResult &r)
    {
        switch (r.outcome)
        {
        case core::transfer::TransferOutcome::kCompleted:
            finishSend(true, "发送完成");
            return;

        case core::transfer::TransferOutcome::kPeerCancelled:
            finishSend(false, "对端取消了传输", r.status);
            return;

        case core::transfer::TransferOutcome::kLocalCancelled:
            finishSend(false, "已取消", r.status);
            return;

        case core::transfer::TransferOutcome::kPeerGone:
            // 发送方看到"对端长时间没动静/连接断了"：这条会话还能不能继续
            // 收文件由 Session 自己判（真正断开时 onClosed 会结束整个 Client），
            // 这里只把结论上报，让上层决定要不要中止批次。
            finishSend(false, "对端已断开（传输未完成）", r.status, true);
            return;

        case core::transfer::TransferOutcome::kFailed:
        default:
            finishSend(false, "传输失败：" + r.status.message, r.status);
            return;
        }
    }

    void cancelSendNow()
    {
        if (!sendActive || !sendPeer || sendPeer->finished())
        {
            return;
        }
        // cancel 会同步走 onSendResult → finishSend（kLocalCancelled），
        // 会话本身不动。
        sendPeer->cancel("cancelled by user");
    }

    // 原地重建 receiver（仅当当前 peer 已经结束）。两个触发点共用：
    //   - onTransferDone 里的 post：完成/失败后的常规重建；
    //   - dispatch 里请求帧先到时的同步重建。
    // 后者先发生时，前者**不能**再建第二个把正等数据的 receiver 顶掉
    // （顶掉的后果是发送方发块没人收，等满 answerTimeout 才报错）。
    void rebuildReceiver()
    {
        if (finished)
        {
            return;
        }
        if (peer && !peer->finished())
        {
            return;
        }
        startReceiving();
    }

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
                if (config.keepReceiving)
                {
                    // 常驻接收：**不 finish**。报完成回调后原地重建
                    // receiver，继续在同一条连接上等下一个请求——服务端
                    // 分配的 id 因此保持不变（见 ClientConfig::keepReceiving
                    // 的注释，这是"发送方表格里的对端 id 不过期"的关键）。
                    // 重建走 post：此刻在 TransferPeer 的完成回调栈上，
                    // 直接替换 peer 成员是回调栈析构的老坑。
                    if (config.onFileReceived)
                    {
                        config.onFileReceived(r.targetPath);
                    }
                    asio::post(ctx, [this] { rebuildReceiver(); });
                    return;
                }
                finish(true, "接收完成：" + r.targetPath);
            }
            else
            {
                finish(true, "发送完成");
            }
            return;
        }

        case core::transfer::TransferOutcome::kPeerCancelled:
            if (action == Action::kReceive && config.keepReceiving)
            {
                // 对端中途取消：连接还活着，同 kFailed 的理由原地重建
                // （见下面 kFailed 分支的注释），否则一次取消就会让本端
                // 换 id，发送方下次就找不到我们了。
                if (config.onReceiveFailed)
                {
                    config.onReceiveFailed("对端取消了传输：" + r.status.message);
                }
                asio::post(ctx, [this] { rebuildReceiver(); });
                return;
            }
            finish(false, "对端取消了传输", r.status);
            return;

        case core::transfer::TransferOutcome::kLocalCancelled:
            finish(false, "已取消", r.status);
            return;

        case core::transfer::TransferOutcome::kPeerGone:
            // 这是 kill 测试里**必然**走到的分支：发送端进程消失，
            // 接收端在 idleTimeout 后得到这个结论。目标文件是否完好
            // 由测试断言，这里的措辞不该暗示"文件坏了"。
            finish(false, "对端已断开（传输未完成）", r.status, true);
            return;

        case core::transfer::TransferOutcome::kFailed:
        default:
            if (action == Action::kReceive && config.keepReceiving)
            {
                // 常驻接收下的**局部失败**：这一次没收到，但到中继的连接
                // 还活着（目标已存在、写盘失败、根校验不过、对端中途取消）。
                // 此时不能 finish——finish 意味着断开重连，服务端会分配
                // 新 id，发送方表格里的对端 id 立刻过期（实测：一次失败
                // 重试 id 就从 client-17 漂到 client-23）。正确动作与收完
                // 一个文件一样：报失败、原地重建 receiver 继续等。
                //
                // 连接已死（kPeerGone）和本端主动取消（kLocalCancelled，
                // GUI 抢占/关窗走的 abort）不在这里——前者重建也没用，
                // 后者重建会顶掉用户的取消意图，两者都走 finish。
                if (config.onReceiveFailed)
                {
                    config.onReceiveFailed("传输失败：" + r.status.message);
                }
                asio::post(ctx, [this] { rebuildReceiver(); });
                return;
            }
            finish(false, "传输失败：" + r.status.message, r.status);
            return;
        }
    }

    // ---------------------------------------------------------------------
    // 帧分派
    // ---------------------------------------------------------------------

    void dispatch(const Frame &f)
    {
        // 对端的目录列举请求：**任何操作模式下都要应答**——"我正在收文件"
        // 不妨碍回答一次列举。放在最前面，避免被 send/recv peer 的认领
        // 逻辑漏掉（它们只认自己 sessionId 的帧）。
        if (f.type == FrameType::kAskFileList)
        {
            serveFileList(f);
            return;
        }

        if (action == Action::kBrowse && f.type == FrameType::kAnswerFileList)
        {
            core::session::FileListStatus status = core::session::FileListStatus::kOk;
            std::vector<core::session::RemoteEntry> entries;
            std::string err;
            if (!core::session::decodeAnswerFileList(f.body, status, entries, err))
            {
                finish(false, "远端列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                return;
            }
            listStatus = status;
            remoteEntries = std::move(entries);

            switch (status)
            {
            case core::session::FileListStatus::kOk:
                finish(true, "远端列表 " + std::to_string(remoteEntries.size()) + " 项");
                return;
            case core::session::FileListStatus::kTruncated:
                finish(true, "远端列表已截断（只返回前 " + std::to_string(remoteEntries.size()) + " 项）");
                return;
            case core::session::FileListStatus::kNotFound:
                finish(false, "对端没有这个目录", Status::Fail(ErrorCode::kNotFound, "remote path not found"));
                return;
            case core::session::FileListStatus::kRejected:
            default:
                finish(false, "对端拒绝列举（未开启共享，或路径越界）",
                       Status::Fail(ErrorCode::kPermissionDenied, "remote list rejected"));
                return;
            }
        }
        // 常驻接收的重建窗口：上一次接收刚结束（成功或失败），post 的
        // 重建还排在事件队列里，而发送方的下一个请求已经到了。此刻 peer
        // 是 finished 状态，onFrame 必然不认；把帧丢掉的话发送方要等满
        // answerTimeout 才知道失败。同步重建 receiver，让下面的 onFrame
        // 能接住这个请求（rebuildReceiver 有"不顶掉活着的 receiver"的
        // 防护，两路触发不会打架）。
        if (action == Action::kReceive && config.keepReceiving && f.type == FrameType::kFileRequestSend &&
            (!peer || peer->finished()))
        {
            rebuildReceiver();
        }

        // 发送 peer 先认：它只认自己 sessionId 的应答/ACK 帧，接收 peer
        // 空闲时虽然 sessionId=0，但只对 kFileRequestSend 认领（见
        // TransferPeer::onFrame），两者不会互相吞帧。
        if (sendActive && sendPeer && sendPeer->onFrame(f))
        {
            return;
        }

        // 再给接收 peer。它认不认是它的事（sessionId 不符会返回 false）。
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

            if (action == Action::kList)
            {
                // list 操作的契约输出：拼文本、结束。临时连接到此为止。
                std::string text = "在线对端 " + std::to_string(peers.size()) + " 个：";
                for (const auto &p : peers)
                {
                    text += "\n  " + p.id + "  " + p.address;
                }
                finish(true, std::move(text));
                return;
            }

            // 常驻接收会话上的刷新（refreshPeers）：报结构化列表就走，
            // **不结束操作**——这条连接还要继续收文件，id 也要保持稳定。
            if (config.onPeerList)
            {
                std::vector<PeerEntry> entries;
                entries.reserve(peers.size());
                for (const auto &p : peers)
                {
                    entries.push_back(PeerEntry{p.id, p.address});
                }
                config.onPeerList(entries);
            }
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
        // 同 finish()：会话级结论统一由下面这次 onDone 发出，正在进行的
        // 搭车发送不再单独回调（避免上层收到两个结论）。
        sendActive = false;
        onSendDone = nullptr;
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

void Client::browsePeer(std::string peerId, std::string relPath)
{
    impl_->action = Action::kBrowse;
    impl_->browsePeerId = std::move(peerId);
    impl_->browseRelPath = std::move(relPath);
    impl_->connect();
}

void Client::refreshPeers()
{
    // 只对常驻接收会话有意义。前提不满足时静默返回而不是报错：调用方
    // （CoreService）只会在会话活着时走到这里，多余的错误路径只会诱导
    // 上层做无意义的分支。
    impl_->refreshPeers();
}

bool Client::sendOnSession(std::string sourcePath, std::string remoteName, std::string toPeerId,
                           std::function<void(const Outcome &)> done)
{
    return impl_->startSendOnSession(std::move(sourcePath), std::move(remoteName), std::move(toPeerId),
                                     std::move(done));
}

void Client::cancelSend()
{
    impl_->cancelSendNow();
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
