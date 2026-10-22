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

// 一次连接所需的全部参数。收拢成结构体而不是继续给每个方法加参数：
// sendFile 已经 6 个参数，且这三项在所有操作里总是一起出现。
struct ConnInfo
{
    QString host;
    quint16 port = 0;
    QString password;

    // 显式声明"中继已关闭认证"（对应服务端 --allow-no-auth）。
    // 不从 password 为空自动推断：漏填口令应当报错，而不是静默降级成
    // 不认证——与服务端侧"要关认证必须显式 --allow-no-auth"是同一条
    // 原则在客户端的镜像。
    bool noAuth = false;
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

    // busy 是否属于"接收挂起等文件"——这种忙可以被打断（见 runOrAfterReceive），
    // 真正的传输忙不能。
    bool canPreempt() const noexcept
    {
        return busy_ && receivePending_;
    }

    // 有已抢占、还没开始执行的排队操作。GUI 据此决定要不要重挂自动接收：
    // 排队的操作马上会把 busy 重新占上，此刻重挂会跟它抢通道。
    bool hasQueuedWork() const noexcept
    {
        return queuedWork_;
    }

    // 把本地文件发给对端。remoteName 为空时用本地文件名。
    // 返回 false = 真忙（传输中），本次请求被拒绝；true = 已开始或已排队。
    bool sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId,
                  const relayer::app::ConnInfo &conn);

    // 挂起等待别人发文件，落到 targetDir 目录。
    void startReceive(const QString &targetDir, const relayer::app::ConnInfo &conn);

    // 拉一次在线列表，结果从 peerListReceived 发出。返回值语义同 sendFile。
    bool listPeers(const relayer::app::ConnInfo &conn);

    // 刷新在线列表：常驻接收会话活着时在**那条连接上**问（不换 id、不
    // 打断接收），否则回落到 listPeers 的临时连接探测。返回 false =
    // 传输中无法刷新，调用方负责提示。
    bool refreshPeers(const relayer::app::ConnInfo &conn);

    // 中止当前操作（硬停：不发 cancel 帧，与 CLI 的 abort 同语义）。
    void cancelCurrent();

signals:
    // 操作开始（busy=true）/结束（busy=false）。
    void busyChanged(bool busy);

    // 自己被分配的身份。可能多次发出（见 ClientConfig::onIdAssigned 注释）。
    void assignedIdChanged(const QString &id);

    // 常驻接收收完一个文件。操作本身不结束（busy 不翻转），会话继续
    // 等下一个文件——id 因此保持稳定，发送方的对端表不会过期。
    void fileReceived(const QString &targetPath);

    // 常驻接收下某一次接收失败（连接还活着，会话原地重建继续等）。
    // 与 fileReceived 对称：busy 不翻转、id 不变，GUI 只需要记日志。
    void receiveFailed(const QString &message);

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

    // 操作入口的统一闸门：
    //   - 空闲：同步置 busy，直接跑 beginOp（io 线程）；
    //   - 接收挂起：抢占——取消接收，beginOp 排队等接收的 onDone 落地后执行；
    //   - 传输中：返回 false，调用方负责提示。
    // beginOp 在 io 线程执行，负责建 Client 并发起操作。
    bool runOrAfterReceive(std::function<void()> beginOp);

    // ClientConfig 组装。回调里 emit 的都是 Qt 信号——从 io 线程 emit
    // 跨线程信号是安全的（AutoConnection 退化为 QueuedConnection）。
    cli::ClientConfig makeConfig(const relayer::app::ConnInfo &conn);

    // 最近一次建 Client 用的连接参数指纹（host:port:认证方式:口令）。
    // 只在 GUI 线程读写（makeConfig 与 refreshPeers 的调用方都在 GUI
    // 线程），refreshPeers 靠它判断"表单里的地址是否还是当前会话的
    // 地址"——用户改了地址再点连接，必须换新连接，不能在旧会话上问
    // 旧服务器的列表。
    QString activeConnKey_;

    // 一次操作的公共收尾（io 线程，onDone 回调里调）。
    void finishOperation(const cli::Outcome &o, bool withPeerList);

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

    // 只属于 io 线程：被抢占接收的下一个 onDone 不发信号（见
    // runOrAfterReceive）。
    bool suppressNextFinished_ = false;

    // busy_ / receivePending_ / queuedWork_ 是 atomic：GUI 线程读它们做
    // 闸门判断，io 线程写。receivePending_ 只在 beginReceive 置 true、
    // finishOperation 清掉；queuedWork_ 表达"有被抢占排队的操作还没开跑"。
    std::atomic<bool> busy_{false};
    std::atomic<bool> receivePending_{false};
    std::atomic<bool> queuedWork_{false};
};

}  // namespace relayer::app

Q_DECLARE_METATYPE(relayer::app::PeerRow)
Q_DECLARE_METATYPE(relayer::app::ConnInfo)
