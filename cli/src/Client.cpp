#include "relayer/cli/Client.h"

#include <algorithm>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>

#include "relayer/common/crypto/Sha256.h"
#include "relayer/core/net/ControlLink.h"
#include "relayer/core/net/TransferLink.h"
#include "relayer/core/session/DataChannel.h"
#include "relayer/core/session/FileList.h"
#include "relayer/core/session/FileOp.h"
#include "relayer/core/session/PathMeta.h"
#include "relayer/core/session/PathText.h"
#include "relayer/core/session/PeerTable.h"
#include "relayer/core/transfer/TransferPeer.h"
#include "relayer/filestore/TarXzArchiver.h"

// Client 的实现：**双连接**客户端门面。
//
//   control_  常驻控制连接（net::ControlLink）—— 认证、在线表、列举、
//             元信息、文件操作、数据通道协商。每个请求带请求号，多条可以
//             同时在飞。
//   data_     数据连接（net::TransferLink）—— 只跑文件传输的帧，按需建立。
//
// 与上一版的根本差别，一句话：**应答不再靠"下一个回来的就是我的"配对**。
// 上一版每条连接上一次只能挂一个请求，于是"列举、元信息、取回、远端操作"
// 四者必须互斥，而且传输期间连列举都做不了（同一条 TCP 上数据帧会把控制帧
// 压在写队列后面）。
//
// 直接后果是代码短了：上一版有一大张 dispatch() 表，按"帧类型 + 哪个在飞
// 标志位"判断这帧归谁；现在每个请求的回调自带上下文，那张表连同
// browseActive / metaActive / pullActive / opActive 四个标志位一起消失。
//
// 线程模型不变：所有成员只在 io 线程上读写，没有锁。回调里可以直接发下一个
// 请求（ControlLink 保证回调时 pending 表已经干净）。

namespace relayer::cli {

namespace {

using core::net::ControlLink;
using core::net::TransferLink;
using protocol::Frame;
using protocol::FrameType;
namespace transfer = core::transfer;

// 数据通道协商的重试节奏。
//
// 为什么要重试：双方都在控制连接就绪后立刻建自己的数据连接，但两条连接的
// 握手是并行的，先到的一方问"你的数据连接叫什么"时，另一方可能还在握手。
// 那时它回 kNotReady，而这不是错误——协议里明写了它是"暂时建不起来"。
// 200ms × 40 = 8 秒，覆盖 PBKDF2 在慢机器上的握手时间。
constexpr auto kNegotiateRetryDelay = std::chrono::milliseconds(200);
constexpr int kNegotiateMaxAttempts = 40;

// 慢请求的应答时限：目录取回与解压的应答在**对端压缩/解压完成之后**才发
// （PROTOCOL.md §11.10.2 / §11.12），几十秒是正常值。
constexpr auto kSlowAskTimeout = std::chrono::seconds(120);

std::string fileNameOf(const std::string& path)
{
    filestore::PathPolicy policy;
    return policy.fileName(path);
}

// 数据连接的 id：`<控制连接 id>~d`。
//
// 服务端把 `~` 前缀保留给内部连接（session::kInternalIdPrefix），在线列表
// 会把它过滤掉——用户看到的"在线对端"是人，不该多出一条自己的数据连接。
//
// 为什么不用服务端分配的 client-N：那个号每次重连都变，对端缓存的
// "数据连接 id"立刻过期；声明成确定的名字后，重连后仍然叫同一个名字。
// 超长时退回不声明（用分配的号），代价只是它会出现在在线列表里。
std::string dataIdFor(const std::string& controlId)
{
    if (controlId.empty()) {
        return {};
    }
    const std::string candidate = controlId + "~d";
    if (candidate.size() > core::session::kMaxDataChannelIdBytes) {
        return {};
    }
    return candidate;
}

}   // namespace

struct Client::Impl {
    Impl(asio::io_context& ctxIn, ClientConfig cfg, std::function<void(const ProgressReport&)> prog,
         std::function<void(const Outcome&)> done)
        : ctx(ctxIn), config(std::move(cfg)), onProgress(std::move(prog)), onDone(std::move(done)),
          alive(std::make_shared<char>(0)), idleTimer(ctxIn), watchdog(ctxIn), connectTimer(ctxIn)
    {
    }

    // ---- 常量 ----

    // 目录取回的打包上限（PROTOCOL.md §11.10.3）。压缩是同步做的，这两个
    // 数字决定了"最坏卡多久"，所以写死而不做成配置。
    static constexpr std::size_t kMaxPackFiles = 20000;
    static constexpr std::uint64_t kMaxPackBytes = 256ULL * 1024 * 1024;

    // ---- 基本 ----

    asio::io_context& ctx;
    ClientConfig config;
    std::function<void(const ProgressReport&)> onProgress;
    std::function<void(const Outcome&)> onDone;

    filestore::StdFileStore store;

    // 回调存活的凭据。Impl 由 unique_ptr 持有，回调里没法用 weak_ptr 指到
    // 它，而 asio 的异步回调可能在 Client 析构之后才轮到（定时器、连接关闭
    // 通知）。凡是捕获 `this` 的异步回调都先看一眼它：use_count() == 1
    // 表示只剩回调自己还持有，Impl 已经不在了。
    std::shared_ptr<char> alive;

    // 正在析构。析构期间不能再往上层报结论——那时上层对象可能已经没了。
    bool destroying = false;

    // ---- 连接 ----

    std::shared_ptr<ControlLink> control;
    std::shared_ptr<TransferLink> data;

    enum class DataState {
        kIdle,
        kConnecting,
        kReady,
        kFailed,
    };
    DataState dataState = DataState::kIdle;
    std::vector<std::function<void(bool)>> dataWaiters;
    std::string dataId;            // 本机数据连接的 id（协商时告诉对端的就是它）
    std::string requestedDataId;   // 声明给服务端的名字（`<控制连接 id>~d`）
    // 等 claim 生效的兜底。claim 可能失败（名字被占用），那时永远不会等到
    // 真名，不能无限等——用临时号也能工作，只是对端缓存的 id 会随重连变。
    asio::steady_timer claimWait{ctx};
    bool claimWaitStarted = false;

    // 对端控制连接 id → 对端数据连接 id。协商一次就记住，之后直接复用。
    std::map<std::string, std::string> peerDataIds;

    // 在飞的协商：peer → 等待者。同一对端只发一次请求，多个调用方排队。
    struct Negotiation {
        std::vector<std::function<void(bool, std::string)>> waiters;
        bool inFlight = false;
    };
    std::map<std::string, Negotiation> negotiations;

    // 在飞的一路发送。进度与结论都按 job 存：并发时"这条进度是谁的"
    // 必须能区分，只有一组全局数字的界面会把 B 的进度画到 C 那一行。
    struct SendEntry {
        std::function<void(const Outcome&)> done;
        std::function<void(const ProgressReport&)> progress;

        // 速率基线也按路单独算——共用一个基线会让两路的瞬时速率互相污染。
        std::uint64_t lastBytes = 0;
        std::chrono::steady_clock::time_point lastAt = std::chrono::steady_clock::now();
        double speed = 0.0;
    };
    std::map<std::uint64_t, SendEntry> sends;

    Action action = Action::kList;
    std::string assignedId;
    bool startedOnce = false;
    bool finished = false;

    // 接收目录（对端发来的文件落在这里，相对路径也按它还原）。
    std::string targetPath;

    // 一次性的自检 / 搭车动作的"只做一次"标志。
    bool selfBrowseStarted = false;
    bool selfSendStarted = false;
    bool pullStarted = false;

    // list 的结果（finish 时要搬进 Outcome）。
    std::vector<core::session::PeerInfo> peers;

    // browse 的结果。
    std::vector<core::session::RemoteEntry> remoteEntries;
    core::session::FileListStatus listStatus = core::session::FileListStatus::kOk;
    bool rootListing = false;

    // fileOp 的结果。
    std::string opSha256Hex;
    std::string opExtractedDir;
    std::vector<std::string> opFailedPaths;
    FileOpKind opKind = FileOpKind::kSha256;

    // 请求参数（一次性动作的输入）。
    std::string browsePeerId;
    std::string browseRelPath;
    std::string toPeerId;
    std::string peerPathSource;
    std::string peerPathRemoteName;

    // 进度速率的基线。
    std::uint64_t lastBytes = 0;
    std::chrono::steady_clock::time_point lastReportAt = std::chrono::steady_clock::now();
    double lastSpeed = 0.0;

    TransferStats lastStats;

    // 空闲看门狗。传输层自己也有一个（TransferPeer 的 finalizeTimeout），
    // 但那只覆盖"收尾阶段等确认"；"传了一半对端消失"必须由调用方判——
    // 发送方进程被 kill 之后，接收方在本端观察到的唯一事实就是"没有进展"。
    asio::steady_timer idleTimer;
    std::uint64_t lastActivityBytes = 0;
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
    // 传输是否真的开始过（收到过非零 total）。用它把"还没开始传"和
    // "传到一半停了"区分开——前者沉默是正常的（发送方在算摘要、对端还没
    // 点发送），后者才是故障。
    bool transferStarted = false;

    asio::steady_timer watchdog;

    // 连接看门狗：从 connect() 起算，到控制连接就绪为止。
    //
    // 必须有它。没有的话，某些"连不上"的路径（TCP 被拒、握手无响应）里
    // 连接层可能既不回调 ready 也不回调 closed，事件循环就此空转——现象是
    // 进程挂着不动，既不成功也不报错，只能靠外面 kill。手写一个上限，
    // 让每一种失败都有一个明确的结论。
    asio::steady_timer connectTimer;

    // ---------------------------------------------------------------------
    // 收尾
    // ---------------------------------------------------------------------

    // 结束只能发生一次。所有路径（成功、失败、超时、连接断）都走这里，
    // 否则会出现"先因为超时报错，两秒后又因为对端回帧报成功"。
    void finish(bool ok, std::string summary, Status status = Status{}, bool connectionLost = false)
    {
        if (finished) {
            return;
        }
        finished = true;
        watchdog.cancel();
        connectTimer.cancel();
        idleTimer.cancel();

        // 会话级结论统一由下面这次 onDone 发出，正在进行的子操作不再单独
        // 回调（上层收到两个结论会把同一项算两遍）。
        sends.clear();
        negotiations.clear();
        dataWaiters.clear();

        if (data) {
            data->close();
        }
        if (control) {
            control->close();
        }

        if (destroying) {
            return;
        }

        if (onDone) {
            Outcome o;
            o.ok = ok;
            o.summary = std::move(summary);
            o.status = std::move(status);
            o.connectionLost = connectionLost;
            o.stats = lastStats;
            if (action == Action::kList) {
                for (const auto& p : peers) {
                    o.peerList.push_back(PeerEntry{p.id, p.address});
                }
            } else if (action == Action::kBrowse || !remoteEntries.empty()) {
                // 除了 ls，**自检列举**（--self-browse）也会带出目录内容，
                // 而它的 action 是 kReceive——只按 action 判会把结果丢掉，
                // 现象是"列了两项却说 count=0"。
                o.remoteEntries = remoteEntries;
                o.listStatus = listStatus;
                o.rootListing = rootListing;
            } else if (action == Action::kFileOp) {
                o.sha256Hex = opSha256Hex;
                o.extractedDir = opExtractedDir;
                o.failedPaths = opFailedPaths;
            }
            auto cb = std::move(onDone);
            onDone = nullptr;
            cb(o);
        }
    }

    // 硬上限：与传输空闲超时不是一回事。后者看"有没有进展"，本项看
    // "总共花了多久"，测试用它兜底，避免一个逻辑 bug 把 CI 拖到全局超时。
    void armHardTimeout()
    {
        if (config.hardTimeout.count() <= 0 || finished) {
            return;
        }
        watchdog.expires_after(config.hardTimeout);
        std::weak_ptr<char> token = alive;
        watchdog.async_wait([this, token](const std::error_code& ec) {
            if (ec || token.expired()) {
                return;
            }
            finish(false, "超时：操作总时长超过上限", Status::Fail(ErrorCode::kTimeout, "hard timeout"));
        });
    }

    // 空闲看门狗：500ms 一次的粗粒度检查。
    //
    // 为什么不是"每次收到帧就重置一个定时器"：那要在每个回调里插一行，
    // 漏一处就永远不超时。周期性检查只依赖两个状态（字节数、开始标志），
    // 加新代码时不会悄悄失效。
    void armIdleWatch()
    {
        if (finished || config.idleTimeout.count() <= 0) {
            return;
        }
        idleTimer.expires_after(std::chrono::milliseconds(500));
        std::weak_ptr<char> token = alive;
        idleTimer.async_wait([this, token](const std::error_code& ec) {
            if (ec || token.expired() || finished) {
                return;
            }
            checkIdle();
            armIdleWatch();
        });
    }

    void checkIdle()
    {
        if (finished || !transferStarted) {
            // 还没开始传：沉默是正常的（发送方在算摘要、接收方在等请求帧）。
            lastActivity = std::chrono::steady_clock::now();
            return;
        }
        if (lastBytes != lastActivityBytes) {
            lastActivityBytes = lastBytes;
            lastActivity = std::chrono::steady_clock::now();
            return;
        }
        const auto idle = std::chrono::steady_clock::now() - lastActivity;
        if (idle < config.idleTimeout) {
            return;
        }

        // 措辞要准：这里的结论是"对端没动静"，不是"传输失败"。对端进程被
        // kill 之后本地能观察到的**唯一**事实就是没有进展——把这点说清楚，
        // 测试失败时人才不会往错的方向查。
        finish(false,
               "对端长时间无响应（" +
                   std::to_string(std::chrono::duration_cast<std::chrono::seconds>(config.idleTimeout).count()) + " 秒无进展）",
               Status::Fail(ErrorCode::kTimeout, "peer idle"),
               // 发送方视角的"对端失联"就是连接级失败：可能是对端进程死了，
               // 也可能是中继链路断了。批次执行者凭它决定"剩余项不再发"。
               true);
    }

    // ---------------------------------------------------------------------
    // 连接
    // ---------------------------------------------------------------------

    void connect()
    {
        ControlLink::Config cc;
        cc.host = config.host;
        cc.port = config.port;
        cc.password = config.password;
        cc.noAuth = config.serverAllowNoAuth;
        cc.requestedId = config.requestedId;

        ControlLink::Handler h;
        std::weak_ptr<char> token = alive;

        h.onReady = [this, token](std::string id) {
            if (token.expired()) {
                return;
            }
            onControlReady(std::move(id));
        };
        h.onClosed = [this, token](Status why) {
            if (token.expired()) {
                return;
            }
            onControlClosed(std::move(why));
        };
        h.onPeerRequest = [this, token](const Frame& req, Frame& answer) {
            if (token.expired()) {
                return false;
            }
            return serveRequest(req, answer);
        };

        control = ControlLink::connect(ctx, std::move(cc), std::move(h));
        armHardTimeout();
        armIdleWatch();

        // 连接 / 认证的整体时限。握手本身有超时，这里兜的是"连 TCP 都没
        // 建立起来且没人报错"的情形。
        connectTimer.expires_after(config.connectTimeout);
        std::weak_ptr<char> ct = alive;
        connectTimer.async_wait([this, ct](const std::error_code& ec) {
            if (ec || ct.expired() || finished || startedOnce) {
                return;
            }
            finish(false, "连不上中继（" + std::to_string(config.connectTimeout.count()) + " 秒内没完成认证）",
                   Status::Fail(ErrorCode::kTimeout, "connect timeout"));
        });
    }

    void onControlReady(std::string id)
    {
        // ControlLink 会在两处通知：握手刚过（此时还不知道自己叫什么）与
        // 收到 kAnswerId 之后（真实 id）。所以本函数必须幂等。
        const std::string previous = assignedId;
        if (!id.empty()) {
            assignedId = std::move(id);
        }

        // 契约输出：id 由服务端分配且递增，测试无法预知接收端会拿到
        // client-几。没有这一行，测试只能靠解析 list 的输出猜，而每次
        // list 又会消耗一个新的 id——猜的过程改变答案。
        //
        // **只打非空、且只打一次**。踩过的坑：第一行若是 `ASSIGNED_ID=`，
        // 脚本 head -1 拿到空串，于是"接收端没拿到 id"这个报错出现在完全
        // 正常的连接上。声明了标识时还要多一条：临时号不算数，只认等于
        // 声明的那个（服务端会先发 client-N，改名成功后再发真名）。
        const bool identityFinal = config.requestedId.empty() || assignedId == config.requestedId;
        if (!assignedId.empty() && assignedId != previous && identityFinal) {
            std::printf("ASSIGNED_ID=%s\n", assignedId.c_str());
            std::fflush(stdout);
            if (config.onIdAssigned) {
                config.onIdAssigned(assignedId);
            }

            // 数据连接的 id 依赖控制连接的最终 id（`<id>~d`），所以在身份
            // 确定之后才建立它。提前建会拿到 `~` 后面跟着临时号的垃圾名字。
            if (action == Action::kReceive || action == Action::kSend || !config.selfSendPath.empty()) {
                ensureDataLink([](bool) {});
            }

            if (action == Action::kReceive && config.askListOnStart) {
                requestPeerList();
            }

            // 自检与搭车动作都要**最终身份**：声明了 --id 时，第一次回调的
            // id 还是临时号（甚至为空），那时发出去的 `to` 是错的。
            if (action == Action::kReceive) {
                startReceiveExtras();
            }
        }

        if (startedOnce) {
            return;
        }
        startedOnce = true;
        connectTimer.cancel();   // 控制连接已就绪，连接看门狗完成使命

        switch (action) {
        case Action::kList:
            requestPeerList();
            return;

        case Action::kBrowse:
            // 只要 `to` 对，不必等自己的 id——服务端在认证时就登记了这条
            // 连接，转发目标查得到。
            requestFileList();
            return;

        case Action::kSend:
            ensureDataLink([this](bool ok) {
                if (!ok) {
                    finish(false, "无法建立数据连接", Status::Fail(ErrorCode::kIoError, "data link failed"));
                    return;
                }
                negotiateDataId(toPeerId, [this](bool ok2, std::string peerDataId) {
                    if (!ok2) {
                        finish(false, "对端的数据连接不可用（对方可能正在关闭或未升级协议）",
                               Status::Fail(ErrorCode::kTimeout, "data channel negotiation failed"));
                        return;
                    }
                    startSend(peerPathSource, peerPathRemoteName, peerDataId);
                });
            });
            return;

        case Action::kReceive:
            // 接收方本身不主动做事：数据连接挂好常驻 receiver 之后等对端推。
            // 自检 / 取回那几件事需要**最终身份**，所以在 onControlReady 的
            // identityFinal 分支里启动（见 startReceiveExtras）。
            return;

        case Action::kFileOp:
            runFileOpOnce();
            return;
        }
    }

    // 接收会话上的"一次性搭车动作"：自检列举、取回、自检发送。
    //
    // 三者都要求身份**最终确定**（见 onControlReady 的注释），且各自只做
    // 一次。放在这里而不是 switch 里，是因为 switch 只在第一次认证回调时
    // 跑一遍，而那时 id 可能还是空的——声明了 --id 的客户端会因此**永远
    // 不启动**这些动作，症状是 recv 挂着不动直到硬超时。
    void startReceiveExtras()
    {
        // 自检列举：在常驻接收会话上向自己发一次 kAskFileList，列完即退出。
        // 存在的理由是 CLI 的 ls 走 browsePeer（一次性连接），那条路与
        // GUI 的"在常驻会话上列举"不是同一份代码，只测 ls 覆盖不到。
        if (config.selfBrowse && !selfBrowseStarted) {
            selfBrowseStarted = true;
            browseNow(assignedId, config.selfBrowsePath, [this](const Outcome& o) {
                // 结果先落回成员再收尾：finish() 是按成员构造 Outcome 的。
                remoteEntries = o.remoteEntries;
                listStatus = o.listStatus;
                rootListing = o.rootListing;
                finish(o.ok, o.summary, o.status);
            });
            return;
        }

        // 取回：请求搭在控制连接上，回程的文件由数据连接收下。
        if (!config.pullFrom.empty()) {
            maybeStartPull();
            return;
        }

        // 自检发送：把自己的文件发给自己（§11.3.1）。一条命令就能验证
        // "中继 + 认证 + 双连接 + 传输 + 落盘"整条链路，不必找第二台机器。
        if (!config.selfSendPath.empty() && !selfSendStarted) {
            selfSendStarted = true;
            ensureDataLink([this](bool ok) {
                if (!ok) {
                    finish(false, "无法建立数据连接", Status::Fail(ErrorCode::kIoError, "data link failed"));
                    return;
                }
                negotiateDataId(assignedId, [this](bool ok2, std::string peerDataId) {
                    if (!ok2) {
                        // 自己是自己，协商失败说明链路有问题：报出来，别让
                        // 进程挂在"等文件"上直到硬超时。
                        finish(false, "自我协商失败（数据连接不可用）",
                               Status::Fail(ErrorCode::kTimeout, "self negotiate failed"));
                        return;
                    }
                    startSend(config.selfSendPath, std::string(), peerDataId, [this](const Outcome& o) {
                        // 成功不做额外动作：接收侧收完会自己 finish。失败要
                        // 提前结束，否则进程挂在"等文件"上，看不出是自检发送
                        // 出了问题。
                        if (!o.ok && !finished) {
                            finish(false, "自检发送失败：" + o.summary, o.status);
                        }
                    });
                });
            });
        }
    }

    void onControlClosed(Status why)
    {
        if (finished) {
            return;
        }

        // 声明标识被拒：这不是"网络断了"，措辞必须让用户知道该改什么。
        // 两种原因的处置完全不同——被占用是要么换名字、要么关掉占用的那台；
        // 不合法是名字本身不能用。
        if (why.code == ErrorCode::kIdTaken) {
            finish(false, "标识「" + config.requestedId + "」已被占用，该标识同时只能有一条连接", std::move(why), true);
            return;
        }
        if (why.code == ErrorCode::kIdInvalid) {
            finish(false, "标识「" + config.requestedId + "」不合法（不能为空/超长/含非法字符）", std::move(why), true);
            return;
        }

        // 漏填 --no-auth 时的症状是"连上立刻被断开"，与中继不可达完全不同，
        // 给一条可照做的提示，而不是让用户去猜协议细节。
        std::string msg = why.message;
        if (!config.serverAllowNoAuth && msg.find("non-auth frame") != std::string::npos) {
            msg += "（中继可能以 --allow-no-auth 启动，客户端请加 --no-auth 重试）";
        }

        // 连接没了，正在跑的传输不会有结论。报一次连接级失败——重试剩余
        // 工作没有意义（批次执行者凭 connectionLost 打住）。
        const bool userClosed = why.code == ErrorCode::kCancelled;
        finish(false, userClosed ? "已断开" : "与中继的连接已断开：" + msg, std::move(why), true);
    }

    // ---------------------------------------------------------------------
    // 数据连接
    // ---------------------------------------------------------------------

    // 建立（或确认已建立）数据连接。cb 在就绪/失败时调用恰好一次。
    void ensureDataLink(std::function<void(bool)> cb)
    {
        if (finished || control == nullptr) {
            cb(false);
            return;
        }
        if (dataState == DataState::kReady && data && data->ready()) {
            cb(true);
            return;
        }
        if (dataState == DataState::kFailed) {
            // 失败过的连接不自动重试：上一次多半是认证失败或端口不可达，
            // 悄悄重试只会把真实的错误藏起来。
            cb(false);
            return;
        }

        dataWaiters.push_back(std::move(cb));
        if (dataState == DataState::kConnecting || dataState == DataState::kReady) {
            return;
        }
        // 身份还没定下来：数据连接的 id 是 `<控制连接 id>~d`，此刻建出来的
        // 会是 `~d` 这种垃圾名字（或者干脆退化成不声明）。等 onControlReady
        // 拿到最终 id 后再启动——它会再调一次 ensureDataLink。
        if (assignedId.empty()) {
            return;
        }
        startDataLink();
    }

    void startDataLink()
    {
        dataState = DataState::kConnecting;
        requestedDataId = dataIdFor(assignedId);
        dataId = requestedDataId;
        claimWaitStarted = false;

        TransferLink::Config dc;
        dc.host = config.host;
        dc.port = config.port;
        dc.password = config.password;
        dc.noAuth = config.serverAllowNoAuth;
        dc.requestedId = dataId;
        dc.receiveDir = targetPath;
        dc.chunkSize = config.chunkSize;
        dc.overwriteExisting = config.overwriteExisting;
        dc.idleTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.idleTimeout);
        // 与 idleTimeout 解耦：它只管"请求帧发出后等 kAnswerSend"这一段。
        // 目标不在线时中继静默丢帧，用 60 秒的 idleTimeout 意味着"发错 id
        // 要干等一分钟"，而 AnswerSend 在对端在线时是毫秒级。
        dc.answerTimeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::min(config.idleTimeout, std::chrono::seconds(10)));
        // 只发不收的客户端不挂 receiver：否则任何知道本机数据连接 id 的
        // 对端都能往接收目录写文件，而纯发送方没理由接受这件事。
        dc.acceptIncoming = (action == Action::kReceive);

        TransferLink::Handler h;
        std::weak_ptr<char> token = alive;

        h.onReady = [this, token](std::string id) {
            if (token.expired()) {
                return;
            }
            // **空 id 不算就绪**。Session 的第一次认证通知发生在"知道自己
            // 叫什么之前"，此时 id 为空，而 dataId 还停在 startDataLink 预设
            // 的猜测值（`<控制连接 id>~d`）——直接判就绪会让协商抢在 claim
            // 之前跑，把临时号发给对端。
            if (id.empty()) {
                return;
            }
            dataId = id;
            if (dataState == DataState::kReady) {
                return;
            }

            // **声明了名字时要等那个名字**。服务端在认证完成时先给一个临时
            // 号（client-N），claim 生效后才改名；而这两次通知之间，协商
            // 完全可能已经跑完了——那时告诉对端的是 client-N，等 claim 一
            // 改名，对端缓存的 id 立刻失效，它下一次发送就发进虚空（中继
            // 找不到目标，静默丢帧，发送方等满 answerTimeout）。
            //
            // 所以这里不把"拿到临时号"当作就绪：在真名到手之前，协商请求
            // 会被回 kNotReady，发起方 200ms 后重试——那条路径本来就有。
            if (!requestedDataId.empty() && dataId != requestedDataId) {
                if (!claimWaitStarted) {
                    claimWaitStarted = true;
                    claimWait.expires_after(std::chrono::seconds(3));
                    claimWait.async_wait([this, token](const std::error_code& ec) {
                        if (ec || token.expired() || dataState == DataState::kReady) {
                            return;
                        }
                        // claim 没成功（名字被占用）：用临时号也能工作。
                        // 3 秒是"一个人等得起、又不会掩盖问题"的折中。
                        markDataReady();
                    });
                }
                return;
            }
            claimWait.cancel();
            markDataReady();
        };
        h.onClosed = [this, token](Status why) {
            if (token.expired()) {
                return;
            }
            // 数据连接断了不影响控制连接：列举、浏览、改名都还能用。
            // 只有正在跑的传输会失败（TransferLink 自己会逐个报出来）。
            claimWait.cancel();
            auto waiters = std::move(dataWaiters);
            dataWaiters.clear();
            for (auto& w : waiters) {
                w(false);
            }

            // 常驻会话（GUI 的接收端）必须自己把数据连接捡回来：没有它，
            // 之后的每一次发送都会失败，而用户看到的只是"发不出去"，
            // 没有任何线索指向"数据连接已经没了"。
            //
            // 一次性操作（CLI 的 send/ls/op）不重建：那条连接用完就退，
            // 重建只会让进程多活一会儿。
            if (config.keepReceiving && !finished) {
                dataState = DataState::kIdle;
                asio::post(ctx, [this, token] {
                    if (token.expired() || finished || dataState != DataState::kIdle) {
                        return;
                    }
                    startDataLink();
                });
                return;
            }
            dataState = DataState::kFailed;
            (void)why;
        };
        h.onProgress = [this, token](const TransferLink::JobInfo& job) {
            if (token.expired()) {
                return;
            }
            reportProgress(job);
        };
        h.onFinished = [this, token](const TransferLink::JobInfo& job, const transfer::TransferResult& r) {
            if (token.expired()) {
                return;
            }
            onJobFinished(job, r);
        };

        data = TransferLink::connect(ctx, std::move(dc), store, std::move(h));
    }

    // 数据连接可以用了：把排队的调用方全部唤醒。
    void markDataReady()
    {
        if (dataState == DataState::kReady) {
            return;
        }
        dataState = DataState::kReady;
        auto waiters = std::move(dataWaiters);
        dataWaiters.clear();
        for (auto& w : waiters) {
            w(true);
        }
    }

    // 换对端的数据连接 id。一次往返，结果缓存。
    void negotiateDataId(std::string peerId, std::function<void(bool, std::string)> cb)
    {
        if (peerId.empty()) {
            cb(false, {});
            return;
        }
        if (const auto it = peerDataIds.find(peerId); it != peerDataIds.end()) {
            cb(true, it->second);
            return;
        }

        auto& n = negotiations[peerId];
        n.waiters.push_back(std::move(cb));
        if (n.inFlight) {
            return;
        }
        n.inFlight = true;
        askDataChannel(peerId, 1);
    }

    void askDataChannel(const std::string& peerId, int attempt)
    {
        if (finished || control == nullptr || dataState != DataState::kReady) {
            finishNegotiation(peerId, false, {});
            return;
        }

        std::weak_ptr<char> token = alive;
        auto body = core::session::encodeAskDataChannel(dataId);
        const bool asked = control->ask(
            FrameType::kAskDataChannel, peerId, std::move(body), [this, token, peerId, attempt](const ControlLink::Answer& a) {
                if (token.expired()) {
                    return;
                }
                if (!a.ok) {
                    finishNegotiation(peerId, false, {});
                    return;
                }
                core::session::DataChannelStatus status = core::session::DataChannelStatus::kRejected;
                std::string peerDataId;
                std::string err;
                if (!core::session::decodeAnswerDataChannel(a.body, status, peerDataId, err)) {
                    finishNegotiation(peerId, false, {});
                    return;
                }
                if (status == core::session::DataChannelStatus::kOk && !peerDataId.empty()) {
                    peerDataIds[peerId] = peerDataId;
                    finishNegotiation(peerId, true, peerDataId);
                    return;
                }
                if (status == core::session::DataChannelStatus::kNotReady && attempt < kNegotiateMaxAttempts) {
                    // 对端的数据连接还在握手。等一会儿再问——这不是
                    // 错误，协议里明写了它是"暂时建不起来"。
                    auto timer = std::make_shared<asio::steady_timer>(ctx);
                    timer->expires_after(kNegotiateRetryDelay);
                    timer->async_wait([this, token, peerId, attempt, timer](const std::error_code& ec) {
                        if (ec || token.expired()) {
                            return;
                        }
                        askDataChannel(peerId, attempt + 1);
                    });
                    return;
                }
                finishNegotiation(peerId, false, {});
            });
        if (!asked) {
            finishNegotiation(peerId, false, {});
        }
    }

    void finishNegotiation(const std::string& peerId, bool ok, std::string peerDataId)
    {
        const auto it = negotiations.find(peerId);
        if (it == negotiations.end()) {
            return;
        }
        auto waiters = std::move(it->second.waiters);
        negotiations.erase(it);
        for (auto& w : waiters) {
            w(ok, peerDataId);
        }
    }

    // ---------------------------------------------------------------------
    // 接收侧：对端发来的请求
    // ---------------------------------------------------------------------

    // 返回 true = 认了这条请求，answer 里装的是应答（`to` 与 `index` 由
    // ControlLink 填）。返回 false = 不认识，对方会等满它自己的超时。
    bool serveRequest(const Frame& req, Frame& answer)
    {
        switch (req.type) {
        case FrameType::kAskFileList:
            serveFileList(req, answer);
            return true;

        case FrameType::kAskPathMeta:
            servePathMeta(req, answer);
            return true;

        case FrameType::kAskDataChannel:
            serveDataChannel(req, answer);
            return true;

        case FrameType::kAskFilePull:
            servePullRequest(req, answer, false);
            return true;

        case FrameType::kAskArchivePull:
            servePullRequest(req, answer, true);
            return true;

        case FrameType::kAskSha256:
        case FrameType::kAskRename:
        case FrameType::kAskCreateDir:
        case FrameType::kAskDelete:
        case FrameType::kAskUnarchive:
            serveFileOp(req, answer);
            return true;

        default:
            return false;
        }
    }

    // 对端登记它的数据连接，同时得知我们的。
    //
    // 这一步不只是"告诉对方一个名字"：请求里带着对方的数据连接 id，我们
    // 存下来——之后对方来要文件（§11.9）时，我们得知道往哪儿发。
    void serveDataChannel(const Frame& req, Frame& answer)
    {
        std::string peerDataId;
        std::string err;
        if (core::session::decodeAskDataChannel(req.body, peerDataId, err) && !peerDataId.empty()) {
            peerDataIds[req.from] = peerDataId;
        }

        answer.type = FrameType::kAnswerDataChannel;
        const bool ready = (dataState == DataState::kReady && data && data->ready());
        core::session::DataChannelStatus status =
            ready ? core::session::DataChannelStatus::kOk : core::session::DataChannelStatus::kNotReady;
        (void)core::session::encodeAnswerDataChannel(status, ready ? dataId : std::string(), answer.body);
    }

    // 对端来问目录：按**绝对路径**列一层。
    //
    // 没有共享根（DESIGN.md §3.8）：本工具是单用户自用，v1 就是全路径访问，
    // 多一层"相对根"只会让"看 D:\照片"变成先配置再浏览两步。代价是任何能
    // 连上中继、知道本机 id 的人都能列举本机路径——这一点写在
    // PROTOCOL.md §11.8.1，不要在别处偷偷把它当安全边界。
    void serveFileList(const Frame& req, Frame& answer)
    {
        answer.type = FrameType::kAnswerFileList;

        std::vector<core::session::RemoteEntry> out;
        auto status = core::session::FileListStatus::kOk;
        bool isRootList = false;

        std::string path;
        std::string err;
        if (!core::session::decodeAskFileList(req.body, path, err)) {
            status = core::session::FileListStatus::kRejected;
        } else if (path.empty()) {
            // 空路径 = 要根列表：盘符 + 家目录。没有这一层，请求方在绝对
            // 路径模型下就没有起点，只能靠猜盘符。
            isRootList = true;
            const auto roots = store.roots();
            if (roots.empty()) {
                status = core::session::FileListStatus::kNotFound;
            } else {
                out.reserve(roots.size());
                for (const auto& r : roots) {
                    core::session::RemoteEntry e;
                    e.name = r;
                    e.isDir = true;
                    const auto st = store.stat(r);
                    if (st.exists) {
                        e.mtime = st.mtime;
                    }
                    out.push_back(std::move(e));
                }
            }
        } else {
            path = core::session::normalizePathText(path);
            const auto st = store.stat(path);
            if (!st.exists || !st.isDir) {
                status = core::session::FileListStatus::kNotFound;
            } else {
                std::vector<filestore::DirEntry> raw;
                if (!store.list(path, false, raw).ok()) {
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

        if (!core::session::encodeAnswerFileList(status, isRootList, out, answer.body).ok()) {
            answer.body.clear();
        }
    }

    // 对端来问"这几个绝对路径在不在、多大、什么时候改的、指纹是多少"。
    //
    // 一次 stat + 一次 quickFingerprint（只读前 64KB）逐条处理。指纹是给
    // 发送方的覆盖提示用的：拿本地文件的指纹一比就知道"内容是否一致"，
    // 不必再单独发一轮采样请求（v1 的 AskFileSamplesBatch 由此省掉）。
    void servePathMeta(const Frame& req, Frame& answer)
    {
        answer.type = FrameType::kAnswerPathMeta;

        std::vector<std::string> paths;
        std::string err;
        std::vector<core::session::RemoteMeta> metas;
        if (!core::session::decodeAskPathMeta(req.body, paths, err)) {
            // 请求结构坏了：回 0 条而不是不回。不发应答会让对端等到超时，
            // 那看起来像网络问题。
            (void)core::session::encodeAnswerPathMeta(metas, answer.body);
            return;
        }

        metas.reserve(paths.size());
        for (const auto& raw : paths) {
            // 绝对路径原样规范化；相对路径按**接收目录**还原，与发送落点
            // 同一条规则（DESIGN §3.8）。不这么做，"落到我的接收目录"这种
            // 最常见的发送，检查会去 stat 对方的进程工作目录，结论恒为
            // "不存在"——检查等于白做，用户以为对方没有那份文件。
            const auto p = absoluteForPeer(raw);
            core::session::RemoteMeta m;
            const auto st = store.stat(p);
            if (st.exists) {
                m.exists = true;
                m.isDir = st.isDir;
                m.size = st.size;
                m.mtime = st.mtime;
                if (!st.isDir) {
                    // 指纹为空只表示"算不出来"（读失败/没权限），调用方
                    // 按"无法粗判"处理，不能让整批失败。
                    m.fingerprint = store.quickFingerprint(p);
                }
            }
            metas.push_back(std::move(m));
        }

        (void)core::session::encodeAnswerPathMeta(metas, answer.body);
    }

    // 对端来要文件 / 要目录（B 侧）：校验 → 回状态 → 在数据连接上推。
    //
    // 应答与文件的先后顺序在这里很要紧：
    //   - 取**文件**：先回"接受"，再推。反过来的话请求方会先收到一个陌生的
    //     传输请求，它甚至不知道该不该收。
    //   - 取**目录**：先打包，**打包成功之后**才回"接受"（§11.10.2）。打包
    //     会失败（磁盘满、权限、文件在压缩中被删）。先回 0 再失败，请求方
    //     只能看到"对方答应了但没来东西"，直到空闲超时——所有失败里最难查的
    //     一种。
    void servePullRequest(const Frame& req, Frame& answer, bool archive)
    {
        using core::session::FilePullStatus;

        answer.type = FrameType::kAnswerFilePull;

        std::string path;
        std::string err;
        auto status = FilePullStatus::kRejected;
        std::string sendPath;
        std::string sendName;
        std::string packPath;   // 打包产生的临时文件，传完要删

        if (!core::session::decodeAskFileList(req.body, path, err)) {
            status = FilePullStatus::kRejected;
        } else if (path.empty()) {
            // 空路径没有意义（根不是文件），明确拒绝。
            status = FilePullStatus::kRejected;
        } else {
            path = core::session::normalizePathText(path);
            const auto st = store.stat(path);
            if (archive) {
                if (!st.exists || !st.isDir) {
                    status = FilePullStatus::kNotFound;   // 文件请走 §11.9
                } else {
                    std::string packErr;
                    packPath = packDirectory(path, packErr);
                    if (packPath.empty()) {
                        status = FilePullStatus::kRejected;
                    } else {
                        status = FilePullStatus::kAccepted;
                        sendPath = packPath;
                        // 包名用目录最后一段，让对方一眼知道落到本地的是什么
                        // （§11.10.3）。绝对路径上分隔符可能是 `/` 也可能是
                        // `\`，两个都要认——否则 C:\foo 会被当成整串名字，
                        // 包名变成 `C:\foo.tar.xz`（非法文件名）。
                        const auto slash = path.find_last_of("/\\");
                        const std::string dirName = (slash == std::string::npos) ? path : path.substr(slash + 1);
                        sendName = dirName + ".tar.xz";
                    }
                }
            } else {
                if (!st.exists || st.isDir) {
                    status = FilePullStatus::kNotFound;
                } else {
                    status = FilePullStatus::kAccepted;
                    sendPath = path;
                    sendName = fileNameOf(path);
                }
            }
        }

        // 需要对方的数据连接 id 才能发。拿不到就明确拒绝——回"接受"然后
        // 静默发不出去，请求方会一直等到空闲超时。
        if (status == FilePullStatus::kAccepted) {
            const auto it = peerDataIds.find(req.from);
            if (it == peerDataIds.end() || it->second.empty() || dataState != DataState::kReady) {
                status = FilePullStatus::kRejected;
                if (!packPath.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(packPath, ec);
                    packPath.clear();
                }
            }
        }

        answer.body = core::session::encodeAnswerFilePull(status);
        if (status != FilePullStatus::kAccepted) {
            return;
        }

        // ControlLink 在本函数返回后**同步**把应答放进写队列，所以这里排的
        // 任务一定排在应答之后——上面的顺序依赖这一点。
        const std::string peerDataId = peerDataIds.at(req.from);
        std::weak_ptr<char> token = alive;
        asio::post(ctx, [this, token, sendPath, sendName, peerDataId, packPath] {
            if (token.expired() || finished) {
                return;
            }
            startSend(sendPath, sendName, peerDataId, [packPath](const Outcome&) {
                // 传完（成功、失败、取消）都删：留着的临时包只会越积越多，
                // 而它已经没用了。
                if (packPath.empty()) {
                    return;
                }
                std::error_code ec;
                std::filesystem::remove(packPath, ec);
            });
        });
    }

    // ---------------------------------------------------------------------
    // 远端文件操作（PROTOCOL.md §11.12）
    // ---------------------------------------------------------------------

    void serveFileOp(const Frame& req, Frame& answer)
    {
        switch (req.type) {
        case FrameType::kAskSha256:
            serveSha256(req, answer);
            return;
        case FrameType::kAskRename:
            serveRename(req, answer);
            return;
        case FrameType::kAskCreateDir:
            serveCreateDir(req, answer);
            return;
        case FrameType::kAskDelete:
            serveDelete(req, answer);
            return;
        case FrameType::kAskUnarchive:
            serveUnarchive(req, answer);
            return;
        default:
            return;
        }
    }

    void serveSha256(const Frame& req, Frame& answer)
    {
        using core::session::FileOpStatus;
        FileOpStatus status = FileOpStatus::kOk;
        std::vector<char> digest;
        std::string path;
        std::string err;

        if (!core::session::decodeAskPath(req.body, path, err)) {
            status = FileOpStatus::kRejected;
        } else {
            path = absoluteForPeer(std::move(path));
            const auto st = store.stat(path);
            if (!st.exists) {
                status = FileOpStatus::kNotFound;
            } else if (st.isDir) {
                // 目录没有"内容的 SHA-256"。回 kRejected 而不是算一个空摘要
                // ——后者会让界面显示一串看起来正常的哈希。
                status = FileOpStatus::kRejected;
            } else {
                digest = sha256Of(path, err);
                if (digest.empty()) {
                    status = FileOpStatus::kDenied;
                }
            }
        }

        answer.type = FrameType::kAnswerSha256;
        answer.body.clear();
        (void)core::session::encodeAnswerSha256(status, digest, answer.body);
    }

    void serveRename(const Frame& req, Frame& answer)
    {
        using core::session::FileOpStatus;
        FileOpStatus status = FileOpStatus::kOk;
        std::string from;
        std::string to;
        std::string err;

        if (!core::session::decodeAskRename(req.body, from, to, err)) {
            status = FileOpStatus::kRejected;
        } else {
            from = absoluteForPeer(std::move(from));
            to = absoluteForPeer(std::move(to));
            const auto src = store.stat(from);
            if (!src.exists) {
                status = FileOpStatus::kNotFound;
            } else if (store.stat(to).exists) {
                // 不覆盖：改名撞上已有文件时覆盖等于"悄悄删掉别人的东西"，
                // 而且不可恢复。让用户自己先改名或先删。
                status = FileOpStatus::kExists;
            } else if (!store.atomicReplace(from, to).ok()) {
                status = FileOpStatus::kFailed;
            }
        }

        answer.type = FrameType::kAnswerRename;
        answer.body.clear();
        (void)core::session::encodeAnswerFileOp(status, answer.body);
    }

    void serveCreateDir(const Frame& req, Frame& answer)
    {
        using core::session::FileOpStatus;
        FileOpStatus status = FileOpStatus::kOk;
        std::string path;
        std::string err;

        if (!core::session::decodeAskPath(req.body, path, err)) {
            status = FileOpStatus::kRejected;
        } else {
            path = absoluteForPeer(std::move(path));
            const auto st = store.stat(path);
            if (st.exists) {
                // 已经存在（目录或同名文件）：报 kExists 而不是"mkdirs 成功"。
                // 两种情形给的下一步动作相同（换个名字），所以统一一个状态。
                status = FileOpStatus::kExists;
            } else if (!store.mkdirs(path).ok()) {
                status = FileOpStatus::kFailed;
            }
        }

        answer.type = FrameType::kAnswerCreateDir;
        answer.body.clear();
        (void)core::session::encodeAnswerFileOp(status, answer.body);
    }

    void serveDelete(const Frame& req, Frame& answer)
    {
        using core::session::FileOpStatus;
        FileOpStatus status = FileOpStatus::kOk;
        std::vector<std::string> paths;
        std::vector<std::string> failed;
        std::string err;

        if (!core::session::decodeAskDelete(req.body, paths, err)) {
            status = FileOpStatus::kRejected;
        } else {
            for (const auto& raw : paths) {
                const auto p = absoluteForPeer(raw);
                std::string why;
                if (!removeRecursive(p, why)) {
                    failed.push_back(p);
                }
            }
            status = failed.empty() ? FileOpStatus::kOk : FileOpStatus::kDenied;
        }

        answer.type = FrameType::kAnswerDelete;
        answer.body.clear();
        (void)core::session::encodeAnswerDelete(status, failed, answer.body);
    }

    void serveUnarchive(const Frame& req, Frame& answer)
    {
        using core::session::FileOpStatus;
        FileOpStatus status = FileOpStatus::kOk;
        std::string dir;
        std::string path;
        std::string err;

        if (!core::session::decodeAskPath(req.body, path, err)) {
            status = FileOpStatus::kRejected;
        } else {
            path = absoluteForPeer(std::move(path));
            const auto st = store.stat(path);
            if (!st.exists || st.isDir) {
                status = FileOpStatus::kNotFound;
            } else {
                // 与本地右键「解压缩」同一语义：解到 `<父目录>/<包名去后缀>/`，
                // 成功后删包。intoSubdir=true：本操作解的是对端自己的包，
                // 形状可能是"有顶层目录"也可能是"没有"，多套一层比散在父目录
                // 安全（散开会把内容混进用户自己的文件里）。
                dir = Client::unpackArchive(path, true);
                if (dir.empty()) {
                    status = FileOpStatus::kFailed;
                } else {
                    // 统一成 `/` 分隔：std::filesystem 在 Windows 上拼出来是
                    // 反斜杠，界面直接显示会与其它远端路径不一致。
                    dir = core::session::normalizePathText(dir);
                }
            }
        }

        answer.type = FrameType::kAnswerUnarchive;
        answer.body.clear();
        (void)core::session::encodeAnswerUnarchive(status, dir, answer.body);
    }

    // 把对端发来的路径还原成本机可用的绝对路径。
    //
    // 两件事：规范化（反斜杠 / 重复分隔符 / 点段），以及"相对路径按接收
    // 目录解析"。后者与发送落点是同一条规则（DESIGN.md §3.8）——对方说
    // "落到你的接收目录"，实际是 `<接收目录>/name`，不是本进程的工作目录。
    std::string absoluteForPeer(std::string p) const
    {
        p = core::session::normalizePathText(p);
        if (!p.empty() && !core::session::isAbsolutePathText(p) && !targetPath.empty()) {
            p = core::session::joinPathText(targetPath, p);
        }
        return p;
    }

    // 流式计算 SHA-256。用 IFileStore 的读流而不是 std::ifstream：路径编码
    // （Windows 宽字符）与"打开失败给 Status"都已在实现里处理过。
    std::vector<char> sha256Of(const std::string& path, std::string& err) const
    {
        Status st;
        auto stream = store.openRead(path, st);
        if (!stream) {
            err = st.message;
            return {};
        }
        crypto::Sha256 hash;
        std::vector<char> buf(1U << 20);
        while (true) {
            const auto n = stream->read(buf.data(), buf.size(), st);
            if (!st.ok()) {
                err = st.message;
                return {};
            }
            if (n == 0) {
                break;   // 正常 EOF
            }
            hash.update(buf.data(), n);
        }
        const auto digest = hash.digest();
        return std::vector<char>(digest.begin(), digest.end());
    }

    // 递归删除。IFileStore::remove 是单层的（只删空目录或单文件），而
    // "删一个目录"要连内容一起走。不用 std::filesystem::remove_all：它在
    // 遇到占用时会中途停下，且不告诉调用方**哪一项**没删掉——界面要的
    // 正是这份清单（v1 的失败清单语义）。
    bool removeRecursive(const std::string& path, std::string& err)
    {
        const auto st = store.stat(path);
        if (!st.exists) {
            err = "路径不存在";
            return false;
        }
        if (!st.isDir) {
            const auto r = store.remove(path);
            if (!r.ok()) {
                err = r.message;
                return false;
            }
            return true;
        }
        std::vector<filestore::DirEntry> entries;
        if (const auto r = store.list(path, false, entries); !r.ok()) {
            err = r.message;
            return false;
        }
        for (const auto& e : entries) {
            if (!removeRecursive(store.join(path, e.name), err)) {
                return false;
            }
        }
        const auto r = store.remove(path);   // 现在空了，能删掉
        if (!r.ok()) {
            err = r.message;
            return false;
        }
        return true;
    }

    // 递归收集要打包的条目。DirEntry.name 只有文件名，所以这里自己拼相对
    // 路径——store.list 的 recursive 模式不返回层级信息。
    //
    // 返回 false 表示超限（文件数或字节数），由调用方转成"拒绝"。
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
    // **不能放进接收目录**：那会让对端在列举里看到这个临时包，也污染用户目录。
    std::string packDirectory(const std::string& absDir, std::string& err)
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
        return path.string();
    }

    // ---------------------------------------------------------------------
    // 请求侧
    // ---------------------------------------------------------------------

    void requestPeerList()
    {
        if (control == nullptr || !control->ready()) {
            return;
        }
        std::weak_ptr<char> token = alive;
        const bool asked =
            control->ask(FrameType::kAskClientList, std::string(), {}, [this, token](const ControlLink::Answer& a) {
                if (token.expired()) {
                    return;
                }
                if (!a.ok) {
                    finish(false, "取在线列表失败：" + a.status.message, a.status);
                    return;
                }
                std::string err;
                std::vector<core::session::PeerInfo> list;
                if (!core::session::decodeClientList(a.body, list, err)) {
                    finish(false, "在线列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                    return;
                }
                peers = std::move(list);

                if (action == Action::kList) {
                    // list 操作的契约输出：拼文本、结束。
                    std::string text = "在线对端 " + std::to_string(peers.size()) + " 个：";
                    for (const auto& p : peers) {
                        text += "\n  " + p.id + "  " + p.address;
                    }
                    finish(true, std::move(text));
                    return;
                }

                // 常驻会话上的刷新（refreshPeers）：报结构化列表
                // 就走，**不结束操作**——这条连接还要继续收文件。
                if (config.onPeerList) {
                    std::vector<PeerEntry> entries;
                    entries.reserve(peers.size());
                    for (const auto& p : peers) {
                        entries.push_back(PeerEntry{p.id, p.address});
                    }
                    config.onPeerList(entries);
                }
            });
        if (!asked) {
            finish(false, "在线列表请求没发出去（控制连接不可用）",
                   Status::Fail(ErrorCode::kCancelled, "control link not ready"));
        }
    }

    void requestFileList()
    {
        if (control == nullptr || !control->ready()) {
            finish(false, "控制连接不可用", Status::Fail(ErrorCode::kCancelled, "control link not ready"));
            return;
        }
        std::weak_ptr<char> token = alive;
        const bool asked =
            control->ask(FrameType::kAskFileList, browsePeerId, core::session::encodeAskFileList(browseRelPath),
                         [this, token](const ControlLink::Answer& a) {
                             if (token.expired()) {
                                 return;
                             }
                             if (!a.ok) {
                                 finish(false, "对端没有响应（可能已离线）", a.status);
                                 return;
                             }
                             core::session::FileListStatus status = core::session::FileListStatus::kOk;
                             std::vector<core::session::RemoteEntry> entries;
                             bool roots = false;
                             std::string err;
                             if (!core::session::decodeAnswerFileList(a.body, status, roots, entries, err)) {
                                 finish(false, "远端列表解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                                 return;
                             }
                             listStatus = status;
                             rootListing = roots;
                             remoteEntries = std::move(entries);

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
                         });
        if (!asked) {
            finish(false, "远端列举请求没发出去", Status::Fail(ErrorCode::kCancelled, "control link not ready"));
        }
    }

    // `op` 子命令：一条连接只做一件事，拿到应答就结束。
    void runFileOpOnce()
    {
        const bool started = startFileOp(config.fileOp, [this](const Outcome& o) { finish(o.ok, o.summary, o.status); });
        if (!started) {
            finish(false, "远端操作请求不合法（路径为空、条数超限）",
                   Status::Fail(ErrorCode::kProtocol, "file op request rejected"));
        }
    }

    // 发一次远端文件操作。**不再与其他请求互斥**——上一版靠"下一个应答
    // 就是我的"配对，所以列举/元信息/取回/操作四者只能挂一个；请求号配对
    // 之后这条限制不存在了。
    bool startFileOp(FileOpRequest req, std::function<void(const Outcome&)> done)
    {
        if (control == nullptr || !control->ready()) {
            return false;
        }

        FrameType askType = FrameType::kAskSha256;
        std::vector<char> body;
        switch (req.kind) {
        case FileOpKind::kSha256:
            askType = FrameType::kAskSha256;
            body = core::session::encodeAskPath(req.path);
            break;
        case FileOpKind::kCreateDir:
            askType = FrameType::kAskCreateDir;
            body = core::session::encodeAskPath(req.path);
            break;
        case FileOpKind::kUnarchive:
            askType = FrameType::kAskUnarchive;
            body = core::session::encodeAskPath(req.path);
            break;
        case FileOpKind::kRename:
            askType = FrameType::kAskRename;
            body = core::session::encodeAskRename(req.path, req.newPath);
            break;
        case FileOpKind::kDelete:
            askType = FrameType::kAskDelete;
            body = core::session::encodeAskDelete(req.paths);
            break;
        }
        if (body.empty()) {
            return false;   // 参数不合法（空路径/超长/条数超限）
        }

        opKind = req.kind;
        opSha256Hex.clear();
        opExtractedDir.clear();
        opFailedPaths.clear();

        // 解压要给足时间（对端在解一个大包，几十秒很正常），其余用默认 10 秒。
        const auto timeout = (req.kind == FileOpKind::kUnarchive) ? kSlowAskTimeout : std::chrono::seconds(0);

        std::weak_ptr<char> token = alive;
        // opKind / 结果字段是**共享成员**：两个远端操作同时在飞时会互相覆盖。
        // 所以把这一次的 kind 与结果都按值捕获，不依赖成员。
        const FileOpKind kind = req.kind;
        return control->ask(
            askType, req.peerId, std::move(body),
            [this, token, kind, done = std::move(done)](const ControlLink::Answer& a) {
                if (token.expired()) {
                    return;
                }
                Outcome o;
                if (!a.ok) {
                    o.ok = false;
                    o.summary = "对端没有响应（可能已离线）";
                    o.status = a.status;
                    done(o);
                    return;
                }
                parseFileOpAnswer(kind, a.body, o);
                done(o);
            },
            timeout);
    }

    void parseFileOpAnswer(FileOpKind kind, const std::vector<char>& body, Outcome& o)
    {
        using core::session::FileOpStatus;
        std::string err;

        if (kind == FileOpKind::kSha256) {
            FileOpStatus status = FileOpStatus::kFailed;
            std::vector<char> digest;
            if (!core::session::decodeAnswerSha256(body, status, digest, err)) {
                o.summary = "SHA-256 应答解码失败：" + err;
                o.status = Status::Fail(ErrorCode::kProtocol, err);
                return;
            }
            if (status == FileOpStatus::kOk && digest.size() == 32) {
                crypto::Sha256::Digest d{};
                for (std::size_t i = 0; i < d.size(); ++i) {
                    d[i] = static_cast<std::uint8_t>(digest[i]);
                }
                opSha256Hex = crypto::Sha256::hex(d);
            }
            fillOpOutcome(status, "SHA-256", o);
            return;
        }

        if (kind == FileOpKind::kDelete) {
            FileOpStatus status = FileOpStatus::kFailed;
            std::vector<std::string> failed;
            if (!core::session::decodeAnswerDelete(body, status, failed, err)) {
                o.summary = "删除应答解码失败：" + err;
                o.status = Status::Fail(ErrorCode::kProtocol, err);
                return;
            }
            opFailedPaths = std::move(failed);
            if (status == FileOpStatus::kOk) {
                o.ok = true;
                o.summary = "删除完成";
                return;
            }
            // 部分失败是常态（目录里有个文件被别人占着）。结论里带上条数，
            // 界面再逐条列出来——"删失败了"这种话没法行动。
            o.ok = false;
            o.summary = "有 " + std::to_string(opFailedPaths.size()) + " 项没删掉";
            o.status = Status::Fail(ErrorCode::kIoError, "delete partially failed");
            o.failedPaths = opFailedPaths;
            return;
        }

        if (kind == FileOpKind::kUnarchive) {
            FileOpStatus status = FileOpStatus::kFailed;
            std::string dir;
            if (!core::session::decodeAnswerUnarchive(body, status, dir, err)) {
                o.summary = "解压应答解码失败：" + err;
                o.status = Status::Fail(ErrorCode::kProtocol, err);
                return;
            }
            opExtractedDir = std::move(dir);
            fillOpOutcome(status, "解压缩", o);
            return;
        }

        // 重命名 / 新建目录：只带状态。
        FileOpStatus status = FileOpStatus::kFailed;
        if (!core::session::decodeAnswerFileOp(body, status, err)) {
            o.summary = "应答解码失败：" + err;
            o.status = Status::Fail(ErrorCode::kProtocol, err);
            return;
        }
        fillOpOutcome(status, kind == FileOpKind::kRename ? "重命名" : "新建文件夹", o);
    }

    void fillOpOutcome(core::session::FileOpStatus status, const std::string& what, Outcome& o)
    {
        const bool ok = (status == core::session::FileOpStatus::kOk);
        o.ok = ok;
        o.summary = what + "：" + core::session::fileOpStatusText(status);
        if (!ok) {
            o.status = Status::Fail(ErrorCode::kIoError, core::session::fileOpStatusText(status));
        }
        o.sha256Hex = opSha256Hex;
        o.extractedDir = opExtractedDir;
        o.failedPaths = opFailedPaths;
    }

    // ---------------------------------------------------------------------
    // 发送
    // ---------------------------------------------------------------------

    void startSend(const std::string& source, const std::string& remoteName, const std::string& peerDataId,
                   std::function<void(const Outcome&)> done = {}, std::function<void(const ProgressReport&)> progress = {})
    {
        if (finished || data == nullptr || !data->ready()) {
            if (done) {
                Outcome o;
                o.ok = false;
                o.summary = "数据连接不可用";
                o.status = Status::Fail(ErrorCode::kIoError, "data link not ready");
                done(o);
            }
            return;
        }

        // 落点名由这里定，不能交给下层：remoteName 为空表示"用源文件名"，
        // 而 TransferPeer 在那种情况下会拿**完整源路径**当落点名，对端的
        // resolveWithin 直接判越界（或落成 `<接收目录>/D:/...` 这种怪路径）。
        const std::string name = remoteName.empty() ? fileNameOf(source) : remoteName;

        const auto jobId = data->sendFile(source, name, peerDataId);
        if (jobId == 0) {
            if (done) {
                Outcome o;
                o.ok = false;
                o.summary = "发送失败：源文件打不开，或同一文件已在发送中";
                o.status = Status::Fail(ErrorCode::kIoError, "sendFile rejected");
                done(o);
            }
            return;
        }
        if (done || progress) {
            SendEntry e;
            e.done = std::move(done);
            e.progress = std::move(progress);
            sends.emplace(jobId, std::move(e));
        }
    }

    // 一路传输的结论。发送与接收共用这个入口（job 的 sending 标志区分）。
    void onJobFinished(const TransferLink::JobInfo& job, const transfer::TransferResult& r)
    {
        // 这一路结束了。之后可能是"等下一个文件"（常驻接收），那段沉默
        // 不该被空闲看门狗当成故障。
        transferStarted = false;

        // 计数必须在这里取：job 一旦从 TransferLink 里摘掉就读不到了。
        // 它们进不了 Outcome 太可惜——诊断丢包（重传了几块）全靠这几个数。
        lastStats.chunksSent = job.chunksSent;
        lastStats.chunksReceived = job.chunksReceived;
        lastStats.chunksRetransmitted = job.chunksRetransmitted;
        lastStats.duplicateChunks = job.duplicateChunks;
        lastStats.acksSent = job.acksSent;
        lastStats.acksReceived = job.acksReceived;
        if (job.sending) {
            std::function<void(const Outcome&)> done;
            if (const auto it = sends.find(job.id); it != sends.end()) {
                done = std::move(it->second.done);
                sends.erase(it);
            }
            Outcome o;
            o.ok = (r.outcome == transfer::TransferOutcome::kCompleted);
            switch (r.outcome) {
            case transfer::TransferOutcome::kCompleted:
                o.summary = "发送完成";
                break;
            case transfer::TransferOutcome::kPeerCancelled:
                o.summary = "对端取消了传输";
                o.status = r.status;
                break;
            case transfer::TransferOutcome::kLocalCancelled:
                o.summary = "已取消";
                o.status = r.status;
                break;
            case transfer::TransferOutcome::kPeerGone:
                o.summary = "对端已断开（传输未完成）";
                o.status = r.status;
                o.connectionLost = true;
                break;
            case transfer::TransferOutcome::kFailed:
            default:
                o.summary = "传输失败：" + r.status.message;
                o.status = r.status;
                break;
            }

            if (done) {
                done(o);
                return;
            }
            // 没有等待者（对端来要文件时的推包）：CLI 的 send 动作要收尾，
            // 常驻接收会话则继续。
            if (action == Action::kSend && !finished) {
                finish(o.ok, o.summary, o.status, o.connectionLost);
            }
            return;
        }

        onReceivedFile(r);
    }

    void onReceivedFile(const transfer::TransferResult& r)
    {
        if (r.outcome == transfer::TransferOutcome::kCompleted) {
            // 最终进度报一次满的。否则最后一行停在"99%"，用户会怀疑是不是
            // 没走完——虽然结论已经说了成功。
            if (onProgress) {
                ProgressReport rep;
                rep.bytesDone = lastBytes;
                rep.bytesTotal = lastBytes;
                rep.bytesPerSecond = lastSpeed;
                onProgress(rep);
            }

            std::string reported = r.targetPath;
            if (config.unpackArchives) {
                const auto dir = unpackArchive(r.targetPath);
                if (!dir.empty()) {
                    reported = dir;
                }
            }

            if (config.keepReceiving) {
                // 常驻接收：**不结束**。报完成回调后继续等下一个请求——
                // 服务端分配的 id 因此保持不变（见 ClientConfig::keepReceiving）。
                if (config.onFileReceived) {
                    config.onFileReceived(reported);
                }
                return;
            }
            finish(true, "接收完成：" + reported);
            return;
        }

        switch (r.outcome) {
        case transfer::TransferOutcome::kPeerCancelled:
        case transfer::TransferOutcome::kFailed:
            if (config.keepReceiving) {
                // 常驻接收下的**局部失败**：这一次没收到，但到中继的连接还
                // 活着（目标已存在、写盘失败、根校验不过、对端中途取消）。
                // 不能 finish——finish 意味着断开重连，本机 id 会变，发送方
                // 表格里的 id 立刻过期。正确动作：报失败，继续等下一个。
                if (config.onReceiveFailed) {
                    config.onReceiveFailed(r.outcome == transfer::TransferOutcome::kPeerCancelled
                                               ? "对端取消了传输：" + r.status.message
                                               : "传输失败：" + r.status.message);
                }
                return;
            }
            finish(false,
                   r.outcome == transfer::TransferOutcome::kPeerCancelled ? "对端取消了传输" : "传输失败：" + r.status.message,
                   r.status);
            return;

        case transfer::TransferOutcome::kLocalCancelled:
            finish(false, "已取消", r.status);
            return;

        case transfer::TransferOutcome::kPeerGone:
            // kill 测试里必然走到的分支：发送端进程消失，接收端在空闲超时后
            // 得到这个结论。目标文件是否完好由调用方断言，这里的措辞不该
            // 暗示"文件坏了"。
            finish(false, "对端已断开（传输未完成）", r.status, true);
            return;

        default:
            finish(false, "传输失败：" + r.status.message, r.status);
            return;
        }
    }

    // ---------------------------------------------------------------------
    // 进度
    // ---------------------------------------------------------------------

    void reportProgress(const TransferLink::JobInfo& job)
    {
        if (job.bytesTotal > 0 || job.bytesDone > 0) {
            // 传输真的开始了：从现在起"没有进展"就是故障，不再豁免。
            transferStarted = true;
        }

        const auto now = std::chrono::steady_clock::now();

        // 发送中的一路：这条进度只属于它，直接交给它的回调。
        if (const auto it = sends.find(job.id); it != sends.end()) {
            auto& e = it->second;
            const auto dt = std::chrono::duration<double>(now - e.lastAt).count();
            if (dt > 0.0 && job.bytesDone >= e.lastBytes) {
                e.speed = static_cast<double>(job.bytesDone - e.lastBytes) / dt;
            }
            e.lastBytes = job.bytesDone;
            e.lastAt = now;
            lastBytes = job.bytesDone;   // 空闲看门狗也用这个
            if (e.progress) {
                ProgressReport r;
                r.bytesDone = job.bytesDone;
                r.bytesTotal = job.bytesTotal;
                r.bytesPerSecond = e.speed;
                e.progress(r);
            }
            return;
        }

        // 接收（唯一一路）：走全局回调，与旧行为一致。
        if (!onProgress) {
            return;
        }
        const auto dt = std::chrono::duration<double>(now - lastReportAt).count();
        if (dt > 0.0 && job.bytesDone >= lastBytes) {
            lastSpeed = static_cast<double>(job.bytesDone - lastBytes) / dt;
        }
        lastBytes = job.bytesDone;
        lastReportAt = now;

        ProgressReport r;
        r.bytesDone = job.bytesDone;
        r.bytesTotal = job.bytesTotal;
        r.bytesPerSecond = lastSpeed;
        onProgress(r);
    }

    // ---------------------------------------------------------------------
    // 常驻会话上的操作（GUI）
    // ---------------------------------------------------------------------

    // 这些方法在上一版里之所以叫 "OnSession"、之所以要判"有没有常驻会话"，
    // 是因为新开连接会顶掉常驻接收会话、把本机 id 换掉。双连接模型下控制
    // 连接本来就是常驻的，"新开一条连接"这件事不再存在——所以这里只剩
    // "控制连接是否可用"一个前提。名字保持不变，免得调用方一起改。
    bool canOperate() const noexcept
    {
        return !finished && control != nullptr && control->ready();
    }

    // 取回：先把数据连接与协商做完，再发请求。
    //
    // 为什么不能直接发请求：对端收到 §11.9 的请求后要**把文件发给我们的
    // 数据连接**，而它得先知道那个连接叫什么。协商就是在交换这个名字。
    void maybeStartPull()
    {
        if (pullStarted || config.pullFrom.empty()) {
            return;
        }
        pullStarted = true;

        const bool archive = config.pullAsArchive;
        ensureDataLink([this, archive](bool ok) {
            if (!ok) {
                finish(false, "无法建立数据连接", Status::Fail(ErrorCode::kIoError, "data link failed"));
                return;
            }
            negotiateDataId(config.pullFrom, [this, archive](bool ok2, std::string) {
                if (!ok2) {
                    finish(false, "对端的数据连接不可用（对方可能正在关闭或未升级协议）",
                           Status::Fail(ErrorCode::kTimeout, "data channel negotiation failed"));
                    return;
                }
                doPull(archive);
            });
        });
    }

    void doPull(bool archive)
    {
        std::weak_ptr<char> token = alive;
        const bool asked = control->ask(
            archive ? FrameType::kAskArchivePull : FrameType::kAskFilePull, config.pullFrom,
            core::session::encodeAskFileList(config.pullPath),
            [this, token, archive](const ControlLink::Answer& a) {
                if (token.expired()) {
                    return;
                }
                if (!a.ok) {
                    finish(false, "取回请求没有响应（对端可能已离线）", a.status);
                    return;
                }
                core::session::FilePullStatus status = core::session::FilePullStatus::kRejected;
                std::string err;
                if (!core::session::decodeAnswerFilePull(a.body, status, err)) {
                    finish(false, "取回应答解码失败：" + err, Status::Fail(ErrorCode::kProtocol, err));
                    return;
                }
                // 契约输出：e2e 靠这一行区分"对方答应了但文件慢"
                // 和"对方根本没答应"。格式固定，不做本地化。
                std::printf("PULL %s\n", (status == core::session::FilePullStatus::kAccepted) ? "accepted" : "failed");
                std::fflush(stdout);

                switch (status) {
                case core::session::FilePullStatus::kAccepted:
                    // 只是"对方接受了"。文件随后由数据连接收下，
                    // 那一步的结论走正常的接收路径。
                    return;
                case core::session::FilePullStatus::kNotFound:
                    finish(false, archive ? "对端没有这个目录（或它不是目录）" : "对端没有这个文件（或它是目录）",
                           Status::Fail(ErrorCode::kNotFound, "remote path not found"));
                    return;
                case core::session::FilePullStatus::kRejected:
                default:
                    finish(false, "对端拒绝取回（未开启共享、路径越界或正忙）",
                           Status::Fail(ErrorCode::kPermissionDenied, "remote pull rejected"));
                    return;
                }
            },
            archive ? kSlowAskTimeout : std::chrono::seconds(0));
        if (!asked) {
            finish(false, "取回请求没发出去", Status::Fail(ErrorCode::kCancelled, "control link not ready"));
        }
    }

    // 在控制连接上列举对端的一个目录。结论只回调一次，操作不结束。
    void browseNow(std::string peerId, std::string relPath, std::function<void(const Outcome&)> done)
    {
        std::weak_ptr<char> token = alive;
        const bool asked = control->ask(
            FrameType::kAskFileList, std::move(peerId), core::session::encodeAskFileList(std::move(relPath)),
            [token, done = std::move(done)](const ControlLink::Answer& a) {
                if (token.expired()) {
                    return;
                }
                Outcome o;
                if (!a.ok) {
                    o.ok = false;
                    o.summary = "对端没有响应（可能已离线）";
                    o.status = a.status;
                    done(o);
                    return;
                }
                core::session::FileListStatus status = core::session::FileListStatus::kOk;
                std::vector<core::session::RemoteEntry> entries;
                bool roots = false;
                std::string err;
                if (!core::session::decodeAnswerFileList(a.body, status, roots, entries, err)) {
                    o.ok = false;
                    o.summary = "远端列表解码失败：" + err;
                    o.status = Status::Fail(ErrorCode::kProtocol, err);
                    done(o);
                    return;
                }
                o.remoteEntries = std::move(entries);
                o.listStatus = status;
                o.rootListing = roots;
                o.ok = (status == core::session::FileListStatus::kOk || status == core::session::FileListStatus::kTruncated);
                o.summary = o.ok
                                ? ("远端列表 " + std::to_string(o.remoteEntries.size()) + " 项")
                                : (status == core::session::FileListStatus::kNotFound ? "对端没有这个目录"
                                                                                      : "对端拒绝列举（未开启共享，或路径越界）");
                if (!o.ok) {
                    o.status = Status::Fail(status == core::session::FileListStatus::kNotFound ? ErrorCode::kNotFound
                                                                                               : ErrorCode::kPermissionDenied,
                                            "remote list failed");
                }
                done(o);
            });
        if (!asked) {
            Outcome o;
            o.ok = false;
            o.summary = "控制连接不可用";
            o.status = Status::Fail(ErrorCode::kCancelled, "control link not ready");
            done(o);
        }
    }

    // 在控制连接上问一次在线列表（GUI 的"刷新"）。结果走 onPeerList，
    // 操作不结束。
    void refreshPeers()
    {
        if (!canOperate()) {
            return;
        }
        requestPeerList();
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
    if (!impl_) {
        return;
    }
    impl_->destroying = true;
    impl_->alive.reset();

    // 只有在 **io 线程**上才走 close 链。close() 会进 TransferPeer::cancel
    // → TransferSender::cancelAll，那条路径假定自己在 io 线程上（它要碰
    // Session 的写队列和发送窗口）；在别的线程上跑就是段错误（Release 实测
    // 100% 崩，Debug 因内存布局不同恰好不崩）。
    //
    // 而**不 close 会漏连接**：Session 的读循环是 async_read 链，handler
    // 持有 shared_ptr<Session>，socket 不关就永远等下去，Session 永不析构
    // ——中继那边这条连接与它占的 id（`<ctrl>~d`）也就永不释放。下一次
    // 连上来 claim 同一个名字会被拒，表现是"数据连接被关，每次接收都报
    // session is closed"。
    //
    // 所以两头都要：io 线程上关（正常路径，靠析构或 shutdown()），
    // 非 io 线程上不碰（交给 socket 析构，进程退出的场景够用）。
    if (impl_->ctx.get_executor().running_in_this_thread()) {
        if (impl_->data) {
            impl_->data->close();
        }
        if (impl_->control) {
            impl_->control->close();
        }
    }
    impl_->finished = true;
}

void Client::shutdown()
{
    if (!impl_) {
        return;
    }
    // 与析构同一套动作，但**由调用方保证在 io 线程上**（CoreService 的重连
    // 路径就是这样用的）。先让回调失效，再关连接：关闭会同步触发 onClosed，
    // 那时上层的 token 检查必须已经失效，否则会往一个正在拆的对象里回调。
    impl_->destroying = true;
    impl_->finished = true;
    impl_->alive.reset();
    if (impl_->data) {
        impl_->data->close();
    }
    if (impl_->control) {
        impl_->control->close();
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

void Client::runFileOp(FileOpRequest req)
{
    impl_->action = Action::kFileOp;
    impl_->config.fileOp = std::move(req);
    impl_->connect();
}

void Client::refreshPeers()
{
    impl_->refreshPeers();
}

bool Client::sendOnSession(std::string sourcePath, std::string remoteName, std::string toPeerId,
                           std::function<void(const Outcome&)> done, std::function<void(const ProgressReport&)> progress)
{
    if (!impl_->canOperate()) {
        return false;
    }
    Impl* p = impl_.get();
    std::weak_ptr<char> token = impl_->alive;
    p->ensureDataLink(
        [p, token, sourcePath, remoteName, toPeerId, done = std::move(done), progress = std::move(progress)](bool ok) mutable {
            if (token.expired()) {
                return;
            }
            if (!ok) {
                if (done) {
                    Outcome o;
                    o.ok = false;
                    o.summary = "数据连接不可用";
                    o.status = Status::Fail(ErrorCode::kIoError, "data link failed");
                    done(o);
                }
                return;
            }
            p->negotiateDataId(toPeerId, [p, token, sourcePath, remoteName, done,
                                          progress = std::move(progress)](bool ok2, std::string peerDataId) mutable {
                if (token.expired()) {
                    return;
                }
                if (!ok2) {
                    if (done) {
                        Outcome o;
                        o.ok = false;
                        o.summary = "对端的数据连接不可用";
                        o.status = Status::Fail(ErrorCode::kTimeout, "data channel negotiation failed");
                        done(o);
                    }
                    return;
                }
                p->startSend(sourcePath, remoteName, peerDataId, done, std::move(progress));
            });
        });
    return true;
}

bool Client::browseOnSession(std::string peerId, std::string relPath, std::function<void(const Outcome&)> done)
{
    if (!impl_->canOperate()) {
        return false;
    }
    impl_->browseNow(std::move(peerId), std::move(relPath), std::move(done));
    return true;
}

bool Client::askPathMetaOnSession(std::string peerId, std::vector<std::string> absPaths, std::function<void(const Outcome&)> done)
{
    if (!impl_->canOperate() || absPaths.empty()) {
        return false;
    }
    auto body = core::session::encodeAskPathMeta(absPaths);
    if (body.empty()) {
        return false;
    }

    std::weak_ptr<char> token = impl_->alive;
    const auto requested = absPaths.size();
    return impl_->control->ask(FrameType::kAskPathMeta, std::move(peerId), std::move(body),
                               [token, requested, done = std::move(done)](const ControlLink::Answer& a) {
                                   if (token.expired()) {
                                       return;
                                   }
                                   Outcome o;
                                   if (!a.ok) {
                                       o.ok = false;
                                       o.summary = "对端没有响应（可能已离线）";
                                       o.status = a.status;
                                       done(o);
                                       return;
                                   }
                                   std::vector<core::session::RemoteMeta> metas;
                                   std::string err;
                                   if (!core::session::decodeAnswerPathMeta(a.body, metas, err)) {
                                       o.ok = false;
                                       o.summary = "路径元信息解码失败：" + err;
                                       o.status = Status::Fail(ErrorCode::kProtocol, err);
                                       done(o);
                                       return;
                                   }
                                   // 条数必须与请求一致：按下标对回路径，条数不对就是整体错位，
                                   // 错位的覆盖提示会让用户点到别的文件上去。
                                   if (metas.size() != requested) {
                                       o.ok = false;
                                       o.summary = "对端返回的条数与请求不一致";
                                       o.status = Status::Fail(ErrorCode::kProtocol, "path meta count mismatch");
                                       done(o);
                                       return;
                                   }
                                   o.ok = true;
                                   o.summary = "路径元信息 " + std::to_string(metas.size()) + " 项";
                                   o.remoteMetas = std::move(metas);
                                   done(o);
                               });
}

bool Client::fileOpOnSession(FileOpRequest req, std::function<void(const Outcome&)> done)
{
    if (!impl_->canOperate()) {
        return false;
    }
    return impl_->startFileOp(std::move(req), std::move(done));
}

bool Client::pullOnSession(std::string peerId, std::string relPath, bool archive, std::function<void(const Outcome&)> done)
{
    if (!impl_->canOperate()) {
        return false;
    }

    Impl* p = impl_.get();
    std::weak_ptr<char> token = impl_->alive;

    // 两步：让对端知道我们的数据连接（协商），再发取回请求。
    // done 只看"对方接不接受"，文件随后由数据连接收下、走接收回调。
    p->ensureDataLink([p, token, peerId, relPath, archive, done = std::move(done)](bool ok) {
        if (token.expired()) {
            return;
        }
        if (!ok) {
            Outcome o;
            o.ok = false;
            o.summary = "数据连接不可用";
            o.status = Status::Fail(ErrorCode::kIoError, "data link failed");
            done(o);
            return;
        }
        p->negotiateDataId(peerId, [p, token, peerId, relPath, archive, done](bool ok2, std::string) {
            if (token.expired()) {
                return;
            }
            if (!ok2) {
                Outcome o;
                o.ok = false;
                o.summary = "对端的数据连接不可用（对方可能正在关闭或未升级协议）";
                o.status = Status::Fail(ErrorCode::kTimeout, "data channel negotiation failed");
                done(o);
                return;
            }
            const bool asked = p->control->ask(
                archive ? FrameType::kAskArchivePull : FrameType::kAskFilePull, peerId, core::session::encodeAskFileList(relPath),
                [token, archive, done](const ControlLink::Answer& a) {
                    if (token.expired()) {
                        return;
                    }
                    Outcome o;
                    if (!a.ok) {
                        o.ok = false;
                        o.summary = "取回请求没有响应（对端可能已离线）";
                        o.status = a.status;
                        done(o);
                        return;
                    }
                    core::session::FilePullStatus status = core::session::FilePullStatus::kRejected;
                    std::string err;
                    if (!core::session::decodeAnswerFilePull(a.body, status, err)) {
                        o.ok = false;
                        o.summary = "取回应答解码失败：" + err;
                        o.status = Status::Fail(ErrorCode::kProtocol, err);
                        done(o);
                        return;
                    }
                    switch (status) {
                    case core::session::FilePullStatus::kAccepted:
                        o.ok = true;
                        o.summary = "对端已开始发送";
                        done(o);
                        return;
                    case core::session::FilePullStatus::kNotFound:
                        o.ok = false;
                        o.summary = archive ? "对端没有这个目录（或它不是目录）" : "对端没有这个文件（或它是目录）";
                        o.status = Status::Fail(ErrorCode::kNotFound, "remote path not found");
                        done(o);
                        return;
                    case core::session::FilePullStatus::kRejected:
                    default:
                        o.ok = false;
                        o.summary = "对端拒绝取回（未开启共享、路径越界或正忙）";
                        o.status = Status::Fail(ErrorCode::kPermissionDenied, "remote pull rejected");
                        done(o);
                        return;
                    }
                },
                archive ? kSlowAskTimeout : std::chrono::seconds(0));
            if (!asked) {
                Outcome o;
                o.ok = false;
                o.summary = "取回请求发送失败（控制连接不可用）";
                o.status = Status::Fail(ErrorCode::kCancelled, "control link not ready");
                done(o);
            }
        });
    });
    return true;
}

bool Client::setReceiveDirOnSession(std::string targetPath)
{
    if (impl_->action != Action::kReceive || impl_->finished) {
        return false;
    }
    // 只改内存里的落点，**不重连**（重连会换 id，那正是常驻要避免的事）。
    impl_->targetPath = std::move(targetPath);
    if (impl_->data) {
        impl_->data->setReceiveDir(impl_->targetPath);
    }
    return true;
}

void Client::cancelSend()
{
    // 先拷出 id 再取消：cancel 会同步触发 onFinished，而那个回调会 erase
    // sends 里的项——直接遍历 map 会踩失效的迭代器。
    std::vector<std::uint64_t> ids;
    ids.reserve(impl_->sends.size());
    for (const auto& [id, done] : impl_->sends) {
        (void)done;
        ids.push_back(id);
    }
    for (const auto id : ids) {
        if (impl_->data) {
            (void)impl_->data->cancel(id, "cancelled by user");
        }
    }
}

void Client::abort()
{
    if (!impl_ || impl_->finished) {
        return;
    }
    impl_->finished = true;
    impl_->watchdog.cancel();
    impl_->sends.clear();
    if (impl_->data) {
        impl_->data->close();
    }
    if (impl_->control) {
        impl_->control->close();
    }
    if (impl_->onDone) {
        Outcome o;
        o.ok = false;
        o.summary = "中止";
        o.status = Status::Fail(ErrorCode::kCancelled, "aborted");
        auto cb = std::move(impl_->onDone);
        impl_->onDone = nullptr;
        cb(o);
    }
}

const std::string& Client::assignedId() const noexcept
{
    return impl_->assignedId;
}

// ---------------------------------------------------------------------------
// 本地归档（不联网）
// ---------------------------------------------------------------------------

bool Client::createArchive(const std::vector<std::string>& sources, const std::string& archivePath, std::string& err)
{
    filestore::StdFileStore store;
    std::vector<filestore::ArchiveItem> items;
    for (const auto& src : sources) {
        std::error_code ec;
        const std::filesystem::path p(src);
        const auto name = p.filename().string();
        if (name.empty()) {
            continue;
        }

        // 目录要展开成条目：archiver 只认 (sourcePath, entryName) 列表。
        // 包内路径相对**各源的父目录**，所以包里有顶层目录——与"取回目录"
        // 那种无顶层的包形状相反，解压时要配 unpackArchive(path, false)。
        if (std::filesystem::is_directory(p, ec)) {
            filestore::ArchiveItem dirItem;
            dirItem.sourcePath = src;
            dirItem.entryName = name + "/";
            items.push_back(std::move(dirItem));

            std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& absDir,
                                                                                   const std::string& prefix) {
                std::vector<filestore::DirEntry> sub;
                if (!store.list(absDir, false, sub).ok()) {
                    return;
                }
                for (const auto& e : sub) {
                    const std::string childAbs = store.join(absDir, e.name);
                    const std::string childRel = prefix + "/" + e.name;
                    if (e.isDir) {
                        filestore::ArchiveItem d;
                        d.sourcePath = childAbs;
                        d.entryName = childRel + "/";
                        items.push_back(std::move(d));
                        walk(childAbs, childRel);
                        continue;
                    }
                    filestore::ArchiveItem f;
                    f.sourcePath = childAbs;
                    f.entryName = childRel;
                    items.push_back(std::move(f));
                }
            };
            walk(src, name);
            continue;
        }

        filestore::ArchiveItem item;
        item.sourcePath = src;
        item.entryName = name;
        items.push_back(std::move(item));
    }

    if (items.empty()) {
        err = "没有可压缩的条目";
        return false;
    }

    filestore::TarXzArchiver archiver(store);
    filestore::ArchiveOptions options;
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
        return {};
    }

    filestore::StdFileStore store;
    const std::filesystem::path pack(archivePath);
    const std::string name = pack.filename().string();
    const std::string dirName = name.substr(0, name.size() - kSuffix.size());
    const auto dest = intoSubdir ? (pack.parent_path() / dirName) : pack.parent_path();

    filestore::TarXzArchiver archiver(store);
    filestore::ArchiveOptions options;
    std::vector<std::string> extracted;
    std::vector<std::string> skipped;
    if (!archiver.extract(archivePath, dest.string(), options, extracted, skipped).ok()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::remove(archivePath, ec);   // 包已经用完，留着只会越积越多
    return dest.string();
}

}   // namespace relayer::cli
