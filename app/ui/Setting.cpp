#include "Setting.h"

#include <QIntValidator>
#include <QMessageBox>
#include <QSettings>

#include "SettingsKeys.h"
#include "ui_Setting.h"

using relayer::app::settings::kAutoRetryDefault;
using relayer::app::settings::kReceiverAutoRetry;
using relayer::app::settings::kReceiverRetryInterval;
using relayer::app::settings::kReceiverRetryMax;
using relayer::app::settings::kRetryIntervalDefault;
using relayer::app::settings::kRetryMaxDefault;
using relayer::app::settings::kSendCheckBeforeSend;
using relayer::app::settings::kSendCheckDefault;

Setting::Setting(QWidget* parent) : QDialog(parent), ui_(new Ui::Setting)
{
    ui_->setupUi(this);

    auto* nonNegative = new QIntValidator(0, 2147483647, this);
    ui_->edReconInterval->setValidator(nonNegative);
    ui_->edMaxRetry->setValidator(nonNegative);

    connect(ui_->rbRecon, &QRadioButton::clicked, this, &Setting::onRetryModeChanged);
    connect(ui_->rbNoRecon, &QRadioButton::clicked, this, &Setting::onRetryModeChanged);
    connect(ui_->btnSave, &QPushButton::clicked, this, &Setting::onSave);
    connect(ui_->btnExit, &QPushButton::clicked, this, &QDialog::reject);

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
    ui_->ckSendCheck->setChecked(settings.value(kSendCheckBeforeSend, kSendCheckDefault).toBool());

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
    if (interval <= 0) {
        interval = kRetryIntervalDefault;   // 0 或空输入都归位到默认值
    }
    settings.setValue(kReceiverRetryInterval, interval);

    const int maxRetry = ui_->edMaxRetry->text().toInt();
    settings.setValue(kReceiverRetryMax, maxRetry < 0 ? 0 : maxRetry);

    settings.setValue(kSendCheckBeforeSend, ui_->ckSendCheck->isChecked());

    accept();
}
