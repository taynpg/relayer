#pragma once

#include <QWidget>

#include "bridge/CoreService.h"
#include "model/TransferTask.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class ConnectorControl;
class ComparisonControl;
class ExplorerControl;
class LogControl;

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
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSendToPeer(const QString &peerId, const QString &peerName);
    void onProgress(quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond);
    void onOperationFinished(bool ok, const QString &summary, const QString &detail);
    void onAssignedIdChanged(const QString &id);
    void onProbeRequested(const relayer::app::ConnInfo &conn);
    void onTransTaskRun(const relayer::app::RelayTaskItems &items);

private:
    void buildLayout();
    void wireSignals();
    void startAutoReceive();

    // 任务执行器（E3.4 的临时实现：顺序逐个发，日志反馈进度；
    // E3.5 的 RelayTask 表单会接管展示与控制）。
    void startNextTaskItem();

    Ui::MainWindow *ui_;
    relayer::app::CoreService *core_;
    ConnectorControl *connector_;
    LogControl *log_;
    ExplorerControl *explorer_ = nullptr;
    ComparisonControl *comparison_ = nullptr;

    // 自动接收状态。receiveActive_ 表达"此刻有常驻接收会话挂着"
    // （keepReceiving：收完文件不断开）；receiveWanted_ 表达"用户想要
    // 自动接收"。被抢占/失败后由 onOperationFinished 恢复。
    bool receiveWanted_ = false;
    bool receiveActive_ = false;
    QString receiveDir_;

    // 接收会话失败重试计数（成功连接/成功操作即清零）。
    int receiveRetries_ = 0;

    // 任务队列：配置表"发送到对端"展开后的文件序列（目录行已展开为文件）。
    relayer::app::RelayTaskItems taskQueue_;
    int taskIndex_ = 0;
    bool taskRunning_ = false;

    // 最近一次探测的连接参数（ConnectorControl::probeRequested 记录）。
    // 每个操作一条连接，之后的所有操作都复用这一组值。
    relayer::app::ConnInfo lastConn_;

    // 进度节流：上次写日志的时间点。
    std::chrono::steady_clock::time_point lastProgressLog_{};

    // 等待文件（总量为 0）的提示只打一次，避免空闲进度刷屏。
    bool idleProgressLogged_ = false;
};
