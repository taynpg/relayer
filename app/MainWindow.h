#pragma once

#include <QHash>
#include <QWidget>

#include "bridge/CoreService.h"
#include "model/TransferTask.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QGroupBox;

class ConnectorControl;
class ComparisonControl;
class ExplorerControl;
class LogControl;

namespace relayer::app {
class RelayTaskControl;
}

// 主窗口：v1 的 relayFile（Gui/relayFile.{h,cpp}）在 v2 的对应物。
//
// 布局镜像 v1 的 initLayout：上半 = 连接面板 + 选项卡，下半留给文件浏览
// （ExplorerControl 属 E3 全量部分，尚未迁移；splitter 先只放上区）。
//
// E3a（最小闭环）承载的接线：
//   - 对端右键"发送文件" → QFileDialog → CoreService::sendFile
//   - 自动接收：拿到自己的 id 后挂起等文件，一次接收结束自动重新挂起
//     （v2 每操作一条连接，"常驻在线"由这个循环实现；落到
//     LogControl 右键设置的接收目录，默认系统下载目录）
//   - 进度节流后写日志（CoreService 100ms 一次，直接刷屏会淹没结论行）
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onSendToPeer(const QString& peerId, const QString& peerName);
    void onProgress(quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond);
    void onOperationFinished(bool ok, const QString& summary, const QString& detail);
    // 单次发送的结论（常驻会话上进行，会话不结束、id 不变）。
    void onSendFinished(bool ok, const QString& summary, const QString& detail);
    void onAssignedIdChanged(const QString& id);
    void onProbeRequested(const relayer::app::ConnInfo& conn);
    void onDisconnectRequested();
    void onTransTaskRun(const relayer::app::RelayTaskItems& items);
    void onRemoteCheckRequested(const relayer::app::RelayTaskItems& items);
    void onDownloadRequested(const relayer::app::RelayTaskItems& items);
    void onRemotePullFinished(bool ok, const QString& summary);

    // 粗校验 / 时间戳校验（从本地面板发起）：与**远端面板当前目录下的同名
    // 文件**比。v1 的语义是跨面板比，v2 的两个面板正好是左本地、右远端。
    void onCompareRequested(const QString& name, bool wantTime);
    void onBrowsePeerRequested(const QString& peerId, const QString& peerName);
    void onRemoteBrowseFinished(bool ok, bool rootListing, const QString& summary, const QList<relayer::app::RemoteRow>& rows);

private:
    void buildLayout();
    void wireSignals();
    void startAutoReceive();

    // 把远端面板切到某个对端的某一层（空 = 根列表）。
    // 两个入口共用：对端表右键「浏览此对端的目录」、对照表右键「访问对端目录」。
    void switchRemotePeer(const QString& peerId, const QString& peerName, const QString& relPath = QString());

    // "检查远端"的逐行队列：CoreService 一次只跑一个列举，所以一行一行来。
    struct RemoteCheck {
        QString peer;
        QString relDir;   // 对端接收根下的相对目录（列举它）
        QString name;     // 期望在里面看到的文件名
        qint64 localSize = 0;
    };
    void nextRemoteCheck();

    QList<RemoteCheck> remoteChecks_;
    int remoteCheckIndex_ = -1;

    // ------------------------------------------------------------------
    // 发送前检查（v1 的"基本检查 + 覆盖提示"）
    // ------------------------------------------------------------------
    //
    // 流程：把这一批要发的目标绝对路径**一次性**问对端（每个对端一次
    // kAskPathMeta），拿回结果后在一个普通模态对话框里逐项问用户要不要
    // 覆盖，最后按回答过滤掉跳过的项再真正开任务。
    //
    // 为什么不在传输中弹窗：那个决策点在 TransferPeer 的 io 线程上，应答
    // 帧压在那里，等用户点按钮会饿死心跳（空闲 20s 发 ping、10s 未答判
    // 死），只能加"用户还没点就超时"的兜底。放到传输之前就没有这个问题。
    struct SendPreCheck {
        QString peerId;
        QStringList paths;   // 对端视角的绝对路径
        QList<int> itemIndexes;
    };

    void sendBatch(const relayer::app::RelayTaskItems& items, const relayer::app::ConnInfo& conn, const QString& defaultPeerId);
    void startSendPreCheck(const relayer::app::RelayTaskItems& items, const relayer::app::ConnInfo& conn,
                           const QString& defaultPeerId);
    void issueNextPreCheck();
    void onPreCheckMetaFinished(bool ok, const QString& summary, const QList<relayer::app::RemoteMetaRow>& rows);
    void promptOverwriteThenSend();

    relayer::app::RelayTaskItems preCheckItems_;
    relayer::app::ConnInfo preCheckConn_;
    QString preCheckDefaultPeer_;
    QList<SendPreCheck> preCheckQueue_;
    int preCheckIndex_ = -1;
    // key = 对端 id + "|" + 绝对路径
    QHash<QString, relayer::app::RemoteMetaRow> preCheckResults_;

    // "下载到本地目录"的逐行队列（对照表右键）。
    //
    // 为什么要排队：协议上一次只允许一个取回请求在飞（Client 的 pullActive），
    // 而且对端在打包/发送期间会拒绝新的取回请求。所以按行串行，一行真正
    // 落地（或失败）之后才发下一行的请求。
    //
    // "落地"的判据是 fileReceived / archiveExtracted —— 只等应答（"对方
    // 接受了"）是不够的：文件还在传，下一行的请求会被对端以"正忙"拒掉。
    struct DownloadJob {
        QString peer;
        QString relPath;   // 对端绝对路径（relDir/name）
        QString name;      // 期望收到的名字（认领 fileReceived 用）
        bool isDir = false;
        QString into;   // 落点：该行的本地目录；空 = 全局接收目录
    };
    void nextDownload();
    void finishDownloadStep();

    QList<DownloadJob> downloads_;
    int downloadIndex_ = -1;         // -1 = 空闲；否则是"已发请求、等接受"的下标
    bool downloadWaiting_ = false;   // 已接受，等文件落地

    // 一次待完成的比对。远端那一侧要先列举才能拿到 size/mtime，所以这里
    // 记住本地侧的数值，等应答回来再比。
    struct PendingCompare {
        QString name;
        bool wantTime = false;
        qint64 localSize = 0;
        qint64 localMtime = 0;
    };
    PendingCompare pendingCompare_;
    bool compareActive_ = false;
    void finishCompare(qint64 peerSize, qint64 peerMtime);

    Ui::MainWindow* ui_;
    relayer::app::CoreService* core_;
    ConnectorControl* connector_;
    LogControl* log_;
    ExplorerControl* explorer_ = nullptr;         // 本地
    ExplorerControl* remoteExplorer_ = nullptr;   // 远端（同一个类，换数据源）
    QGroupBox* remoteBox_ = nullptr;              // 标题带对端名，切换时一眼可见
    ComparisonControl* comparison_ = nullptr;
    relayer::app::RelayTaskControl* taskControl_ = nullptr;

    // 自动接收状态。receiveActive_ 表达"此刻有常驻接收会话挂着"
    // （keepReceiving：收完文件不断开）；receiveWanted_ 表达"用户想要
    // 自动接收"。被抢占/失败后由 onOperationFinished 恢复。
    bool receiveWanted_ = false;
    bool receiveActive_ = false;
    QString receiveDir_;

    // 接收会话失败重试计数（成功连接/成功操作即清零）。
    int receiveRetries_ = 0;

    // 最近一次探测的连接参数（ConnectorControl::probeRequested 记录）。
    // 每个操作一条连接，之后的所有操作都复用这一组值。
    relayer::app::ConnInfo lastConn_;

    // 进度节流：上次写日志的时间点。
    std::chrono::steady_clock::time_point lastProgressLog_{};

    // 等待文件（总量为 0）的提示只打一次，避免空闲进度刷屏。
    bool idleProgressLogged_ = false;
};
