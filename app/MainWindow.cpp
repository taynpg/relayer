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
#include "ui/Control/RelayTaskControl.h"
#include "ui/LogControl.h"
#include "ui/Setting.h"
#include "ui/SettingsKeys.h"
#include "ui_MainWindow.h"

using relayer::app::settings::kAutoRetryDefault;
using relayer::app::settings::kReceiverAutoRetry;
using relayer::app::settings::kReceiverDir;
using relayer::app::settings::kReceiverRetryInterval;
using relayer::app::settings::kReceiverRetryMax;
using relayer::app::settings::kRetryIntervalDefault;
using relayer::app::settings::kRetryMaxDefault;

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
    receiveDir_ = settings.value(kReceiverDir, defaultReceiveDir()).toString();

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
    taskControl_ = new relayer::app::RelayTaskControl(this);
    taskControl_->attachCoreService(core_);
    auto *tabWidget = new QTabWidget(this);
    tabWidget->addTab(log_, QStringLiteral("日志"));
    tabWidget->addTab(taskControl_, QStringLiteral("任务"));
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
    connect(taskControl_, &relayer::app::RelayTaskControl::logMessage, log_, &LogControl::appendLog);
    connect(taskControl_, &relayer::app::RelayTaskControl::batchFinished, this,
            [this](bool)
            {
                // 批次结束（成败皆然）：传输通道空出来，恢复自动接收。
                // startAutoReceive 自带 busy/receiveActive 检查，重复调用无害。
                if (receiveWanted_)
                {
                    startAutoReceive();
                }
            });
    connect(log_, &LogControl::receiveDirChanged, this,
            [this](const QString &dir)
            {
                receiveDir_ = dir;
                log_->appendLog(QStringLiteral("[接收] 落点目录已改为 %1\n").arg(dir));
            });

    connect(core_, &relayer::app::CoreService::progressChanged, this, &MainWindow::onProgress);
    connect(core_, &relayer::app::CoreService::operationFinished, this, &MainWindow::onOperationFinished);
    connect(core_, &relayer::app::CoreService::sendFinished, this, &MainWindow::onSendFinished);
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

    // 常驻会话活着时发送直接搭在那条连接上（不换 id，也不打断接收），
    // 所以这里不再需要"接收让位"；没有会话时会回落一次性连接。
    if (core_->sendInFlight())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有文件正在发送，请等它结束"));
        return;
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

void MainWindow::onSendFinished(bool ok, const QString &summary, const QString &detail)
{
    // 批次进行中：结论由任务表单按行消费并记日志，这里不重复打。
    if (!taskControl_->isRunning())
    {
        log_->appendLog(QStringLiteral("%1%2\n").arg(
            summary, (!ok && !detail.isEmpty()) ? QStringLiteral("（%1）").arg(detail) : QString()));
    }

    // 常驻会话上的发送不会翻 busy（会话还在收）；只有回落的一次性连接
    // 发送结束后通道才空出来，那时按需重挂自动接收。
    if (receiveWanted_ && !core_->busy())
    {
        startAutoReceive();
    }
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

    // 任务批次进行中：推进与展示由 RelayTaskControl 接管（它也在收同一条
    // operationFinished），通道让给任务链；批次结束时经 batchFinished
    // 恢复自动接收。
    if (taskControl_->isRunning())
    {
        return;
    }

    // 常驻接收收完文件**不会** finished；能到这里且接收没挂着，说明会话
    // 被抢占让位或失败断开——只要用户还想要接收、通道空闲，就重挂。
    if (!receiveWanted_ || receiveActive_ || core_->busy() || core_->hasQueuedWork())
    {
        return;
    }
    if (wasReceiving && !ok)
    {
        // 接收会话自身失败（中继没了、认证被拒……）：按设置里的重试
        // 策略延迟重试（Setting 对话框可配；默认 3 次 × 3 秒，次数
        // 0 表示一直重试）。关闭重试或次数用尽则停下来等用户处理。
        QSettings settings;
        const bool autoRetry = settings.value(kReceiverAutoRetry, kAutoRetryDefault).toBool();
        const int intervalSec = qMax(1, settings.value(kReceiverRetryInterval, kRetryIntervalDefault).toInt());
        const int maxRetry = qMax(0, settings.value(kReceiverRetryMax, kRetryMaxDefault).toInt());
        if (autoRetry && (maxRetry == 0 || ++receiveRetries_ <= maxRetry))
        {
            const QString countText =
                maxRetry == 0 ? QString() : QStringLiteral("（第 %1/%2 次）").arg(receiveRetries_).arg(maxRetry);
            log_->appendLog(QStringLiteral("[接收] %1 秒后重试%2\n").arg(intervalSec).arg(countText));
            QTimer::singleShot(intervalSec * 1000, this, &MainWindow::startAutoReceive);
        }
        else
        {
            receiveRetries_ = 0;
            log_->appendLog(QStringLiteral("[接收] 已停止自动重试，请检查中继或在设置里调整后点「连接」恢复\n"));
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
// 任务入口（E3.5）：只做闸门检查，展示与执行交给 RelayTaskControl。
// ---------------------------------------------------------------------------

void MainWindow::onTransTaskRun(const relayer::app::RelayTaskItems &items)
{
    if (taskControl_->isRunning())
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
    if (core_->sendInFlight())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有文件正在发送，请等它结束"));
        return;
    }
    if (lastConn_.host.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先在连接面板连接中继"));
        return;
    }

    // 整批都没填对端、也没设当前通信对象：没有任何收件人，早说比让任务
    // 表刷一排"已跳过"好。只有部分行空的情况交给任务表单按行处理。
    const QString defaultPeer = connector_->currentPeerId();

    // 填了对端的行要校验（"只有填了才验证"）：
    //   - 与"当前通信对象"不一致 → 直接拦下。这两个值同时存在时代表两种
    //     意图，用户明确要求拦住而不是猜他要发哪个；
    //   - 不在最新在线列表里 → 只提示。列表可能是旧的（对端刚上线还没
    //     刷新），硬拦会误伤，发送本身会在 10 秒应答超时后给出结论。
    for (int i = 0; i < items.size(); ++i)
    {
        const QString &peer = items.at(i).peerId;
        if (peer.isEmpty())
        {
            continue;
        }
        if (!defaultPeer.isEmpty() && peer != defaultPeer)
        {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("第 %1 行对端 %2 与当前通信对象 %3 不一致。\n"
                                                "要发当前通信对象就把该行对端留空；要发别处请先在连接面板"
                                                "把它设为通信对象。")
                                     .arg(i + 1)
                                     .arg(peer, defaultPeer));
            return;
        }
        if (!connector_->isPeerOnline(peer))
        {
            log_->appendLog(
                QStringLiteral("[任务] 对端 %1 不在最新在线列表里，仍会尝试发送（可先点「刷新」）\n").arg(peer));
        }
    }

    if (defaultPeer.isEmpty())
    {
        bool allEmpty = true;
        for (const auto &it : items)
        {
            if (!it.peerId.isEmpty())
            {
                allEmpty = false;
                break;
            }
        }
        if (allEmpty)
        {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("这批任务没有指定对端，也没有选「当前通信对象」。\n"
                                                "请在连接面板右键在线对端 → 设为通信对象，或在对照表里填对端 id。"));
            return;
        }
    }

    // 批次直接搭在常驻会话上发（不新建连接 → id 不变，接收也不中断），
    // 所以不再需要"接收让位"。
    taskControl_->startTasks(items, lastConn_, defaultPeer);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    receiveWanted_ = false;
    taskControl_->stopBatch();
    core_->cancelCurrent();
    event->accept();
}
