#include "CoreService.h"

#include <QMetaObject>
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

void CoreService::sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId,
                           const ConnInfo &conn)
{
    if (busy_)
    {
        return;
    }
    busy_ = true;
    emit busyChanged(true);

    runOnIo(
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
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [this](const cli::ProgressReport &p)
                { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
                [this](const cli::Outcome &o)
                {
                    // io 线程。先发列表（list 的惯例顺序：先数据后结论），
                    // 再发结论与 busy 翻转——GUI 侧依赖"收到 finished 时
                    // busy 已可用"来解锁按钮。
                    if (!o.peerList.empty())
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
                    emit busyChanged(false);
                });
            client_->sendFile(fromQt(localPath), fromQt(remoteName), fromQt(toPeerId));
        });
}

void CoreService::startReceive(const QString &targetDir, const ConnInfo &conn)
{
    if (busy_)
    {
        return;
    }
    busy_ = true;
    emit busyChanged(true);

    runOnIo(
        [this, targetDir, cfg = makeConfig(conn)]() mutable
        {
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [this](const cli::ProgressReport &p)
                { emit progressChanged(p.bytesDone, p.bytesTotal, p.bytesPerSecond); },
                [this](const cli::Outcome &o)
                {
                    emit operationFinished(o.ok, toQt(o.summary), toQt(o.status.message));
                    busy_ = false;
                    emit busyChanged(false);
                });
            client_->receiveFile(fromQt(targetDir));
        });
}

void CoreService::listPeers(const ConnInfo &conn)
{
    if (busy_)
    {
        return;
    }
    busy_ = true;
    emit busyChanged(true);

    runOnIo(
        [this, cfg = makeConfig(conn)]() mutable
        {
            // 不需要进度。必须是显式 lambda 而不是 {}：make_shared 是
            // 可变参转发，braced-init-list 无法在转发中推导参数类型。
            client_ = std::make_shared<cli::Client>(
                ctx_, std::move(cfg), [](const cli::ProgressReport &) {},
                [this](const cli::Outcome &o)
                {
                    QList<PeerRow> rows;
                    rows.reserve(static_cast<qsizetype>(o.peerList.size()));
                    for (const auto &p : o.peerList)
                    {
                        rows.push_back(PeerRow{toQt(p.id), toQt(p.address)});
                    }
                    emit peerListReceived(rows);
                    emit operationFinished(o.ok, toQt(o.summary), toQt(o.status.message));
                    busy_ = false;
                    emit busyChanged(false);
                });
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
