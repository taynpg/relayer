#include "LogControl.h"

#include <QFileDialog>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>

#include "ui_LogControl.h"

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
                const QString current = settings.value("receiver/dir", defaultReceiveDir()).toString();

                const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("设置接收目录"), current);
                if (dir.isEmpty())
                {
                    return;
                }
                settings.setValue("receiver/dir", dir);
                emit receiveDirChanged(dir);
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
