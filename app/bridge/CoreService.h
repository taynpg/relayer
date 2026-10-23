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

// 远端目录列举的一条（core::session::RemoteEntry 的 QString 版）。
struct RemoteRow
{
    QString name;
    bool isDir = false;
    quint64 size = 0;
    qint64 mtime = 0;
};

// 一次连接所需的全部参数。收拢成结构体而不是继续给每个方法加参数：
// sendFile 已经 6 个参数，且这三项在所有操作里总是一起出现。
struct ConnInfo
{
    QString host;
    quint16 port = 0;
    QString password;

    // 声明"中继已关闭认证"（对应服务端 --allow-no-auth）。
    // 不从 password 为空自动推断：漏填口令应当报错，而不是静默降级成
    // 不认证——与服务端侧"要关认证必须显式 --allow-no-auth"是同一条
    // 原则在客户端的镜像。
    bool noAuth = false;

    // 本机标识（PROTOCOL.md §11.7）。空 = 由中继分配 client-N。
    //
    // 非空时客户端认证后立刻声明它：接受则本机 id 固定下来（跨重连不变），
    // 被占用则连接失败。对照表的对端 id 依赖这个稳定性——id 漂移会让
    // "这行固定发 A"悄悄变成发给别人。
    QString requestedId;
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

    // 最近一次失败是否属于连接级失败（中继断开/对端失联，见
    // cli::Outcome::connectionLost）。任务批次凭它决定"剩余项不再发"。
    // 在 finishOperation 发出 operationFinished 之前置位，GUI 槽读到的
    // 一定是本次结论的值。
    bool lastFailureConnectionLost() const noexcept
    {
        return lastConnectionLost_.load(std::memory_order_acquire);
    }

    // 把本地文件发给对端。remoteName 为空时用本地文件名。
    // 返回 false = 真忙（传输中），本次请求被拒绝；true = 已开始或已排队。
    //
    // 常驻会话活着时在**那条连接上**发（不新建连接、不换本机 id、
    // 不打断接收）；会话不在时回落到一次性连接。
    bool sendFile(const QString &localPath, const QString &remoteName, const QString &toPeerId,
                  const relayer::app::ConnInfo &conn);

    // 有"搭车发送"正在进行（会话内发送）。GUI 用它挡重复点击。
    bool sendInFlight() const noexcept
    {
        return sendActive_;
    }

    // 挂起等待别人发文件，落到 targetDir 目录。
    void startReceive(const QString &targetDir, const relayer::app::ConnInfo &conn);

    // 拉一次在线列表，结果从 peerListReceived 发出。返回值语义同 sendFile。
    bool listPeers(const relayer::app::ConnInfo &conn);

    // 刷新在线列表：常驻接收会话活着时在**那条连接上**问（不换 id、不
    // 打断接收），否则回落到 listPeers 的临时连接探测。返回 false =
    // 传输中无法刷新，调用方负责提示。
    bool refreshPeers(const relayer::app::ConnInfo &conn);

    // 在常驻会话上问一次远端目录（PROTOCOL.md §11.8）。
    //
    // 返回 false = 没有可用的常驻会话（没连接、或正在传输）。**不回落成
    // 新连接**：那会顶掉接收会话、换掉本机 id，是文档里反复警告的回归。
    bool browsePeerOnSession(const QString &peerId, const QString &relPath);

    // 在常驻会话上请对端把某个文件发过来（PROTOCOL.md §11.9）。
    // relPath 相对对端共享根。文件会落到本机的接收目录——落点由本机配置
    // 决定，共享方无权指定（协议里这条是写死的）。
    bool pullFileFromPeer(const QString &peerId, const QString &relPath);

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

    // 一次**发送**的结论（每个文件一次）。与 operationFinished 的区别是
    // 语义层级：operationFinished 表达"会话/操作结束"（busy 翻转、可能要
    // 重连换 id），sendFinished 只表达"这个文件发完了"，常驻会话照旧
    // 活着——GUI 的接收状态机绝不能被它推动。
    void sendFinished(bool ok, const QString &summary, const QString &technicalDetail);

    // list 的结果。与 operationFinished 同批到达（先列表后结论）。
    void peerListReceived(const QList<relayer::app::PeerRow> &rows);

    // 远端列举的结果（browsePeerOnSession 的回程）。一次请求一次信号，
    // 会话不结束。
    void remoteBrowseFinished(bool ok, const QString &summary, const QList<relayer::app::RemoteRow> &rows);

    // 取回请求的结论（收到对端应答时触发，**不是**文件传完）。
    // ok=true 只表示"对方接受了"，文件随后由接收路径报告落点。
    void remotePullFinished(bool ok, const QString &summary);

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

    // 会话内发送的结论（io 线程，Client 的 sendOnSession 回调里调）。
    void onSendFinished(const cli::Outcome &o);

    // 一次性连接上的发送收尾：发 sendFinished 并翻转 busy（io 线程）。
    void finishSendOperation(const cli::Outcome &o);

    // 常驻会话 + 表单参数一致 + 当前没有发送在跑 → 可以搭车发送。
    bool canSendOnSession(const relayer::app::ConnInfo &conn) const;

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

    // 有"搭车发送"在常驻会话上进行。GUI 线程读写（挡重复发送）、
    // io 线程在结论回调里清。
    std::atomic<bool> sendActive_{false};

    // 最近一次结论的连接级失败标记（io 线程写、GUI 线程读，见
    // lastFailureConnectionLost() 的注释）。
    std::atomic<bool> lastConnectionLost_{false};
};

}  // namespace relayer::app

Q_DECLARE_METATYPE(relayer::app::PeerRow)
Q_DECLARE_METATYPE(relayer::app::RemoteRow)
Q_DECLARE_METATYPE(relayer::app::ConnInfo)
