#include "MainWindow.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ui/ConnectorControl.h"
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
    // 镜像 v1 relayFile.cpp initLayout：上（连接面板 + 选项卡）/下（文件浏览，
    // 待 ExplorerControl 迁移后加入）。splitter 的下半暂时没有内容，等
    // E3 全量时把 sFile 加回来。
    auto *tabWidget = new QTabWidget(this);
    tabWidget->addTab(log_, QStringLiteral("日志"));

    auto *sTop = new QSplitter(Qt::Horizontal, this);
    sTop->setHandleWidth(0);
    sTop->addWidget(connector_);
    sTop->addWidget(tabWidget);
    sTop->setStretchFactor(0, 0);
    sTop->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sTop);
}

void MainWindow::wireSignals()
{
    connect(connector_, &ConnectorControl::sendToPeerRequested, this, &MainWindow::onSendToPeer);
    connect(connector_, &ConnectorControl::logMessage, log_, &LogControl::appendLog);
    connect(connector_, &ConnectorControl::probeRequested, this, &MainWindow::onProbeRequested);
    connect(log_, &LogControl::receiveDirChanged, this,
            [this](const QString &dir)
            {
                receiveDir_ = dir;
                log_->appendLog(QStringLiteral("[接收] 落点目录已改为 %1\n").arg(dir));
            });

    connect(core_, &relayer::app::CoreService::progressChanged, this, &MainWindow::onProgress);
    connect(core_, &relayer::app::CoreService::operationFinished, this, &MainWindow::onOperationFinished);
    connect(core_, &relayer::app::CoreService::assignedIdChanged, this, &MainWindow::onAssignedIdChanged);
}

void MainWindow::onProbeRequested(const QString &host, quint16 port, const QString &password)
{
    lastHost_ = host;
    lastPort_ = port;
    lastPassword_ = password;
}

void MainWindow::onAssignedIdChanged(const QString &id)
{
    log_->appendLog(QStringLiteral("[身份] 本机 id = %1\n").arg(id));

    // 拿到身份即开始自动接收。v1 是常驻连接天然在线；v2 的"在线"由
    // 自动接收循环维持——这是 GUI 用户能被别人发文件的前提。
    receiveWanted_ = true;
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
    core_->startReceive(receiveDir_, lastHost_, lastPort_, lastPassword_);
}

void MainWindow::onSendToPeer(const QString &peerId, const QString &peerName)
{
    if (peerId.isEmpty())
    {
        return;
    }
    if (core_->busy())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有操作正在进行，请等它结束"));
        return;
    }

    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (file.isEmpty())
    {
        return;
    }

    lastProgressLog_ = std::chrono::steady_clock::now();
    log_->appendLog(QStringLiteral("[发送] %1 → %2（%3）\n").arg(QDir(file).dirName(), peerId, peerName));
    core_->sendFile(file, QString(), peerId, lastHost_, lastPort_, lastPassword_);
}

void MainWindow::onProgress(quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond)
{
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
    log_->appendLog(QStringLiteral("%1%2\n").arg(
        summary, (!ok && !detail.isEmpty()) ? QStringLiteral("（%1）").arg(detail) : QString()));

    if (receiveActive_)
    {
        receiveActive_ = false;
    }

    // 一次接收结束后立刻重新挂起，维持"在线"。
    if (receiveWanted_ && !core_->busy())
    {
        startAutoReceive();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    receiveWanted_ = false;
    core_->cancelCurrent();
    event->accept();
}
