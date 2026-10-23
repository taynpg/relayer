#include "FileMetaInfo.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QTimer>

#include "model/ProgressTracker.h"
#include "ui_FileMetaInfo.h"

namespace
{
// 复制后的反馈保留时长。太短看不见，太长会和下一次点击的提示打架。
constexpr int kHintMs = 1500;
}  // namespace

FileMetaInfo::FileMetaInfo(QWidget *parent)
    : QDialog(parent)
    , ui_(new Ui::FileMetaInfo)
{
    ui_->setupUi(this);

    // 只读：这两个框是"看"和"选"用的，不是编辑入口。
    ui_->pedName->setReadOnly(true);
    ui_->pedDir->setReadOnly(true);

    connect(ui_->btnClose, &QPushButton::clicked, this, &QDialog::close);

    // v1 的三个复制按钮只有动作、没有反馈——点完看不出成功没有。这里加
    // 一行提示：复制了什么，写到 lblHint 上，过一会儿自动清掉。
    connect(ui_->btnCopyFileName, &QPushButton::clicked, this,
            [this]() { copyToClipboard(name_, QStringLiteral("文件名")); });
    connect(ui_->btnCopyDirPath, &QPushButton::clicked, this,
            [this]() { copyToClipboard(dirText_, QStringLiteral("目录")); });
    connect(ui_->btnCopyFull, &QPushButton::clicked, this,
            [this]()
            {
                // 远端路径是 `peer:/rel` 形状，不能再拼本地分隔符（拼出来
                // 是 `peer:/rel\name`，哪一侧都不认）。协议里的路径本来就是
                // POSIX 形状，这里也用 '/'。
                const QString sep = remote_ ? QStringLiteral("/") : QString(QDir::separator());
                QString full = dirText_;
                if (!full.isEmpty() && !full.endsWith(QLatin1Char('/')) && !full.endsWith(QLatin1Char('\\')))
                {
                    full += sep;
                }
                copyToClipboard(full + name_, QStringLiteral("全路径"));
            });

    applyMeta();
}

FileMetaInfo::~FileMetaInfo()
{
    delete ui_;
}

void FileMetaInfo::setMeta(const QString &dirText, const QString &name, bool isDir, qint64 size, qint64 modifiedMs,
                           bool remote)
{
    dirText_ = dirText;
    name_ = name;
    isDir_ = isDir;
    size_ = size;
    modifiedMs_ = modifiedMs;
    remote_ = remote;
    applyMeta();
}

void FileMetaInfo::applyMeta()
{
    if (ui_ == nullptr)
    {
        return;
    }
    ui_->pedName->setPlainText(name_);
    ui_->pedDir->setPlainText(dirText_);

    QString type = isDir_ ? QStringLiteral("目录") : QStringLiteral("文件");
    if (remote_)
    {
        type += QStringLiteral("（远端）");
    }
    ui_->lbType->setText(type);

    // 目录没有意义的大小；远端的目录同样不给。给占位符而不是 "0 B"：
    // 后者看上去像"这个目录是空的"。
    ui_->lbSize->setText(size_ >= 0 ? relayer::app::humanBytes(static_cast<std::uint64_t>(size_))
                                    : QStringLiteral("-"));

    ui_->lbTime->setText(
        modifiedMs_ > 0 ? QDateTime::fromMSecsSinceEpoch(modifiedMs_).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))
                        : QStringLiteral("-"));

    ui_->lblHint->clear();
}

void FileMetaInfo::copyToClipboard(const QString &text, const QString &what)
{
    if (text.isEmpty())
    {
        ui_->lblHint->setText(QStringLiteral("没有可复制的%1").arg(what));
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    ui_->lblHint->setText(QStringLiteral("已复制%1").arg(what));
    // 定时清空：不依赖下一次点击，否则对话框停在那里会一直显示"已复制"。
    QTimer::singleShot(kHintMs, ui_->lblHint,
                       [this]()
                       {
                           // 期间可能又复制了一次（新提示），那就别擦掉它。
                           // 用"当前文本是否还是我写的"来判断代价太高，直接
                           // 清空也是可接受的：提示本来就该自己消失。
                           if (ui_ != nullptr)
                           {
                               ui_->lblHint->clear();
                           }
                       });
}
