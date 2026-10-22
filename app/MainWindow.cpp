#include "MainWindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/ConnectorControl.h"
#include "ui/Control/ComparisonControl.h"
#include "ui/Control/ExplorerControl.h"
#include "ui/LogControl.h"
#include "ui_MainWindow.h"

namespace
{
QString humanBytes(quint64 n)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        ++i;
    }
    return QString::number(v, 'f', 1) + QLatin1String(units[i]);
}

QString defaultReceiveDir()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}
}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
    , ui_(new Ui::MainWindow)
    , core_(new relayer::app::CoreService(this))
{
    ui_->setupUi(this);

    connector_ = new ConnectorControl(this);
    log_ = new LogControl(this);
    connector_->attachCoreService(core_);

    buildLayout();
    wireSignals();

    setWindowIcon(QIcon(":/Resource/Gui.ico"));
    setWindowTitle(QStringLiteral("relayer v%1").arg(RELAYER_VERSION));

    QSettings settings;
    receiveDir_ = settings.value("receiver/dir", defaultReceiveDir()).toString();

    log_->appendLog(QStringLiteral("relayer v%1\n").arg(RELAYER_VERSION));
}

MainWindow::~MainWindow()
{
    delete ui_;
}

void MainWindow::buildLayout()
{
    // 镜像 v1 relayFile.cpp initLayout：上（连接面板 + 选项卡）/ 下（文件浏览）。
    // 垂直 splitter 让下半部的浏览区可拖大——大目录浏览时这是高频操作。
    comparison_ = new ComparisonControl(this);
    auto *tabWidget = new QTabWidget(this);
    tabWidget->addTab(log_, QStringLiteral("日志"));
    tabWidget->addTab(comparison_, QStringLiteral("对照表"));

    auto *sTop = new QSplitter(Qt::Horizontal, this);
    sTop->setHandleWidth(0);
    sTop->addWidget(connector_);
    sTop->addWidget(tabWidget);
    sTop->setStretchFactor(0, 0);
    sTop->setStretchFactor(1, 1);

    explorer_ = new ExplorerControl(this);
    connect(explorer_, &ExplorerControl::notice, this,
            [this](const QString &msg) { log_->appendLog(QStringLiteral("[浏览] %1\n").arg(msg)); });

    auto *sMain = new QSplitter(Qt::Vertical, this);
    sMain->setHandleWidth(2);
    sMain->addWidget(sTop);
    sMain->addWidget(explorer_);
    sMain->setStretchFactor(0, 0);
    sMain->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sMain);
}

void MainWindow::wireSignals()
{
    connect(connector_, &ConnectorControl::sendToPeerRequested, this, &MainWindow::onSendToPeer);
    connect(connector_, &ConnectorControl::logMessage, log_, &LogControl::appendLog);
    connect(connector_, &ConnectorControl::probeRequested, this, &MainWindow::onProbeRequested);

    connect(comparison_, &ComparisonControl::logMessage, log_, &LogControl::appendLog);
    connect(comparison_, &ComparisonControl::exploreLocalRequested, explorer_,
            [this](const QString &path)
            {
                explorer_->navigateTo(path);
                log_->appendLog(QStringLiteral("[浏览] 已跳转 %1\n").arg(path));
            });
    connect(comparison_, &ComparisonControl::transTaskRun, this, &MainWindow::onTransTaskRun);
    connect(log_, &LogControl::receiveDirChanged, this,
            [this](const QString &dir)
            {
                receiveDir_ = dir;
                log_->appendLog(QStringLiteral("[接收] 落点目录已改为 %1\n").arg(dir));
            });

    connect(core_, &relayer::app::CoreService::progressChanged, this, &MainWindow::onProgress);
    connect(core_, &relayer::app::CoreService::operationFinished, this, &MainWindow::onOperationFinished);
    connect(core_, &relayer::app::CoreService::assignedIdChanged, this, &MainWindow::onAssignedIdChanged);
    connect(core_, &relayer::app::CoreService::fileReceived, this,
            [this](const QString &targetPath)
            {
                log_->appendLog(QStringLiteral("[接收完成] %1\n").arg(targetPath));
                // 常驻接收继续等下一个文件；重置空闲提示，让"等待文件……"
                // 再出现一次，用户才知道会话还活着。
                idleProgressLogged_ = false;
            });
    connect(core_, &relayer::app::CoreService::receiveFailed, this,
            [this](const QString &message)
            {
                // 常驻接收下的单次失败：会话还活着（id 不变），原地继续等。
                // 这不是操作结束，只记一条日志；操作结束（要重连、换 id）
                // 走 operationFinished 那条路。
                log_->appendLog(QStringLiteral("%1（会话保持，继续等待下一个文件）\n").arg(message));
                idleProgressLogged_ = false;
            });
}

void MainWindow::onProbeRequested(const relayer::app::ConnInfo &conn)
{
    lastConn_ = conn;

    // 连接/刷新都汇聚到这里（ConnectorControl 只做参数校验）：
    //   - 常驻接收会话活着 → 在现有连接上刷列表（id 不变，会话不动）；
    //   - 会话不在 → 建立常驻接收会话，认证拿到 id 后 Client 自动问
    //     列表（askListOnStart）。整条链路一条连接、一个 id——早先
    //     "先开临时列表连接再挂接收"的流程一次连接要消耗两个 id，
    //     是"点一次刷新 id 涨一号"的另一半根因。
    receiveWanted_ = true;
    receiveRetries_ = 0;

    if (core_->busy())
    {
        if (core_->canPreempt() && !core_->hasQueuedWork() && core_->refreshPeers(conn))
        {
            return;
        }
        // 传输中（或刚好有被抢占排队的动作）：不动会话，提示即可。
        log_->appendLog(QStringLiteral("[连接] 有传输正在进行，本次连接/刷新已忽略\n"));
        return;
    }
    startAutoReceive();
}

void MainWindow::onAssignedIdChanged(const QString &id)
{
    log_->appendLog(QStringLiteral("[身份] 本机 id = %1\n").arg(id));

    // 拿到身份即开始自动接收。v1 是常驻连接天然在线；v2 的"在线"由
    // 常驻接收会话维持（keepReceiving：收完文件不断开，id 保持稳定）——
    // 这是 GUI 用户能被别人发文件、且发送方的对端表不过期的前提。
    receiveWanted_ = true;
    receiveRetries_ = 0;
    startAutoReceive();
}

void MainWindow::startAutoReceive()
{
    if (!receiveWanted_ || receiveActive_ || core_->busy())
    {
        return;
    }
    receiveActive_ = true;
    log_->appendLog(QStringLiteral("[接收] 等待接收，落点 %1\n").arg(receiveDir_));
    core_->startReceive(receiveDir_, lastConn_);
}

void MainWindow::onSendToPeer(const QString &peerId, const QString &peerName)
{
    if (peerId.isEmpty())
    {
        return;
    }

    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (file.isEmpty())
    {
        return;
    }

    // 挂起的接收让位给发送（CoreService 抢占），发送结束自动重挂——
    // 否则"等文件"这个永不结束的操作会把发送通道永久堵死。
    if (core_->busy() && core_->canPreempt())
    {
        log_->appendLog(QStringLiteral("[接收] 让位于发送，结束后自动恢复\n"));
    }
    if (!core_->sendFile(file, QString(), peerId, lastConn_))
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有传输正在进行，请等它结束"));
        return;
    }

    lastProgressLog_ = std::chrono::steady_clock::now();
    log_->appendLog(QStringLiteral("[发送] %1 → %2（%3）\n").arg(QDir(file).dirName(), peerId, peerName));
}

void MainWindow::onProgress(quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond)
{
    // 接收挂起等文件时总量为 0，进度没有信息量。cli::Client 的进度定时器
    // 仍会 100ms 一次地报零值，全打出来就是一屏"进度 -- 0.0B"——只提示
    // 一次"等待文件"，等真正有总量了再恢复进度日志。
    if (bytesTotal == 0)
    {
        if (!idleProgressLogged_)
        {
            idleProgressLogged_ = true;
            log_->appendLog(QStringLiteral("  等待文件……\n"));
        }
        return;
    }
    idleProgressLogged_ = false;

    // 节流到每秒一条：进度本来 100ms 一次，全量写日志会淹没结论行。
    const auto now = std::chrono::steady_clock::now();
    if (now - lastProgressLog_ < std::chrono::seconds(1))
    {
        return;
    }
    lastProgressLog_ = now;

    const QString pct =
        bytesTotal > 0
            ? QString::number(100.0 * static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 'f', 1) +
                  QStringLiteral("%")
            : QStringLiteral("--");
    log_->appendLog(
        QStringLiteral("  进度 %1  %2 / %3  %4/s\n")
            .arg(pct, humanBytes(bytesDone), humanBytes(bytesTotal),
                 bytesPerSecond > 0 ? humanBytes(static_cast<quint64>(bytesPerSecond)) : QStringLiteral("-")));
}

void MainWindow::onOperationFinished(bool ok, const QString &summary, const QString &detail)
{
    // wasReceiving：结束的是不是接收会话本身（失败重试只对它计数）。
    const bool wasReceiving = receiveActive_;
    if (receiveActive_)
    {
        receiveActive_ = false;
    }

    log_->appendLog(QStringLiteral("%1%2\n").arg(
        summary, (!ok && !detail.isEmpty()) ? QStringLiteral("（%1）").arg(detail) : QString()));

    // 任务链的推进在这里接管：一步的结束就是下一步的开始信号。
    if (taskRunning_)
    {
        if (!ok)
        {
            log_->appendLog(
                QStringLiteral("[任务] 第 %1/%2 项失败，剩余项中止\n").arg(taskIndex_ + 1).arg(taskQueue_.size()));
            taskQueue_.clear();
            taskRunning_ = false;
        }
        else
        {
            ++taskIndex_;
            startNextTaskItem();
        }
        return;  // 任务进行中不恢复自动接收，通道让给任务
    }

    // 常驻接收收完文件**不会** finished；能到这里且接收没挂着，说明会话
    // 被抢占让位或失败断开——只要用户还想要接收、通道空闲，就重挂。
    if (!receiveWanted_ || receiveActive_ || core_->busy() || core_->hasQueuedWork())
    {
        return;
    }
    if (wasReceiving && !ok)
    {
        // 接收会话自身失败（中继没了、认证被拒……）：延迟有限重试，
        // 不限次的话中继一停日志就无限刷。
        if (++receiveRetries_ <= 3)
        {
            log_->appendLog(QStringLiteral("[接收] 3 秒后重试（第 %1/3 次）\n").arg(receiveRetries_));
            QTimer::singleShot(3000, this, &MainWindow::startAutoReceive);
        }
        else
        {
            receiveRetries_ = 0;
            log_->appendLog(QStringLiteral("[接收] 连续失败已停止重试，请检查中继后点「连接」恢复\n"));
        }
        return;
    }
    if (ok)
    {
        receiveRetries_ = 0;
    }
    startAutoReceive();
}

// ---------------------------------------------------------------------------
// 配置表任务执行器（E3.4 临时实现）
//
// CoreService 一次只跑一个操作，所以顺序逐个发：每步 sendFile 后由
// operationFinished 唤醒下一步。E3.5 的 RelayTask 表单会接管任务展示
// 与控制（暂停/重传/逐行状态），这里只保证链路能跑通。
// ---------------------------------------------------------------------------

void MainWindow::onTransTaskRun(const relayer::app::RelayTaskItems &items)
{
    if (taskRunning_)
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有任务正在进行，请等它结束"));
        return;
    }
    // 忙但只是接收挂起：可抢占，继续；传输中：拒绝。
    if (core_->busy() && !core_->canPreempt())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有传输正在进行，请等它结束"));
        return;
    }
    if (lastConn_.host.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先在连接面板连接中继"));
        return;
    }

    taskQueue_.clear();
    taskIndex_ = 0;

    // 目录行展开为文件序列：目录内容落到 relDir/name/ 之下（与 v1 的
    // 发送语义一致）。文件行落 relDir/name。
    for (const auto &item : items)
    {
        if (!item.isDir)
        {
            // 拖入的文件行 localDir 就是文件完整路径；手填行是"目录 + 名称"。
            QString filePath = item.localDir;
            if (QFileInfo(filePath).isDir())
            {
                filePath = QDir(filePath).filePath(item.name);
            }
            taskQueue_.push_back({item.name, false, filePath, item.peerId,
                                  item.relDir.isEmpty() ? item.name : item.relDir + QStringLiteral("/") + item.name});
            continue;
        }

        // 目录行：localDir 是目录完整路径（拖入语义），root = relDir/name。
        const QString rootRel = item.relDir.isEmpty() ? item.name : item.relDir + QStringLiteral("/") + item.name;
        QDirIterator it(item.localDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString abs = it.next();
            const QString rel = QDir(item.localDir).relativeFilePath(abs);
            taskQueue_.push_back(
                {QFileInfo(abs).fileName(), false, abs, item.peerId, rootRel + QStringLiteral("/") + rel});
        }
    }

    if (taskQueue_.isEmpty())
    {
        log_->appendLog(QStringLiteral("[任务] 没有可发送的文件（目录为空？）\n"));
        return;
    }
    taskRunning_ = true;
    if (core_->busy() && core_->canPreempt())
    {
        log_->appendLog(QStringLiteral("[接收] 让位于任务，结束后自动恢复\n"));
    }
    log_->appendLog(QStringLiteral("[任务] 开始：%1 个文件\n").arg(taskQueue_.size()));
    startNextTaskItem();
}

void MainWindow::startNextTaskItem()
{
    if (taskIndex_ >= taskQueue_.size())
    {
        log_->appendLog(QStringLiteral("[任务] 全部完成（%1 个文件）\n").arg(taskQueue_.size()));
        taskQueue_.clear();
        taskRunning_ = false;
        if (receiveWanted_)
        {
            startAutoReceive();
        }
        return;
    }

    const auto &t = taskQueue_.at(taskIndex_);
    log_->appendLog(QStringLiteral("[任务] %1/%2  %3 → %4:%5\n")
                        .arg(taskIndex_ + 1)
                        .arg(taskQueue_.size())
                        .arg(QFileInfo(t.localDir).fileName(), t.peerId, t.relDir));
    if (!core_->sendFile(t.localDir, t.relDir, t.peerId, lastConn_))
    {
        // 防御：入口已挡过真忙，到这说明通道被意外占住。宁可中止任务
        // 也不能让任务链卡在"等一个永远不来的 operationFinished"。
        log_->appendLog(QStringLiteral("[任务] 通道被占用，任务中止\n"));
        taskQueue_.clear();
        taskRunning_ = false;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    receiveWanted_ = false;
    taskQueue_.clear();
    taskRunning_ = false;
    core_->cancelCurrent();
    event->accept();
}
