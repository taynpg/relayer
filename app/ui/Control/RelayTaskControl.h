#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QSet>

#include "bridge/CoreService.h"
#include "model/ProgressTracker.h"
#include "model/TransferTask.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace relayer::app
{

// 任务表单（E3.5）：v1 Form/RelayTask 对话框的嵌入版。
//
// 与 v1 的差异（都有明确理由）：
//   - 不做模态对话框，做成主窗口"任务"选项卡：批次往往持续几分钟到
//     更久，模态会锁死整个界面（v1 实测的痛点），用户需要同时看日志、
//     改对照表。
//   - 分页先不做：v2 的批次由对照表逐行勾选产生，单批规模远小于 v1
//     的"整个目录树"；QTableWidget 万行内没有性能问题。真到需要时
//     再搬 v1 的分页代码。
//   - 压缩归档（tar.xz 打包）不在本控件做：那是阶段 F 的远端浏览/
//     下载能力，v2 协议目前没有"请求对端打包"的帧。
//
// 执行模型：CoreService 一次只跑一个操作，批次内顺序逐个 sendFile，
// 由 operationFinished 链式推进。**单个文件失败不中断整批**（v1 的
// 正确决策），但连接级失败（中继断开/对端失联）会终止——链路没了，
// 剩余文件只会逐个等满 answerTimeout。
class RelayTaskControl : public QDialog
{
    Q_OBJECT

public:
    explicit RelayTaskControl(QWidget *parent = nullptr);

    // 注入 CoreService 并接线（与 ConnectorControl::attachCoreService
    // 同一约定：构造顺序不敏感，依赖显式可见）。
    void attachCoreService(relayer::app::CoreService *core);

    bool isRunning() const noexcept
    {
        return running_;
    }

public slots:
    // 开始一批任务。items 里目录行在此展开为文件序列（从 MainWindow
    // 的临时执行器迁来）。conn 是发送用的连接参数（MainWindow 持有）。
    //
    // defaultPeerId 是连接面板右键选定的"当前通信对象"：对端列留空的行
    // 默认发给它（不额外验证——选它本身就是用户的授权）；填了对端的行
    // 才按填的 id 发送。两者都空时该行标"已跳过"。
    void startTasks(const relayer::app::RelayTaskItems &items, const relayer::app::ConnInfo &conn,
                    const QString &defaultPeerId = QString());

    // 停止批次：当前项硬取消，剩余等待项标"已取消"。批次结束照常发
    // batchFinished（ok=false）。
    void stopBatch();

signals:
    void logMessage(const QString &msg);

    // 批次结束（无论成败）。MainWindow 靠它恢复自动接收——批次进行中
    // 传输通道让给任务，接收不能抢。
    void batchFinished(bool allOk);

private slots:
    // 单个文件的发送结论（CoreService::sendFinished）。批次逐行推进靠它。
    // 并发发送里某一项的结论。taskId = 行号 + 1（0 保留给单文件发送）。
    void onSendTaskFinished(qint64 taskId, bool ok, const QString &summary, const QString &detail);
    // 会话级结论（CoreService::operationFinished）：中继断开/认证失败等，
    // 当前项不会再有 sendFinished，批次就此中止。
    void onSessionEnded(bool ok, const QString &summary, const QString &detail);
    void onSendTaskProgress(qint64 taskId, quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond);
    void onRetryFailed();
    void onStopClicked();
    void onClearClicked();

public:
    // 行状态。公开是因为 .cpp 的匿名命名空间辅助函数（状态文本/颜色）
    // 要按它分派。
    enum Status
    {
        kWait,       // 等待
        kTransing,   // 传输中
        kDone,       // 完成
        kFailed,     // 失败
        kSkipped,    // 跳过（未指定对端且未选定通信对象 / 连接断开后剩余）
        kCancelled,  // 已取消
    };

    struct Row
    {
        QString name;       // 文件名（不含路径）
        QString localPath;  // 完整本地路径
        QString peerId;     // 目标对端
        QString relDir;     // 对端接收根目录之下的相对路径（含文件名）
        quint64 size = 0;
        Status status = kWait;
    };

    void buildLayout();
    // 把并发窗口补满：从 rows_ 里找还没开始的行，启动直到 active_ 达到上限。
    void fillWindow();
    void finishBatch(bool allOk);
    void rebuildTable();
    void setStatus(int row, Status st);
    void refreshRowCells(int row);
    void updateButtons();
    void updateSummary();

    QTableWidget *table_ = nullptr;
    QPushButton *btnStop_ = nullptr;
    QPushButton *btnRetryFailed_ = nullptr;
    QPushButton *btnClear_ = nullptr;
    QLabel *summaryLabel_ = nullptr;

    relayer::app::CoreService *core_ = nullptr;
    relayer::app::ConnInfo conn_;

    QList<Row> rows_;

    // 同时进行的行（0 基行号）。阶段 F 的多任务并行：对照表里发往 B/C/D
    // 的行同时在跑，而不是排成一队。
    //
    // 上限 kConcurrency 是有意的：每多一路就多一条连接、多一份握手，
    // 而总吞吐由网络决定，开太多只会让每条流都变慢、进度条一起爬。
    QSet<int> active_;

    // 每行独立的进度跟踪。**不能共用一个**——并发时 B 的速度会画到 C 的
    // 那一行上，用户看到的是"两个任务速度一样"的假象。
    QHash<int, relayer::app::ProgressTracker> trackers_;

    bool running_ = false;
    bool stopping_ = false;
};

}  // namespace relayer::app
