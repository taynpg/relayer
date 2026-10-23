#include "OwnTableWidget.h"

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>

#include "Base/InfoDrop.h"

// ---------------------------------------------------------------------------
// ComDropTable
// ---------------------------------------------------------------------------

ComDropTable::ComDropTable(QWidget *parent)
    : QTableWidget(parent)
{
    setAcceptDrops(true);
}

bool ComDropTable::isPathColumn(int col)
{
    // v2 只有一个拖拽路径列：本地目录。对端与相对目录是打字填的，
    // 从资源管理器拖不出这两个值。
    return col == toInt(ComColumn::LocalDir);
}

bool ComDropTable::isReadOnlyColumn(int col)
{
    // 列的可编辑性在这里一处定义（清单 #1 的"表头配置表"）。拖拽、
    // 补列、恢复配置全部走 ensureItem → 这一个函数，不存在 v1 的
    // "建 item 时设了只读、已有 item 时不设"的分叉。
    return col == toInt(ComColumn::Id) ||    // 程序维护
           col == toInt(ComColumn::Type) ||  // 由 Name 推导
           col == toInt(ComColumn::Mark);
}

QTableWidgetItem *ComDropTable::ensureItem(int row, int col)
{
    if (auto *existing = item(row, col))
    {
        return existing;
    }
    auto *created = new QTableWidgetItem();
    if (isReadOnlyColumn(col))
    {
        created->setFlags(created->flags() & ~Qt::ItemIsEditable);
    }
    setItem(row, col, created);
    return created;
}

void ComDropTable::setText(int row, int col, const QString &text)
{
    // 前置 ensureItem：调用方不必关心该格此前是否存在。
    ensureItem(row, col)->setText(text);
}

void ComDropTable::setReadOnly(int row, int col, bool readOnly)
{
    auto *it = ensureItem(row, col);
    Qt::ItemFlags flags = it->flags();
    if (readOnly)
    {
        flags &= ~Qt::ItemIsEditable;
    }
    else
    {
        flags |= Qt::ItemIsEditable;
    }
    it->setFlags(flags);
}

void ComDropTable::ensureRowComplete(int row)
{
    for (int col = 0; col < columnCount(); ++col)
    {
        ensureItem(row, col);
    }
}

void ComDropTable::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasFormat(MY_MIME_DROP_TYPE) || event->mimeData()->hasFormat(MY_MIME_DROP_REMOTE_MARK))
    {
        // 远端来的载荷里 from 是"相对共享根"，不是本地路径——落进本地目录列
        // 只会写出一句谁都认不出的路径。整个拒收，比落进去再报错好。
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void ComDropTable::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(MY_MIME_DROP_TYPE) || event->mimeData()->hasFormat(MY_MIME_DROP_REMOTE_MARK))
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void ComDropTable::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasFormat(MY_MIME_DROP_REMOTE_MARK))
    {
        event->ignore();
        return;
    }
    const QByteArray payload = event->mimeData()->data(MY_MIME_DROP_TYPE);
    const InfoDrop infoDrop = infoUnpack<InfoDrop>(payload);

    const QPoint pos = event->position().toPoint();
    const int startRow = rowAt(pos.y());
    const int startCol = columnAt(pos.x());

    // 清单 #2：落点判定不再写死 4/5。
    if (!isPathColumn(startCol))
    {
        return;
    }

    int curRow = startRow < 0 ? rowCount() : startRow;
    for (const auto &dropped : infoDrop.items)
    {
        if (curRow >= rowCount())
        {
            insertRow(rowCount());
        }
        ensureRowComplete(curRow);

        const bool isDir = dropped.type == 0;
        setText(curRow, toInt(ComColumn::Name), dropped.fileName);
        setText(curRow, toInt(ComColumn::Type), isDir ? QStringLiteral("Dir") : QStringLiteral("File"));
        setText(curRow, toInt(ComColumn::Mark), markFor(dropped.fileName, isDir));

        // 目录落的是"源目录/名字"，文件直接落拖拽来源的完整路径（v1 语义）。
        const QString pathContent = isDir ? QDir(infoDrop.from).filePath(dropped.fileName) : infoDrop.from;
        setText(curRow, toInt(ComColumn::LocalDir), pathContent);

        ++curRow;
    }
    event->acceptProposedAction();
}

QString ComDropTable::markFor(const QString &filePath, bool isDir) const
{
    if (isDir)
    {
        return QStringLiteral("Dir");
    }
    const QString suffix = QFileInfo(filePath).suffix();
    return suffix.isEmpty() ? QStringLiteral("File") : suffix.toLower();
}

// ---------------------------------------------------------------------------
// ExpDropTable
// ---------------------------------------------------------------------------

ExpDropTable::ExpDropTable(QWidget *parent)
    : QTableWidget(parent)
{
    setAcceptDrops(true);
    setDragEnabled(true);
}

void ExpDropTable::setGetOwnRoot(std::function<QString()> getOwnRoot)
{
    getOwnRoot_ = std::move(getOwnRoot);
}

void ExpDropTable::setOnInfoDropped(std::function<void(const InfoDrop &)> onDropped)
{
    onInfoDropped_ = std::move(onDropped);
}

void ExpDropTable::setRemoteSource(bool remote)
{
    remoteSource_ = remote;
}

void ExpDropTable::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        dragStart_ = event->position().toPoint();
    }
    QTableWidget::mousePressEvent(event);
}

void ExpDropTable::mouseMoveEvent(QMouseEvent *event)
{
    if (!(event->buttons() & Qt::LeftButton))
    {
        return;
    }
    if ((event->position().toPoint() - dragStart_).manhattanLength() < QApplication::startDragDistance())
    {
        return;
    }

    // 清单 #4/#7：不假设"选中项必然整行且列数对齐"。selectedRows()
    // 给出的是行号集合，每行按列名取值，选中不完整时宁可少拖也不取错。
    InfoDrop infoDrop;
    if (getOwnRoot_)
    {
        infoDrop.from = getOwnRoot_();
    }
    const QList<QTableWidgetItem *> selected = selectedItems();
    QSet<int> rows;
    for (auto *it : selected)
    {
        rows.insert(it->row());
    }
    for (int row : rows)
    {
        auto *nameItem = item(row, toInt(ExpColumn::Name));
        auto *typeItem = item(row, toInt(ExpColumn::Type));
        if (!nameItem || !typeItem)
        {
            continue;
        }
        InfoDropItem dropItem;
        dropItem.fileName = nameItem->text();
        dropItem.type = typeItem->text() == QStringLiteral("Dir") ? 0 : 1;
        infoDrop.items.push_back(dropItem);
    }
    if (infoDrop.items.isEmpty())
    {
        return;
    }

    auto *mimeData = new QMimeData();
    mimeData->setData(MY_MIME_DROP_TYPE, infoPack(infoDrop));
    if (remoteSource_)
    {
        // 只给对照表看的"别收"标记（见头文件）。宿主的表格类型检查不认它，
        // 认它的只有 ComDropTable。
        mimeData->setData(MY_MIME_DROP_REMOTE_MARK, QByteArrayLiteral("1"));
    }

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction | Qt::MoveAction);
}

void ExpDropTable::dropEvent(QDropEvent *event)
{
    const auto *source = qobject_cast<const QTableWidget *>(event->source());
    if (source != this && event->mimeData()->hasFormat(MY_MIME_DROP_TYPE))
    {
        if (onInfoDropped_)
        {
            onInfoDropped_(infoUnpack<InfoDrop>(event->mimeData()->data(MY_MIME_DROP_TYPE)));
        }
        event->acceptProposedAction();
        return;
    }
    // 自拖不处理（本表内移动没有意义），交给默认行为忽略。
    QTableWidget::dropEvent(event);
}

void ExpDropTable::dragEnterEvent(QDragEnterEvent *event)
{
    const auto *source = qobject_cast<const QTableWidget *>(event->source());
    if (source == this)
    {
        event->ignore();
        return;
    }
    if (!event->mimeData()->hasFormat(MY_MIME_DROP_TYPE))
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void ExpDropTable::dragMoveEvent(QDragMoveEvent *event)
{
    const auto *source = qobject_cast<const QTableWidget *>(event->source());
    if (source == this)
    {
        event->ignore();
        return;
    }
    if (!event->mimeData()->hasFormat(MY_MIME_DROP_TYPE))
    {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}
