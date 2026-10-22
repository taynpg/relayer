#include "CoreService.h"

#include <QMetaObject>
#include <QSettings>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>

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
    cfg.overwriteExisting = settings.value("receiver/overwrite", true).toBool();

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

bool CoreService::sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId,
                           const ConnInfo &conn)
{
    return runOrAfterReceive(
        [this, localPath, remoteName, toPeerId, cfg = makeConfig(conn)]() mutable
        {
            // 上一个 Client 结束后残留的 timer cancel 需要一轮 run 才落地，
            // 这里的 post 天然排在它们之后，直接替换是安全的。
            //
            // **不要**在 onDone 回调里 reset client_：finish 的调用点
            // （pump 定时器回调、checkIdle 等）在自己返回后还会访问 Impl
            // 的成员，那时析构 Client 就是 use-after-free。CLI 的 main
            // 从不中途释放，所以那条路没暴露过；这里改成"留着到下一次
            // 操作再替换"，替换点在本 lambda 里，不在任何回调栈上。
            busy_ = true;
            receivePending_ = false;
            queuedWork_ = false;
            emit busyChanged(true);
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [this](const cli::ProgressReport &p)
                { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
                [this](const cli::Outcome &o) { finishOperation(o, true); });
            client_->sendFile(fromQt(localPath), fromQt(remoteName), fromQt(toPeerId));
        });
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

void CoreService::cancelCurrent()
{
    // 只在忙时才有可取消的东西。post 到 io 线程执行，与 Client 的
    // 单线程契约一致。
    if (!busy_)
    {
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
