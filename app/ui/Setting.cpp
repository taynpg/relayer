#include "Setting.h"

#include <QFileDialog>
#include <QIntValidator>
#include <QMessageBox>
#include <QSettings>

#include "SettingsKeys.h"
#include "ui_Setting.h"

using relayer::app::settings::kAutoRetryDefault;
using relayer::app::settings::kReceiverAutoRetry;
using relayer::app::settings::kReceiverOverwrite;
using relayer::app::settings::kReceiverOverwriteDefault;
using relayer::app::settings::kReceiverRetryInterval;
using relayer::app::settings::kReceiverRetryMax;
using relayer::app::settings::kReceiverShareRoot;
using relayer::app::settings::kRetryIntervalDefault;
using relayer::app::settings::kRetryMaxDefault;

Setting::Setting(QWidget *parent)
    : QDialog(parent)
    , ui_(new Ui::Setting)
{
    ui_->setupUi(this);

    auto *nonNegative = new QIntValidator(0, 2147483647, this);
    ui_->edReconInterval->setValidator(nonNegative);
    ui_->edMaxRetry->setValidator(nonNegative);

    connect(ui_->rbRecon, &QRadioButton::clicked, this, &Setting::onRetryModeChanged);
    connect(ui_->rbNoRecon, &QRadioButton::clicked, this, &Setting::onRetryModeChanged);
    connect(ui_->btnSave, &QPushButton::clicked, this, &Setting::onSave);
    connect(ui_->btnExit, &QPushButton::clicked, this, &QDialog::reject);

    // 共享目录（PROTOCOL.md §11.8）。放在这里而不是日志右键菜单：那是
    // "接收目录"的入口，而共享是另一个概念（往里写 vs 让人看）。
    connect(ui_->btnShareBrowse, &QPushButton::clicked, this,
            [this]()
            {
                const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要对端可见的目录"),
                                                                      ui_->edShareRoot->text());
                if (!dir.isEmpty())
                {
                    ui_->edShareRoot->setText(dir);
                }
            });
    connect(ui_->btnShareClear, &QPushButton::clicked, this, [this]() { ui_->edShareRoot->clear(); });

    loadSettings();
}

Setting::~Setting()
{
    delete ui_;
}

void Setting::loadSettings()
{
    QSettings settings;
    const bool autoRetry = settings.value(kReceiverAutoRetry, kAutoRetryDefault).toBool();
    ui_->rbRecon->setChecked(autoRetry);
    ui_->rbNoRecon->setChecked(!autoRetry);
    ui_->edReconInterval->setText(settings.value(kReceiverRetryInterval, kRetryIntervalDefault).toString());
    ui_->edMaxRetry->setText(settings.value(kReceiverRetryMax, kRetryMaxDefault).toString());
    ui_->ckOverwrite->setChecked(settings.value(kReceiverOverwrite, kReceiverOverwriteDefault).toBool());
    ui_->edShareRoot->setText(settings.value(kReceiverShareRoot).toString());

    // 停止重试时间隔/次数不可用，表达"这两个值不会被使用"（同连接面板
    // 的无认证勾选禁用口令框的手法）。
    onRetryModeChanged();
}

void Setting::onRetryModeChanged()
{
    const bool retrying = ui_->rbRecon->isChecked();
    ui_->edReconInterval->setEnabled(retrying);
    ui_->label->setEnabled(retrying);
    ui_->edMaxRetry->setEnabled(retrying);
    ui_->label_2->setEnabled(retrying);
}

void Setting::onSave()
{
    QSettings settings;
    settings.setValue(kReceiverAutoRetry, ui_->rbRecon->isChecked());

    int interval = ui_->edReconInterval->text().toInt();
    if (interval <= 0)
    {
        interval = kRetryIntervalDefault;  // 0 或空输入都归位到默认值
    }
    settings.setValue(kReceiverRetryInterval, interval);

    const int maxRetry = ui_->edMaxRetry->text().toInt();
    settings.setValue(kReceiverRetryMax, maxRetry < 0 ? 0 : maxRetry);

    settings.setValue(kReceiverOverwrite, ui_->ckOverwrite->isChecked());

    // 共享目录改动只在**下一次连接**生效：正在跑的那个接收会话已经拿到
    // 了一份配置副本，改 QSettings 不会回头影响它。提示写在 onSave 里。
    const QString previousShare = settings.value(kReceiverShareRoot).toString();
    const QString newShare = ui_->edShareRoot->text().trimmed();
    settings.setValue(kReceiverShareRoot, newShare);

    accept();

    // 共享目录改了但当前会话不会受影响（会话启动时已拷走配置）。不说清楚
    // 的话，用户会以为是"设置没保存"或"对端列举有问题"。
    if (previousShare != newShare)
    {
        QMessageBox::information(this, QStringLiteral("共享目录已保存"),
                                 QStringLiteral("共享范围在下一次连接后生效。\n"
                                                "若希望立刻生效，请断开再连接（或重启程序）。"));
    }
}
