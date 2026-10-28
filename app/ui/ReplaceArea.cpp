#include "ReplaceArea.h"

#include <QPushButton>

#include "ui_ReplaceArea.h"

ReplaceArea::ReplaceArea(QWidget* parent) : QDialog(parent), ui_(new Ui::ReplaceArea)
{
    ui_->setupUi(this);

    // 默认全字段、区分大小写、普通替换（同 v1 的初始状态）。
    ui_->cBoxName->setChecked(true);
    ui_->cBoxLocal->setChecked(true);
    ui_->cBoxPeer->setChecked(true);
    ui_->cBoxRel->setChecked(true);
    ui_->rbUseCase->setChecked(true);
    ui_->rbNormalRep->setChecked(true);

    // 「确认」做成默认按钮：回车即确认，Esc 即取消（QDialog 自带）。
    ui_->btnOk->setDefault(true);
    connect(ui_->btnOk, &QPushButton::clicked, this, [this]() {
        result_.apply = true;
        result_.name = ui_->cBoxName->isChecked();
        result_.localDir = ui_->cBoxLocal->isChecked();
        result_.peer = ui_->cBoxPeer->isChecked();
        result_.relDir = ui_->cBoxRel->isChecked();
        result_.caseSensitive = ui_->rbUseCase->isChecked();
        result_.useRegex = ui_->rbRegexRep->isChecked();
        accept();
    });
    connect(ui_->btnCancel, &QPushButton::clicked, this, [this]() {
        result_.apply = false;
        reject();
    });
}

ReplaceArea::~ReplaceArea()
{
    delete ui_;
}

ReplaceScope ReplaceArea::result() const
{
    return result_;
}
