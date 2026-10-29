#include "MainWindow.h"

#include <QAbstractButton>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "Base/InfoDrop.h"
#include "model/PathText.h"
#include "model/ProgressTracker.h"
#include "relayer/filestore/StdFileStore.h"
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
using relayer::app::settings::kSendCheckBeforeSend;
using relayer::app::settings::kSendCheckDefault;

namespace {
// humanBytes 已收拢到 app/model/ProgressTracker.h（原与 RelayTaskControl
// 各有一份相同实现）。

QString defaultReceiveDir()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}
}   // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent), ui_(new Ui::MainWindow), core_(new relayer::app::CoreService(this))
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

    restoreWindowGeometry();
}

// v1 relayFile::initAfter 的规则，一处不落：
//   1. 有几何数据且 restoreGeometry 成功 → 用它（它自己会恢复最大化状态）；
//   2. 恢复出来的矩形与本机任一屏幕都不相交（外接屏已经拔掉）→ 居中到主屏；
//   3. 没有几何数据（首次启动 / 旧配置）→ 主屏 70%，并限制在 90% 以内再居中。
//
// 第 3 步的上限不是洁癖：如果哪天真存进了一个"接近整屏的宽高"，不限制的话
// 窗口边框会被任务栏吃掉一截，而用户没有明显手段拖回来。
void MainWindow::restoreWindowGeometry()
{
    QSettings settings;
    const QByteArray saved = settings.value(relayer::app::settings::kWindowGeometry).toByteArray();
    if (!saved.isEmpty() && restoreGeometry(saved)) {
        const QRect current = geometry();
        bool onScreen = false;
        const auto screens = QGuiApplication::screens();
        for (const QScreen* screen : screens) {
            if (screen->availableGeometry().intersects(current)) {
                onScreen = true;
                break;
            }
        }
        if (onScreen || screens.isEmpty()) {
            return;   // 最大化状态由 Qt 在首次 show 时恢复
        }
        const QRect avail = screens.first()->availableGeometry();
        move(avail.x() + (avail.width() - current.width()) / 2, avail.y() + (avail.height() - current.height()) / 2);
        return;
    }

    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    int w = static_cast<int>(avail.width() * 0.7);
    int h = static_cast<int>(avail.height() * 0.7);
    w = qMin(w, static_cast<int>(avail.width() * 0.9));
    h = qMin(h, static_cast<int>(avail.height() * 0.9));
    resize(w, h);
    move(avail.x() + (avail.width() - w) / 2, avail.y() + (avail.height() - h) / 2);
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
    auto* tabWidget = new QTabWidget(this);
    tabWidget->addTab(log_, QStringLiteral("日志"));
    tabWidget->addTab(taskControl_, QStringLiteral("任务"));
    tabWidget->addTab(comparison_, QStringLiteral("对照表"));

    auto* sTop = new QSplitter(Qt::Horizontal, this);
    sTop->setHandleWidth(0);
    sTop->addWidget(connector_);
    sTop->addWidget(tabWidget);
    sTop->setStretchFactor(0, 0);
    sTop->setStretchFactor(1, 1);

    // 本地与远端**并排常驻**（v1 是各占一半）：远端即使还没选对端也要看得见，
    // 否则点完"浏览此对端的目录"像是没反应。两者是同一个类，只是数据源不同。
    explorer_ = new ExplorerControl(this);
    remoteExplorer_ = new ExplorerControl(this);
    remoteExplorer_->setRemotePeer(QString(), QString());   // 空闲态：显示"<未选择对端>"
    for (auto* e : {explorer_, remoteExplorer_}) {
        connect(e, &ExplorerControl::notice, this,
                [this](const QString& msg) { log_->appendLog(QStringLiteral("[浏览] %1\n").arg(msg)); });
    }

    auto* localBox = new QGroupBox(QStringLiteral("本地"), this);
    auto* localLay = new QVBoxLayout(localBox);
    localLay->setContentsMargins(4, 4, 4, 4);
    localLay->addWidget(explorer_);

    remoteBox_ = new QGroupBox(QStringLiteral("远端（未选择对端）"), this);
    auto* remoteLay = new QVBoxLayout(remoteBox_);
    remoteLay->setContentsMargins(4, 4, 4, 4);
    remoteLay->addWidget(remoteExplorer_);

    auto* sBottom = new QSplitter(Qt::Horizontal, this);
    sBottom->setHandleWidth(2);
    sBottom->addWidget(localBox);
    sBottom->addWidget(remoteBox_);
    sBottom->setStretchFactor(0, 1);
    sBottom->setStretchFactor(1, 1);

    auto* sMain = new QSplitter(Qt::Vertical, this);
    sMain->setHandleWidth(2);
    sMain->addWidget(sTop);
    sMain->addWidget(sBottom);
    sMain->setStretchFactor(0, 0);
    sMain->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(sMain);
}

void MainWindow::wireSignals()
{
    connect(connector_, &ConnectorControl::sendToPeerRequested, this, &MainWindow::onSendToPeer);
    connect(connector_, &ConnectorControl::logMessage, log_, &LogControl::appendLog);
    connect(connector_, &ConnectorControl::probeRequested, this, &MainWindow::onProbeRequested);
    connect(connector_, &ConnectorControl::disconnectRequested, this, &MainWindow::onDisconnectRequested);
    connect(connector_, &ConnectorControl::browsePeerRequested, this, &MainWindow::onBrowsePeerRequested);
    connect(explorer_, &ExplorerControl::compareRequested, this, &MainWindow::onCompareRequested);
    connect(explorer_, &ExplorerControl::localTransferRequested, this, [this](const QStringList& paths) {
        if (paths.isEmpty()) {
            return;
        }
        relayer::app::RelayTaskItems items;
        for (const QString& p : paths) {
            const QFileInfo info(p);
            if (info.fileName().isEmpty()) {
                continue;   // 盘根之类没有名字的条目
            }
            relayer::app::RelayTaskItem item;
            item.name = info.fileName();
            item.isDir = info.isDir();
            // 两种形状（与 RelayTaskControl::startTasks 的展开逻辑对应）：
            //   - 文件：localDir 给父目录，它会拼上 name；
            //   - 目录：localDir 给目录本身，它会递归展开。
            item.localDir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
            item.peerId = QString();   // 空 = 用连接面板选定的"当前通信对象"
            items.append(item);
        }
        // 复用对照表那条入口：忙检查、对端校验、批次启动都在里面。
        onTransTaskRun(items);
    });
    connect(remoteExplorer_, &ExplorerControl::remotePullRequested, this,
            [this](const QString& peer, const QString& relPath, bool asArchive) {
                if (!core_->pullFileFromPeer(peer, relPath, asArchive)) {
                    log_->appendLog(QStringLiteral("[取回] %1：当前不能取回（未连接或正在传输）\n").arg(relPath));
                    remoteExplorer_->applyRemoteEntries(false, false, QStringLiteral("当前不能取回（会话不可用或正在传输）"), {});
                }
            });
    connect(core_, &relayer::app::CoreService::archiveExtracted, this,
            [this](bool ok, const QString& dir, const QString& detail) {
                if (ok) {
                    log_->appendLog(QStringLiteral("[取回] 已解包到 %1\n").arg(dir));
                    // 行级下载的"目录行"：包解开才算落地（包本身已被删）。
                    // 目录名与行名一致才算这一行的（见 fileReceived 的说明）。
                    if (downloadWaiting_ && downloadIndex_ >= 0 && downloadIndex_ < downloads_.size()) {
                        const auto job = downloads_.at(downloadIndex_);
                        if (QFileInfo(dir).fileName() == job.name) {
                            finishDownloadStep();
                        }
                    }
                } else {
                    log_->appendLog(QStringLiteral("[取回] %1\n").arg(detail));
                    // 解包失败：包还在接收目录，这一行算"取到了但没解开"，
                    // 不该卡住整个队列。
                    if (downloadWaiting_) {
                        finishDownloadStep();
                    }
                }
            });
    connect(core_, &relayer::app::CoreService::remotePullFinished, this, &MainWindow::onRemotePullFinished);

    // 远端文件操作（§11.12）：面板 → CoreService → 结论回日志 + 刷新面板。
    connect(remoteExplorer_, &ExplorerControl::remoteOpRequested, this, &MainWindow::onRemoteOpRequested);
    connect(core_, &relayer::app::CoreService::remoteFileOpFinished, this, &MainWindow::onRemoteFileOpFinished);

    // 发送前检查与对照表检查共用 kAskPathMeta，靠 pathMetaPurpose_ 分派。
    connect(core_, &relayer::app::CoreService::pathMetaFinished, this, &MainWindow::onPathMetaFinished);
    connect(remoteExplorer_, &ExplorerControl::remoteListRequested, this, [this](const QString& peer, const QString& relPath) {
        if (!core_->browsePeerOnSession(peer, relPath)) {
            remoteExplorer_->applyRemoteEntries(false, false, QStringLiteral("当前不能列举（会话不可用或正在传输）"), {});
        }
    });

    // 跨面板拖拽（v1 的 peerDf 形状：一套代码、两个方向）。
    //
    // 方向由"谁收到了"决定，不由载荷决定：
    //   - 本地表收到 → 载荷来自远端 → 下载，落到**本地表的当前目录**；
    //   - 远端表收到 → 载荷来自本地 → 上传，落到**远端表的当前目录**
    //     （这就是"传到对方某个子目录"——先在对端目录里进到那一层）。
    explorer_->setOnInfoDropped([this](const InfoDrop& drop) {
        if (remoteExplorer_->source() != ExplorerControl::Source::Remote || remoteExplorer_->remotePeerId().isEmpty()) {
            log_->appendLog(QStringLiteral("[拖拽] 远端面板还没有对端，无法下载（先右键对端「浏览此对端的目录」）\n"));
            return;
        }
        const QString peer = remoteExplorer_->remotePeerId();
        const QString relFrom = drop.from;   // 远端绝对目录（载荷 from）

        relayer::app::RelayTaskItems items;
        for (const auto& it : drop.items) {
            relayer::app::RelayTaskItem item;
            item.name = it.fileName;
            item.isDir = it.type == 0;
            item.peerId = peer;
            item.relDir = relFrom;                      // 从对端的这一层取
            item.localDir = explorer_->currentPath();   // 落到本地的这一层
            items.append(item);
        }
        log_->appendLog(QStringLiteral("[拖拽] 下载 %1 项：%2 → %3\n")
                            .arg(items.size())
                            .arg(relFrom.isEmpty() ? QStringLiteral("<根>") : relFrom, explorer_->currentPath()));
        onDownloadRequested(items);
    });

    remoteExplorer_->setOnInfoDropped([this](const InfoDrop& drop) {
        const QString peer = remoteExplorer_->remotePeerId();
        if (peer.isEmpty()) {
            log_->appendLog(QStringLiteral("[拖拽] 远端面板还没有对端，无法上传\n"));
            return;
        }
        const QString localFrom = drop.from;   // 本地绝对目录（载荷 from）

        // 拖到谁的面板就是发给谁：面板标题显示的那个对端，本来就是这个
        // 动作唯一合理的收件人。不切换的话 onTransTaskRun 会以"对端与
        // 当前通信对象不一致"拦下——那对用户来说等于"拖了没反应"。
        if (connector_->currentPeerId() != peer) {
            connector_->setCurrentPeer(peer);
            log_->appendLog(QStringLiteral("[拖拽] 通信对象已切到 %1（拖拽目标就是它）\n").arg(peer));
        }

        relayer::app::RelayTaskItems items;
        for (const auto& it : drop.items) {
            relayer::app::RelayTaskItem item;
            item.name = it.fileName;
            item.isDir = it.type == 0;
            item.peerId = peer;
            // 两种形状（与 RelayTaskControl::startTasks 的展开逻辑对应）：
            // 文件给父目录 + 名字；目录给目录本身（`from` 是它的父目录）。
            item.localDir = item.isDir ? QDir(localFrom).filePath(it.fileName) : localFrom;
            item.relDir = remoteExplorer_->currentPath();   // 落到对端的这一层
            items.append(item);
        }
        log_->appendLog(
            QStringLiteral("[拖拽] 上传 %1 项：%2 → %3:%4\n")
                .arg(items.size())
                .arg(localFrom, peer,
                     remoteExplorer_->currentPath().isEmpty() ? QStringLiteral("<根>") : remoteExplorer_->currentPath()));
        onTransTaskRun(items);
    });

    connect(comparison_, &ComparisonControl::logMessage, log_, &LogControl::appendLog);
    connect(comparison_, &ComparisonControl::exploreLocalRequested, explorer_, [this](const QString& path) {
        explorer_->navigateTo(path);
        log_->appendLog(QStringLiteral("[浏览] 已跳转 %1\n").arg(path));
    });
    connect(comparison_, &ComparisonControl::exploreRemoteRequested, this, [this](const QString& peer, const QString& relDir) {
        // 对端留空 = "当前通信对象"（与发送的解析规则一致）。
        const QString target = peer.isEmpty() ? connector_->currentPeerId() : peer;
        if (target.isEmpty()) {
            log_->appendLog(QStringLiteral("[浏览] 该行没填对端，也没有选「当前通信对象」\n"));
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("该行没有填对端，也没有设「当前通信对象」。"));
            return;
        }
        switchRemotePeer(target, target, relDir);
    });
    connect(comparison_, &ComparisonControl::transTaskRun, this, &MainWindow::onTransTaskRun);
    connect(comparison_, &ComparisonControl::remoteCheckRequested, this, &MainWindow::onRemoteCheckRequested);
    connect(comparison_, &ComparisonControl::downloadRequested, this, &MainWindow::onDownloadRequested);
    connect(core_, &relayer::app::CoreService::remoteBrowseFinished, this, &MainWindow::onRemoteBrowseFinished);
    connect(taskControl_, &relayer::app::RelayTaskControl::logMessage, log_, &LogControl::appendLog);
    connect(taskControl_, &relayer::app::RelayTaskControl::batchFinished, this, [this](bool) {
        // 批次结束（成败皆然）：传输通道空出来，恢复自动接收。
        // startAutoReceive 自带 busy/receiveActive 检查，重复调用无害。
        if (receiveWanted_) {
            startAutoReceive();
        }
    });
    connect(log_, &LogControl::receiveDirChanged, this, [this](const QString& dir) {
        receiveDir_ = dir;
        log_->appendLog(QStringLiteral("[接收] 落点目录已改为 %1\n").arg(dir));
    });

    connect(core_, &relayer::app::CoreService::progressChanged, this, &MainWindow::onProgress);
    connect(core_, &relayer::app::CoreService::operationFinished, this, &MainWindow::onOperationFinished);
    connect(core_, &relayer::app::CoreService::sendFinished, this, &MainWindow::onSendFinished);
    connect(core_, &relayer::app::CoreService::assignedIdChanged, this, &MainWindow::onAssignedIdChanged);
    connect(core_, &relayer::app::CoreService::fileReceived, this, [this](const QString& targetPath) {
        log_->appendLog(QStringLiteral("[接收完成] %1\n").arg(targetPath));
        // 常驻接收继续等下一个文件；重置空闲提示，让"等待文件……"
        // 再出现一次，用户才知道会话还活着。
        idleProgressLogged_ = false;

        // 对照表的行级下载：这一份是刚才那一行要的东西吗？
        // 只按"名字对得上"认领——队列表不可能知道来源，但名字不符
        // 就说明这是别人推来的文件，队列继续等自己那份。
        if (!downloadWaiting_ || downloadIndex_ < 0 || downloadIndex_ >= downloads_.size()) {
            return;
        }
        const auto job = downloads_.at(downloadIndex_);
        const QFileInfo got(targetPath);
        if (got.fileName() != job.name) {
            return;
        }
        log_->appendLog(QStringLiteral("[下载] %1：已落到 %2\n").arg(job.name, got.absolutePath()));
        finishDownloadStep();
    });
    connect(core_, &relayer::app::CoreService::receiveFailed, this, [this](const QString& message) {
        // 常驻接收下的单次失败：会话还活着（id 不变），原地继续等。
        // 这不是操作结束，只记一条日志；操作结束（要重连、换 id）
        // 走 operationFinished 那条路。
        log_->appendLog(QStringLiteral("%1（会话保持，继续等待下一个文件）\n").arg(message));
        idleProgressLogged_ = false;

        // 行级下载在等一份文件时失败：这一行算失败，继续下一行。
        // 不推进的话队列会永远停在这一格，用户以为"卡住了"。
        if (downloadWaiting_) {
            log_->appendLog(QStringLiteral("[下载] 这一项没收到，继续下一项\n"));
            finishDownloadStep();
        }
    });
}

void MainWindow::onProbeRequested(const relayer::app::ConnInfo& conn)
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

    if (core_->busy()) {
        if (core_->canPreempt() && core_->refreshPeers(conn)) {
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

    if (taskControl_->isRunning()) {
        // 批次进行中：交给 stopBatch——它把当前项硬取消、剩余项标已取消、
        // 收尾批次并通知恢复自动接收（此时 receiveWanted_ 已是 false，
        // 那个通知不会真的重挂）。
        taskControl_->stopBatch();
    } else {
        // abort 同语义：关闭会话、不发 cancel 帧；onDone 带回"中止"。
        core_->cancelCurrent();
    }
    log_->appendLog(QStringLiteral("[连接] 已断开（会话结束，本机 id 作废）\n"));
}

void MainWindow::onAssignedIdChanged(const QString& id)
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
    if (!receiveWanted_ || receiveActive_ || core_->busy()) {
        return;
    }
    receiveActive_ = true;
    log_->appendLog(QStringLiteral("[接收] 等待接收，落点 %1\n").arg(receiveDir_));
    core_->startReceive(receiveDir_, lastConn_);
}

void MainWindow::onSendToPeer(const QString& peerId, const QString& peerName)
{
    if (peerId.isEmpty()) {
        return;
    }

    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (file.isEmpty()) {
        return;
    }

    // 常驻会话活着时发送直接搭在那条连接上（不换 id，也不打断接收）；
    // 没有会话时会回落一次性连接。
    //
    // **不再挡"已有发送在跑"**：双连接的数据连接支持多路并存（各路一个
    // sessionId），这也是批量任务能并发的前提。以前那句"请等它结束"是
    // 单连接时代的限制，现在挡着只会让用户莫名其妙。
    if (!core_->sendFile(file, QString(), peerId, lastConn_)) {
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
    if (bytesTotal == 0) {
        if (!idleProgressLogged_) {
            idleProgressLogged_ = true;
            log_->appendLog(QStringLiteral("  等待文件……\n"));
        }
        return;
    }
    idleProgressLogged_ = false;

    // 节流到每秒一条：进度本来 100ms 一次，全量写日志会淹没结论行。
    const auto now = std::chrono::steady_clock::now();
    if (now - lastProgressLog_ < std::chrono::seconds(1)) {
        return;
    }
    lastProgressLog_ = now;

    const QString pct = bytesTotal > 0
                            ? QString::number(100.0 * static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 'f', 1) +
                                  QStringLiteral("%")
                            : QStringLiteral("--");
    log_->appendLog(QStringLiteral("  进度 %1  %2 / %3  %4/s\n")
                        .arg(pct, humanBytes(bytesDone), humanBytes(bytesTotal),
                             bytesPerSecond > 0 ? humanBytes(static_cast<quint64>(bytesPerSecond)) : QStringLiteral("-")));
}

void MainWindow::onSendFinished(bool ok, const QString& summary, const QString& detail)
{
    // 批次进行中：结论由任务表单按行消费并记日志，这里不重复打。
    if (!taskControl_->isRunning()) {
        log_->appendLog(
            QStringLiteral("%1%2\n").arg(summary, (!ok && !detail.isEmpty()) ? QStringLiteral("（%1）").arg(detail) : QString()));
    }

    // 常驻会话上的发送不会翻 busy（会话还在收）；只有回落的一次性连接
    // 发送结束后通道才空出来，那时按需重挂自动接收。
    if (receiveWanted_ && !core_->busy()) {
        startAutoReceive();
    }
}

void MainWindow::onOperationFinished(bool ok, const QString& summary, const QString& detail)
{
    // 检查远端的队列挂在常驻会话上：会话一结束，队列里的请求就不会再有
    // 回音了。不清掉的话界面会永远停在"检查中"。
    if (checkGroupIndex_ >= 0) {
        remoteChecks_.clear();
        checkGroups_.clear();
        checkGroupIndex_ = -1;
        if (pathMetaPurpose_ == PathMetaPurpose::kRemoteCheck) {
            pathMetaPurpose_ = PathMetaPurpose::kNone;
        }
        log_->appendLog(QStringLiteral("[检查] 会话结束，检查中止\n"));
    }

    // wasReceiving：结束的是不是接收会话本身（失败重试只对它计数）。
    const bool wasReceiving = receiveActive_;
    if (receiveActive_) {
        receiveActive_ = false;
    }

    log_->appendLog(
        QStringLiteral("%1%2\n").arg(summary, (!ok && !detail.isEmpty()) ? QStringLiteral("（%1）").arg(detail) : QString()));

    // 任务批次进行中：推进与展示由 RelayTaskControl 接管（它也在收同一条
    // operationFinished），通道让给任务链；批次结束时经 batchFinished
    // 恢复自动接收。
    if (taskControl_->isRunning()) {
        return;
    }

    // 常驻接收收完文件**不会** finished；能到这里且接收没挂着，说明会话
    // 被抢占让位或失败断开——只要用户还想要接收、通道空闲，就重挂。
    if (!receiveWanted_ || receiveActive_ || core_->busy()) {
        return;
    }
    if (wasReceiving && !ok) {
        // 接收会话自身失败（中继没了、认证被拒……）：按设置里的重试
        // 策略延迟重试（Setting 对话框可配；默认 3 次 × 3 秒，次数
        // 0 表示一直重试）。关闭重试或次数用尽则停下来等用户处理。
        QSettings settings;
        const bool autoRetry = settings.value(kReceiverAutoRetry, kAutoRetryDefault).toBool();
        const int intervalSec = qMax(1, settings.value(kReceiverRetryInterval, kRetryIntervalDefault).toInt());
        const int maxRetry = qMax(0, settings.value(kReceiverRetryMax, kRetryMaxDefault).toInt());
        if (autoRetry && (maxRetry == 0 || ++receiveRetries_ <= maxRetry)) {
            const QString countText =
                maxRetry == 0 ? QString() : QStringLiteral("（第 %1/%2 次）").arg(receiveRetries_).arg(maxRetry);
            log_->appendLog(QStringLiteral("[接收] %1 秒后重试%2\n").arg(intervalSec).arg(countText));
            QTimer::singleShot(intervalSec * 1000, this, &MainWindow::startAutoReceive);
        } else {
            receiveRetries_ = 0;
            log_->appendLog(QStringLiteral("[接收] 已停止自动重试，请检查中继或在设置里调整后点「连接」恢复\n"));
        }
        return;
    }
    if (ok) {
        receiveRetries_ = 0;
    }
    startAutoReceive();
}

// ---------------------------------------------------------------------------
// 任务入口（E3.5）：只做闸门检查，展示与执行交给 RelayTaskControl。
// ---------------------------------------------------------------------------

// 粗校验 / 时间戳校验：本地选中的文件 vs **远端面板当前目录下的同名文件**。
//
// 为什么放在 MainWindow 而不是 ExplorerControl：需要同时知道两个面板各自的
// 当前路径，而两个面板互相不认识。v1 用共享状态回调（tellInfoCall_）解决，
// v2 让 MainWindow 做中间人——它本来就持有两者。
//
// 为什么走 kAskPathMeta 而不是列举对端目录：一次往返、只查这一个文件，
// 还顺带拿到内容指纹。列举一个大目录只为比一个文件是 v2 早先的写法，
// 又慢又只能比大小。
void MainWindow::onCompareRequested(const QString& name, bool wantTime)
{
    if (!core_->busy()) {
        log_->appendLog(QStringLiteral("[校验] 请先连接中继\n"));
        return;
    }
    if (compareActive_) {
        log_->appendLog(QStringLiteral("[校验] 上一次校验还没结束\n"));
        return;
    }

    const QString localPath = QDir(explorer_->currentPathForCompare()).filePath(name);
    const QFileInfo local(localPath);
    if (!local.exists() || !local.isFile()) {
        log_->appendLog(QStringLiteral("[校验] 本地没有这个文件：%1\n").arg(name));
        return;
    }

    const QString peer = remoteExplorer_->remotePeerId();
    if (peer.isEmpty()) {
        log_->appendLog(QStringLiteral("[校验] 远端面板还没有对端（先右键对端「浏览此对端的目录」）\n"));
        return;
    }

    pendingCompare_.name = name;
    pendingCompare_.peer = peer;
    pendingCompare_.localPath = localPath;
    pendingCompare_.absPath = relayer::app::joinRemotePath(remoteExplorer_->currentPathForCompare(), name);
    pendingCompare_.wantTime = wantTime;
    pendingCompare_.localSize = local.size();
    pendingCompare_.localMtime = local.lastModified().toMSecsSinceEpoch();
    compareActive_ = true;

    pathMetaPurpose_ = PathMetaPurpose::kLocalCompare;
    if (!core_->askPathMeta(peer, QStringList{pendingCompare_.absPath})) {
        compareActive_ = false;
        pathMetaPurpose_ = PathMetaPurpose::kNone;
        log_->appendLog(QStringLiteral("[校验] 当前不能查询对端（未连接或正在传输）\n"));
        return;
    }
    log_->appendLog(
        QStringLiteral("[校验] %1：本地 %2 字节，正在取对端信息…\n").arg(name, QString::number(pendingCompare_.localSize)));
}

void MainWindow::onLocalCompareMetaFinished(bool ok, const QString& summary, const QList<relayer::app::RemoteMetaRow>& rows)
{
    const auto pc = pendingCompare_;
    compareActive_ = false;
    pathMetaPurpose_ = PathMetaPurpose::kNone;

    if (!ok) {
        log_->appendLog(QStringLiteral("[校验] 取对端信息失败：%1\n").arg(summary));
        return;
    }
    if (rows.isEmpty() || !rows.first().exists) {
        log_->appendLog(QStringLiteral("[校验] %1：对端没有这个文件（%2:%3）\n")
                            .arg(pc.name, pc.peer, pc.absPath.isEmpty() ? QStringLiteral("<接收目录>") : pc.absPath));
        return;
    }
    finishCompare(rows.first());
}

// 对端信息到手：时间戳校验比 mtime；粗校验先比大小再比内容指纹。
//
// 内容级用 quickFingerprint（采样指纹），与 v1 的 SampleCompare 采样块是
// 同一个思路、同一份成本量级——比整文件哈希便宜，比"只看大小"强得多。
void MainWindow::finishCompare(const relayer::app::RemoteMetaRow& peerMeta)
{
    const auto pc = pendingCompare_;
    const qint64 peerSize = static_cast<qint64>(peerMeta.size);
    const qint64 peerMtime = peerMeta.mtimeMs > 0 ? peerMeta.mtimeMs : 0;

    if (pc.wantTime) {
        const auto fmt = [](qint64 ms) {
            return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        };
        if (pc.localMtime == peerMtime) {
            log_->appendLog(QStringLiteral("[校验] %1：两端修改时间相同（%2）\n").arg(pc.name, fmt(pc.localMtime)));
            return;
        }
        if (peerMtime <= 0) {
            log_->appendLog(QStringLiteral("[校验] %1：对端没有给出修改时间（本地 %2）\n").arg(pc.name, fmt(pc.localMtime)));
            return;
        }
        const bool localNewer = pc.localMtime > peerMtime;
        qint64 diffSec = qAbs(pc.localMtime - peerMtime) / 1000;
        const qint64 days = diffSec / 86400;
        const qint64 hours = (diffSec % 86400) / 3600;
        const qint64 mins = (diffSec % 3600) / 60;
        const qint64 secs = diffSec % 60;
        QStringList parts;
        if (days > 0) {
            parts << QStringLiteral("%1天").arg(days);
        }
        if (hours > 0) {
            parts << QStringLiteral("%1小时").arg(hours);
        }
        if (mins > 0) {
            parts << QStringLiteral("%1分").arg(mins);
        }
        if (secs > 0 || parts.isEmpty()) {
            parts << QStringLiteral("%1秒").arg(secs);
        }
        log_->appendLog(QStringLiteral("[校验] %1：%2更新（本地 %3，对端 %4，相差 %5）\n")
                            .arg(pc.name, localNewer ? QStringLiteral("本地") : QStringLiteral("对端"), fmt(pc.localMtime),
                                 fmt(peerMtime), parts.join(QString())));
        return;
    }

    // 粗校验：大小不一致就没必要算指纹了。
    if (peerSize != pc.localSize) {
        log_->appendLog(QStringLiteral("[校验] %1：大小不符（本地 %2，对端 %3）\n")
                            .arg(pc.name, humanBytes(static_cast<quint64>(pc.localSize)), humanBytes(peerMeta.size)));
        return;
    }

    relayer::filestore::StdFileStore store;
    const QString localFp = QString::fromStdString(store.quickFingerprint(pc.localPath.toStdString()));
    if (localFp.isEmpty() || peerMeta.fingerprint.isEmpty()) {
        log_->appendLog(QStringLiteral("[校验] %1：大小一致（%2），内容无法粗判（%3）\n")
                            .arg(pc.name, humanBytes(peerMeta.size),
                                 localFp.isEmpty() ? QStringLiteral("本地读取失败") : QStringLiteral("对端未给出指纹")));
        return;
    }
    if (localFp == peerMeta.fingerprint) {
        log_->appendLog(QStringLiteral("[校验] %1：内容粗判一致（%2）\n").arg(pc.name, humanBytes(peerMeta.size)));
    } else {
        log_->appendLog(QStringLiteral("[校验] %1：大小一致但内容不一致（%2）\n").arg(pc.name, humanBytes(peerMeta.size)));
    }
}

// 远端文件操作：面板只给"对谁、对哪个路径、做什么"，这里转给 CoreService。
//
// 特意不在本层做路径拼接或存在性检查：面板给的已经是**对端绝对路径**，
// 而"路径对不对"只有对端知道——本地猜一遍只会制造两套判断。
void MainWindow::onRemoteOpRequested(const QString& kind, const QString& peerId, const QString& absPath, const QString& newName,
                                     const QStringList& paths)
{
    bool started = false;
    if (kind == QLatin1String("sha256")) {
        started = core_->remoteOpSha256(peerId, absPath);
    } else if (kind == QLatin1String("rename")) {
        started = core_->remoteOpRename(peerId, absPath, newName);
    } else if (kind == QLatin1String("mkdir")) {
        started = core_->remoteOpCreateDir(peerId, absPath);
    } else if (kind == QLatin1String("delete")) {
        started = core_->remoteOpDelete(peerId, paths);
    } else if (kind == QLatin1String("unpack")) {
        started = core_->remoteOpUnarchive(peerId, absPath);
    }

    if (!started) {
        log_->appendLog(QStringLiteral("[远端] 当前不能执行「%1」：未连接、正在传输，或已有别的远端请求在飞\n").arg(kind));
    }
}

void MainWindow::onRemoteFileOpFinished(const QString& kind, bool ok, const QString& summary, const QString& detail,
                                        const QString& result)
{
    if (!ok) {
        log_->appendLog(
            QStringLiteral("[远端] %1 失败：%2%3\n")
                .arg(kind, summary, detail.isEmpty() ? QString() : QStringLiteral("（") + detail + QStringLiteral("）")));
        if (!result.isEmpty()) {
            log_->appendLog(QStringLiteral("[远端] 没处理掉的项：\n%1\n").arg(result));
        }
    } else if (kind == QLatin1String("sha256")) {
        // 与本地 SHA256 同一个交互：算完自动进剪贴板，用户下一步多半是粘贴。
        QGuiApplication::clipboard()->setText(result);
        log_->appendLog(QStringLiteral("[远端] SHA-256（已复制到剪贴板）：%1\n").arg(result));
    } else if (kind == QLatin1String("unpack")) {
        log_->appendLog(QStringLiteral("[远端] 已解压到 %1\n").arg(result));
    } else {
        log_->appendLog(QStringLiteral("[远端] %1\n").arg(summary));
    }

    // 变更类操作之后重列当前目录：删了/改名了/建了目录却不刷新，用户会
    // 以为没生效，然后再点一次。sha256 不改动文件系统，不必刷。
    if (kind != QLatin1String("sha256") && !remoteExplorer_->remotePeerId().isEmpty()) {
        remoteExplorer_->navigateTo(remoteExplorer_->currentPathForCompare());
    }
}
// 对照表右键"检查远端"：按对端分组，每组一次 kAskPathMeta（§11.11）。
//
// v1 的「粗校验」是"逐文件两次往返采样块"。v2 用一条批量查询把存在性、
// 大小、时间、内容指纹一次拿全：一处 RTT 换一整套结论，还少了"逐行列举
// 一个大目录"的开销。语义没有缩水——quickFingerprint 就是采样指纹，
// 与 v1 的 SampleBlock 采样是同一个思路（比整文件哈希便宜得多）。
void MainWindow::onRemoteCheckRequested(const relayer::app::RelayTaskItems& items)
{
    if (!core_->busy()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先连接中继——检查远端要靠常驻接收会话，"
                                            "新开连接会把本机 id 换掉。"));
        return;
    }
    if (checkGroupIndex_ >= 0) {
        log_->appendLog(QStringLiteral("[检查] 上一批还没跑完，请等它结束\n"));
        return;
    }

    const QString defaultPeer = connector_->currentPeerId();
    remoteChecks_.clear();
    checkGroups_.clear();

    for (const auto& it : items) {
        const QString peer = it.peerId.isEmpty() ? defaultPeer : it.peerId;
        if (peer.isEmpty()) {
            log_->appendLog(QStringLiteral("[检查] %1：没有指定对端，跳过\n").arg(it.name));
            continue;
        }
        if (it.name.isEmpty()) {
            continue;
        }

        RemoteCheck rc;
        rc.peer = peer;
        rc.name = it.name;
        rc.absPath = relayer::app::joinRemotePath(it.relDir, it.name);
        rc.localPath = QDir(it.localDir).filePath(it.name);
        const QFileInfo local(rc.localPath);
        rc.isDir = it.isDir || local.isDir();
        rc.localSize = local.size();
        remoteChecks_.append(rc);

        // 分组：同一个对端的目标路径合成一次请求。
        int group = -1;
        for (int i = 0; i < checkGroups_.size(); ++i) {
            if (checkGroups_.at(i).peer == peer) {
                group = i;
                break;
            }
        }
        if (group < 0) {
            CheckGroup g;
            g.peer = peer;
            checkGroups_.append(g);
            group = static_cast<int>(checkGroups_.size()) - 1;
        }
        checkGroups_[group].paths << rc.absPath;
        checkGroups_[group].checkIndexes << static_cast<int>(remoteChecks_.size()) - 1;
    }

    if (remoteChecks_.isEmpty()) {
        return;
    }
    checkGroupIndex_ = 0;
    log_->appendLog(QStringLiteral("[检查] 开始检查 %1 项（%2 个对端）\n").arg(remoteChecks_.size()).arg(checkGroups_.size()));
    issueNextCheckGroup();
}

void MainWindow::issueNextCheckGroup()
{
    while (checkGroupIndex_ >= 0 && checkGroupIndex_ < checkGroups_.size()) {
        const auto& group = checkGroups_.at(checkGroupIndex_);
        pathMetaPurpose_ = PathMetaPurpose::kRemoteCheck;
        if (core_->askPathMeta(group.peer, group.paths)) {
            return;   // 等 onRemoteCheckMetaFinished
        }
        log_->appendLog(QStringLiteral("[检查] 无法查询 %1（会话不可用或正忙），该组跳过\n").arg(group.peer));
        ++checkGroupIndex_;
    }

    if (checkGroupIndex_ >= 0) {
        checkGroupIndex_ = -1;
        pathMetaPurpose_ = PathMetaPurpose::kNone;
        remoteChecks_.clear();
        checkGroups_.clear();
        log_->appendLog(QStringLiteral("[检查] 结束\n"));
    }
}

void MainWindow::onRemoteCheckMetaFinished(bool ok, const QString& summary, const QList<relayer::app::RemoteMetaRow>& rows)
{
    if (checkGroupIndex_ < 0 || checkGroupIndex_ >= checkGroups_.size()) {
        return;   // 迟到的应答
    }
    const auto group = checkGroups_.at(checkGroupIndex_);   // 值拷贝：下面会改下标
    ++checkGroupIndex_;

    if (!ok) {
        log_->appendLog(QStringLiteral("[检查] %1 的检查失败：%2\n").arg(group.peer, summary));
        issueNextCheckGroup();
        return;
    }

    for (int i = 0; i < group.checkIndexes.size() && i < rows.size(); ++i) {
        const auto& rc = remoteChecks_.at(group.checkIndexes.at(i));
        const auto& meta = rows.at(i);

        if (!meta.exists) {
            log_->appendLog(QStringLiteral("[检查] %1：对方没有（%2:%3）\n")
                                .arg(rc.name, rc.peer, rc.absPath.isEmpty() ? QStringLiteral("<接收目录>") : rc.absPath));
            continue;
        }
        if (rc.isDir || meta.isDir) {
            log_->appendLog(QStringLiteral("[检查] %1：对方已有同名目录\n").arg(rc.name));
            continue;
        }

        const qint64 peerSize = static_cast<qint64>(meta.size);
        if (peerSize != rc.localSize) {
            log_->appendLog(
                QStringLiteral("[检查] %1：大小不符（本地 %2，对方 %3）\n")
                    .arg(rc.name, humanBytes(static_cast<quint64>(rc.localSize)), humanBytes(static_cast<quint64>(meta.size))));
            continue;
        }

        // 大小一致才值得比内容。任一侧算不出指纹都按"无法粗判"报，
        // 不替用户判定"大概一样"——那个结论会被用来决定要不要覆盖。
        relayer::filestore::StdFileStore store;
        const QString localFp = QString::fromStdString(store.quickFingerprint(rc.localPath.toStdString()));
        if (localFp.isEmpty() || meta.fingerprint.isEmpty()) {
            log_->appendLog(
                QStringLiteral("[检查] %1：大小一致，内容无法粗判（%2）\n")
                    .arg(rc.name, localFp.isEmpty() ? QStringLiteral("本地读取失败") : QStringLiteral("对端未给出指纹")));
        } else if (localFp == meta.fingerprint) {
            log_->appendLog(
                QStringLiteral("[检查] %1：已存在且内容一致（%2）\n").arg(rc.name, humanBytes(static_cast<quint64>(meta.size))));
        } else {
            log_->appendLog(QStringLiteral("[检查] %1：大小一致但内容不一致\n").arg(rc.name));
        }
    }

    issueNextCheckGroup();
}

// 对照表右键"下载到本地目录"：把每行在 peer:relDir 下的 name 取回来，
// 落到该行的本地目录。是"发送到对端"的逆操作。
//
// 与"检查远端"同构：本控件只给行数据，网络动作在这里逐行发起。差别是
// 这里的每一步要等**文件落地**才算完（只等应答会被对端以"正忙"拒掉）。
void MainWindow::onDownloadRequested(const relayer::app::RelayTaskItems& items)
{
    if (!core_->busy()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先连接中继——下载要靠常驻接收会话，"
                                            "新开连接会把本机 id 换掉。"));
        return;
    }
    if (downloadIndex_ >= 0) {
        log_->appendLog(QStringLiteral("[下载] 上一批还没跑完，请等它结束\n"));
        return;
    }

    const QString defaultPeer = connector_->currentPeerId();
    downloads_.clear();
    for (const auto& it : items) {
        if (it.name.isEmpty()) {
            continue;
        }
        const QString peer = it.peerId.isEmpty() ? defaultPeer : it.peerId;
        if (peer.isEmpty()) {
            log_->appendLog(QStringLiteral("[下载] %1：没有指定对端，跳过\n").arg(it.name));
            continue;
        }

        DownloadJob job;
        job.peer = peer;
        job.name = it.name;
        job.isDir = it.isDir;
        // 目标路径与发送走同一份拼接规则（app/model/PathText.h）。
        //
        // 别再自己 `replace('\\','/')` 再 trim 首尾斜杠：老代码连开头的
        // '/' 都剥掉，把绝对路径打成了相对的，落点从 `D:/photos/a.jpg`
        // 变成"接收目录/photos/a.jpg"。
        job.relPath = relayer::app::joinRemotePath(it.relDir, it.name);

        // 落点：行里写了本地目录、且它确实存在 → 就落在那儿（v1 语义）。
        // 不存在则退回全局接收目录——下落点由接收方决定，不能凭一行
        // 手填的路径就去建一整套目录树。
        if (!it.localDir.isEmpty()) {
            const QFileInfo dirInfo(it.localDir);
            if (dirInfo.isDir()) {
                job.into = dirInfo.absoluteFilePath();
            } else {
                log_->appendLog(QStringLiteral("[下载] %1：本地目录不存在（%2），改落接收目录\n").arg(it.name, it.localDir));
            }
        }
        downloads_.append(job);
    }

    if (downloads_.isEmpty()) {
        return;
    }
    downloadIndex_ = 0;
    downloadWaiting_ = false;
    log_->appendLog(QStringLiteral("[下载] 开始 %1 项（逐项串行，等落地再发下一项）\n").arg(downloads_.size()));
    nextDownload();
}

void MainWindow::nextDownload()
{
    while (downloadIndex_ >= 0 && downloadIndex_ < downloads_.size()) {
        const auto& job = downloads_.at(downloadIndex_);
        if (core_->pullFileFromPeer(job.peer, job.relPath, job.isDir, job.into)) {
            return;   // 等 onRemotePullFinished
        }
        log_->appendLog(QStringLiteral("[下载] %1：当前无法发起取回（会话不可用或正忙）\n").arg(job.name));
        ++downloadIndex_;
    }

    if (downloadIndex_ >= 0) {
        downloadIndex_ = -1;
        downloadWaiting_ = false;
        downloads_.clear();
        log_->appendLog(QStringLiteral("[下载] 结束\n"));
    }
}

// 当前这一项有着落了（收到 / 解包完成 / 失败）：推进一格。
void MainWindow::finishDownloadStep()
{
    if (downloadIndex_ < 0) {
        return;
    }
    downloadWaiting_ = false;
    ++downloadIndex_;
    nextDownload();   // 结束时它会清队列并把 index 归 -1
}

void MainWindow::onRemotePullFinished(bool ok, const QString& summary)
{
    // 队列在跑时，这条应答属于队首那一项——面板下载的应答不会混进来
    // （同一时刻只允许一个取回请求在飞，队列占着它）。
    if (downloadIndex_ >= 0 && downloadIndex_ < downloads_.size()) {
        // 值拷贝：finishDownloadStep 可能清空队列，引用会悬空。
        const auto job = downloads_.at(downloadIndex_);
        if (ok) {
            // 只是"对方接受了"，文件还在路上。此时发下一项会被对端以
            // "正忙"拒掉，所以必须等 fileReceived / archiveExtracted。
            downloadWaiting_ = true;
            log_->appendLog(QStringLiteral("[下载] %1：对方已接受，等待接收…\n").arg(job.name));
            return;
        }
        log_->appendLog(QStringLiteral("[下载] %1：对端没能给出（%2）\n").arg(job.name, summary));
        finishDownloadStep();
        return;
    }

    if (ok) {
        // 只是"对方接受了"。落点由接收路径报（[接收完成] 那一行），
        // 这里别写文件名——它还不知道文件会不会被改名。
        log_->appendLog(QStringLiteral("[取回] %1，文件将落到接收目录\n").arg(summary));
    } else {
        log_->appendLog(QStringLiteral("[取回] 失败：%1\n").arg(summary));
    }
}

void MainWindow::onBrowsePeerRequested(const QString& peerId, const QString& peerName)
{
    switchRemotePeer(peerId, peerName);
}

void MainWindow::switchRemotePeer(const QString& peerId, const QString& peerName, const QString& relPath)
{
    if (peerId.isEmpty()) {
        return;
    }
    if (!core_->busy()) {
        // busy 为假 = 没有常驻接收会话可搭车。写日志再弹窗：弹窗关掉之后
        // 日志里还得留得下线索，否则事后无从判断"当时到底连没连上"。
        log_->appendLog(QStringLiteral("[浏览] 没有常驻会话，无法浏览 %1（请先连接）\n").arg(peerId));
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先连接中继——浏览对端要走常驻接收会话。"));
        return;
    }
    // v1 的语义：浏览面板只有一套，切的是数据源（AskType → Source）。
    // 不做独立窗口——那样会把表格/导航/筛选复制一遍，且两个窗口之间
    // 没有共享状态可用（跨面板拖拽正是靠这个）。
    remoteExplorer_->setRemotePeer(peerId, peerName, relPath);
    // 标题带对端名：切换成功与否一眼可见（面板内容可能因对方未开共享而为空，
    // 只靠表格判断不了"到底切没切"）。
    if (remoteBox_) {
        remoteBox_->setTitle(QStringLiteral("远端 —— %1").arg(peerName.isEmpty() ? peerId : peerName));
    }
    log_->appendLog(QStringLiteral("[浏览] 远端面板切到 %1%2\n")
                        .arg(peerId, relPath.isEmpty() ? QStringLiteral(" 的共享目录") : QStringLiteral(" 的 ").append(relPath)));
}

void MainWindow::onRemoteBrowseFinished(bool ok, bool rootListing, const QString& summary,
                                        const QList<relayer::app::RemoteRow>& rows)
{
    // 远端面板是唯一的消费者：粗校验/检查远端都改走 kAskPathMeta 了
    // （一次往返、带指纹），不再和面板抢同一路列举应答。
    remoteExplorer_->applyRemoteEntries(ok, rootListing, summary, rows);
}

void MainWindow::onTransTaskRun(const relayer::app::RelayTaskItems& items)
{
    if (taskControl_->isRunning()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有任务正在进行，请等它结束"));
        return;
    }
    // 忙但只是接收挂起：可抢占，继续；传输中：拒绝。
    if (core_->busy() && !core_->canPreempt()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有传输正在进行，请等它结束"));
        return;
    }
    // 不挡"已有发送在跑"：批量的每一路本来就是并发的（见 onSendFileRequested）。
    if (lastConn_.host.isEmpty()) {
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
    for (int i = 0; i < items.size(); ++i) {
        const QString& peer = items.at(i).peerId;
        if (peer.isEmpty()) {
            continue;
        }
        if (!defaultPeer.isEmpty() && peer != defaultPeer) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("第 %1 行对端 %2 与当前通信对象 %3 不一致。\n"
                                                "要发当前通信对象就把该行对端留空；要发别处请先在连接面板"
                                                "把它设为通信对象。")
                                     .arg(i + 1)
                                     .arg(peer, defaultPeer));
            return;
        }
        if (!connector_->isPeerOnline(peer)) {
            log_->appendLog(QStringLiteral("[任务] 对端 %1 不在最新在线列表里，仍会尝试发送（可先点「刷新」）\n").arg(peer));
        }
    }

    if (defaultPeer.isEmpty()) {
        bool allEmpty = true;
        for (const auto& it : items) {
            if (!it.peerId.isEmpty()) {
                allEmpty = false;
                break;
            }
        }
        if (allEmpty) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("这批任务没有指定对端，也没有选「当前通信对象」。\n"
                                                "请在连接面板右键在线对端 → 设为通信对象，或在对照表里填对端 id。"));
            return;
        }
    }

    // 发送前检查（v1 的基本检查 + 覆盖提示）。默认开：直接照发意味着
    // 静默覆盖对方机器上的同名文件，而这是不可撤销的。
    const bool checkFirst = QSettings().value(kSendCheckBeforeSend, kSendCheckDefault).toBool();
    if (checkFirst) {
        startSendPreCheck(items, lastConn_, defaultPeer);
    } else {
        sendBatch(items, lastConn_, defaultPeer);
    }
}

void MainWindow::sendBatch(const relayer::app::RelayTaskItems& items, const relayer::app::ConnInfo& conn,
                           const QString& defaultPeerId)
{
    // 批次直接搭在常驻会话上发（不新建连接 → id 不变，接收也不中断），
    // 所以不再需要"接收让位"。
    taskControl_->startTasks(items, conn, defaultPeerId);
}

// 目标绝对路径：对照表的 relDir 就是**对端视角的绝对目录**（DESIGN.md
// §3.8）。留空 = 落到对端接收目录，这时只给文件名（相对路径）。
//
// 规范化交给 joinRemotePath：手填的 `D://Download//`、粘来的 `D:///Download`
// 都会变成 `D:/Download/name`。自己在每个调用点 trim 分隔符，迟早漏一处。
static QString remoteTargetPath(const relayer::app::RelayTaskItem& item)
{
    return relayer::app::joinRemotePath(item.relDir, item.name);
}

// 本地文件的 quickFingerprint（与对端算指纹时同一份算法）。空 = 算不出来。
static QString localFingerprint(const QString& path)
{
    relayer::filestore::StdFileStore store;
    return QString::fromStdString(store.quickFingerprint(path.toStdString()));
}

void MainWindow::startSendPreCheck(const relayer::app::RelayTaskItems& items, const relayer::app::ConnInfo& conn,
                                   const QString& defaultPeerId)
{
    preCheckItems_ = items;
    preCheckConn_ = conn;
    preCheckDefaultPeer_ = defaultPeerId;
    preCheckQueue_.clear();
    preCheckResults_.clear();
    preCheckIndex_ = -1;
    pathMetaPurpose_ = PathMetaPurpose::kSendPreCheck;

    // 按对端分组：一次请求只能问一个对端，而一批任务可能跨多个对端。
    QHash<QString, int> byPeer;   // peerId -> preCheckQueue_ 下标
    for (int i = 0; i < items.size(); ++i) {
        const auto& item = items.at(i);
        const QString peer = item.peerId.isEmpty() ? defaultPeerId : item.peerId;
        if (peer.isEmpty()) {
            continue;   // 没对端的行由任务表标"已跳过"，不必问
        }
        const QString path = remoteTargetPath(item);
        auto it = byPeer.find(peer);
        if (it == byPeer.end()) {
            SendPreCheck req;
            req.peerId = peer;
            preCheckQueue_.append(req);
            it = byPeer.insert(peer, static_cast<int>(preCheckQueue_.size()) - 1);
        }
        SendPreCheck& req = preCheckQueue_[it.value()];
        req.paths << path;
        req.itemIndexes << i;
    }

    if (preCheckQueue_.isEmpty()) {
        sendBatch(preCheckItems_, preCheckConn_, preCheckDefaultPeer_);
        return;
    }

    int total = 0;
    for (const auto& req : preCheckQueue_) {
        total += static_cast<int>(req.paths.size());
    }
    log_->appendLog(
        QStringLiteral("[检查] 正在向 %1 个对端确认 %2 个目标路径是否已存在…\n").arg(preCheckQueue_.size()).arg(total));
    issueNextPreCheck();
}

void MainWindow::issueNextPreCheck()
{
    ++preCheckIndex_;
    if (preCheckIndex_ >= preCheckQueue_.size()) {
        promptOverwriteThenSend();
        return;
    }
    const auto& req = preCheckQueue_.at(preCheckIndex_);
    if (!core_->askPathMeta(req.peerId, req.paths)) {
        // 会话不可用/正在传输：检查做不了不是"发送失败"，如实说一句然后
        // 照发。硬拦在这里会让"明明能发"变成不能发。
        log_->appendLog(QStringLiteral("[检查] 无法查询 %1（会话不可用），跳过检查直接发送\n").arg(req.peerId));
        ++preCheckIndex_;
        issueNextPreCheck();
    }
}

// kAskPathMeta 应答的统一入口：两个消费者（发送前检查、对照表检查）串行使用
// 同一条通道，靠 pathMetaPurpose_ 分派。
//
// 不做"按内容猜归属"：一批发送前检查可能来自三个对端，一次检查也可能是
// 一个对端的一批路径，只看条数分不清。标志位由发起方自己置、自己清。
void MainWindow::onPathMetaFinished(bool ok, const QString& summary, const QList<relayer::app::RemoteMetaRow>& rows)
{
    switch (pathMetaPurpose_) {
    case PathMetaPurpose::kSendPreCheck:
        onPreCheckMetaFinished(ok, summary, rows);
        return;
    case PathMetaPurpose::kRemoteCheck:
        onRemoteCheckMetaFinished(ok, summary, rows);
        return;
    case PathMetaPurpose::kLocalCompare:
        onLocalCompareMetaFinished(ok, summary, rows);
        return;
    case PathMetaPurpose::kNone:
    default:
        // 没有归属的应答（例如发起方已被取消）：丢掉，不猜。
        return;
    }
}

void MainWindow::onPreCheckMetaFinished(bool ok, const QString& summary, const QList<relayer::app::RemoteMetaRow>& rows)
{
    if (preCheckIndex_ < 0 || preCheckIndex_ >= preCheckQueue_.size()) {
        return;   // 迟到的应答（检查已被取消）
    }
    const auto& req = preCheckQueue_.at(preCheckIndex_);
    if (!ok) {
        log_->appendLog(QStringLiteral("[检查] %1 的检查失败：%2（跳过检查直接发送）\n").arg(req.peerId, summary));
        ++preCheckIndex_;
        issueNextPreCheck();
        return;
    }

    for (int i = 0; i < rows.size() && i < static_cast<int>(req.paths.size()); ++i) {
        preCheckResults_.insert(req.peerId + QStringLiteral("|") + req.paths.at(i), rows.at(i));
    }
    ++preCheckIndex_;
    issueNextPreCheck();
}

void MainWindow::promptOverwriteThenSend()
{
    pathMetaPurpose_ = PathMetaPurpose::kNone;
    relayer::app::RelayTaskItems kept;
    kept.reserve(preCheckItems_.size());

    bool overwriteAll = false;
    bool skipAll = false;
    int conflicts = 0;
    int skipped = 0;

    for (int i = 0; i < preCheckItems_.size(); ++i) {
        const auto& item = preCheckItems_.at(i);
        const QString peer = item.peerId.isEmpty() ? preCheckDefaultPeer_ : item.peerId;
        const QString path = remoteTargetPath(item);
        const auto it = preCheckResults_.constFind(peer + QStringLiteral("|") + path);
        const bool exists = (it != preCheckResults_.constEnd()) && it->exists;

        if (!exists) {
            kept.append(item);
            continue;
        }
        ++conflicts;

        if (skipAll) {
            ++skipped;
            log_->appendLog(QStringLiteral("[检查] 跳过（已存在）：%1\n").arg(path));
            continue;
        }
        if (overwriteAll) {
            kept.append(item);
            continue;
        }

        // 内容粗判（v1 的"粗判一致/不一致"）：对端在元信息里带了
        // quickFingerprint，本地算同一份比对即可。大小不同、或任一侧
        // 算不出来，一律按"不一致"——宁可多问一次，也不要替用户判定
        // "一样，覆盖也没关系"。
        bool same = false;
        if (!item.isDir) {
            const QString localPath = QDir(item.localDir).filePath(item.name);
            const QString localFp = localFingerprint(localPath);
            same = !localFp.isEmpty() && !it->fingerprint.isEmpty() && localFp == it->fingerprint;
        }

        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("目标已存在"));
        box.setText(same ? QStringLiteral("内容粗判一致，是否覆盖？\n%1").arg(path)
                         : QStringLiteral("内容不一致，是否覆盖？\n%1").arg(path));
        box.setInformativeText(item.isDir ? QStringLiteral("远端已有同名目录，其中的同名文件会被覆盖。")
                                          : QStringLiteral("远端文件 %1 会被替换。").arg(humanBytes(it->size)));
        auto* bYes = box.addButton(QStringLiteral("覆盖"), QMessageBox::AcceptRole);
        auto* bAll = box.addButton(QStringLiteral("全部覆盖"), QMessageBox::YesRole);
        auto* bNo = box.addButton(QStringLiteral("跳过"), QMessageBox::RejectRole);
        auto* bAllNo = box.addButton(QStringLiteral("全部跳过"), QMessageBox::NoRole);
        box.setDefaultButton(bNo);   // 回车不覆盖：默认动作必须是"不动用户的文件"
        box.exec();

        if (box.clickedButton() == bYes) {
            kept.append(item);
        } else if (box.clickedButton() == bAll) {
            overwriteAll = true;
            kept.append(item);
        } else if (box.clickedButton() == bAllNo) {
            skipAll = true;
            ++skipped;
            log_->appendLog(QStringLiteral("[检查] 跳过（已存在）：%1\n").arg(path));
        } else if (box.clickedButton() == bNo) {
            ++skipped;
            log_->appendLog(QStringLiteral("[检查] 跳过（已存在）：%1\n").arg(path));
        } else {
            // 关掉对话框 = 取消整批。这是唯一能"反悔"的出口，不能当成跳过。
            log_->appendLog(QStringLiteral("[检查] 已取消本次发送\n"));
            return;
        }
    }

    if (conflicts == 0) {
        log_->appendLog(QStringLiteral("[检查] 目标都还不存在，直接发送\n"));
    } else {
        log_->appendLog(QStringLiteral("[检查] 共 %1 项已存在，跳过 %2 项\n").arg(conflicts).arg(skipped));
    }
    if (kept.isEmpty()) {
        log_->appendLog(QStringLiteral("[任务] 全部被跳过，没有要发的内容\n"));
        return;
    }
    sendBatch(kept, preCheckConn_, preCheckDefaultPeer_);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // saveGeometry 同时保存"还原态矩形"与最大化状态，当前是最大化也安全。
    QSettings().setValue(relayer::app::settings::kWindowGeometry, saveGeometry());

    receiveWanted_ = false;
    taskControl_->stopBatch();
    core_->cancelCurrent();
    event->accept();
}
