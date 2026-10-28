#include "relayer/cli/Client.h"

#include <algorithm>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <filesystem>
#include <utility>

#include "relayer/core/session/FileList.h"
#include "relayer/core/session/PeerTable.h"
#include "relayer/core/transfer/TransferPeer.h"
#include "relayer/filestore/TarXzArchiver.h"

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

namespace relayer::cli {

namespace {

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

std::string fileNameOf(const std::string& path)
{
    filestore::PathPolicy policy;
    return policy.fileName(path);
}

}   // namespace

struct Client::Impl {
    Impl(asio::io_context& ctxIn, ClientConfig cfg, std::function<void(const ProgressReport&)> prog,
         std::function<void(const Outcome&)> done)
        : ctx(ctxIn), config(std::move(cfg)), onProgress(std::move(prog)), onDone(std::move(done)), pumpTimer(ctxIn),
          watchdog(ctxIn), progressTimer(ctxIn)
    {
    }

    asio::io_context& ctx;
    ClientConfig config;
    std::function<void(const ProgressReport&)> onProgress;
    std::function<void(const Outcome&)> onDone;

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
    std::function<void(const Outcome&)> onSendDone;
    bool sendActive = false;

    asio::steady_timer pumpTimer;
    asio::steady_timer watchdog;   // 空闲 / 总时长
    asio::steady_timer progressTimer;
    // 会话内列举的应答超时。对端离线时服务端**静默丢帧**（§11.3 没有"投递
    // 失败"这一类），不设这个定时器，远端浏览面板会永远停在"读取中"。
    asio::steady_timer browseTimer{ctx};

    Action action = Action::kList;
    std::string toPeerId;

    // kBrowse 的目标与相对路径（PROTOCOL.md §11.8）。
    std::string browsePeerId;
    std::string browseRelPath;

    // 常驻会话上的列举（GUI 用，见 Client::browseOnSession）。与 sendActive
    // 同一套思路：会话不结束，只把这一次的结论回调给上层。
    bool browseActive = false;
    std::function<void(const Outcome&)> onBrowseDone;

    // 会话内的一次"取回请求"（PROTOCOL.md §11.9）。只等到 kAnswerFilePull
    // 为止——真正的文件由常驻接收会话收下，走的是普通接收路径。
    bool pullActive = false;
    // 这一次是"取目录"还是"取文件"。只影响失败文案：同一个 kNotFound，
    // 对文件请求是"不存在或它是目录"，对目录请求是"不存在或它不是目录"。
    bool pullIsArchive = false;
    std::function<void(const Outcome&)> onPullDone;
    asio::steady_timer pullTimer{ctx};
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
    // 自检列举已发起（见 ClientConfig::selfBrowse）。
    bool selfBrowseStarted = false;
    // 取回请求已发起（见 ClientConfig::pullFrom）。
    bool pullStarted = false;
    // 会话内列举出过结论。finish() 据此把结果搬进 Outcome——action 是
    // kReceive（自检列举）时不满足 `action == Action::kBrowse` 那个条件。
    bool browseDone = false;

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
        if (finished) {
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
        // 挂起的列举同理：会话结束时上层已经收到 onDone，这次列举的回调
        // 不能再单独发（否则上层会收到两个结论）。
        browseActive = false;
        onBrowseDone = nullptr;
        browseTimer.cancel();

        if (!ok) {
            // 失败时**不**调 session->close()：先让调用方看到结论，关闭
            // 交给析构。反过来（先关再报）会让 main 拿到结论时连接已经
            // 没了，日志里看不出是哪一步断的。
        }

        if (onDone) {
            Outcome o;
            o.ok = ok;
            o.summary = std::move(summary);
            o.status = std::move(status);
            o.connectionLost = connectionLost;
            if (peer) {
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
            if (action == Action::kList) {
                for (const auto& p : peers) {
                    o.peerList.push_back(PeerEntry{p.id, p.address});
                }
            } else if (action == Action::kBrowse || browseDone) {
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
        if (finished) {
            return;
        }
        pumpTimer.expires_after(kPumpInterval);
        pumpTimer.async_wait([this](std::error_code ec) {
            if (ec || finished) {
                return;
            }
            if (sendPeer) {
                sendPeer->pump();
            }
            if (peer) {
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
        if (finished) {
            return;
        }
        progressTimer.expires_after(kProgressInterval);
        progressTimer.async_wait([this](std::error_code ec) {
            if (ec || finished) {
                return;
            }
            reportProgress();
            startProgressTimer();
        });
    }

    void armHardTimeout()
    {
        if (config.hardTimeout.count() <= 0 || finished) {
            return;
        }
        watchdog.expires_after(config.hardTimeout);
        watchdog.async_wait([this](std::error_code ec) {
            if (ec || finished) {
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
        if (finished || !authenticated) {
            return;
        }

        // 看门狗盯"当前在动的那件事"：有会话内发送时看发送，否则看接收。
        // 两者同时在跑时只看发送——接收那边的沉默由它自己的收尾/空闲
        // 判定负责，而批次场景下用户关心的是发送是否卡住。
        auto* active = (sendActive && sendPeer) ? sendPeer.get() : peer.get();
        if (active) {
            const auto p = active->progress();
            if (p.bytesDone != lastActivityBytes) {
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
            if (!active->started()) {
                lastActivity = std::chrono::steady_clock::now();
                return;
            }
        }

        const auto idle = std::chrono::steady_clock::now() - lastActivity;
        if (idle < config.idleTimeout) {
            return;
        }

        // 注意措辞：这里的结论是"对端没动静"，不是"传输失败"。对端进程
        // 被 kill 之后，本地能观察到的**唯一**事实就是没有进展——把这点
        // 说清楚，测试失败时人才不会往错的方向查。
        finish(false,
               "对端长时间无响应（" +
                   std::to_string(std::chrono::duration_cast<std::chrono::seconds>(config.idleTimeout).count()) + " 秒无进展）",
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
        auto* active = (sendActive && sendPeer) ? sendPeer.get() : peer.get();
        if (!onProgress || !active) {
            return;
        }
        const auto p = active->progress();

        const auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<double>(now - lastReportAt).count();
        if (dt > 0.0 && p.bytesDone >= lastBytes) {
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

        h.onAuthenticated = [this](core::session::Session& self, std::string id) {
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
            if (!config.requestedId.empty() && !claimSent) {
                claimSent = true;
                if (const auto st = self.claimPeerId(config.requestedId); !st.ok()) {
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
            if (!assignedId.empty() && assignedId != previous && identityFinal) {
                std::printf("ASSIGNED_ID=%s\n", assignedId.c_str());
                std::fflush(stdout);
                if (config.onIdAssigned) {
                    config.onIdAssigned(assignedId);
                }

                // askListOnStart 放在"真实 id 到手"这一刻而不是 startReceiving：
                // 首次 onAuthenticated 回调时身份还是空的，此刻问列表，服务端
                // 返回的名单里未必有自己（id 都还没发），GUI 的自标红就会失效。
                if (action == Action::kReceive && config.askListOnStart) {
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
            if (action == Action::kReceive && identityFinal && !config.selfSendPath.empty() && !selfSendStarted) {
                selfSendStarted = true;
                const bool started = startSendOnSession(config.selfSendPath, std::string(), assignedId, [this](const Outcome& o) {
                    // 成功不做额外动作：接收侧收完会自己 finish（退出码
                    // 由它决定）。失败要提前结束，否则进程挂在"等文件"
                    // 上直到硬超时，看不出是自检发送出了问题。
                    if (!o.ok) {
                        finish(false, "自检发送失败：" + o.summary, o.status, o.connectionLost);
                    }
                });
                if (!started) {
                    finish(false, "自检发送未能启动（接收会话不可用）",
                           Status::Fail(ErrorCode::kUnknown, "self-send unavailable"), false);
                }
            }

            // 远端取回（PROTOCOL.md §11.9）：在接收会话上请对端把文件发过来。
            // 走 recv 模式是因为回程的文件需要这个常驻会话收下。
            if (action == Action::kReceive && identityFinal && !config.pullFrom.empty() && !pullStarted) {
                pullStarted = true;
                const bool started =
                    startPullOnSession(config.pullFrom, config.pullPath, config.pullAsArchive, [this](const Outcome& o) {
                        std::printf("PULL %s\n", o.ok ? "accepted" : "failed");
                        std::fflush(stdout);
                        if (!o.ok) {
                            // 拒绝/超时：不会再等文件了，直接结束。
                            // 成功则继续等回程的文件（接收路径负责收尾）。
                            finish(false, "取回失败：" + o.summary, o.status, false);
                        }
                    });
                if (!started) {
                    finish(false, "取回请求未能发起（会话不可用或正忙）", Status::Fail(ErrorCode::kUnknown, "pull unavailable"),
                           false);
                }
            }

            // 自检列举：在接收会话上向自己要一份目录（GUI 远端浏览的同一条
            // 路径——请求搭在 kReceive 会话上，而不是新开连接）。自投让请求
            // 与应答都绕回本连接，所以一个进程就把服务端应答侧也验了。
            if (action == Action::kReceive && identityFinal && config.selfBrowse && !selfBrowseStarted) {
                selfBrowseStarted = true;
                const bool started = startBrowseOnSession(assignedId, std::string(), [this](const Outcome& o) {
                    // 列完就结束：这是自检，不是长期接收。
                    finish(o.ok, o.summary, o.status, false);
                });
                if (!started) {
                    finish(false, "自检列举未能启动（接收会话不可用或正忙）",
                           Status::Fail(ErrorCode::kUnknown, "self-browse unavailable"), false);
                }
            }
        };

        h.onFrame = [this](core::session::Session&, const Frame& f) {
            touch();
            dispatch(f);
        };

        h.onClosed = [this](std::string, Status st) {
            if (finished) {
                return;
            }
            // 未认证状态收到业务帧，几乎总是"客户端按认证模式连了一个
            // 无认证中继"：服务端跳过握手直接发 answer_id，被客户端按
            // 协议拒绝。这个场景足够常见（服务端 --allow-no-auth 部署），
            // 给一条可照做的提示，而不是让用户去猜协议细节。
            if (!config.serverAllowNoAuth && st.message.find("non-auth frame") != std::string::npos) {
                st.message += "（中继可能以 --allow-no-auth 启动，客户端请加 --no-auth 重试）";
            }
            // 声明标识被拒：这不是"网络断了"，措辞必须让用户知道该改什么。
            // 两种原因的处置完全不同——被占用是要么换名字、要么关掉占用的
            // 那台；不合法是名字本身不能用。
            if (st.code == ErrorCode::kIdTaken) {
                finish(false, "标识「" + config.requestedId + "」已被占用，该标识同时只能有一条连接", std::move(st), true);
                return;
            }
            if (st.code == ErrorCode::kIdInvalid) {
                finish(false, "标识「" + config.requestedId + "」不合法（不能为空/超长/含非法字符）", std::move(st), true);
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
        if (startedOnce) {
            return;
        }
        startedOnce = true;

        switch (action) {
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
        if (!session->send(f).ok()) {
            finish(false, "远端列举请求发送失败", Status::Fail(ErrorCode::kProtocol, "send failed"));
            return;
        }

        // 同 GUI 路径：对端离线时服务端静默丢帧，不设超时就会一直等。
        browseTimer.expires_after(std::chrono::seconds(10));
        browseTimer.async_wait([this](const std::error_code& ec) {
            if (ec || action != Action::kBrowse) {
                return;
            }
            finish(false, "对端没有响应（可能已离线）", Status::Fail(ErrorCode::kTimeout, "browse answer timeout"));
        });
    }

    // 对端来问目录：按共享根解析、列一层、回一条。**只服务这一个根**。
    //
    // 目录列举是同步做的：一次 readdir 在该规模下是毫秒级，而引入工作线程
    // 会把"谁保证 store 生命周期"的问题带进来。真遇到网络盘这类慢列举，
    // 再按传输路径那样丢进 WorkerPool。
    void serveFileList(const Frame& req)
    {
        Frame answer;
        answer.type = FrameType::kAnswerFileList;
        // 请求者的身份由服务端在转发时绑定（不是自称），可以直接当回信地址。
        answer.to = req.from;

        std::vector<core::session::RemoteEntry> out;
        auto status = core::session::FileListStatus::kOk;

        std::string rel;
        std::string err;
        if (!core::session::decodeAskFileList(req.body, rel, err)) {
            status = core::session::FileListStatus::kRejected;
        } else if (config.shareRoot.empty()) {
            // 没开共享：明确拒绝。静默不回会让对方等满应答超时，
            // 看起来像网络问题。
            status = core::session::FileListStatus::kRejected;
        } else {
            const std::string target = rel.empty() ? config.shareRoot : store.resolveWithin(config.shareRoot, rel);
            if (target.empty()) {
                status = core::session::FileListStatus::kRejected;   // 绝对路径 / 越界
            } else {
                const auto st = store.stat(target);
                if (!st.exists || !st.isDir) {
                    status = core::session::FileListStatus::kNotFound;
                } else {
                    std::vector<filestore::DirEntry> raw;
                    if (!store.list(target, false, raw).ok()) {
                        status = core::session::FileListStatus::kNotFound;
                    } else {
                        // 目录在前、同类型按名字排：列表是给人看的，顺序稳定
                        // 比"文件系统返回什么算什么"重要（也方便测试断言）。
                        std::sort(raw.begin(), raw.end(), [](const filestore::DirEntry& a, const filestore::DirEntry& b) {
                            if (a.isDir != b.isDir) {
                                return a.isDir;
                            }
                            return a.name < b.name;
                        });
                        if (raw.size() > core::session::kMaxListEntries) {
                            raw.resize(core::session::kMaxListEntries);
                            status = core::session::FileListStatus::kTruncated;
                        }
                        out.reserve(raw.size());
                        for (const auto& e : raw) {
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

        if (const auto st = core::session::encodeAnswerFileList(status, out, answer.body); !st.ok()) {
            return;   // 编码失败只可能是内部 bug，没必要再回一个坏帧
        }
        (void)session->send(answer);
    }

    void refreshPeers()
    {
        // 在现有连接上问列表，不新建连接（见 Client::refreshPeers 的注释）。
        // touch 刷新空闲基线：等响应的往返不该被看门狗当成"对端无响应"。
        if (finished || action != Action::kReceive || !authenticated || !session) {
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
        if (config.chunkSize > 0) {
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
        c.answerTimeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::min(config.idleTimeout, std::chrono::seconds(10)));

        // 覆盖策略透传。默认 false（拒绝覆盖）是 core 层的设计；CLI 的
        // --overwrite 与 GUI 的接收开关在这里汇入。
        c.overwriteExisting = config.overwriteExisting;
        return c;
    }

    void startSending()
    {
        if (toPeerId.empty()) {
            finish(false, "内部错误：发送时缺少目标对端", Status::Fail(ErrorCode::kUnknown, "empty toPeerId"));
            return;
        }

        auto cfg = peerConfig();

        // sessionId 由发送方选。这里用时间戳而不是随机数：多进程测试里
        // 两边都从同一个种子起算，撞车的概率与"进程启动在同一毫秒"相当，
        // 而时间戳在日志里能直接看出"哪一次"。
        const auto sid =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        peer = core::transfer::TransferPeer::createSender(ctx, session, store, pool, peerPathSource, peerPathRemoteName, toPeerId,
                                                          sid, std::move(cfg),
                                                          [this](const core::transfer::TransferResult& r) { onTransferDone(r); });

        // createSender 内部可能已经同步失败（源文件打不开），此时 onDone
        // 已经在构造函数里被调用过，peer 处于 finished 状态。
        if (peer && peer->finished()) {
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
        peer =
            core::transfer::TransferPeer::createReceiver(ctx, session, store, pool, targetPath, 0, std::move(cfg),
                                                         [this](const core::transfer::TransferResult& r) { onTransferDone(r); });

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

    // 现在就能起一次搭车发送吗（会话在 + 没有别的发送在跑）。
    // servePullRequest 靠它决定"接受"还是"正忙，拒绝"。
    bool canStartSendOnSession() const noexcept
    {
        return canSendOnSession() && !sendActive;
    }

    // 常驻会话上请对端把某个文件发给我（PROTOCOL.md §11.9）。
    //
    // 回调在**收到 kAnswerFilePull 时就触发**，不等文件传完：上层要区分
    // "对方不给我"（立刻可知）和"给了但传得慢"（随后由接收路径报告）。
    bool startPullOnSession(std::string peerId, std::string relPath, bool archive, std::function<void(const Outcome&)> done)
    {
        if (!canSendOnSession() || pullActive) {
            return false;
        }
        pullActive = true;
        pullIsArchive = archive;
        onPullDone = std::move(done);

        Frame f;
        f.type = archive ? FrameType::kAskArchivePull : FrameType::kAskFilePull;
        f.to = std::move(peerId);
        // 请求体与 kAskFileList 同构（相对共享根的路径），复用同一套编解码。
        f.body = core::session::encodeAskFileList(relPath);

        touch();
        if (const auto st = session->send(f); !st.ok()) {
            finishPull(false, "取回请求发送失败", st);
            return false;
        }

        // 与列举同理：对端离线时服务端静默丢帧，不设超时就会一直等。
        //
        // 归档请求等 120 秒：对端要先遍历 + 压缩，应答是在**打包完成之后**
        // 才发的（§11.10.2）。同一个 10 秒对"打开文件"够用，对"压缩一个
        // 目录"不够——用 10 秒会让正常的目录取回被误判成对端离线。
        pullTimer.expires_after(archive ? std::chrono::seconds(120) : std::chrono::seconds(10));
        pullTimer.async_wait([this](const std::error_code& ec) {
            if (ec || !pullActive) {
                return;
            }
            finishPull(false, "对端没有响应（可能已离线）", Status::Fail(ErrorCode::kTimeout, "pull answer timeout"));
        });
        return true;
    }

    void finishPull(bool ok, std::string summary, Status status = Status{})
    {
        if (!pullActive) {
            return;
        }
        pullActive = false;
        pullTimer.cancel();
        auto done = std::move(onPullDone);
        onPullDone = nullptr;

        Outcome o;
        o.ok = ok;
        o.summary = std::move(summary);
        o.status = std::move(status);
        if (done) {
            done(o);
        }
    }

    // 目录取回的打包上限（PROTOCOL.md §11.10.3）。压缩是同步做的，这两个
    // 数字决定了"最坏卡多久"，所以写死而不做成配置。
    static constexpr std::size_t kMaxPackFiles = 20000;
    static constexpr std::uint64_t kMaxPackBytes = 256ULL * 1024 * 1024;

    // 递归收集要打包的条目。DirEntry.name 只有文件名，所以这里自己拼相对
    // 路径——store.list 的 recursive 模式不返回层级信息。
    //
    // 返回 false 表示超限（文件数或字节数），由调用方转成 status=2。
    bool collectItems(const std::string& absDir, const std::string& relPrefix, std::vector<filestore::ArchiveItem>& items,
                      std::uint64_t& bytes) const
    {
        std::vector<filestore::DirEntry> entries;
        if (!store.list(absDir, false, entries).ok()) {
            return true;   // 某一层读不了就跳过它，不因此让整次打包失败
        }
        for (const auto& e : entries) {
            if (e.isDir) {
                // 空目录也要进包：只收文件会让"解出来少了几个空目录"，
                // 而空目录有时是有意义的（占位、目录结构约定）。
                const std::string rel = relPrefix.empty() ? e.name : relPrefix + "/" + e.name;
                filestore::ArchiveItem dirItem;
                dirItem.sourcePath = absDir + "/" + e.name;
                dirItem.entryName = rel + "/";
                items.push_back(std::move(dirItem));

                if (!collectItems(absDir + "/" + e.name, rel, items, bytes)) {
                    return false;
                }
                continue;
            }
            bytes += e.size;
            if (items.size() >= kMaxPackFiles || bytes > kMaxPackBytes) {
                return false;
            }
            filestore::ArchiveItem item;
            item.sourcePath = absDir + "/" + e.name;
            item.entryName = relPrefix.empty() ? e.name : relPrefix + "/" + e.name;
            items.push_back(std::move(item));
        }
        return true;
    }

    // 把目录打包到系统临时目录，返回包路径（失败返回空串）。
    //
    // **不能放进共享根**：那会让对端在列举里看到这个临时包，也污染用户目录。
    std::string packDirectory(const std::string& absDir, const std::string& dirName, std::string& err)
    {
        std::vector<filestore::ArchiveItem> items;
        std::uint64_t bytes = 0;
        if (!collectItems(absDir, std::string(), items, bytes)) {
            err = "目录过大（超过 " + std::to_string(kMaxPackFiles) + " 个文件或 256 MiB），请分批取回";
            return std::string();
        }
        if (items.empty()) {
            err = "目录是空的";
            return std::string();
        }

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() / ("relayer-pack-" + std::to_string(stamp) + ".tar.xz");

        filestore::TarXzArchiver archiver(store);
        filestore::ArchiveOptions options;
        // 级别 1：传输场景里网络是瓶颈，压缩比多省的那点字节不值"多等 30 秒"
        // （PROTOCOL.md §11.10.3）。
        options.compressionLevel = 1;
        std::vector<std::string> skipped;
        const auto st = archiver.create(path.string(), items, options, skipped);
        if (!st.ok()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);   // 半成品不能留下
            err = "打包失败：" + st.message;
            return std::string();
        }
        (void)dirName;
        return path.string();
    }

    // 对端来要文件（B 侧）：校验共享根 → 回 status → 用普通"推"流程发送。
    void servePullRequest(const Frame& req)
    {
        Frame answer;
        answer.type = FrameType::kAnswerFilePull;
        // 请求方的身份由服务端在转发时绑定，可以直接当回信地址。
        answer.to = req.from;

        std::string rel;
        std::string err;
        auto status = core::session::FilePullStatus::kRejected;
        std::string target;

        if (!core::session::decodeAskFileList(req.body, rel, err)) {
            status = core::session::FilePullStatus::kRejected;
        } else if (config.shareRoot.empty()) {
            // 没开共享：明确拒绝。静默不回会让对方等满超时，看起来像网络问题。
            status = core::session::FilePullStatus::kRejected;
        } else if (!canStartSendOnSession()) {
            // 自己正在发别的：明确拒绝而不是排进队列。排队会让请求方看到
            // 一个不动的进度条，而它并不知道前面还有别人在传。
            status = core::session::FilePullStatus::kRejected;
        } else {
            const std::string resolved = rel.empty() ? config.shareRoot : store.resolveWithin(config.shareRoot, rel);
            if (resolved.empty()) {
                status = core::session::FilePullStatus::kRejected;   // 绝对路径 / 越界
            } else {
                const auto st = store.stat(resolved);
                if (!st.exists || st.isDir) {
                    // 目录不支持：打包归档还没实现，递归取回语义也不清楚
                    // （PROTOCOL.md §11.9.5）。
                    status = core::session::FilePullStatus::kNotFound;
                } else {
                    status = core::session::FilePullStatus::kAccepted;
                    target = resolved;
                }
            }
        }

        answer.body = core::session::encodeAnswerFilePull(status);
        if (const auto st = session->send(answer); !st.ok()) {
            return;
        }

        if (status == core::session::FilePullStatus::kAccepted) {
            // 先回 status 再发文件：应答里那个 0 是"我接受"，文件随后才到。
            // 顺序反了的话，请求方会先看到一个陌生的 kFileRequestSend。
            (void)startSendOnSession(target, std::string(), req.from, [](const Outcome&) {});   // 结论由接收方自己报
        }
    }

    // 对端来要目录（B 侧）：校验 → 打包 → **打包完成后**才回 status → 推包。
    //
    // 应答放在打包之后是刻意的（§11.10.2）：打包会失败（磁盘满、权限、
    // 文件在压缩中被删）。先回 0 再失败，请求方只能看到"对方答应了但没来
    // 东西"，直到会话空闲超时——所有失败里最难查的一种。
    void serveArchivePullRequest(const Frame& req)
    {
        Frame answer;
        answer.type = FrameType::kAnswerFilePull;
        answer.to = req.from;

        std::string rel;
        std::string err;
        auto status = core::session::FilePullStatus::kRejected;
        std::string packPath;

        if (!core::session::decodeAskFileList(req.body, rel, err)) {
            status = core::session::FilePullStatus::kRejected;
        } else if (config.shareRoot.empty() || rel.empty()) {
            // 空路径 = 整个共享根。不打包它：多半是点错了，而且一个根目录
            // 打包出来对方也不知道该解到哪。
            status = core::session::FilePullStatus::kRejected;
        } else if (!canStartSendOnSession()) {
            status = core::session::FilePullStatus::kRejected;
        } else {
            const std::string resolved = store.resolveWithin(config.shareRoot, rel);
            if (resolved.empty()) {
                status = core::session::FilePullStatus::kRejected;   // 绝对路径 / 越界
            } else {
                const auto st = store.stat(resolved);
                if (!st.exists || !st.isDir) {
                    status = core::session::FilePullStatus::kNotFound;   // 文件请走 §11.9
                } else {
                    std::string packErr;
                    packPath = packDirectory(resolved, rel, packErr);
                    status =
                        packPath.empty() ? core::session::FilePullStatus::kRejected : core::session::FilePullStatus::kAccepted;
                }
            }
        }

        answer.body = core::session::encodeAnswerFilePull(status);
        if (const auto st = session->send(answer); !st.ok()) {
            return;
        }

        if (status != core::session::FilePullStatus::kAccepted) {
            return;
        }

        // 包名用目录名，让对方一眼知道落到本地的是什么（§11.10.3）。
        const auto slash = rel.find_last_of('/');
        const std::string dirName = (slash == std::string::npos) ? rel : rel.substr(slash + 1);
        const std::string packName = dirName + ".tar.xz";

        const bool started = startSendOnSession(packPath, packName, req.from, [packPath](const Outcome&) {
            // 传完（成功、失败、取消）都删：留着的临时包
            // 只会越积越多，而它已经没用了。
            std::error_code ec;
            std::filesystem::remove(packPath, ec);
        });
        if (!started) {
            // 兜底：post 与发送之间的竞态（比如会话刚好断了）。
            std::error_code ec;
            std::filesystem::remove(packPath, ec);
        }
    }

    // 常驻会话上问一次远端目录。会话继续活着（id 不变、接收不中断），
    // 这与 Client::browsePeer 的一次性连接是两条路：后者给 CLI 用，前者
    // 给 GUI——GUI 一旦新开连接就会顶掉常驻接收会话、把自己的 id 换掉。
    bool startBrowseOnSession(std::string peerId, std::string relPath, std::function<void(const Outcome&)> done)
    {
        if (!canSendOnSession() || browseActive) {
            return false;
        }
        browseActive = true;
        onBrowseDone = std::move(done);
        browsePeerId = std::move(peerId);
        browseRelPath = std::move(relPath);

        Frame f;
        f.type = FrameType::kAskFileList;
        f.to = browsePeerId;
        f.body = core::session::encodeAskFileList(browseRelPath);

        touch();
        if (const auto st = session->send(f); !st.ok()) {
            finishBrowse(false, "远端列举请求发送失败", st);
            return false;
        }

        // 应答超时：与"对端没应答"同一时限语义，但独立计时——会话本身
        // 还活着，不能靠会话级看门狗（那会连接收一起判死）。
        browseTimer.expires_after(std::chrono::seconds(10));
        browseTimer.async_wait([this](const std::error_code& ec) {
            if (ec || !browseActive) {
                return;
            }
            finishBrowse(false, "对端没有响应（可能已离线）", Status::Fail(ErrorCode::kTimeout, "browse answer timeout"));
        });
        return true;
    }

    // 一次会话内列举的结论。与 finishSend 对称：只回调这一次，不动 busy。
    void finishBrowse(bool ok, std::string summary, Status status = Status{})
    {
        if (!browseActive) {
            return;
        }
        browseActive = false;
        browseDone = true;
        browseTimer.cancel();
        auto done = std::move(onBrowseDone);
        onBrowseDone = nullptr;

        Outcome o;
        o.ok = ok;
        o.summary = std::move(summary);
        o.status = std::move(status);
        o.remoteEntries = remoteEntries;
        o.listStatus = listStatus;
        if (done) {
            done(o);
        }
    }

    bool startSendOnSession(std::string source, std::string remoteName, std::string targetPeerId,
                            std::function<void(const Outcome&)> done)
    {
        if (!canSendOnSession() || sendActive) {
            return false;
        }
        sendActive = true;
        onSendDone = std::move(done);
        sendSourcePath = std::move(source);
        sendRemoteName = remoteName.empty() ? fileNameOf(sendSourcePath) : std::move(remoteName);
        sendToPeerId = std::move(targetPeerId);

        auto cfg = peerConfig();
        const auto sid =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        auto created = core::transfer::TransferPeer::createSender(
            ctx, session, store, pool, sendSourcePath, sendRemoteName, sendToPeerId, sid, std::move(cfg),
            [this](const core::transfer::TransferResult& r) { onSendResult(r); });
        sendPeer = std::move(created);

        // 源文件打不开时 createSender 会同步回调 onSendResult（sendActive
        // 已被清、结论已上报），这里的 touch 无害。
        touch();
        return true;
    }

    // 一次"会话内发送"的结论。**不是**整条会话的结论：会话继续活着。
    void finishSend(bool ok, std::string summary, Status status = Status{}, bool connectionLost = false)
    {
        if (!sendActive) {
            return;
        }
        sendActive = false;

        Outcome o;
        o.ok = ok;
        o.summary = std::move(summary);
        o.status = std::move(status);
        o.connectionLost = connectionLost;
        if (sendPeer) {
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
        asio::post(ctx, [this] {
            if (!sendActive) {
                sendPeer.reset();
            }
        });
        if (cb) {
            cb(o);
        }
    }

    void onSendResult(const core::transfer::TransferResult& r)
    {
        switch (r.outcome) {
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
        if (!sendActive || !sendPeer || sendPeer->finished()) {
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
        if (finished) {
            return;
        }
        if (peer && !peer->finished()) {
            return;
        }
        startReceiving();
    }

    void onTransferDone(const core::transfer::TransferResult& r)
    {
        if (finished) {
            return;
        }

        switch (r.outcome) {
        case core::transfer::TransferOutcome::kCompleted: {
            // 最终进度报一次满的。否则最后一行停在"99%"，用户会怀疑
            // 是不是没走完——虽然 onDone 已经说了成功。
            if (onProgress && peer) {
                const auto p = peer->progress();
                ProgressReport rep;
                rep.bytesDone = p.bytesTotal == 0 ? p.bytesDone : p.bytesTotal;
                rep.bytesTotal = p.bytesTotal;
                rep.bytesPerSecond = lastSpeed;
                onProgress(rep);
            }

            if (action == Action::kReceive) {
                if (config.keepReceiving) {
                    // 常驻接收：**不 finish**。报完成回调后原地重建
                    // receiver，继续在同一条连接上等下一个请求——服务端
                    // 分配的 id 因此保持不变（见 ClientConfig::keepReceiving
                    // 的注释，这是"发送方表格里的对端 id 不过期"的关键）。
                    // 重建走 post：此刻在 TransferPeer 的完成回调栈上，
                    // 直接替换 peer 成员是回调栈析构的老坑。
                    // 自动解包（--unpack / 目录取回）：解成功就报目录，
                    // 失败则照常报包——两种情况用户都能看到明确的落点。
                    std::string reported = r.targetPath;
                    if (config.unpackArchives) {
                        const auto dir = unpackArchive(r.targetPath);
                        if (!dir.empty()) {
                            reported = dir;
                        }
                    }
                    if (config.onFileReceived) {
                        config.onFileReceived(reported);
                    }
                    asio::post(ctx, [this] { rebuildReceiver(); });
                    return;
                }
                {
                    std::string reported = r.targetPath;
                    if (config.unpackArchives) {
                        const auto dir = unpackArchive(r.targetPath);
                        if (!dir.empty()) {
                            reported = dir;
                        }
                    }
                    finish(true, "接收完成：" + reported);
                }
                return;
            } else {
                finish(true, "发送完成");
            }
            return;
        }

        case core::transfer::TransferOutcome::kPeerCancelled:
            if (action == Action::kReceive && config.keepReceiving) {
                // 对端中途取消：连接还活着，同 kFailed 的理由原地重建
                // （见下面 kFailed 分支的注释），否则一次取消就会让本端
                // 换 id，发送方下次就找不到我们了。
                if (config.onReceiveFailed) {
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
            if (action == Action::kReceive && config.keepReceiving) {
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
                if (config.onReceiveFailed) {
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

    void dispatch(const Frame& f)
    {
        // 对端的目录列举请求：**任何操作模式下都要应答**——"我正在收文件"
        // 不妨碍回答一次列举。放在最前面，避免被 send/recv peer 的认领
        // 逻辑漏掉（它们只认自己 sessionId 的帧）。
        if (f.type == FrameType::kAskFileList) {
            serveFileList(f);
            return;
        }

        // 对端要文件：与列举一样，**任何操作模式下都要应答**——"我正在收
        // 文件"不妨碍把共享目录里的东西给对方，前提是当前没有发送在跑
        // （servePullRequest 里判）。
        if (f.type == FrameType::kAskFilePull) {
            servePullRequest(f);
            return;
        }

        // 目录取回：打包 + 推包，应答同样是 kAnswerFilePull。
        if (f.type == FrameType::kAskArchivePull) {
            serveArchivePullRequest(f);
            return;
        }

        if (f.type == FrameType::kAnswerFilePull && pullActive) {
            core::session::FilePullStatus status = core::session::FilePullStatus::kRejected;
            std::string err;
            if (!core::session::decodeAnswerFilePull(f.body, status, err)) {
                finishPull(false, "取回应答解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                return;
            }
            switch (status) {
            case core::session::FilePullStatus::kAccepted:
                // 只是"对方接受了"。文件随后由常驻接收会话收下，那一步的
                // 结论走正常接收路径（finish / onFileReceived），不在这里报。
                finishPull(true, "对端已开始发送");
                return;
            case core::session::FilePullStatus::kNotFound:
                finishPull(false, pullIsArchive ? "对端没有这个目录（或它不是目录）" : "对端没有这个文件（或它是目录）",
                           Status::Fail(ErrorCode::kNotFound, "remote path not found"));
                return;
            case core::session::FilePullStatus::kRejected:
            default:
                finishPull(false, "对端拒绝取回（未开启共享、路径越界或正忙）",
                           Status::Fail(ErrorCode::kPermissionDenied, "remote pull rejected"));
                return;
            }
        }

        if (f.type == FrameType::kAnswerFileList && (action == Action::kBrowse || browseActive)) {
            core::session::FileListStatus status = core::session::FileListStatus::kOk;
            std::vector<core::session::RemoteEntry> entries;
            std::string err;
            if (!core::session::decodeAnswerFileList(f.body, status, entries, err)) {
                finish(false, "远端列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                return;
            }
            listStatus = status;
            remoteEntries = std::move(entries);

            if (action != Action::kBrowse) {
                // 常驻会话上的列举：只回调这一次，会话继续。
                const bool ok =
                    (status == core::session::FileListStatus::kOk || status == core::session::FileListStatus::kTruncated);
                const std::string summary =
                    ok ? ("远端列表 " + std::to_string(remoteEntries.size()) + " 项")
                       : (status == core::session::FileListStatus::kNotFound ? "对端没有这个目录"
                                                                             : "对端拒绝列举（未开启共享，或路径越界）");
                finishBrowse(ok, summary, ok ? Status{} : Status::Fail(ErrorCode::kNotFound, "remote list failed"));
                return;
            }

            switch (status) {
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
            (!peer || peer->finished())) {
            rebuildReceiver();
        }

        // 发送 peer 先认：它只认自己 sessionId 的应答/ACK 帧，接收 peer
        // 空闲时虽然 sessionId=0，但只对 kFileRequestSend 认领（见
        // TransferPeer::onFrame），两者不会互相吞帧。
        if (sendActive && sendPeer && sendPeer->onFrame(f)) {
            return;
        }

        // 再给接收 peer。它认不认是它的事（sessionId 不符会返回 false）。
        if (peer && peer->onFrame(f)) {
            return;
        }

        if (f.type == FrameType::kAnswerClientList) {
            std::string err;
            std::vector<core::session::PeerInfo> list;
            if (!core::session::decodeClientList(f.body, list, err)) {
                finish(false, "在线列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                return;
            }
            peers = std::move(list);

            if (action == Action::kList) {
                // list 操作的契约输出：拼文本、结束。临时连接到此为止。
                std::string text = "在线对端 " + std::to_string(peers.size()) + " 个：";
                for (const auto& p : peers) {
                    text += "\n  " + p.id + "  " + p.address;
                }
                finish(true, std::move(text));
                return;
            }

            // 常驻接收会话上的刷新（refreshPeers）：报结构化列表就走，
            // **不结束操作**——这条连接还要继续收文件，id 也要保持稳定。
            if (config.onPeerList) {
                std::vector<PeerEntry> entries;
                entries.reserve(peers.size());
                for (const auto& p : peers) {
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
        if (finished) {
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
        // 挂起的列举同理：会话结束时上层已经收到 onDone，这次列举的回调
        // 不能再单独发（否则上层会收到两个结论）。
        browseActive = false;
        onBrowseDone = nullptr;
        browseTimer.cancel();
        if (session) {
            session->close();
        }
        if (onDone) {
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

Client::Client(asio::io_context& ctx, ClientConfig config, std::function<void(const ProgressReport&)> onProgress,
               std::function<void(const Outcome&)> onDone)
    : impl_(std::make_unique<Impl>(ctx, std::move(config), std::move(onProgress), std::move(onDone)))
{
}

Client::~Client()
{
    if (impl_ && impl_->session) {
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
                           std::function<void(const Outcome&)> done)
{
    return impl_->startSendOnSession(std::move(sourcePath), std::move(remoteName), std::move(toPeerId), std::move(done));
}

bool Client::browseOnSession(std::string peerId, std::string relPath, std::function<void(const Outcome&)> done)
{
    return impl_->startBrowseOnSession(std::move(peerId), std::move(relPath), std::move(done));
}

bool Client::createArchive(const std::vector<std::string>& sources, const std::string& archivePath, std::string& err)
{
    std::vector<filestore::ArchiveItem> items;
    for (const auto& src : sources) {
        std::error_code ec;
        const std::filesystem::path p(src);
        if (!std::filesystem::exists(p, ec)) {
            err = "路径不存在：" + src;
            return false;
        }
        // 相对**父目录**：包内有顶层目录，解压时配 unpackArchive(path, false)。
        const auto base = p.parent_path();
        if (std::filesystem::is_directory(p, ec)) {
            std::error_code iterEc;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, iterEc);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(iterEc)) {
                if (iterEc) {
                    break;
                }
                // lexically_relative 是**纯字符串计算**，不访问文件系统：
                // std::filesystem::relative 会去查真实路径（还带 ec 出参），
                // 一旦它失败，整个循环就把所有条目都 continue 掉了——表现
                // 成"没有可压缩的内容"，而目录明明是好的。
                const auto rel = it->path().lexically_relative(base);
                if (rel.empty()) {
                    continue;
                }
                filestore::ArchiveItem item;
                item.sourcePath = it->path().string();
                item.entryName = rel.generic_string() + (it->is_directory() ? "/" : "");
                items.push_back(std::move(item));
            }
        } else {
            items.push_back(filestore::ArchiveItem{src, p.filename().string()});
        }
    }

    if (items.empty()) {
        err = "没有可压缩的内容";
        return false;
    }

    filestore::StdFileStore store;
    filestore::TarXzArchiver archiver(store);
    filestore::ArchiveOptions options;
    // 与 §11.10.3 同一条理由：传输/存储场景里，压缩比的边际收益不值多等。
    options.compressionLevel = 1;
    std::vector<std::string> skipped;
    const auto st = archiver.create(archivePath, items, options, skipped);
    if (!st.ok()) {
        err = st.message;
        return false;
    }
    return true;
}

std::string Client::unpackArchive(const std::string& archivePath, bool intoSubdir)
{
    static const std::string kSuffix = ".tar.xz";
    if (archivePath.size() <= kSuffix.size() ||
        archivePath.compare(archivePath.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return std::string();   // 不是我们的包，什么都不做
    }

    const std::filesystem::path pack(archivePath);
    const std::string name = pack.filename().string();
    const std::string dirName = name.substr(0, name.size() - kSuffix.size());
    const auto dest = intoSubdir ? (pack.parent_path() / dirName) : pack.parent_path();

    // 用局部 store：解包是"一次性文件操作"，不该为此在类里再挂一个成员。
    // StdFileStore 本身很轻（只是路径到系统调用的映射）。
    filestore::StdFileStore store;
    filestore::TarXzArchiver archiver(store);
    filestore::ArchiveOptions options;
    std::vector<std::string> extracted;
    std::vector<std::string> skipped;
    if (!archiver.extract(archivePath, dest.string(), options, extracted, skipped).ok()) {
        // 失败保留包：用户还能自己解，总比"包被删了又没解开"好。
        return std::string();
    }

    std::error_code ec;
    std::filesystem::remove(archivePath, ec);   // 包已经用完，留着只会越积越多
    return dest.string();
}

bool Client::pullOnSession(std::string peerId, std::string relPath, bool archive, std::function<void(const Outcome&)> done)
{
    return impl_->startPullOnSession(std::move(peerId), std::move(relPath), archive, std::move(done));
}

bool Client::setReceiveDirOnSession(std::string targetPath)
{
    // 判据与 canSendOnSession 一致：会话在、已认证、当前动作就是接收。
    // 不看 pullActive/sendActive——改落点本身不发帧，与在飞的操作不冲突；
    // 反而正是"取回请求已经在路上"时才需要它（见 GUI 的行级下载）。
    Impl* p = impl_.get();
    if (p == nullptr || p->finished || !p->authenticated || !p->session || p->action != Action::kReceive) {
        return false;
    }
    p->targetPath = std::move(targetPath);
    return true;
}

void Client::cancelSend()
{
    impl_->cancelSendNow();
}

void Client::abort()
{
    impl_->abortNow();
}

const std::string& Client::assignedId() const noexcept
{
    return impl_->assignedId;
}

}   // namespace relayer::cli
