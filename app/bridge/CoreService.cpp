#include "CoreService.h"

#include <QMetaObject>
#include <QSettings>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>

#include "ui/SettingsKeys.h"

namespace relayer::app
{

namespace
{

QString toQt(const std::string &s)
{
    return QString::fromStdString(s);  // QString 与 UTF-8 互转，中文路径无损
}

std::string fromQt(const QString &s)
{
    return s.toStdString();
}

}  // namespace

CoreService::CoreService(QObject *parent)
    : QObject(parent)
    , guard_(asio::make_work_guard(ctx_))
{
}

CoreService::~CoreService()
{
    // 停线程必须先于成员析构：io 线程回调里会 emit this 的信号、会碰
    // client_，任何一个晚于析构都是 use-after-free。
    stopping_ = true;
    guard_.reset();
    ctx_.stop();
    if (ioThread_.joinable())
    {
        ioThread_.join();
    }
    // 线程已停，这里不再是并发上下文，直接释放即可。
    client_.reset();
}

void CoreService::startIoThread()
{
    if (ioThread_.joinable())
    {
        return;
    }
    ioThread_ = std::thread(
        [this]
        {
            // CLI 的 main 里 run_for 循环的同款写法（见 cli/main.cpp 注释）：
            // run() 在没有事件时直接返回，而 Client 结束后仍可能有排队的
            // handler 要跑完；用 run_for 轮询 + stopping_ 检查保证退出及时。
            while (!stopping_)
            {
                if (ctx_.stopped())
                {
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

cli::ClientConfig CoreService::makeConfig(const ConnInfo &conn)
{
    cli::ClientConfig cfg;
    cfg.host = fromQt(conn.host);
    cfg.port = conn.port;
    cfg.password = fromQt(conn.password);
    cfg.serverAllowNoAuth = conn.noAuth;

    // 记下这组参数（GUI 线程），供 refreshPeers 判断会话是否对应表单地址。
    activeConnKey_ = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                     (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;

    // GUI 的自动接收必须常驻（收完不断开）：每操作一条连接 + 服务端递增
    // 分配 id，"收完就重挂"会让接收方每收一个文件就换一次 id，发送方
    // 表格里的对端 id 立刻过期——再发就发进虚空，等满 answerTimeout 才报错。
    cfg.keepReceiving = true;
    cfg.onFileReceived = [this](const std::string &targetPath) { emit fileReceived(toQt(targetPath)); };
    cfg.onReceiveFailed = [this](const std::string &message) { emit receiveFailed(toQt(message)); };

    // 接收覆盖：GUI 的落点是用户自己选的接收目录，"同一个文件反复发到
    // 同一个目录"是核心场景——默认拒绝覆盖会让第二次发送起永远失败，
    // 还会诱发接收端反复重试换 id（两者实测都发生过）。CLI 侧保持默认
    // 拒绝、由 --overwrite 显式打开；GUI 侧默认打开，用户可在日志菜单
    // 里关掉（设置持久化，见 LogControl 的菜单项）。
    const QSettings settings;
    cfg.overwriteExisting =
        settings.value(relayer::app::settings::kReceiverOverwrite, relayer::app::settings::kReceiverOverwriteDefault)
            .toBool();

    cfg.onIdAssigned = [this](const std::string &id)
    {
        // io 线程。同一个 id 只发一次：服务端重复通知不该让界面闪两下。
        if (id == lastId_)
        {
            return;
        }
        lastId_ = id;
        emit assignedIdChanged(toQt(id));
    };

    // 常驻接收会话上的列表刷新回程 + 认证后自动问列表。两者配合，
    // GUI 的"连接/刷新"全程只有一条连接、一个 id（见 ClientConfig
    // 两个成员的注释）。
    cfg.onPeerList = [this](const std::vector<cli::PeerEntry> &entries)
    {
        QList<PeerRow> rows;
        rows.reserve(static_cast<qsizetype>(entries.size()));
        for (const auto &p : entries)
        {
            rows.push_back(PeerRow{toQt(p.id), toQt(p.address)});
        }
        emit peerListReceived(rows);
    };
    cfg.askListOnStart = true;

    return cfg;
}

bool CoreService::runOrAfterReceive(std::function<void()> beginOp)
{
    if (!busy_)
    {
        // 空闲：同步占上 busy（gate 住 GUI 线程的后续请求），io 线程开跑。
        busy_ = true;
        emit busyChanged(true);
        runOnIo(std::move(beginOp));
        return true;
    }
    if (!receivePending_)
    {
        return false;  // 传输中，真忙
    }

    // 抢占挂起的接收：排队动作 → 取消接收 → onDone 落地后动作开跑。
    //
    // 顺序要点：queuedWork_ 置 true 必须先于 abort()——abort 同步走 onDone，
    // onDone 里 emit 的 operationFinished 会被 queued 投递到 GUI；GUI 处理它时
    // 决定要不要重挂自动接收，判据就是 queuedWork_。若在 abort 之后才置位，
    // GUI 可能在 io 线程两条语句之间读到旧值，抢先重挂、和排队的动作抢通道。
    runOnIo(
        [this, act = std::move(beginOp)]() mutable
        {
            queuedWork_ = true;
            // 被抢占接收的 abort 也会走 onDone（"中止"），但那不是用户
            // 操作的结束，是内部让位——不发 finished，否则任务链会把它
            // 误判成"第一项失败"（实测如此），自动接收逻辑也会被搅乱。
            suppressNextFinished_ = true;
            if (client_)
            {
                client_->abort();  // 同步走 onDone：finishOperation 消费标记
            }
            // act 里会重新占 busy。post 而不是直接调：此刻仍在 abort 的
            // 调用链上，直接建新 Client 替换 client_ 又是回调栈析构的老坑。
            asio::post(ctx_, std::move(act));
        });
    return true;
}

void CoreService::finishOperation(const cli::Outcome &o, bool withPeerList)
{
    // 会话级结论：正在进行的搭车发送到此为止（Client 那边也不会再回调
    // 它了，见 Client::Impl::finish）。清掉标记，让 onSendFinished 的
    // 迟到回调被静默丢弃。
    sendActive_ = false;

    // io 线程。被抢占接收的"中止"属于内部机制（见 runOrAfterReceive）：
    // 翻转内部状态但不发信号——busy 会被紧接着的排队动作重新占上，
    // 中间发 false 再发 true 只会让按钮闪一下、让上层误推进。
    if (suppressNextFinished_)
    {
        suppressNextFinished_ = false;
        busy_ = false;
        receivePending_ = false;
        return;
    }

    // 先发列表（list 的惯例顺序：先数据后结论），再发结论与
    // busy 翻转——GUI 侧依赖"收到 finished 时 busy 已可用"来解锁按钮。
    // connectionLost 标记必须在结论**之前**落地：跨线程 queued 信号
    // 的槽执行晚于本函数返回，先置位才能保证读到的不是上一次的旧值。
    lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
    if (withPeerList && !o.peerList.empty())
    {
        QList<PeerRow> rows;
        rows.reserve(static_cast<qsizetype>(o.peerList.size()));
        for (const auto &p : o.peerList)
        {
            rows.push_back(PeerRow{toQt(p.id), toQt(p.address)});
        }
        emit peerListReceived(rows);
    }
    emit operationFinished(o.ok, toQt(o.summary), toQt(o.status.message));

    busy_ = false;
    receivePending_ = false;
    emit busyChanged(false);
}

bool CoreService::canSendOnSession(const ConnInfo &conn) const
{
    if (!busy_.load() || !receivePending_.load() || queuedWork_.load() || sendActive_.load())
    {
        return false;
    }
    const QString key = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                        (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;
    return key == activeConnKey_;
}

bool CoreService::sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId,
                           const ConnInfo &conn)
{
    // 常驻会话活着：直接在**这条连接**上发。不新建连接 → 服务端不会重新
    // 分配 id；接收也不受影响（两边靠 sessionId 区分各自的帧）。这是
    // "点一次发送自己的 id 就涨一号"的根治点。
    if (canSendOnSession(conn))
    {
        sendActive_ = true;
        runOnIo(
            [this, path = fromQt(localPath), name = fromQt(remoteName), peer = fromQt(toPeerId)]
            {
                const bool started = client_ && client_->sendOnSession(path, name, peer, [this](const cli::Outcome &o)
                                                                       { onSendFinished(o); });
                if (started)
                {
                    return;
                }
                // 会话在 GUI 判断与 post 落地之间结束了（比如中继刚断）：
                // 如实报一次结论，别让调用方等一个永远不来的回调。
                if (sendActive_.exchange(false))
                {
                    emit sendFinished(false, QStringLiteral("当前会话不可用，请重新连接后再发"), QString());
                }
            });
        return true;
    }

    // 会话不在（还没连过 / 接收已停）：回落到一次性连接。这一次仍会换 id，
    // 但没有在线会话时别无选择——GUI 连上之后走不到这条路。
    return runOrAfterReceive(
        [this, localPath, remoteName, toPeerId, cfg = makeConfig(conn)]() mutable
        {
            // 一次性连接的 id 不往界面报：那是用完即弃的身份，显示出去
            // 就成了"发送一次 id 变一次"。
            cfg.onIdAssigned = nullptr;

            busy_ = true;
            receivePending_ = false;
            queuedWork_ = false;
            emit busyChanged(true);
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [this](const cli::ProgressReport &p)
                { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
                [this](const cli::Outcome &o) { finishSendOperation(o); });
            client_->sendFile(fromQt(localPath), fromQt(remoteName), fromQt(toPeerId));
        });
}

void CoreService::finishSendOperation(const cli::Outcome &o)
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

void CoreService::onSendFinished(const cli::Outcome &o)
{
    // 会话级结论先到时（finishOperation 已把 sendActive_ 清掉），这里不能
    // 再发一次：一次发送只能有一个结论。
    if (!sendActive_.exchange(false))
    {
        return;
    }
    lastConnectionLost_.store(o.connectionLost, std::memory_order_release);
    emit sendFinished(o.ok, toQt(o.summary), toQt(o.status.message));
}

void CoreService::startReceive(const QString &targetDir, const ConnInfo &conn)
{
    if (busy_)
    {
        return;  // 只有 MainWindow 调用，且它有 receiveActive_ 状态机，到不了这里
    }
    busy_ = true;
    emit busyChanged(true);

    runOnIo(
        [this, targetDir, cfg = makeConfig(conn)]() mutable
        {
            busy_ = true;
            receivePending_ = true;
            queuedWork_ = false;
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [this](const cli::ProgressReport &p)
                { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
                [this](const cli::Outcome &o) { finishOperation(o, false); });
            client_->receiveFile(fromQt(targetDir));
        });
}

bool CoreService::listPeers(const ConnInfo &conn)
{
    return runOrAfterReceive(
        [this, cfg = makeConfig(conn)]() mutable
        {
            // 临时连接的 id 不往界面报（同 sendFile 的道理）：刷新列表
            // 不该让"本机 id"跳动，界面上那个 id 只属于常驻接收会话。
            cfg.onIdAssigned = nullptr;

            busy_ = true;
            receivePending_ = false;
            queuedWork_ = false;
            emit busyChanged(true);
            // 不需要进度。必须是显式 lambda 而不是 {}：make_shared 是
            // 可变参转发，braced-init-list 无法在转发中推导参数类型。
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [](const cli::ProgressReport &) {},
                [this](const cli::Outcome &o) { finishOperation(o, true); });
            client_->listPeers();
        });
}

bool CoreService::refreshPeers(const ConnInfo &conn)
{
    // 表单参数必须与当前会话一致才复用：用户改了地址/口令再点连接，
    // 意图是"换服务器"，在旧会话上问列表等于问错人。
    const QString key = conn.host + QStringLiteral(":") + QString::number(conn.port) + QStringLiteral(":") +
                        (conn.noAuth ? QStringLiteral("N") : QStringLiteral("P")) + QStringLiteral(":") + conn.password;

    // 常驻接收会话活着（挂起等文件，且没有排队中的被抢占操作）：直接在
    // 那条连接上问列表。不占通道、不翻转 busy、不产生 operationFinished
    // ——GUI 的接收状态机与身份显示完全无感，id 自然不会变。
    //
    // 条件里必须有 !queuedWork_：有排队的动作说明接收刚被抢占、马上要
    // 换操作，此刻往旧 client 上发列表是往一条将死的连接上说话。
    if (busy_.load() && receivePending_.load() && !queuedWork_.load() && key == activeConnKey_)
    {
        runOnIo(
            [this]
            {
                // post 排队后才读 client_：GUI 线程不碰这个指针（单线程
                // 契约），此刻它可能正被替换。
                if (client_)
                {
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

void CoreService::cancelCurrent()
{
    // 只在忙时才有可取消的东西。post 到 io 线程执行，与 Client 的
    // 单线程契约一致。
    if (!busy_)
    {
        return;
    }
    if (sendActive_)
    {
        // 有搭车发送在跑：只取消这一次发送，会话（和本机 id）保持不动。
        // 结论由 sendFinished 带回，busy 不翻转。
        runOnIo(
            [this]
            {
                if (client_)
                {
                    client_->cancelSend();
                }
            });
        return;
    }
    runOnIo(
        [this]
        {
            if (client_)
            {
                client_->abort();
                // abort 会同步走 onDone（"中止"），busy 的翻转交给
                // operationFinished 的路径，这里不重复处理。
            }
        });
}

}  // namespace relayer::app
