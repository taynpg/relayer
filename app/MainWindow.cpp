#include "MainWindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "model/ProgressTracker.h"
#include "ui/ConnectorControl.h"
#include "ui/Control/ComparisonControl.h"
#include "ui/Control/ExplorerControl.h"
#include "ui/Control/RelayTaskControl.h"
#include "ui/LogControl.h"
#include "ui/Setting.h"
#include "ui/SettingsKeys.h"
#include "ui_MainWindow.h"

using relayer::app::humanBytes;
using relayer::app::settings::kAutoRetryDefault;
using relayer::app::settings::kReceiverAutoRetry;
using relayer::app::settings::kReceiverDir;
using relayer::app::settings::kReceiverRetryInterval;
using relayer::app::settings::kReceiverRetryMax;
using relayer::app::settings::kRetryIntervalDefault;
using relayer::app::settings::kRetryMaxDefault;

namespace
{
// humanBytes 已收拢到 app/model/ProgressTracker.h（原与 RelayTaskControl
// 各有一份相同实现）。

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

    // 本地面板一开始就落在用户目录：空白表格会让人以为功能没做。
    explorer_->navigateTo(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
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

    // 本地与远端**并排常驻**（v1 是各占一半）：远端即使还没选对端也要看得见，
    // 否则点完"浏览此对端的目录"像是没反应。两者是同一个类，只是数据源不同。
    explorer_ = new ExplorerControl(this);
    remoteExplorer_ = new ExplorerControl(this);
    remoteExplorer_->setRemotePeer(QString(), QString());  // 空闲态：显示"<未选择对端>"
    for (auto *e : {explorer_, remoteExplorer_})
    {
        connect(e, &ExplorerControl::notice, this,
                [this](const QString &msg) { log_->appendLog(QStringLiteral("[浏览] %1\n").arg(msg)); });
    }

    auto *localBox = new QGroupBox(QStringLiteral("本地"), this);
    auto *localLay = new QVBoxLayout(localBox);
    localLay->setContentsMargins(4, 4, 4, 4);
    localLay->addWidget(explorer_);

    auto *remoteBox = new QGroupBox(QStringLiteral("远端（对端共享目录）"), this);
    auto *remoteLay = new QVBoxLayout(remoteBox);
    remoteLay->setContentsMargins(4, 4, 4, 4);
    remoteLay->addWidget(remoteExplorer_);

    auto *sBottom = new QSplitter(Qt::Horizontal, this);
    sBottom->setHandleWidth(2);
    sBottom->addWidget(localBox);
    sBottom->addWidget(remoteBox);
    sBottom->setStretchFactor(0, 1);
    sBottom->setStretchFactor(1, 1);

    auto *sMain = new QSplitter(Qt::Vertical, this);
    sMain->setHandleWidth(2);
    sMain->addWidget(sTop);
    sMain->addWidget(sBottom);
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
    connect(connector_, &ConnectorControl::disconnectRequested, this, &MainWindow::onDisconnectRequested);
    connect(connector_, &ConnectorControl::browsePeerRequested, this, &MainWindow::onBrowsePeerRequested);
    connect(remoteExplorer_, &ExplorerControl::remoteListRequested, this,
            [this](const QString &peer, const QString &relPath)
            {
                if (!core_->browsePeerOnSession(peer, relPath))
                {
                    remoteExplorer_->applyRemoteEntries(false, QStringLiteral("当前不能列举（会话不可用或正在传输）"),
                                                        {});
                }
            });

    connect(comparison_, &ComparisonControl::logMessage, log_, &LogControl::appendLog);
    connect(comparison_, &ComparisonControl::exploreLocalRequested, explorer_,
            [this](const QString &path)
            {
                explorer_->navigateTo(path);
                log_->appendLog(QStringLiteral("[浏览] 已跳转 %1\n").arg(path));
            });
    connect(comparison_, &ComparisonControl::transTaskRun, this, &MainWindow::onTransTaskRun);
    connect(comparison_, &ComparisonControl::remoteCheckRequested, this, &MainWindow::onRemoteCheckRequested);
    connect(core_, &relayer::app::CoreService::remoteBrowseFinished, this, &MainWindow::onRemoteBrowseFinished);
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

// 用户在连接面板点了「断开」：真的结束常驻会话。
//
// 为什么不在 ConnectorControl 里直接 cancelCurrent：只有这里知道自动接收
// 的状态机。不清 receiveWanted_ 的话，abort 引发的 operationFinished 会被
// 下面的重试/重挂逻辑当成"接收会话失败"，立刻把连接又拉起来——用户会看到
// "断开之后自己连回去了"。
void MainWindow::onDisconnectRequested()
{
    receiveWanted_ = false;
    receiveRetries_ = 0;

    if (taskControl_->isRunning())
    {
        // 批次进行中：交给 stopBatch——它把当前项硬取消、剩余项标已取消、
        // 收尾批次并通知恢复自动接收（此时 receiveWanted_ 已是 false，
        // 那个通知不会真的重挂）。
        taskControl_->stopBatch();
    }
    else
    {
        // abort 同语义：关闭会话、不发 cancel 帧；onDone 带回"中止"。
        core_->cancelCurrent();
    }
    log_->appendLog(QStringLiteral("[连接] 已断开（会话结束，本机 id 作废）\n"));
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
    // 检查远端的队列挂在常驻会话上：会话一结束，队列里的请求就不会再有
    // 回音了。不清掉的话界面会永远停在"检查中"。
    if (remoteCheckIndex_ >= 0)
    {
        remoteChecks_.clear();
        remoteCheckIndex_ = -1;
        log_->appendLog(QStringLiteral("[检查] 会话结束，检查中止\n"));
    }

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

// 对照表右键"检查远端"：逐行问对方"这个文件在不在这、大小对不对"。
//
// 这是"粗校验"——比对的是文件名 + 大小，不是内容哈希。理由：哈希要对方
// 读整个文件，成本与传一遍差不多；而"发错人 / 发错目录"这类事故里，
// 文件在不在这、大小对不对已经能看出来。内容级校验留给传输后的校验和。
void MainWindow::onRemoteCheckRequested(const relayer::app::RelayTaskItems &items)
{
    if (!core_->busy())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先连接中继——检查远端要靠常驻接收会话，"
                                            "新开连接会把本机 id 换掉。"));
        return;
    }
    if (remoteCheckIndex_ >= 0)
    {
        log_->appendLog(QStringLiteral("[检查] 上一批还没跑完，请等它结束\n"));
        return;
    }

    const QString defaultPeer = connector_->currentPeerId();
    remoteChecks_.clear();
    for (const auto &it : items)
    {
        const QString peer = it.peerId.isEmpty() ? defaultPeer : it.peerId;
        if (peer.isEmpty())
        {
            log_->appendLog(QStringLiteral("[检查] %1：没有指定对端，跳过\n").arg(it.name));
            continue;
        }
        if (it.isDir)
        {
            // 目录的"是否已有"没有意义（对方可能只收到一半）。真要支持，
            // 得递归比对，那是另一件事。
            log_->appendLog(QStringLiteral("[检查] %1：目录行暂不支持，跳过\n").arg(it.name));
            continue;
        }
        RemoteCheck rc;
        rc.peer = peer;
        rc.relDir = it.relDir;
        rc.name = it.name;
        rc.localSize = QFileInfo(QDir(it.localDir).filePath(it.name)).size();
        remoteChecks_.append(rc);
    }

    if (remoteChecks_.isEmpty())
    {
        return;
    }
    remoteCheckIndex_ = 0;
    log_->appendLog(QStringLiteral("[检查] 开始检查 %1 项\n").arg(remoteChecks_.size()));
    nextRemoteCheck();
}

void MainWindow::onBrowsePeerRequested(const QString &peerId, const QString &peerName)
{
    if (peerId.isEmpty())
    {
        return;
    }
    if (!core_->busy())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接中继——浏览对端要走常驻接收会话。"));
        return;
    }
    // v1 的语义：浏览面板只有一套，切的是数据源（AskType → Source）。
    // 不做独立窗口——那样会把表格/导航/筛选复制一遍，且以后做本地↔远端
    // 拖拽时两个窗口之间没有共享状态可用。
    remoteExplorer_->setRemotePeer(peerId, peerName);
    log_->appendLog(QStringLiteral("[浏览] 远端面板切到 %1 的共享目录\n").arg(peerId));
}

void MainWindow::nextRemoteCheck()
{
    while (remoteCheckIndex_ < remoteChecks_.size())
    {
        const auto &rc = remoteChecks_.at(remoteCheckIndex_);
        if (core_->browsePeerOnSession(rc.peer, rc.relDir))
        {
            return;  // 等 remoteBrowseFinished
        }
        log_->appendLog(QStringLiteral("[检查] %1：当前无法列举（会话不可用或正忙）\n").arg(rc.name));
        ++remoteCheckIndex_;
    }
    remoteChecks_.clear();
    remoteCheckIndex_ = -1;
    log_->appendLog(QStringLiteral("[检查] 结束\n"));
}

void MainWindow::onRemoteBrowseFinished(bool ok, const QString &summary, const QList<relayer::app::RemoteRow> &rows)
{
    if (remoteCheckIndex_ < 0 || remoteCheckIndex_ >= remoteChecks_.size())
    {
        return;
    }
    const auto rc = remoteChecks_.at(remoteCheckIndex_);
    ++remoteCheckIndex_;

    if (!ok)
    {
        // 最常见的两种：对方没开共享（拒绝），或目录不存在。两者都让
        // "对方没有这个文件"成立，但原因要写清楚，否则用户会以为传失败了。
        log_->appendLog(QStringLiteral("[检查] %1：无法确认（%2）\n").arg(rc.name, summary));
        nextRemoteCheck();
        return;
    }

    const auto it =
        std::find_if(rows.begin(), rows.end(), [&](const relayer::app::RemoteRow &r) { return r.name == rc.name; });
    if (it == rows.end())
    {
        log_->appendLog(QStringLiteral("[检查] %1：对方没有（%2:%3 下未找到）\n")
                            .arg(rc.name, rc.peer, rc.relDir.isEmpty() ? QStringLiteral("<根>") : rc.relDir));
    }
    else if (static_cast<qint64>(it->size) != rc.localSize)
    {
        log_->appendLog(QStringLiteral("[检查] %1：大小不符（本地 %2，对方 %3）\n")
                            .arg(rc.name)
                            .arg(rc.localSize)
                            .arg(static_cast<qint64>(it->size)));
    }
    else
    {
        log_->appendLog(QStringLiteral("[检查] %1：已存在且大小一致\n").arg(rc.name));
    }
    nextRemoteCheck();
}

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
