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
// 远端路径一律用 `a/b`（协议里也是这个形式），不套用本地平台规则：
// 对端可能是另一个系统，本地规则未必适用。
QString joinRel(const QString &base, const QString &name)
{
    return base.isEmpty() ? name : base + QLatin1Char('/') + name;
}

// 条目名能不能当成"一个目录名"往当前路径上拼。
//
// core 的 decodeAnswerFileList 已经拦了空名 / `.` / `..` / 含 `/` 的条目
// （那是协议层的输入合法性，所有消费者都受保护）。这里补的是**本平台的**
// 分隔符语义：Windows 上反斜杠也是分隔符，而 Unix 允许文件名含它。
// 不按平台区分的话，要么 Windows 上会把名字拆成两段（可能走到共享根之外），
// 要么 Unix 上点不进一个合法目录。
bool isPathLikeName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
    {
        return true;
    }
    if (name.contains(QLatin1Char('/')))
    {
        return true;
    }
#ifdef Q_OS_WIN
    if (name.contains(QLatin1Char('\\')))
    {
        return true;
    }
#endif
    return false;
}

QString parentRel(const QString &path)
{
    const auto slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

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
    table_->setColumnCount(static_cast<int>(headers.size()));
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
    if (source_ == Source::Remote)
    {
        if (peerId_.isEmpty())
        {
            // 常驻的面板还没选对端：不发请求，保持空表（displayPath 已经
            // 显示"<未选择对端>"）。
            entries_.clear();
            rebuildView();
            return;
        }
        // 远端：异步。只发请求，等 applyRemoteEntries 回来才动 currentPath_。
        const QString rel = path;
        remotePendingPath_ = rel;
        // 先显示"要去的地方"，回来再以 currentPath_ 为准（失败时停在原地）。
        // 用 showPath 而不是 setCurrentText：后者在不可编辑的 QComboBox 上
        // 对不存在的项是静默空操作，界面会看起来毫无反应。
        showPath(QStringLiteral("%1:/%2").arg(peerId_, rel));
        table_->setRowCount(0);
        setStatus(QStringLiteral("正在读取 %1 …（对端离线时约 10 秒后超时）")
                      .arg(rel.isEmpty() ? QStringLiteral("<共享根>") : rel));
        emit notice(
            QStringLiteral("[浏览] 读取 %1:%2 …").arg(peerId_, rel.isEmpty() ? QStringLiteral("<共享根>") : rel));
        emit remoteListRequested(peerId_, rel);
        return;
    }

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

    // 本地面板也给状态：两栏并排时，一边有字数统计、一边空白会让人以为
    // 另一边没在工作。
    int dirs = 0;
    for (const auto &e : items)
    {
        if (e.isDir)
        {
            ++dirs;
        }
    }
    setStatus(QStringLiteral("%1 项（%2 个目录，%3 个文件）").arg(items.size()).arg(dirs).arg(items.size() - dirs));
}

void ExplorerControl::onEnter()
{
    if (source_ == Source::Remote)
    {
        // 远端路径不可手输：它相对对端共享根，手敲绝对路径没有意义。
        // 导航靠双击、上一级与"根目录"按钮。
        return;
    }
    enterPath(ui_->cbPath->currentText().trimmed());
}

void ExplorerControl::onHome()
{
    if (source_ == Source::Remote)
    {
        // 远端模式下这个按钮是"回到共享根"（按钮文字在 setRemotePeer 里改）。
        enterPath(QString());
        return;
    }

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
    if (source_ == Source::Remote)
    {
        if (currentPath_.isEmpty())
        {
            return;  // 已在根
        }
        enterPath(parentRel(currentPath_));
        return;
    }
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
    if (source_ == Source::Remote)
    {
        const QString name = nameItem->text();
        if (isPathLikeName(name))
        {
            // 拒绝进入而不是"尽力拼一下"：拼出来的路径可能指向共享根之外，
            // 服务端会再拒一次，但客户端不该把这种请求发出去。
            emit notice(QStringLiteral("[浏览] 条目名不是单段，拒绝进入：%1").arg(name));
            return;
        }
        enterPath(joinRel(currentPath_, name));
        return;
    }
    enterPath(QDir(currentPath_).filePath(nameItem->text()));
}

void ExplorerControl::showPath(const QString &text)
{
    // 远端模式 cbPath 不可编辑：setCurrentText 在非可编辑的 QComboBox 上
    // 只查已有项，查不到就静默忽略。改成"清空 + 加一项 + 选中"。
    ui_->cbPath->clear();
    ui_->cbPath->addItem(text);
    ui_->cbPath->setCurrentIndex(0);
}

void ExplorerControl::setStatus(const QString &text, bool error)
{
    ui_->lblStatus->setText(text);
    ui_->lblStatus->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

QString ExplorerControl::displayPath() const
{
    if (source_ == Source::Local)
    {
        return currentPath_;
    }
    if (peerId_.isEmpty())
    {
        return QStringLiteral("<未选择对端>");
    }
    return QStringLiteral("%1:/%2").arg(peerId_, currentPath_);
}

void ExplorerControl::setRemotePeer(const QString &peerId, const QString &peerName)
{
    source_ = Source::Remote;
    peerId_ = peerId;
    peerName_ = peerName;
    currentPath_.clear();
    entries_.clear();
    remotePendingPath_.clear();

    // 远端只读：拖出（载荷是本地路径）与文件系统操作都不适用，
    // 直接关掉入口，而不是留着能点但点了报错。
    table_->setDragEnabled(false);
    ui_->cbPath->setEditable(false);
    ui_->btnEnter->setEnabled(false);
    ui_->btnHome->setText(QStringLiteral("共享根"));
    ui_->filterWidget->setVisible(ui_->btnFilter->isChecked());

    showPath(displayPath());
    rebuildView();

    if (peerId_.isEmpty())
    {
        // 远端面板常驻显示（v1 是本地/远端各占一半），未选对端时也要有
        // 明确状态，否则看起来像坏了。
        showPath(QStringLiteral("<未选择对端>"));
        setStatus(QStringLiteral("未选择对端——在上方对端表右键「浏览此对端的目录」。"));
        return;
    }
    enterPath(QString());
}

void ExplorerControl::useLocalSource()
{
    source_ = Source::Local;
    peerId_.clear();
    peerName_.clear();
    currentPath_.clear();
    entries_.clear();

    table_->setDragEnabled(true);
    ui_->cbPath->setEditable(true);
    ui_->btnEnter->setEnabled(true);
    ui_->btnHome->setText(QStringLiteral("家目录"));
    rebuildView();
    onHome();
}

void ExplorerControl::applyRemoteEntries(bool ok, const QString &summary, const QList<relayer::app::RemoteRow> &rows)
{
    if (source_ != Source::Remote)
    {
        return;  // 用户已经切回本地，迟到的应答丢掉
    }
    if (!ok)
    {
        // 失败时不要停在"没进去的目录"上：路径保持原样，表格清空并说明原因。
        entries_.clear();
        rebuildView();
        showPath(displayPath());
        setStatus(summary, true);
        emit notice(QStringLiteral("[浏览] %1").arg(summary));
        return;
    }

    currentPath_ = remotePendingPath_;
    entries_.clear();
    entries_.reserve(rows.size());
    for (const auto &r : rows)
    {
        FileEntry e;
        e.name = r.name;
        e.isDir = r.isDir;
        e.size = static_cast<qint64>(r.size);
        e.modifiedMs = r.mtime > 0 ? r.mtime * 1000 : 0;
        entries_.push_back(e);
    }
    showPath(displayPath());
    updateExtOptions();
    rebuildView();

    int dirs = 0;
    for (const auto &e : entries_)
    {
        if (e.isDir)
        {
            ++dirs;
        }
    }
    setStatus(
        QStringLiteral("%1 项（%2 个目录，%3 个文件）").arg(entries_.size()).arg(dirs).arg(entries_.size() - dirs));
    emit notice(QStringLiteral("[浏览] %1 共 %2 项").arg(displayPath()).arg(entries_.size()));
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
    if (source_ == Source::Remote)
    {
        // 远端只读：本地那套（打开/复制路径/重命名/删除/新建）全不适用。
        // 能做的只有两件——把文件**拿过来**（§11.9）和刷新。
        const QString name = singleRow ? table_->item(rows[0], toInt(ExpColumn::Name))->text() : QString();
        const bool isFile = singleRow && table_->item(rows[0], toInt(ExpColumn::Type))->text() != QStringLiteral("Dir");
        const QString peer = peerId_;

        // 文件走 §11.9（原样发），目录走 §11.10（对端打包成 tar.xz）。
        // 分两项而不是一项自适应：两者的结果不一样——目录拿到的是一个包，
        // 用户需要知道这件事，否则会以为文件没解压。
        if (isFile)
        {
            auto *pull = menu.addAction(QStringLiteral("下载到接收目录"));
            pull->setEnabled(!isPathLikeName(name));
            pull->setToolTip(QStringLiteral("文件会落到本机的接收目录"));
            connect(pull, &QAction::triggered, this,
                    [this, peer, name]()
                    {
                        const QString rel = joinRel(currentPath_, name);
                        emit remotePullRequested(peer, rel, false);
                        setStatus(QStringLiteral("已请求取回 %1 …").arg(rel));
                    });
        }
        else
        {
            auto *pullDir = menu.addAction(QStringLiteral("下载目录（打包为 tar.xz）"));
            pullDir->setEnabled(singleRow && !isPathLikeName(name));
            pullDir->setToolTip(QStringLiteral("对端会把它压缩成一个 .tar.xz 发过来，\n"
                                               "落到接收目录；本版不自动解包"));
            connect(pullDir, &QAction::triggered, this,
                    [this, peer, name]()
                    {
                        const QString rel = joinRel(currentPath_, name);
                        emit remotePullRequested(peer, rel, true);
                        setStatus(QStringLiteral("已请求打包 %1（对方压缩中，可能要几秒）…").arg(rel));
                    });
        }

        connect(menu.addAction(QStringLiteral("刷新")), &QAction::triggered, this,
                [this]() { enterPath(currentPath_); });
        menu.exec(table_->viewport()->mapToGlobal(pos));
        return;
    }
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
