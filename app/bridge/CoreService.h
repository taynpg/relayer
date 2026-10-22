#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <asio/io_context.hpp>

#include "relayer/cli/Client.h"

// Core → Qt 适配层（REWRITE-PLAN §3.7.3 的第一件实事）。
//
// 职责只有三条：
//   1. 生命周期：持有 io_context 与运行它的 io 线程；cli::Client 的实例
//      也由它持有。GUI 线程永不直接碰 Client。
//   2. 线程边界：Client 的状态机是"只在事件循环线程上读写"的单线程设计
//      （见 cli/Client.cpp 文件头），而 GUI 的事件循环在另一个线程。所以
//      所有对 Client 的调用都 asio::post 到 io 线程执行；Client 的回调
//      都发生在 io 线程，信号用跨线程 queued 连接投递回 GUI 线程。
//   3. 字符串边界：core 侧一律 UTF-8 std::string，Qt 侧一律 QString，
//      转换只发生在这里（QString 与 UTF-8 互转是无损的，含中文路径）。
//
// 刻意**不做**的事：
//   - 不把 core 的类型包成 Qt 类型再到处传——POD 的转换在本类边界一次做完；
//   - 不实现"同时跑多个操作"。CLI 门面是一次一操作的，v1 的多任务并行是
//     阶段 F 的功能，到时候每任务一个 Client 实例即可，现在不预造。

namespace relayer::app
{

// 在线对端的 Qt 侧视图（PeerEntry 的 QString 版，供表格直接用）。
struct PeerRow
{
    QString id;
    QString address;
};

class CoreService : public QObject
{
    Q_OBJECT

public:
    explicit CoreService(QObject *parent = nullptr);
    ~CoreService() override;

    CoreService(const CoreService &) = delete;
    CoreService &operator=(const CoreService &) = delete;

    bool busy() const noexcept
    {
        return busy_;
    }

    // 把本地文件发给对端。remoteName 为空时用本地文件名。
    void sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId, const QString &host,
                  quint16 port, const QString &password);

    // 挂起等待别人发文件，落到 targetDir 目录。
    void startReceive(const QString &targetDir, const QString &host, quint16 port, const QString &password);

    // 拉一次在线列表，结果从 peerListReceived 发出。
    void listPeers(const QString &host, quint16 port, const QString &password);

    // 中止当前操作（硬停：不发 cancel 帧，与 CLI 的 abort 同语义）。
    void cancelCurrent();

signals:
    // 操作开始（busy=true）/结束（busy=false）。
    void busyChanged(bool busy);

    // 自己被分配的身份。可能多次发出（见 ClientConfig::onIdAssigned 注释）。
    void assignedIdChanged(const QString &id);

    // 传输进度。bytesPerSecond 是瞬时速率，可能为 0（尚未有两次采样）。
    void progressChanged(quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond);

    // 一次操作的最终结论。发出后 busy 变 false，之后不会再有该操作的信号。
    // technicalDetail 失败时可能非空（Status 的 message）。
    void operationFinished(bool ok, const QString &summary, const QString &technicalDetail);

    // list 的结果。与 operationFinished 同批到达（先列表后结论）。
    void peerListReceived(const QList<relayer::app::PeerRow> &rows);

private:
    void startIoThread();
    void runOnIo(std::function<void()> fn);

    // ClientConfig 组装。回调里 emit 的都是 Qt 信号——从 io 线程 emit
    // 跨线程信号是安全的（AutoConnection 退化为 QueuedConnection）。
    cli::ClientConfig makeConfig(const QString &host, quint16 port, const QString &password);

    asio::io_context ctx_;

    // 让 ctx.run() 在没有事件时也不返回，否则 io 线程一空闲就退出，
    // 下一个操作就没线程可用了。销毁时用 guard_.reset() + ctx.stop() 收。
    asio::executor_work_guard<asio::io_context::executor_type> guard_;

    std::thread ioThread_;
    std::atomic<bool> stopping_{false};

    // 以下成员只属于 io 线程（与 cli::Client 的单线程契约一致），
    // GUI 线程不碰；busy_ 是唯一例外，它是 atomic。
    //
    // client_ 的生命周期：操作结束后**保留**而不是立即释放——Client 的
    // finish 调用点返回后还会访问自己的成员，在回调栈上析构是 use-after-free。
    // 释放发生在两处：下一次操作时替换（post 进 io 线程执行，不在回调栈上）、
    // 以及 CoreService 析构（此时 io 线程已 join）。
    std::shared_ptr<cli::Client> client_;
    std::string lastId_;  // 去重：同一个 id 不重复发 assignedIdChanged

    std::atomic<bool> busy_{false};
};

}  // namespace relayer::app

Q_DECLARE_METATYPE(relayer::app::PeerRow)
