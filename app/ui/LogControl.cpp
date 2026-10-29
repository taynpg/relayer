#include "LogControl.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <cstdio>

#include "SettingsKeys.h"
#include "ui_LogControl.h"

using relayer::app::settings::kReceiverDir;
using relayer::app::settings::kSendCheckBeforeSend;
using relayer::app::settings::kSendCheckDefault;

namespace {
// 日志控件最大保留行数，超出后最早的日志被挤掉（与 v1 一致）。
constexpr int kMaxLogLines = 10000;

// 单个日志文件的写入上限。超过后停止写盘（界面照常追加），避免长时间
// 挂着不关的实例把磁盘写满。
constexpr qint64 kMaxLogFileBytes = 16LL * 1024 * 1024;

// 当前活着的日志控件。Qt 的消息 handler 是纯函数指针、没有 this 可用，
// 靠它把消息排回 GUI 线程。QPointer 在控件析构后自动变空，不必手工清理。
QPointer<LogControl> g_logControl;

QString defaultReceiveDir()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}

QString levelText(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtInfoMsg:
        return QStringLiteral("INFO");
    case QtWarningMsg:
        return QStringLiteral("WARN");
    case QtCriticalMsg:
        return QStringLiteral("ERROR");
    case QtFatalMsg:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("LOG");
}
}   // namespace

LogControl::LogControl(QWidget* parent) : QDialog(parent), ui_(new Ui::LogControl)
{
    ui_->setupUi(this);
    ui_->pedLog->setReadOnly(true);
    ui_->pedLog->setMaximumBlockCount(kMaxLogLines);
    initMenu();
    installMessageHandler();
}

LogControl::~LogControl()
{
    delete ui_;
}

void LogControl::installMessageHandler()
{
    g_logControl = this;

    // 进程级只能装一次：第二个 LogControl（目前不存在，但不该靠"目前"）
    // 装了会把上一个的转发链断掉。
    static bool installed = false;
    if (!installed) {
        installed = true;
        qInstallMessageHandler(&LogControl::messageHandler);
    }
}

void LogControl::messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    // 重入保护：appendLog 过程中的任何一步如果又产生一条 Qt 消息（比如
    // 模型越界警告），没有这个开关就会无限递归直到栈溢出。
    static thread_local bool inHandler = false;

    // 默认输出照旧送 stderr：桌面程序平时没有控制台，但调试构建在终端里
    // 启动时还得看得见。
    const QString line =
        QStringLiteral("[%1] %2%3\n")
            .arg(levelText(type), msg,
                 context.file != nullptr ? QStringLiteral("  (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line)
                                         : QString());
    std::fputs(qPrintable(line), stderr);

    // warning 以下不进界面日志（见头文件）。
    //
    // 显式列举而不是 `type < QtWarningMsg`：QtMsgType 的枚举顺序是
    // Debug=0/Warning=1/Critical=2/Fatal=3/**Info=4**，比较大小会把 Info
    // 当成比 Fatal 还严重。
    const bool important = type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
    if (!important || inHandler) {
        return;
    }
    inHandler = true;
    LogControl* target = g_logControl.data();
    if (target != nullptr) {
        // Queued：handler 可能在 io 线程或后台线程上执行，而 appendLog 动的是
        // QPlainTextEdit 与 QFile，只能在 GUI 线程碰。
        QMetaObject::invokeMethod(target, [target, line]() { target->appendLog(line); }, Qt::QueuedConnection);
    }
    inHandler = false;
}

void LogControl::initMenu()
{
    auto* menu = new QMenu(ui_->pedLog);

    auto* acReceiveDir = menu->addAction(QStringLiteral("设置接收目录..."));
    connect(acReceiveDir, &QAction::triggered, this, [this]() {
        QSettings settings;
        const QString current = settings.value(kReceiverDir, defaultReceiveDir()).toString();

        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("设置接收目录"), current);
        if (dir.isEmpty()) {
            return;
        }
        settings.setValue(kReceiverDir, dir);
        emit receiveDirChanged(dir);
    });

    // 发送前检查（v1 的"基本检查 + 覆盖提示"）。与「设定」对话框里的
    // 同名选项共用 kSendCheckBeforeSend 一个键，两处入口同步。
    QSettings settings;
    auto* acSendCheck = menu->addAction(QStringLiteral("发送前检查目标是否已存在"));
    acSendCheck->setCheckable(true);
    acSendCheck->setChecked(settings.value(kSendCheckBeforeSend, kSendCheckDefault).toBool());
    connect(acSendCheck, &QAction::toggled, this, [this](bool checked) {
        QSettings s;
        s.setValue(kSendCheckBeforeSend, checked);
        appendLog(checked ? QStringLiteral("[发送] 已开启发送前检查：对端已有同名文件时会逐项询问\n")
                          : QStringLiteral("[发送] 已关闭发送前检查：直接发送，对端已有文件会被覆盖\n"));
    });

    // 打开日志目录。v1 写了文件却没有入口，而"日志到底在哪"是事后第一个
    // 要问的问题——顺手给个能点的。
    auto* acOpenLogDir = menu->addAction(QStringLiteral("打开日志目录"));
    connect(acOpenLogDir, &QAction::triggered, this, []() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    auto* acClear = menu->addAction(QStringLiteral("清空"));
    connect(acClear, &QAction::triggered, this, [this]() { ui_->pedLog->clear(); });

    ui_->pedLog->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui_->pedLog, &QPlainTextEdit::customContextMenuRequested, this,
            [this, menu](const QPoint& pos) { menu->exec(ui_->pedLog->viewport()->mapToGlobal(pos)); });
}

void LogControl::appendLog(const QString& msg)
{
    ui_->pedLog->moveCursor(QTextCursor::End);
    ui_->pedLog->insertPlainText(msg);
    writeToFile(msg);
}

void LogControl::openLogFile(const QDate& day)
{
    if (logFile_.isOpen()) {
        logFile_.close();
    }
    logDay_ = day;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(dir);
    logFile_.setFileName(QStringLiteral("%1/gui-%2.log").arg(dir, day.toString(QStringLiteral("yyyy-MM-dd"))));
    // Append：同一天内多次启动接着写，不覆盖上一次的现场。
    // 打不开（磁盘满、目录只读）就返回：writeToFile 会看到 isOpen()==false
    // 并跳过写盘，界面日志不受影响。
    if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
}

void LogControl::writeToFile(const QString& text)
{
    const QDate today = QDate::currentDate();
    if (!logFile_.isOpen() || logDay_ != today) {
        openLogFile(today);
        if (logFile_.isOpen()) {
            // 每次启动留一条分隔，事后能看出"这一次"从哪开始。
            logFile_.write(QStringLiteral("----- %1 启动 relayer v%2 -----\n")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")),
                                    QCoreApplication::applicationVersion())
                               .toUtf8());
        }
    }
    if (!logFile_.isOpen()) {
        return;
    }
    if (logFile_.size() >= kMaxLogFileBytes) {
        return;   // 到上限就只留界面，不再写盘
    }
    logFile_.write(text.toUtf8());
    // 立刻 flush：崩溃时最后几行往往正是线索，不能留在缓冲区里丢掉。
    logFile_.flush();
}
