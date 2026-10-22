#include "ExplorerControl.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <algorithm>

#include "ui_ExplorerControl.h"

namespace
{
// 后缀筛选里代表"无后缀文件"的特殊标签。
constexpr const char *kNoExtLabel = "(无后缀)";

QString typeText(bool isDir)
{
    return isDir ? QStringLiteral("Dir") : QStringLiteral("File");
}

QString sizeText(qint64 bytes)
{
    // 与 MainWindow::humanBytes 同规则；目录不显示大小。
    if (bytes <= 0)
    {
        return QStringLiteral("0.0 B");
    }
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        ++i;
    }
    return QString::number(v, 'f', 1) + QLatin1String(units[i]);
}
}  // namespace

ExplorerControl::ExplorerControl(QWidget *parent)
    : QDialog(parent)
    , ui_(new Ui::ExplorerControl)
{
    ui_->setupUi(this);
    initControl();
    initSignals();
}

void ExplorerControl::initControl()
{
    ui_->cbPath->setEditable(true);

    table_ = new ExpDropTable(this);
    const QStringList headers = {QStringLiteral(""), QStringLiteral("文件名称"), QStringLiteral("最后修改时间"),
                                 QStringLiteral("类型"), QStringLiteral("大小")};
    table_->setColumnCount(headers.size());
    table_->setHorizontalHeaderLabels(headers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ExpColumn::Icon), QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ExpColumn::Name), QHeaderView::Stretch);
    table_->setColumnWidth(toInt(ExpColumn::Icon), 30);
    table_->setColumnWidth(toInt(ExpColumn::Modified), 170);
    table_->setColumnWidth(toInt(ExpColumn::Type), 70);
    table_->setColumnWidth(toInt(ExpColumn::Size), 90);

    // 表头点击排序。
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->horizontalHeader()->setSortIndicatorShown(true);

    // 表格塞进 .ui 预留的占位区域。
    auto *layout = new QHBoxLayout(ui_->widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(table_);

    table_->setGetOwnRoot([this]() { return currentPath_; });
}

void ExplorerControl::initSignals()
{
    connect(ui_->btnEnter, &QPushButton::clicked, this, &ExplorerControl::onEnter);
    connect(ui_->btnHome, &QPushButton::clicked, this, &ExplorerControl::onHome);
    connect(ui_->btnUp, &QPushButton::clicked, this, &ExplorerControl::onUp);
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) { onDoubleClick(item ? item->row() : -1); });
    connect(table_, &QTableWidget::customContextMenuRequested, this, &ExplorerControl::onTableContextMenu);
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, &ExplorerControl::onHeaderClicked);

    // 名称筛选 200ms 防抖：大目录下避免每次按键都全量重建。
    auto *filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(200);
    connect(filterTimer, &QTimer::timeout, this, &ExplorerControl::onFilterChanged);
    connect(ui_->edSearch, &QLineEdit::textChanged, filterTimer, qOverload<>(&QTimer::start));
    // 后缀是点击式多选，无需防抖。
    connect(ui_->cbExt, &CheckableComboBox::checkedItemsChanged, this,
            [this](const QStringList &) { onFilterChanged(); });
    connect(ui_->btnFilter, &QPushButton::toggled, this, &ExplorerControl::onToggleFilter);
    connect(ui_->btnResetFilter, &QPushButton::clicked, this, &ExplorerControl::onResetFilter);

    // 筛选行默认隐藏，与按钮初始状态同步。
    ui_->filterWidget->setVisible(ui_->btnFilter->isChecked());
}

bool ExplorerControl::listDir(const QString &dir, QVector<FileEntry> &entries) const
{
    const QFileInfoList items =
        QDir(dir).entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Unsorted);
    entries.clear();
    entries.reserve(items.size());
    for (const QFileInfo &fi : items)
    {
        FileEntry e;
        e.name = fi.fileName();
        e.isDir = fi.isDir();
        e.size = fi.size();
        e.modifiedMs = fi.lastModified().toMSecsSinceEpoch();
        entries.push_back(e);
    }
    return true;
}

void ExplorerControl::enterPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(path);
    if (cleaned.isEmpty())
    {
        return;
    }
    QVector<FileEntry> items;
    if (!listDir(cleaned, items))
    {
        emit notice(QStringLiteral("无法访问目录：%1").arg(cleaned));
        return;
    }
    currentPath_ = cleaned;
    entries_ = items;
    ui_->cbPath->setCurrentText(cleaned);
    updateExtOptions();
    rebuildView();
}

void ExplorerControl::onEnter()
{
    enterPath(ui_->cbPath->currentText().trimmed());
}

void ExplorerControl::onHome()
{
    // 家目录 + 本机盘符列表进下拉，覆盖"换盘"的入口（v1 的 AskHomeAndDriver）。
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    ui_->cbPath->clear();
    ui_->cbPath->addItem(home);
    for (const QFileInfo &drive : QDir::drives())
    {
        ui_->cbPath->addItem(drive.absoluteFilePath());
    }
    ui_->cbPath->setCurrentText(home);
    enterPath(home);
}

void ExplorerControl::onUp()
{
    if (currentPath_.isEmpty())
    {
        return;
    }
    QDir dir(currentPath_);
    if (dir.cdUp())
    {
        enterPath(dir.absolutePath());
    }
}

void ExplorerControl::onDoubleClick(int row)
{
    if (row < 0)
    {
        return;
    }
    auto *typeItem = table_->item(row, toInt(ExpColumn::Type));
    auto *nameItem = table_->item(row, toInt(ExpColumn::Name));
    if (!typeItem || !nameItem || typeItem->text() != QStringLiteral("Dir"))
    {
        return;  // 只有目录可进入；文件的"打开"在右键菜单里
    }
    enterPath(QDir(currentPath_).filePath(nameItem->text()));
}

void ExplorerControl::onHeaderClicked(int index)
{
    // 第 0 列是图标，不参与排序。
    if (index <= 0)
    {
        return;
    }
    if (index == sortColumn_)
    {
        sortOrder_ = (sortOrder_ == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    }
    else
    {
        sortColumn_ = index;
        // 时间/大小默认降序（文件管理器习惯：最新的/最大的在前）。
        sortOrder_ = (index == toInt(ExpColumn::Modified) || index == toInt(ExpColumn::Size)) ? Qt::DescendingOrder
                                                                                              : Qt::AscendingOrder;
    }
    rebuildView();
}

void ExplorerControl::onFilterChanged()
{
    filterName_ = ui_->edSearch->text().trimmed();
    filterExts_ = ui_->cbExt->checkedItems();
    rebuildView();
}

void ExplorerControl::onToggleFilter(bool checked)
{
    ui_->filterWidget->setVisible(checked);
}

void ExplorerControl::onResetFilter()
{
    // edSearch 清空走防抖重建；后缀清空立即触发 checkedItemsChanged → onFilterChanged。
    ui_->edSearch->clear();
    ui_->cbExt->setCheckedItems({});
}

void ExplorerControl::updateExtOptions()
{
    QStringList exts;
    bool hasNoExt = false;
    for (const FileEntry &e : entries_)
    {
        if (e.isDir)
        {
            continue;
        }
        const QString ext = QFileInfo(e.name).suffix().toLower();
        if (ext.isEmpty())
        {
            hasNoExt = true;
        }
        else if (!exts.contains(ext))
        {
            exts.append(ext);
        }
    }
    exts.sort();
    if (hasNoExt)
    {
        exts.prepend(QLatin1String(kNoExtLabel));
    }
    // setOptions 自动保留同名旧勾选：跨目录导航时"只看某类文件"得以延续。
    ui_->cbExt->setOptions(exts);
}

void ExplorerControl::rebuildView()
{
    // 1) 过滤；目录与文件分两组（目录恒置顶，与资源管理器一致）。
    QVector<const FileEntry *> dirs;
    QVector<const FileEntry *> files;
    for (const FileEntry &e : entries_)
    {
        if (!filterName_.isEmpty() && !e.name.contains(filterName_, Qt::CaseInsensitive))
        {
            continue;
        }
        if (!filterExts_.isEmpty())
        {
            // 选了后缀后只显示匹配的文件，目录一律隐藏。
            if (e.isDir)
            {
                continue;
            }
            const QString rawExt = QFileInfo(e.name).suffix().toLower();
            const QString key = rawExt.isEmpty() ? QLatin1String(kNoExtLabel) : rawExt;
            if (!filterExts_.contains(key))
            {
                continue;
            }
        }
        (e.isDir ? dirs : files).push_back(&e);
    }

    // 2) 各组内独立排序，再按方向翻转（保持目录置顶）。
    const int col = sortColumn_;
    auto cmp = [col](const FileEntry *a, const FileEntry *b) { return lessThan(*a, *b, col); };
    std::stable_sort(dirs.begin(), dirs.end(), cmp);
    std::stable_sort(files.begin(), files.end(), cmp);
    if (sortOrder_ == Qt::DescendingOrder)
    {
        std::reverse(dirs.begin(), dirs.end());
        std::reverse(files.begin(), files.end());
    }

    // 3) 重建表格。大目录分批 processEvents，避免长时间白屏——代价是
    // 重建期间用户可能看到中间态，可接受（v1 同款取舍）。
    table_->setUpdatesEnabled(false);
    table_->clearContents();
    table_->setRowCount(0);
    int row = 0;
    for (const auto *group : {&dirs, &files})
    {
        for (const FileEntry *e : *group)
        {
            table_->insertRow(row);
            setFileItem(*e, row);
            ++row;
            if (row % 50 == 0)
            {
                QGuiApplication::processEvents();
            }
        }
    }
    table_->horizontalHeader()->blockSignals(true);
    table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    table_->horizontalHeader()->blockSignals(false);
    table_->setUpdatesEnabled(true);
}

bool ExplorerControl::lessThan(const FileEntry &a, const FileEntry &b, int column)
{
    // 主键比较；并列时以名称（不区分大小写）兜底，保证严格弱序。
    switch (column)
    {
    case static_cast<int>(ExpColumn::Modified):
        if (a.modifiedMs != b.modifiedMs)
        {
            return a.modifiedMs < b.modifiedMs;
        }
        break;
    case static_cast<int>(ExpColumn::Type):
        if (a.isDir != b.isDir)
        {
            return b.isDir;  // 目录排前
        }
        break;
    case static_cast<int>(ExpColumn::Size):
        if (a.size != b.size)
        {
            return a.size < b.size;
        }
        break;
    default:  // 名称
        break;
    }
    return a.name.toLower() < b.name.toLower();
}

void ExplorerControl::setFileItem(const FileEntry &entry, int row)
{
    static const QIcon dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    static const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    auto *iconItem = new QTableWidgetItem();
    iconItem->setIcon(entry.isDir ? dirIcon : fileIcon);
    iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    iconItem->setFlags(iconItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Icon), iconItem);

    auto *nameItem = new QTableWidgetItem(entry.name);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Name), nameItem);

    auto *timeItem = new QTableWidgetItem(
        QDateTime::fromMSecsSinceEpoch(entry.modifiedMs).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Modified), timeItem);

    auto *typeItem = new QTableWidgetItem(typeText(entry.isDir));
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Type), typeItem);

    auto *sizeItem = new QTableWidgetItem(entry.isDir ? QString() : sizeText(entry.size));
    sizeItem->setFlags(sizeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Size), sizeItem);
}

void ExplorerControl::onTableContextMenu(const QPoint &pos)
{
    // 动作表注册式写法（清单 #5）：{显示条件, 文字, 回调}，不再写
    // 十个 if (selectAction == xxx) 的平铺比较。
    const QList<QTableWidgetItem *> selected = table_->selectedItems();
    QSet<int> rowSet;
    for (auto *it : selected)
    {
        rowSet.insert(it->row());
    }
    const QVector<int> rows(rowSet.cbegin(), rowSet.cend());
    const bool singleRow = rows.size() == 1;
    const QString selectedPath =
        singleRow ? QDir(currentPath_).filePath(table_->item(rows[0], toInt(ExpColumn::Name))->text()) : QString();

    QMenu menu(this);
    const struct
    {
        bool visible;
        QString label;
        std::function<void()> run;
    } actions[] = {
        {true, QStringLiteral("新建文件夹"), [this]() { onNewDir(); }},
        {singleRow, QStringLiteral("打开"),
         [this, selectedPath, rows]()
         {
             const bool isDir = table_->item(rows[0], toInt(ExpColumn::Type))->text() == QStringLiteral("Dir");
             QDesktopServices::openUrl(QUrl::fromLocalFile(isDir ? selectedPath : currentPath_));
         }},
        {singleRow, QStringLiteral("复制全路径"),
         [this, selectedPath]() { QApplication::clipboard()->setText(selectedPath); }},
        {singleRow, QStringLiteral("重命名"), [this, rows]() { onRename(rows[0]); }},
        {!rows.isEmpty(), QStringLiteral("删除"), [this, rows]() { onDelete(rows); }},
    };
    for (const auto &a : actions)
    {
        if (!a.visible)
        {
            continue;
        }
        connect(menu.addAction(a.label), &QAction::triggered, this, a.run);
    }
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void ExplorerControl::onRename(int row)
{
    const QString oldName = table_->item(row, toInt(ExpColumn::Name))->text();
    bool ok = false;
    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("请输入新文件名"),
                                                  QLineEdit::Normal, oldName, &ok)
                                .trimmed();
    if (!ok || newName.isEmpty() || newName == oldName)
    {
        return;
    }
    const QDir dir(currentPath_);
    if (!QFile::rename(dir.filePath(oldName), dir.filePath(newName)))
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("重命名失败（目标名已存在？权限不足？）"));
        return;
    }
    enterPath(currentPath_);  // 重列：简单且正确，不做手术式更新
}

void ExplorerControl::onDelete(const QVector<int> &rows)
{
    if (rows.isEmpty())
    {
        return;
    }
    QStringList names;
    for (int row : rows)
    {
        names << table_->item(row, toInt(ExpColumn::Name))->text();
    }
    const auto choice =
        QMessageBox::question(this, QStringLiteral("确认"),
                              QStringLiteral("删除选中的 %1 项？目录将连同内容一起删除，不可恢复。").arg(names.size()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }
    const QDir dir(currentPath_);
    QStringList failed;
    for (const QString &name : names)
    {
        const QString path = dir.filePath(name);
        const QFileInfo fi(path);
        bool ok = false;
        if (fi.isDir())
        {
            ok = QDir(path).removeRecursively();
        }
        else
        {
            ok = QFile::remove(path);
        }
        if (!ok)
        {
            failed << name;
        }
    }
    if (!failed.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("%1 个项目删除失败（可能被占用或权限不足）：%2")
                                 .arg(failed.size())
                                 .arg(failed.join(QStringLiteral("、"))));
    }
    enterPath(currentPath_);
}

void ExplorerControl::onNewDir()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"), QStringLiteral("请输入文件夹名"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
    {
        return;
    }
    const QDir dir(currentPath_);
    if (!dir.mkdir(name))
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("新建失败（同名已存在？权限不足？）"));
        return;
    }
    enterPath(currentPath_);
}
