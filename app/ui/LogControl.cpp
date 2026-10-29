#include "LogControl.h"

#include <QFileDialog>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>

#include "SettingsKeys.h"
#include "ui_LogControl.h"

using relayer::app::settings::kReceiverDir;
using relayer::app::settings::kSendCheckBeforeSend;
using relayer::app::settings::kSendCheckDefault;

namespace {
// 日志控件最大保留行数，超出后最早的日志被挤掉（与 v1 一致）。
constexpr int kMaxLogLines = 10000;

QString defaultReceiveDir()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}
}   // namespace

LogControl::LogControl(QWidget* parent) : QDialog(parent), ui_(new Ui::LogControl)
{
    ui_->setupUi(this);
    ui_->pedLog->setReadOnly(true);
    ui_->pedLog->setMaximumBlockCount(kMaxLogLines);
    initMenu();
}

LogControl::~LogControl()
{
    delete ui_;
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
    //
    // 接收端没有"覆盖开关"了：覆盖与否由发送方在传输开始前问完，接收端
    // 收到什么就是对方明确要发的（CoreService::makeConfig）。
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
}
