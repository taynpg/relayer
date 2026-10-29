#include "CoreService.h"

#include <QMetaObject>
#include <QSettings>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>
#include <utility>

#include "ui/SettingsKeys.h"

namespace relayer::app {

namespace {

QString toQt(const std::string& s)
{
    return QString::fromStdString(s);   // QString 与 UTF-8 互转，中文路径无损
}

std::string fromQt(const QString& s)
{
    return s.toStdString();
}

}   // namespace

CoreService::CoreService(QObject* parent) : QObject(parent), guard_(asio::make_work_guard(ctx_))
{
}

CoreService::~CoreService()
{
    // 停线程必须先于成员析构：io 线程回调里会 emit this 的信号、会碰
    // client_，任何一个晚于析构都是 use-after-free。
    stopping_ = true;
    guard_.reset();
    ctx_.stop();
    if (ioThread_.joinable()) {
        ioThread_.join();
    }

    // 线程已停，这里不再是并发上下文，直接释放即可。
    client_.reset();

    // **把 io_context 的队列跑空**，再让它析构。
    //
    // 上面那步 client_.reset() 会连锁关闭 Session / Connection，而关闭会把
    // 取消回调排进 ctx_ 的队列（asio 的 socket 关闭总要投递一次操作完成）。
    // 队列里那些 handler 持有 socket、缓冲区、shared_ptr——它们必须**在
    // ctx_ 还活着的时候**被销毁。
    //
    // 少了这一步的后果是 Release 下的段错误（Debug 因为内存布局不同，恰好
    // 看不出来）：ctx_ 析构时销毁残留 handler，而那些 handler 引用的对象
    // 已经在清理链条上拆掉了一半。
    ctx_.restart();
    ctx_.poll();
}

void CoreService::startIoThread()
{
    if (ioThread_.joinable()) {
        return;
    }
    ioThread_ = std::thread([this] {
        // CLI 的 main 里 run_for 循环的同款写法（见 cli/main.cpp 注释）：
        // run() 在没有事件时直接返回，而 Client 结束后仍可能有排队的
        // handler 要跑完；用 run_for 轮询 + stopping_ 检查保证退出及时。
        while (!stopping_) {
            if (ctx_.stopped()) {
                ctx_.restart();
            }
            ctx_.run_for(std::chrono::milliseconds(100));
        }
    });
}

void CoreService::runOnIo(std::function<void()> fn)
{
    startIoThread();
    asio::post(ctx_, std::move(fn));
}

cli::ClientConfig CoreService::makeConfig(const ConnInfo& conn)
{
    cli::ClientConfig cfg;
    cfg.host = fromQt(conn.host);
    cfg.port = conn.port;
    cfg.password = fromQt(conn.password);
    cfg.serverAllowNoAuth = conn.noAuth;
    // 声明本机标识（PROTOCOL.md §11.7）：空则由中继分配 client-N。
    cfg.requestedId = fromQt(conn.requestedId);

    // 记下这组参数（GUI 线程），供 refreshPeers 判断会话是否对应表单地址。
    activeConnKey_ = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                     (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;

    // GUI 的自动接收必须常驻（收完不断开）：每操作一条连接 + 服务端递增
    // 分配 id，"收完就重挂"会让接收方每收一个文件就换一次 id，发送方
    // 表格里的对端 id 立刻过期——再发就发进虚空，等满 answerTimeout 才报错。
    cfg.keepReceiving = true;
    cfg.onFileReceived = [this](const std::string& targetPath) {
        // 临时落点只服务"这一次取回"，落地即失效：不还原的话，后续别人
        // 主动发来的文件会跟着落进那一行指定的本地目录。
        restoreReceiveDir();

        if (unpackNextArchive_) {
            unpackNextArchive_ = false;
            const auto dir = cli::Client::unpackArchive(targetPath);
            if (!dir.empty()) {
                // 包已删，不再发 fileReceived：上层看到两条结论（"收到包"
                // 加"已解包"）会以为收到了两个东西。
                emit archiveExtracted(true, toQt(dir), QString());
                return;
            }
            emit archiveExtracted(false, QString(), QStringLiteral("解包失败，包已保留在接收目录"));
        }
        emit fileReceived(toQt(targetPath));
    };
    cfg.onReceiveFailed = [this](const std::string& message) {
        // 失败也要还原：这一次取回没落地，落点照样该回到全局接收目录。
        // 不还原就会留下"下一个人发来的文件落到上一行目录"的暗坑。
        restoreReceiveDir();
        emit receiveFailed(toQt(message));
    };

    // 接收端覆盖策略：**总是覆盖**（v1 语义）。
    //
    // 覆盖与否的决策已经整体挪到发送方的"发送前检查"里（见
    // MainWindow::onTransTaskRun）：发送方先问对方这些路径在不在，用户在
    // 一个普通对话框里逐项回答，被跳过的文件根本不会发出来。所以接收端
    // 此刻收到的一切都是对方明确要发的，再拒绝一次只会让"我明明点了覆盖"
    // 变成一次传输失败。
    //
    // 为什么不做成"接收时弹窗询问"：那个决策点在 TransferPeer 的 io 线程
    // 上，应答帧正压在那里，弹窗等用户点击会饿死心跳（空闲 20s 发 ping、
    // 10s 未答判死），只能加一个"用户还没点就超时"的兜底——那比不弹更糟。
    cfg.overwriteExisting = true;

    cfg.onIdAssigned = [this](const std::string& id) {
        // io 线程。同一个 id 只发一次：服务端重复通知不该让界面闪两下。
        if (id == lastId_) {
            return;
        }
        lastId_ = id;
        emit assignedIdChanged(toQt(id));
    };

    // 常驻接收会话上的列表刷新回程 + 认证后自动问列表。两者配合，
    // GUI 的"连接/刷新"全程只有一条连接、一个 id（见 ClientConfig
    // 两个成员的注释）。
    cfg.onPeerList = [this](const std::vector<cli::PeerEntry>& entries) {
        QList<PeerRow> rows;
        rows.reserve(static_cast<qsizetype>(entries.size()));
        for (const auto& p : entries) {
            rows.push_back(PeerRow{toQt(p.id), toQt(p.address)});
        }
        emit peerListReceived(rows);
    };
    cfg.askListOnStart = true;

    return cfg;
}

bool CoreService::runOrAfterReceive(std::function<void()> beginOp)
{
    // 名字里的 "AfterReceive" 是历史遗留：单连接时代，任何新操作都得先把
    // 挂起的接收**推倒**（abort + 重建连接），否则新连接会顶掉它、换掉
    // 本机 id。双连接之后这件事不存在了——控制连接能并发处理请求，
    // 浏览、元信息、文件操作在传输期间照样发得出去（它们各自走 ask，
    // 应答按请求号配对），接收不必让位。
    //
    // 于是这里只剩一个闸门：**有没有一次性操作在跑**。有就拒绝，因为
    // 一次性操作用的是临时连接，两条连接同时存在会让"本机 id 是哪个"
    // 变得含糊。
    if (busy_) {
        return false;
    }
    busy_ = true;
    emit busyChanged(true);
    runOnIo(std::move(beginOp));
    return true;
}

void CoreService::finishOperation(const cli::Outcome& o, bool withPeerList)
{
    // 会话级结论：正在进行的搭车发送到此为止（Client 那边也不会再回调
    // 它们了，见 Client::Impl::finish）。计数清零，界面上"还在发"的
    // 提示随之消失。
    sendCount_.store(0, std::memory_order_release);

    // 先发列表（list 的惯例顺序：先数据后结论），再发结论与
    // busy 翻转——GUI 侧依赖"收到 finished 时 busy 已可用"来解锁按钮。
    // connectionLost 标记必须在结论**之前**落地：跨线程 queued 信号
    // 的槽执行晚于本函数返回，先置位才能保证读到的不是上一次的旧值。
    lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
    if (withPeerList && !o.peerList.empty()) {
        QList<PeerRow> rows;
        rows.reserve(static_cast<qsizetype>(o.peerList.size()));
        for (const auto& p : o.peerList) {
            rows.push_back(PeerRow{toQt(p.id), toQt(p.address)});
        }
        emit peerListReceived(rows);
    }
    emit operationFinished(o.ok, toQt(o.summary), toQt(o.status.message));

    busy_ = false;
    receivePending_ = false;
    emit busyChanged(false);
}

bool CoreService::canSendOnSession(const ConnInfo& conn) const
{
    // 不再限制"只能有一路在发"：数据连接本来就支持多路并存（sender 各自
    // 一个 sessionId）。同一对端的并发由上层串行（RelayTaskControl 的
    // 窗口填充里按 peerId 去重），因为对端的接收端一次只收一个文件。
    if (!busy_.load() || !receivePending_.load()) {
        return false;
    }
    const QString key = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                        (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;
    return key == activeConnKey_;
}

bool CoreService::sendFile(const QString& localPath, const QString& remoteName, const QString& toPeerId, const ConnInfo& conn)
{
    // 常驻会话活着：直接在**这条连接**上发。不新建连接 → 服务端不会重新
    // 分配 id；接收也不受影响（两边靠 sessionId 区分各自的帧）。这是
    // "点一次发送自己的 id 就涨一号"的根治点。
    if (canSendOnSession(conn)) {
        sendCount_.fetch_add(1, std::memory_order_acq_rel);
        runOnIo([this, path = fromQt(localPath), name = fromQt(remoteName), peer = fromQt(toPeerId)] {
            const bool started =
                client_ && client_->sendOnSession(path, name, peer, [this](const cli::Outcome& o) { onSendFinished(o); });
            if (started) {
                return;
            }
            // 会话在 GUI 判断与 post 落地之间结束了（比如中继刚断）：
            // 如实报一次结论，别让调用方等一个永远不来的回调。
            if (sendCount_.fetch_sub(1, std::memory_order_acq_rel) > 0) {
                emit sendFinished(false, QStringLiteral("当前会话不可用，请重新连接后再发"), QString());
            }
        });
        return true;
    }

    // 会话不在（还没连过 / 接收已停）：回落到一次性连接。这一次仍会换 id，
    // 但没有在线会话时别无选择——GUI 连上之后走不到这条路。
    return runOrAfterReceive([this, localPath, remoteName, toPeerId, cfg = makeConfig(conn)]() mutable {
        // 一次性连接的 id 不往界面报：那是用完即弃的身份，显示出去
        // 就成了"发送一次 id 变一次"。
        cfg.onIdAssigned = nullptr;

        busy_ = true;
        receivePending_ = false;
        emit busyChanged(true);
        // 换 Client 之前先把旧的关干净。不关的后果很具体：旧的数据连接
        // 还占着 `<本机 id>~d` 这个名字，新会话 claim 同一个名字会被服务端
        // 拒绝，而"拒绝"的表现是**数据连接被关掉**——控制连接却正常，
        // 界面看起来一切正常，只是每次接收都报 "session is closed"。
        // 用户实跑时撞到的就是这个（一次失败 → 3 秒后重试 → 一直失败）。
        if (client_) {
            client_->shutdown();
        }
        client_ = std::make_shared<cli::Client>(
            ctx_, std::move(cfg),
            [this](const cli::ProgressReport& p) { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
            [this](const cli::Outcome& o) { finishSendOperation(o); });
        client_->sendFile(fromQt(localPath), fromQt(remoteName), fromQt(toPeerId));
    });
}

bool CoreService::sendFileTask(qint64 taskId, const QString& localPath, const QString& remoteName, const QString& toPeerId,
                               const ConnInfo& conn)
{
    if (taskId <= 0 || localPath.isEmpty() || toPeerId.isEmpty()) {
        return false;
    }
    (void)conn;

    // 并发发送也走**常驻会话的数据连接**。
    //
    // 上一版是"每个任务一条新连接"：那在单连接模型下是唯一能做到并发的
    // 办法，代价是每个文件两次握手，而且每路都换一个服务端 id。双连接
    // 之后数据连接本来就支持多路并存（sender 各自一个 sessionId），
    // 按任务开连接纯属浪费。
    //
    // 仍然要求"有常驻会话"：没有它就没有数据连接，回落成一次性连接会
    // 让本机 id 跳号，而批量发送最忌讳这个（对端表里的我们随时过期）。
    if (!busy_ || !receivePending_) {
        return false;
    }

    runOnIo([this, taskId, path = fromQt(localPath), name = fromQt(remoteName), peer = fromQt(toPeerId)] {
        if (!client_) {
            emit sendTaskFinished(taskId, false, QStringLiteral("没有可用的常驻会话"), QString());
            return;
        }
        const bool started = client_->sendOnSession(
            path, name, peer,
            [this, taskId](const cli::Outcome& o) {
                lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
                emit sendTaskFinished(taskId, o.ok, toQt(o.summary), toQt(o.status.message));
            },
            // 每路一份进度：全局的 progressChanged 不带任务身份，多路并发
            // 时会把 B 的进度画到 C 那一行上。
            [this, taskId](const cli::ProgressReport& p) {
                emit sendTaskProgress(taskId, p.bytesDone, p.bytesTotal, p.bytesPerSecond);
            });
        if (!started) {
            emit sendTaskFinished(taskId, false, QStringLiteral("当前会话不可用，请重新连接后再发"), QString());
        }
    });
    return true;
}

void CoreService::cancelSendTask(qint64 taskId)
{
    (void)taskId;
    // 双连接下所有在飞的发送共用一条数据连接，"只取消这一路"没有独立
    // 入口——Client 只有 cancelSend()（全部）。批量的停止本来就是全停，
    // 界面上也只有"停止"一个按钮，所以这不是妥协而是语义对齐。
    runOnIo([this] {
        if (client_) {
            client_->cancelSend();
        }
    });
}

void CoreService::finishSendOperation(const cli::Outcome& o)
{
    // 一次性连接的发送：这次结论同时也是操作结论（busy 要翻），但只发
    // sendFinished——operationFinished 的消费方（接收状态机）不该被一次
    // 发送推动。busy 翻转放在后面，与 operationFinished 的约定一致。
    lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
    emit sendFinished(o.ok, toQt(o.summary), toQt(o.status.message));
    busy_ = false;
    receivePending_ = false;
    emit busyChanged(false);
}

void CoreService::onSendFinished(const cli::Outcome& o)
{
    // 每一路都要发自己的结论。多路并发时，"计数减到 0 了才发"是错的——
    // 那会让后完成的几路永远不报。计数只用于界面上"还在发"的提示。
    sendCount_.fetch_sub(1, std::memory_order_acq_rel);
    lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
    emit sendFinished(o.ok, toQt(o.summary), toQt(o.status.message));
}

void CoreService::startReceive(const QString& targetDir, const ConnInfo& conn)
{
    if (busy_) {
        return;   // 只有 MainWindow 调用，且它有 receiveActive_ 状态机，到不了这里
    }
    busy_ = true;
    emit busyChanged(true);

    runOnIo([this, targetDir, cfg = makeConfig(conn)]() mutable {
        busy_ = true;
        receivePending_ = true;
        // 记下默认落点：行级下载会临时改落点，收完改回这里。
        defaultReceiveDir_ = fromQt(targetDir);
        pullTargetDir_.clear();
        // 换 Client 之前先把旧的关干净。不关的后果很具体：旧的数据连接
        // 还占着 `<本机 id>~d` 这个名字，新会话 claim 同一个名字会被服务端
        // 拒绝，而"拒绝"的表现是**数据连接被关掉**——控制连接却正常，
        // 界面看起来一切正常，只是每次接收都报 "session is closed"。
        // 用户实跑时撞到的就是这个（一次失败 → 3 秒后重试 → 一直失败）。
        if (client_) {
            client_->shutdown();
        }
        client_ = std::make_shared<cli::Client>(
            ctx_, std::move(cfg),
            [this](const cli::ProgressReport& p) { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
            [this](const cli::Outcome& o) { finishOperation(o, false); });
        client_->receiveFile(fromQt(targetDir));
    });
}

bool CoreService::listPeers(const ConnInfo& conn)
{
    return runOrAfterReceive([this, cfg = makeConfig(conn)]() mutable {
        // 临时连接的 id 不往界面报（同 sendFile 的道理）：刷新列表
        // 不该让"本机 id"跳动，界面上那个 id 只属于常驻接收会话。
        cfg.onIdAssigned = nullptr;

        busy_ = true;
        receivePending_ = false;
        emit busyChanged(true);
        // 不需要进度。必须是显式 lambda 而不是 {}：make_shared 是
        // 可变参转发，braced-init-list 无法在转发中推导参数类型。
        // 换 Client 之前先把旧的关干净。不关的后果很具体：旧的数据连接
        // 还占着 `<本机 id>~d` 这个名字，新会话 claim 同一个名字会被服务端
        // 拒绝，而"拒绝"的表现是**数据连接被关掉**——控制连接却正常，
        // 界面看起来一切正常，只是每次接收都报 "session is closed"。
        // 用户实跑时撞到的就是这个（一次失败 → 3 秒后重试 → 一直失败）。
        if (client_) {
            client_->shutdown();
        }
        client_ = std::make_shared<cli::Client>(
            ctx_, std::move(cfg), [](const cli::ProgressReport&) {}, [this](const cli::Outcome& o) { finishOperation(o, true); });
        client_->listPeers();
    });
}

bool CoreService::refreshPeers(const ConnInfo& conn)
{
    // 表单参数必须与当前会话一致才复用：用户改了地址/口令再点连接，
    // 意图是"换服务器"，在旧会话上问列表等于问错人。
    const QString key = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                        (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;

    // 常驻接收会话活着：直接在**那条连接**上问列表。不占通道、不翻转
    // busy、不产生 operationFinished——GUI 的接收状态机与身份显示完全
    // 无感，id 自然不会变。
    if (busy_.load() && receivePending_.load() && key == activeConnKey_) {
        runOnIo([this] {
            // post 排队后才读 client_：GUI 线程不碰这个指针（单线程
            // 契约），此刻它可能正被替换。
            if (client_) {
                client_->refreshPeers();
            }
        });
        return true;
    }

    // 会话不在（没连过、接收停了）或参数变了：回落到临时连接探测——
    // 这一次仍会消耗一个新 id，但那是"建立在线状态"的必要代价；建立
    // 之后刷新就不再换 id。
    return listPeers(conn);
}

bool CoreService::browsePeerOnSession(const QString& peerId, const QString& relPath)
{
    if (peerId.isEmpty()) {
        return false;
    }
    // 没有常驻会话就没有"搭车"的可能。这里**不**回落成新连接：新连接会
    // 顶掉接收会话并换掉本机 id（见头文件）。
    if (!busy_) {
        return false;
    }

    runOnIo([this, peer = fromQt(peerId), path = fromQt(relPath)] {
        if (!client_) {
            emit remoteBrowseFinished(false, false, QStringLiteral("没有可用的常驻会话"), {});
            return;
        }
        const bool started = client_->browseOnSession(peer, path, [this](const cli::Outcome& o) {
            QList<relayer::app::RemoteRow> rows;
            rows.reserve(static_cast<int>(o.remoteEntries.size()));
            for (const auto& e : o.remoteEntries) {
                relayer::app::RemoteRow r;
                r.name = toQt(e.name);
                r.isDir = e.isDir;
                r.size = e.size;
                r.mtime = e.mtime;
                rows.append(r);
            }
            // 从 io 线程 emit
            // 跨线程信号是安全的（QueuedConnection）。
            emit remoteBrowseFinished(o.ok, o.rootListing, toQt(o.summary), rows);
        });
        if (!started) {
            emit remoteBrowseFinished(false, false, QStringLiteral("当前不能列举（正在传输或会话不可用）"), {});
        }
    });
    return true;
}

bool CoreService::askPathMeta(const QString& peerId, const QStringList& absPaths)
{
    if (peerId.isEmpty() || absPaths.isEmpty()) {
        return false;
    }
    if (!busy_) {
        return false;   // 与列举同理：不回落成新连接
    }

    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(absPaths.size()));
    for (const auto& p : absPaths) {
        paths.push_back(fromQt(p));
    }

    runOnIo([this, peer = fromQt(peerId), paths = std::move(paths)] {
        if (!client_) {
            emit pathMetaFinished(false, QStringLiteral("没有可用的常驻会话"), {});
            return;
        }
        const bool started = client_->askPathMetaOnSession(peer, paths, [this](const cli::Outcome& o) {
            QList<relayer::app::RemoteMetaRow> rows;
            rows.reserve(static_cast<qsizetype>(o.remoteMetas.size()));
            for (const auto& m : o.remoteMetas) {
                relayer::app::RemoteMetaRow r;
                r.exists = m.exists;
                r.isDir = m.isDir;
                r.size = m.size;
                r.mtimeMs = m.mtime > 0 ? m.mtime * 1000 : 0;
                r.fingerprint = toQt(m.fingerprint);
                rows.append(r);
            }
            // 从 io 线程 emit 跨线程信号是安全的（QueuedConnection）。
            emit pathMetaFinished(o.ok, toQt(o.summary), rows);
        });
        if (!started) {
            emit pathMetaFinished(false, QStringLiteral("当前不能查询（正在传输或会话不可用）"), {});
        }
    });
    return true;
}

bool CoreService::runRemoteOp(cli::FileOpRequest req, const QString& kind)
{
    // 与列举、元信息同理：没有常驻会话就不做，**不**回落成新连接——
    // 新连接会顶掉接收会话并换掉本机 id。
    if (!busy_) {
        return false;
    }
    runOnIo([this, req = std::move(req), kind]() {
        if (!client_) {
            emit remoteFileOpFinished(kind, false, QStringLiteral("没有可用的常驻会话"), {}, {});
            return;
        }
        const bool started = client_->fileOpOnSession(req, [this, kind](const cli::Outcome& o) {
            // result 的含义按 kind 决定，这里不做解释——调用方知道自己在等什么。
            QString result;
            if (!o.sha256Hex.empty()) {
                result = toQt(o.sha256Hex);
            } else if (!o.extractedDir.empty()) {
                result = toQt(o.extractedDir);
            } else if (!o.failedPaths.empty()) {
                QStringList failed;
                failed.reserve(static_cast<qsizetype>(o.failedPaths.size()));
                for (const auto& p : o.failedPaths) {
                    failed << toQt(p);
                }
                result = failed.join(QLatin1Char('\n'));
            }
            emit remoteFileOpFinished(kind, o.ok, toQt(o.summary), toQt(o.status.message), result);
        });
        if (!started) {
            emit remoteFileOpFinished(kind, false, QStringLiteral("当前不能操作（正在传输或会话不可用）"), {}, {});
        }
    });
    return true;
}

bool CoreService::remoteOpSha256(const QString& peerId, const QString& absPath)
{
    if (peerId.isEmpty() || absPath.isEmpty()) {
        return false;
    }
    cli::FileOpRequest req;
    req.kind = cli::FileOpKind::kSha256;
    req.peerId = fromQt(peerId);
    req.path = fromQt(absPath);
    return runRemoteOp(std::move(req), QStringLiteral("sha256"));
}

bool CoreService::remoteOpRename(const QString& peerId, const QString& absPath, const QString& newName)
{
    if (peerId.isEmpty() || absPath.isEmpty() || newName.isEmpty()) {
        return false;
    }
    // 协议里的目标也是**完整路径**，所以新名字要拼回同一个目录。用 '/' 找
    // 父目录（远端路径在本工具内部一律 `/`，见 DESIGN.md §3.8）。
    const qsizetype slash = absPath.lastIndexOf(QLatin1Char('/'));
    const QString target = (slash < 0) ? newName : absPath.left(slash + 1) + newName;

    cli::FileOpRequest req;
    req.kind = cli::FileOpKind::kRename;
    req.peerId = fromQt(peerId);
    req.path = fromQt(absPath);
    req.newPath = fromQt(target);
    return runRemoteOp(std::move(req), QStringLiteral("rename"));
}

bool CoreService::remoteOpCreateDir(const QString& peerId, const QString& absPath)
{
    if (peerId.isEmpty() || absPath.isEmpty()) {
        return false;
    }
    cli::FileOpRequest req;
    req.kind = cli::FileOpKind::kCreateDir;
    req.peerId = fromQt(peerId);
    req.path = fromQt(absPath);
    return runRemoteOp(std::move(req), QStringLiteral("mkdir"));
}

bool CoreService::remoteOpDelete(const QString& peerId, const QStringList& absPaths)
{
    if (peerId.isEmpty() || absPaths.isEmpty()) {
        return false;
    }
    cli::FileOpRequest req;
    req.kind = cli::FileOpKind::kDelete;
    req.peerId = fromQt(peerId);
    req.paths.reserve(static_cast<std::size_t>(absPaths.size()));
    for (const auto& p : absPaths) {
        req.paths.push_back(fromQt(p));
    }
    return runRemoteOp(std::move(req), QStringLiteral("delete"));
}

bool CoreService::remoteOpUnarchive(const QString& peerId, const QString& absPath)
{
    if (peerId.isEmpty() || absPath.isEmpty()) {
        return false;
    }
    cli::FileOpRequest req;
    req.kind = cli::FileOpKind::kUnarchive;
    req.peerId = fromQt(peerId);
    req.path = fromQt(absPath);
    return runRemoteOp(std::move(req), QStringLiteral("unpack"));
}

bool CoreService::pullFileFromPeer(const QString& peerId, const QString& relPath, bool asArchive, const QString& intoDir)
{
    if (peerId.isEmpty() || relPath.isEmpty()) {
        return false;
    }
    // 与列举同理：没有常驻会话就不做，**不**回落成新连接（新连接会顶掉
    // 接收会话并换掉本机 id，而且回程的文件还需要这个会话去收）。
    if (!busy_) {
        return false;
    }

    runOnIo([this, peer = fromQt(peerId), path = fromQt(relPath), asArchive, into = fromQt(intoDir)] {
        if (!client_) {
            emit remotePullFinished(false, QStringLiteral("没有可用的常驻会话"));
            return;
        }
        // 先改落点再发请求：应答可能在 pullOnSession 返回前就到（同一
        // 个 io 线程上不会，但把顺序写死能省掉一次"为什么落到别处"）。
        if (!into.empty()) {
            if (!client_->setReceiveDirOnSession(into)) {
                emit remotePullFinished(false, QStringLiteral("当前无法指定落点（会话不可用）"));
                return;
            }
            pullTargetDir_ = into;
        } else {
            pullTargetDir_.clear();
        }
        if (asArchive) {
            unpackNextArchive_ = true;
        }
        const bool started = client_->pullOnSession(peer, path, asArchive, [this](const cli::Outcome& o) {
            if (!o.ok) {
                // 没接受就不会有包来，清掉标记——
                // 否则下一次收到别人的 .tar.xz 会被
                // 当成这次的包解开。
                unpackNextArchive_ = false;
                // 临时落点同理：没取成就得改回去。
                restoreReceiveDir();
            }
            // 从 io 线程 emit 跨线程信号是安全的。
            emit remotePullFinished(o.ok, toQt(o.summary));
        });
        if (!started) {
            unpackNextArchive_ = false;
            restoreReceiveDir();
            emit remotePullFinished(false, QStringLiteral("当前不能取回（正在传输或会话不可用）"));
        }
    });
    return true;
}

void CoreService::restoreReceiveDir()
{
    // io 线程。没有临时落点（或根本没有会话）时什么都不做——这个函数会在
    // 每一次接收结束时被调用，不能有副作用。
    if (pullTargetDir_.empty()) {
        return;
    }
    pullTargetDir_.clear();
    if (client_ && !defaultReceiveDir_.empty()) {
        client_->setReceiveDirOnSession(defaultReceiveDir_);
    }
}

void CoreService::cancelCurrent()
{
    // 只在忙时才有可取消的东西。post 到 io 线程执行，与 Client 的
    // 单线程契约一致。
    if (!busy_) {
        return;
    }
    if (sendCount_.load(std::memory_order_acquire) > 0) {
        // 有搭车发送在跑：只取消这一次发送，会话（和本机 id）保持不动。
        // 结论由 sendFinished 带回，busy 不翻转。
        runOnIo([this] {
            if (client_) {
                client_->cancelSend();
            }
        });
        return;
    }
    runOnIo([this] {
        if (client_) {
            client_->abort();
            // abort 会同步走 onDone（"中止"），busy 的翻转交给
            // operationFinished 的路径，这里不重复处理。
        }
    });
}

}   // namespace relayer::app
