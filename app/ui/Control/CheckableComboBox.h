#pragma once

#include <QComboBox>
#include <QStringList>

class QStandardItemModel;

// 可多选的组合框：下拉项带复选框，点击勾选且弹层不关闭，
// 选中项以分隔符拼接显示在只读输入框内；无选中时显示占位提示。
//
// v1 原样迁移（ExplorerControl 用它做后缀过滤器）。自包含控件，不依赖
// 项目内任何其他类，唯一调整是 Qt6 的 QMouseEvent::position()。
class CheckableComboBox : public QComboBox
{
    Q_OBJECT
public:
    explicit CheckableComboBox(QWidget *parent = nullptr);

    // 重建可选项；同名的旧选中状态自动保留
    void setOptions(const QStringList &options);
    QStringList checkedItems() const;

public slots:
    // 按名称设置选中项（不存在的名称忽略），更新显示并发射信号
    void setCheckedItems(const QStringList &items);

signals:
    void checkedItemsChanged(const QStringList &items);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyChecked(const QStringList &items);
    void toggleIndex(const QModelIndex &index);
    void updateDisplay();
};
