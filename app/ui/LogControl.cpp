#include "LogControl.h"

#include <QFileDialog>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>

#include "SettingsKeys.h"
#include "ui_LogControl.h"

using relayer::app::settings::kReceiverDir;
using relayer::app::settings::kReceiverOverwrite;
using relayer::app::settings::kReceiverOverwriteDefault;

namespace
{
// 日志控件最大保留行数，超出后最早的日志被挤掉（与 v1 一致）。
constexpr int kMaxLogLines = 10000;

QString defaultReceiveDir()
{
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return downloads.isEmpty() ? QDir::homePath() : downloads;
}
}  // namespace

LogControl::LogControl(QWidget *parent)
    : QDialog(parent)
    , ui_(new Ui::LogControl)
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
    auto *menu = new QMenu(ui_->pedLog);

    auto *acReceiveDir = menu->addAction(QStringLiteral("设置接收目录..."));
    connect(acReceiveDir, &QAction::triggered, this,
            [this]()
            {
                QSettings settings;
                const QString current = settings.value(kReceiverDir, defaultReceiveDir()).toString();

                const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("设置接收目录"), current);
                if (dir.isEmpty())
                {
                    return;
                }
                settings.setValue(kReceiverDir, dir);
                emit receiveDirChanged(dir);
            });

    // 共享目录**不在这里**：它是"给别人看什么"，与"往哪写"是两个概念，
    // 入口统一放在「设定」对话框（Setting.ui 的 gbShare），避免同一设置
    // 两个入口、改了一处以为生效。

    // 接收覆盖开关。默认开：反复把同一个文件发到对方同一目录是核心
    // 场景，拒绝覆盖会让第二次发送起永远失败。关掉则恢复"目标已存在
    // 就拒绝"的保护行为。与 CoreService::makeConfig 读的是同一个键。
    QSettings settings;
    auto *acOverwrite = menu->addAction(QStringLiteral("覆盖已存在的文件"));
    acOverwrite->setCheckable(true);
    acOverwrite->setChecked(settings.value(kReceiverOverwrite, kReceiverOverwriteDefault).toBool());
    connect(acOverwrite, &QAction::toggled, this,
            [this](bool checked)
            {
                QSettings s;
                s.setValue(kReceiverOverwrite, checked);
                appendLog(checked ? QStringLiteral("[接收] 已开启覆盖：目标已存在时删旧收新\n")
                                  : QStringLiteral("[接收] 已关闭覆盖：目标已存在时拒绝接收\n"));
            });

    auto *acClear = menu->addAction(QStringLiteral("清空"));
    connect(acClear, &QAction::triggered, this, [this]() { ui_->pedLog->clear(); });

    ui_->pedLog->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui_->pedLog, &QPlainTextEdit::customContextMenuRequested, this,
            [this, menu](const QPoint &pos) { menu->exec(ui_->pedLog->viewport()->mapToGlobal(pos)); });
}

void LogControl::appendLog(const QString &msg)
{
    ui_->pedLog->moveCursor(QTextCursor::End);
    ui_->pedLog->insertPlainText(msg);
}
