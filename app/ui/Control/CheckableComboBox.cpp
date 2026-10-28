#include "CheckableComboBox.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardItemModel>

CheckableComboBox::CheckableComboBox(QWidget* parent) : QComboBox(parent)
{
    setModel(new QStandardItemModel(this));
    setEditable(true);
    lineEdit()->setReadOnly(true);
    lineEdit()->setPlaceholderText(QStringLiteral("全部后缀（不筛选）"));

    // 在视图视口上拦截鼠标释放：勾选而不触发默认的“选中并关闭弹层”
    view()->viewport()->installEventFilter(this);
}

void CheckableComboBox::setOptions(const QStringList& options)
{
    const QStringList previously = checkedItems();
    auto* m = qobject_cast<QStandardItemModel*>(model());
    m->removeRows(0, m->rowCount());

    for (const QString& opt : options) {
        auto* item = new QStandardItem(opt);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setData(Qt::Unchecked, Qt::CheckStateRole);
        m->appendRow(item);
    }

    applyChecked(previously);
    updateDisplay();
}

QStringList CheckableComboBox::checkedItems() const
{
    QStringList result;
    const auto* m = qobject_cast<const QStandardItemModel*>(model());
    for (int i = 0; i < m->rowCount(); ++i) {
        const auto* item = m->item(i);
        if (item->checkState() == Qt::Checked) {
            result.append(item->text());
        }
    }
    return result;
}

void CheckableComboBox::setCheckedItems(const QStringList& items)
{
    applyChecked(items);
    updateDisplay();
    emit checkedItemsChanged(checkedItems());
}

void CheckableComboBox::applyChecked(const QStringList& items)
{
    auto* m = qobject_cast<QStandardItemModel*>(model());
    for (int i = 0; i < m->rowCount(); ++i) {
        auto* item = m->item(i);
        item->setCheckState(items.contains(item->text()) ? Qt::Checked : Qt::Unchecked);
    }
}

bool CheckableComboBox::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == view()->viewport() && event->type() == QEvent::MouseButtonRelease) {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            // Qt6 用 position()（QPointF）；v1 的 pos() 已弃用。
            const QModelIndex index = view()->indexAt(me->position().toPoint());
            if (index.isValid()) {
                toggleIndex(index);
                return true;   // 拦截：弹层保持打开
            }
        }
    }
    return QComboBox::eventFilter(obj, event);
}

void CheckableComboBox::keyPressEvent(QKeyEvent* event)
{
    // 弹层打开时空格键同样可勾选当前项
    if (event->key() == Qt::Key_Space && view()->isVisible() && view()->currentIndex().isValid()) {
        toggleIndex(view()->currentIndex());
        return;
    }
    QComboBox::keyPressEvent(event);
}

void CheckableComboBox::toggleIndex(const QModelIndex& index)
{
    auto* m = qobject_cast<QStandardItemModel*>(model());
    auto* item = m->itemFromIndex(index);
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) {
        return;
    }
    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    updateDisplay();
    emit checkedItemsChanged(checkedItems());
}

void CheckableComboBox::updateDisplay()
{
    const QStringList checked = checkedItems();
    // 无选中时清空文本以显示 placeholder（QComboBox::setEditText 空串不会显示占位，故用 clear）
    if (checked.isEmpty()) {
        lineEdit()->clear();
    } else {
        setEditText(checked.join(QStringLiteral("; ")));
    }
}
