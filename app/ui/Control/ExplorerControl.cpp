#include "ExplorerControl.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QRunnable>
#include <QStandardPaths>
#include <QStyle>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <memory>

#include "model/PathText.h"
#include "relayer/cli/Client.h"
#include "relayer/common/crypto/Sha256.h"
#include "ui/FileMetaInfo.h"
#include "ui_ExplorerControl.h"

namespace {
// 远端路径的规范化与拼接统一走 app/model/PathText.h（规则实现与单测在
// core/session/PathText）。这里只留本文件真正私有的辅助件。

// 条目名能不能当成"一个目录名"往当前路径上拼。
//
// core 的 decodeAnswerFileList 已经拦了空名 / `.` / `..` / 含 `/` 的条目
// （那是协议层的输入合法性，所有消费者都受保护）。这里补的是**本平台的**
// 分隔符语义：Windows 上反斜杠也是分隔符，而 Unix 允许文件名含它。
// 不按平台区分的话，要么 Windows 上会把名字拆成两段（走到别的目录去），
// 要么 Unix 上点不进一个合法目录。
bool isPathLikeName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return true;
    }
    if (name.contains(QLatin1Char('/'))) {
        return true;
    }
#ifdef Q_OS_WIN
    if (name.contains(QLatin1Char('\\'))) {
        return true;
    }
#endif
    return false;
}

// 本地文件的 SHA-256（流式读，不整个进内存）。
//
// 不设大小上限：现在它只在**后台线程**跑（见 runAsync），再大的文件也只是
// "多等一会儿"，界面不会僵。v2 早先在 GUI 线程里同步算，为了不让界面冻住
// 才砍到 512 MiB——那个限制是症状，不是需求；v1 在 WorkerThread 里算，没有上限。
QString fileSha256(const QString& path, QString& err)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        err = file.errorString();
        return QString();
    }

    relayer::crypto::Sha256 hash;
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    while (true) {
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0) {
            err = file.errorString();
            return QString();
        }
        if (n == 0) {
            break;
        }
        hash.update(buffer.constData(), static_cast<std::size_t>(n));
    }
    return QString::fromStdString(relayer::crypto::Sha256::hex(hash.digest()));
}

// 后缀筛选里代表"无后缀文件"的特殊标签。
constexpr const char* kNoExtLabel = "(无后缀)";

QString typeText(bool isDir)
{
    return isDir ? QStringLiteral("Dir") : QStringLiteral("File");
}

QString sizeText(qint64 bytes)
{
    // 与 MainWindow::humanBytes 同规则；目录不显示大小。
    if (bytes <= 0) {
        return QStringLiteral("0.0 B");
    }
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    return QString::number(v, 'f', 1) + QLatin1String(units[i]);
}
}   // namespace

ExplorerControl::ExplorerControl(QWidget* parent) : QDialog(parent), ui_(new Ui::ExplorerControl)
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
    auto* layout = new QHBoxLayout(ui_->widget);
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
            [this](QTableWidgetItem* item) { onDoubleClick(item ? item->row() : -1); });
    connect(table_, &QTableWidget::customContextMenuRequested, this, &ExplorerControl::onTableContextMenu);
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, &ExplorerControl::onHeaderClicked);

    // 名称筛选 200ms 防抖：大目录下避免每次按键都全量重建。
    auto* filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(200);
    connect(filterTimer, &QTimer::timeout, this, &ExplorerControl::onFilterChanged);
    connect(ui_->edSearch, &QLineEdit::textChanged, filterTimer, qOverload<>(&QTimer::start));
    // 后缀是点击式多选，无需防抖。
    connect(ui_->cbExt, &CheckableComboBox::checkedItemsChanged, this, [this](const QStringList&) { onFilterChanged(); });
    connect(ui_->btnFilter, &QPushButton::toggled, this, &ExplorerControl::onToggleFilter);
    connect(ui_->btnResetFilter, &QPushButton::clicked, this, &ExplorerControl::onResetFilter);

    // 筛选行默认隐藏，与按钮初始状态同步。
    ui_->filterWidget->setVisible(ui_->btnFilter->isChecked());
}

bool ExplorerControl::listDir(const QString& dir, QVector<FileEntry>& entries) const
{
    const QFileInfoList items =
        QDir(dir).entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Unsorted);
    entries.clear();
    entries.reserve(items.size());
    for (const QFileInfo& fi : items) {
        FileEntry e;
        e.name = fi.fileName();
        e.isDir = fi.isDir();
        e.size = fi.size();
        e.modifiedMs = fi.lastModified().toMSecsSinceEpoch();
        entries.push_back(e);
    }
    return true;
}

void ExplorerControl::enterPath(const QString& path)
{
    if (source_ == Source::Remote) {
        if (peerId_.isEmpty()) {
            // 常驻的面板还没选对端：不发请求，保持空表（displayPath 已经
            // 显示"<未选择对端>"）。
            entries_.clear();
            rebuildView();
            return;
        }
        // 远端：异步。只发请求，等 applyRemoteEntries 回来才动 currentPath_。
        //
        // 路径是**对端视角的绝对路径**（DESIGN.md §3.8）；空串 = 根列表
        // （盘符 + 家目录），这是绝对路径模型下唯一的"起点"。
        // 先规范化：根列表给的是 `D://` 这种写法，直接留着会和后面的
        // 拼接打架（`D://` + "/" + name = `D:///name`）。
        const QString abs = relayer::app::normalizeRemotePath(path);
        remotePendingPath_ = abs;
        // 先显示"要去的地方"，回来再以 currentPath_ 为准（失败时停在原地）。
        // 用 showPath 而不是 setCurrentText：后者在不可编辑的 QComboBox 上
        // 对不存在的项是静默空操作，界面会看起来毫无反应。
        showPath(abs.isEmpty() ? QStringLiteral("%1:/").arg(peerId_) : QStringLiteral("%1:%2").arg(peerId_, abs));
        table_->setRowCount(0);
        const QString what = abs.isEmpty() ? QStringLiteral("<根：盘符与家目录>") : abs;
        setStatus(QStringLiteral("正在读取 %1 …（对端离线时约 10 秒后超时）").arg(what));
        emit notice(QStringLiteral("[浏览] 读取 %1:%2 …").arg(peerId_, what));
        emit remoteListRequested(peerId_, abs);
        return;
    }

    const QString cleaned = QDir::cleanPath(path);
    if (cleaned.isEmpty()) {
        return;
    }
    QVector<FileEntry> items;
    if (!listDir(cleaned, items)) {
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
    for (const auto& e : items) {
        if (e.isDir) {
            ++dirs;
        }
    }
    setStatus(QStringLiteral("%1 项（%2 个目录，%3 个文件）").arg(items.size()).arg(dirs).arg(items.size() - dirs));
}

void ExplorerControl::onEnter()
{
    if (source_ == Source::Remote) {
        // 远端路径不可手输：它是**对端**的绝对路径，在本机敲没有意义。
        // 导航靠双击、上一级与"根目录"按钮。
        return;
    }
    enterPath(ui_->cbPath->currentText().trimmed());
}

void ExplorerControl::onHome()
{
    if (source_ == Source::Remote) {
        // 远端模式下这个按钮是"回到根"（盘符 + 家目录，文字在 setRemotePeer 里改）。
        enterPath(QString());
        return;
    }

    // 家目录 + 本机盘符列表进下拉，覆盖"换盘"的入口（v1 的 AskHomeAndDriver）。
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    ui_->cbPath->clear();
    ui_->cbPath->addItem(home);
    for (const QFileInfo& drive : QDir::drives()) {
        ui_->cbPath->addItem(drive.absoluteFilePath());
    }
    ui_->cbPath->setCurrentText(home);
    enterPath(home);
}

void ExplorerControl::onUp()
{
    if (source_ == Source::Remote) {
        if (currentPath_.isEmpty()) {
            return;   // 已在根列表
        }
        enterPath(relayer::app::parentRemotePath(currentPath_));
        return;
    }
    if (currentPath_.isEmpty()) {
        return;
    }
    QDir dir(currentPath_);
    if (dir.cdUp()) {
        enterPath(dir.absolutePath());
    }
}

void ExplorerControl::onDoubleClick(int row)
{
    if (row < 0) {
        return;
    }
    auto* typeItem = table_->item(row, toInt(ExpColumn::Type));
    auto* nameItem = table_->item(row, toInt(ExpColumn::Name));
    if (!typeItem || !nameItem || typeItem->text() != QStringLiteral("Dir")) {
        return;   // 只有目录可进入；文件的"打开"在右键菜单里
    }
    if (source_ == Source::Remote) {
        const QString name = nameItem->text();
        if (remoteRootListing_) {
            // 根列表的条目名本来就是完整路径（盘符 / 家目录），直接用。
            // 这里不做"单段名"检查——它天生带分隔符。
            enterPath(name);
            return;
        }
        if (isPathLikeName(name)) {
            // 拒绝进入而不是"尽力拼一下"：拼出来的路径可能指向别处，
            // 服务端会再拒一次，但客户端不该把这种请求发出去。
            emit notice(QStringLiteral("[浏览] 条目名不是单段，拒绝进入：%1").arg(name));
            return;
        }
        enterPath(relayer::app::joinRemotePath(currentPath_, name));
        return;
    }
    enterPath(QDir(currentPath_).filePath(nameItem->text()));
}

QStringList ExplorerControl::selectedLocalPaths() const
{
    if (source_ != Source::Local) {
        return {};   // 远端条目的"路径"是对端的绝对路径，不是本地路径
    }
    QSet<int> rowSet;
    for (auto* it : table_->selectedItems()) {
        rowSet.insert(it->row());
    }
    QVector<int> rows(rowSet.cbegin(), rowSet.cend());
    std::sort(rows.begin(), rows.end());   // 保持表格顺序，任务表按它排队

    QStringList out;
    out.reserve(rows.size());
    for (int r : rows) {
        if (auto* item = table_->item(r, toInt(ExpColumn::Name))) {
            out << QDir(currentPath_).filePath(item->text());
        }
    }
    return out;
}

void ExplorerControl::showPath(const QString& text)
{
    // 远端模式 cbPath 不可编辑：setCurrentText 在非可编辑的 QComboBox 上
    // 只查已有项，查不到就静默忽略。改成"清空 + 加一项 + 选中"。
    ui_->cbPath->clear();
    ui_->cbPath->addItem(text);
    ui_->cbPath->setCurrentIndex(0);
}

void ExplorerControl::setStatus(const QString& text, bool error)
{
    ui_->lblStatus->setText(text);
    ui_->lblStatus->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void ExplorerControl::runAsync(const QString& statusText, std::function<bool(QString&)> job,
                               std::function<void(bool, const QString&)> done)
{
    if (busy_) {
        setStatus(QStringLiteral("上一个本地操作还没结束，请稍候"), true);
        return;
    }
    busy_ = true;
    setStatus(statusText);
    // 先把状态行画出来：后台活可能跑几十秒，不给反馈就像没反应。
    QCoreApplication::processEvents();

    QPointer<ExplorerControl> self(this);
    auto jobPtr = std::make_shared<std::function<bool(QString&)>>(std::move(job));
    auto donePtr = std::make_shared<std::function<void(bool, const QString&)>>(std::move(done));

    QThreadPool::globalInstance()->start(QRunnable::create([self, jobPtr, donePtr]() {
        QString err;
        const bool ok = (*jobPtr)(err);
        // 回 GUI 线程。用 self 作 context：窗口在这期间被关掉的话，
        // 这次投递会被丢弃（job 已经跑完，没人接结果而已）。
        ExplorerControl* target = self.data();
        if (target == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            target,
            [target, ok, err, donePtr]() {
                target->busy_ = false;
                (*donePtr)(ok, err);
            },
            Qt::QueuedConnection);
    }));
}

QString ExplorerControl::displayPath() const
{
    if (source_ == Source::Local) {
        return currentPath_;
    }
    if (peerId_.isEmpty()) {
        return QStringLiteral("<未选择对端>");
    }
    // 根列表（currentPath_ 为空）显示成 peer:/，具体盘符在表格里。
    return currentPath_.isEmpty() ? QStringLiteral("%1:/").arg(peerId_) : QStringLiteral("%1:%2").arg(peerId_, currentPath_);
}

void ExplorerControl::setRemotePeer(const QString& peerId, const QString& peerName, const QString& relPath)
{
    source_ = Source::Remote;
    peerId_ = peerId;
    peerName_ = peerName;
    currentPath_.clear();
    entries_.clear();
    remotePendingPath_.clear();

    // 远端模式下路径框不可手输（那是**对端**的路径，在本机敲没有意义），
    // 「进入」按钮随之禁用。其余交互（右键菜单、拖出）与本地面板一致——
    // 菜单只有一套，差异由"这一端有没有执行体"决定，见 onTableContextMenu。
    //
    // 拖出**保留**：载荷是相对路径，落到本地面板就是"下载"（v1 的跨面板
    // 拖拽在 v2 的对应物）。表格上的标记让对照表拒收这种载荷。
    table_->setDragEnabled(true);
    table_->setRemoteSource(true);
    ui_->cbPath->setEditable(false);
    ui_->btnEnter->setEnabled(false);
    ui_->btnHome->setText(QStringLiteral("根"));
    ui_->filterWidget->setVisible(ui_->btnFilter->isChecked());

    showPath(displayPath());
    rebuildView();

    if (peerId_.isEmpty()) {
        // 远端面板常驻显示（v1 是本地/远端各占一半），未选对端时也要有
        // 明确状态，否则看起来像坏了。
        showPath(QStringLiteral("<未选择对端>"));
        setStatus(QStringLiteral("未选择对端——在上方对端表右键「浏览此对端的目录」。"));
        return;
    }
    // 不在这里再调 navigateTo：一次切换只发一个列举请求（见头文件）。
    enterPath(relPath);
}

void ExplorerControl::useLocalSource()
{
    source_ = Source::Local;
    peerId_.clear();
    peerName_.clear();
    currentPath_.clear();
    entries_.clear();

    table_->setDragEnabled(true);
    table_->setRemoteSource(false);
    ui_->cbPath->setEditable(true);
    ui_->btnEnter->setEnabled(true);
    ui_->btnHome->setText(QStringLiteral("家目录"));
    rebuildView();
    onHome();
}

void ExplorerControl::applyRemoteEntries(bool ok, bool rootListing, const QString& summary,
                                         const QList<relayer::app::RemoteRow>& rows)
{
    if (source_ != Source::Remote) {
        return;   // 用户已经切回本地，迟到的应答丢掉
    }
    if (!ok) {
        // 失败时不要停在"没进去的目录"上：路径保持原样，表格清空并说明原因。
        entries_.clear();
        rebuildView();
        showPath(displayPath());
        setStatus(summary, true);
        emit notice(QStringLiteral("[浏览] %1").arg(summary));
        return;
    }

    currentPath_ = remotePendingPath_;
    remoteRootListing_ = rootListing;
    entries_.clear();
    entries_.reserve(rows.size());
    for (const auto& r : rows) {
        FileEntry e;
        // 根列表的条目名就是完整路径（`D://`、`/home/me`），显示成
        // `D:/` 这种统一形式——否则同一列里盘符是反斜杠、子目录是斜杠。
        e.name = rootListing ? relayer::app::normalizeRemotePath(r.name) : r.name;
        e.isDir = r.isDir;
        e.size = static_cast<qint64>(r.size);
        e.modifiedMs = r.mtime > 0 ? r.mtime * 1000 : 0;
        entries_.push_back(e);
    }
    showPath(displayPath());
    updateExtOptions();
    rebuildView();

    int dirs = 0;
    for (const auto& e : entries_) {
        if (e.isDir) {
            ++dirs;
        }
    }
    setStatus(QStringLiteral("%1 项（%2 个目录，%3 个文件）").arg(entries_.size()).arg(dirs).arg(entries_.size() - dirs));
    emit notice(QStringLiteral("[浏览] %1 共 %2 项").arg(displayPath()).arg(entries_.size()));
}

void ExplorerControl::onHeaderClicked(int index)
{
    // 第 0 列是图标，不参与排序。
    if (index <= 0) {
        return;
    }
    if (index == sortColumn_) {
        sortOrder_ = (sortOrder_ == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        sortColumn_ = index;
        // 时间/大小默认降序（文件管理器习惯：最新的/最大的在前）。
        sortOrder_ =
            (index == toInt(ExpColumn::Modified) || index == toInt(ExpColumn::Size)) ? Qt::DescendingOrder : Qt::AscendingOrder;
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
    for (const FileEntry& e : entries_) {
        if (e.isDir) {
            continue;
        }
        const QString ext = QFileInfo(e.name).suffix().toLower();
        if (ext.isEmpty()) {
            hasNoExt = true;
        } else if (!exts.contains(ext)) {
            exts.append(ext);
        }
    }
    exts.sort();
    if (hasNoExt) {
        exts.prepend(QLatin1String(kNoExtLabel));
    }
    // setOptions 自动保留同名旧勾选：跨目录导航时"只看某类文件"得以延续。
    ui_->cbExt->setOptions(exts);
}

void ExplorerControl::rebuildView()
{
    // 1) 过滤；目录与文件分两组（目录恒置顶，与资源管理器一致）。
    QVector<const FileEntry*> dirs;
    QVector<const FileEntry*> files;
    for (const FileEntry& e : entries_) {
        if (!filterName_.isEmpty() && !e.name.contains(filterName_, Qt::CaseInsensitive)) {
            continue;
        }
        if (!filterExts_.isEmpty()) {
            // 选了后缀后只显示匹配的文件，目录一律隐藏。
            if (e.isDir) {
                continue;
            }
            const QString rawExt = QFileInfo(e.name).suffix().toLower();
            const QString key = rawExt.isEmpty() ? QLatin1String(kNoExtLabel) : rawExt;
            if (!filterExts_.contains(key)) {
                continue;
            }
        }
        (e.isDir ? dirs : files).push_back(&e);
    }

    // 2) 各组内独立排序，再按方向翻转（保持目录置顶）。
    const int col = sortColumn_;
    auto cmp = [col](const FileEntry* a, const FileEntry* b) { return lessThan(*a, *b, col); };
    std::stable_sort(dirs.begin(), dirs.end(), cmp);
    std::stable_sort(files.begin(), files.end(), cmp);
    if (sortOrder_ == Qt::DescendingOrder) {
        std::reverse(dirs.begin(), dirs.end());
        std::reverse(files.begin(), files.end());
    }

    // 3) 重建表格。大目录分批 processEvents，避免长时间白屏——代价是
    // 重建期间用户可能看到中间态，可接受（v1 同款取舍）。
    table_->setUpdatesEnabled(false);
    table_->clearContents();
    table_->setRowCount(0);
    int row = 0;
    for (const auto* group : {&dirs, &files}) {
        for (const FileEntry* e : *group) {
            table_->insertRow(row);
            setFileItem(*e, row);
            ++row;
            if (row % 50 == 0) {
                QGuiApplication::processEvents();
            }
        }
    }
    table_->horizontalHeader()->blockSignals(true);
    table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    table_->horizontalHeader()->blockSignals(false);
    table_->setUpdatesEnabled(true);
}

bool ExplorerControl::lessThan(const FileEntry& a, const FileEntry& b, int column)
{
    // 主键比较；并列时以名称（不区分大小写）兜底，保证严格弱序。
    switch (column) {
    case static_cast<int>(ExpColumn::Modified):
        if (a.modifiedMs != b.modifiedMs) {
            return a.modifiedMs < b.modifiedMs;
        }
        break;
    case static_cast<int>(ExpColumn::Type):
        if (a.isDir != b.isDir) {
            return b.isDir;   // 目录排前
        }
        break;
    case static_cast<int>(ExpColumn::Size):
        if (a.size != b.size) {
            return a.size < b.size;
        }
        break;
    default:   // 名称
        break;
    }
    return a.name.toLower() < b.name.toLower();
}

void ExplorerControl::setFileItem(const FileEntry& entry, int row)
{
    static const QIcon dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    static const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    auto* iconItem = new QTableWidgetItem();
    iconItem->setIcon(entry.isDir ? dirIcon : fileIcon);
    iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    iconItem->setFlags(iconItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Icon), iconItem);

    auto* nameItem = new QTableWidgetItem(entry.name);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Name), nameItem);

    auto* timeItem =
        new QTableWidgetItem(QDateTime::fromMSecsSinceEpoch(entry.modifiedMs).toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Modified), timeItem);

    auto* typeItem = new QTableWidgetItem(typeText(entry.isDir));
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Type), typeItem);

    auto* sizeItem = new QTableWidgetItem(entry.isDir ? QString() : sizeText(entry.size));
    sizeItem->setFlags(sizeItem->flags() & ~Qt::ItemIsEditable);
    table_->setItem(row, toInt(ExpColumn::Size), sizeItem);
}

const ExplorerControl::FileEntry* ExplorerControl::findEntry(const QString& name) const
{
    for (const auto& e : entries_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

void ExplorerControl::showMetaInfo(int row)
{
    if (row < 0 || row >= table_->rowCount()) {
        return;
    }
    const QString name = table_->item(row, toInt(ExpColumn::Name))->text();
    const FileEntry* entry = findEntry(name);
    const bool isDir =
        entry != nullptr ? entry->isDir : table_->item(row, toInt(ExpColumn::Type))->text() == QStringLiteral("Dir");

    FileMetaInfo dlg(this);
    // 目录没有"大小"（-1 = 不适用，显示为 "-"，避免被读成 0 字节的空目录）；
    // 远端条目由 applyRemoteEntries 带回 mtime，本地由 listDir 带回。
    dlg.setMeta(displayPath(), name, isDir, (isDir || entry == nullptr) ? -1 : entry->size,
                entry != nullptr ? entry->modifiedMs : -1, source_ == Source::Remote);
    dlg.exec();
}

void ExplorerControl::onTableContextMenu(const QPoint& pos)
{
    // **一套菜单，两端共用**（v1 的形状）：`source_` 只决定"谁去执行"，
    // 不决定"显示哪些项"。唯一按端过滤的是「在资源管理器中打开」——它只
    // 对本地有意义，远端那一侧自动不出现。
    //
    // v2 早先把两端拆成两个 if 块，代价是"远端少一半功能"且两处措辞各写
    // 一套。现在每项声明两个执行体（local / remote），为空表示这一端不
    // 支持、直接不显示——"远端不支持就隐藏"成了一条规则，不用每项各写
    // 一次 `if (source_ == Remote)`。
    const bool remote = (source_ == Source::Remote);

    QSet<int> rowSet;
    for (auto* it : table_->selectedItems()) {
        rowSet.insert(it->row());
    }
    QVector<int> sel(rowSet.cbegin(), rowSet.cend());
    std::sort(sel.begin(), sel.end());

    const bool single = sel.size() == 1;
    // 根列表那一行是整个盘符 / 家目录：改名、删除、新建、下载它都没有意义
    // （"把整个 D 盘打包发过来"不是用户想要的）。
    const bool rootListing = remote && remoteRootListing_;
    const bool singleEntry = single && !rootListing;

    const auto nameOf = [this](int row) { return table_->item(row, toInt(ExpColumn::Name))->text(); };
    const auto isDirRow = [this](int row) { return table_->item(row, toInt(ExpColumn::Type))->text() == QStringLiteral("Dir"); };
    // 根列表里的条目名本身就是完整路径（盘符 / 家目录），不能再拼。
    const auto remotePathOf = [this](int row) {
        const QString n = table_->item(row, toInt(ExpColumn::Name))->text();
        return remoteRootListing_ ? relayer::app::normalizeRemotePath(n) : relayer::app::joinRemotePath(currentPath_, n);
    };

    const QString name = single ? nameOf(sel.at(0)) : QString();
    const bool isDir = single && isDirRow(sel.at(0));
    const bool isFile = single && !isDir;
    // 同一个"这一项"在两端各是一个绝对路径：本地是本机路径，远端是对端
    // 绝对路径。下面的动作只关心"对它做什么"，不关心它在谁的盘上。
    const QString absPath = single ? (remote ? remotePathOf(sel.at(0)) : QDir(currentPath_).filePath(name)) : QString();
    const bool isArchive = isFile && name.endsWith(QStringLiteral(".tar.xz"), Qt::CaseInsensitive);

    QMenu menu(this);
    // visible 决定"这一次要不要出现"；两个执行体里为空的表示该端不支持，
    // 同样不显示。两端给同一段逻辑时写两份字面量，不要复用同一个
    // std::function（会被 move 走，第二次只剩空壳）。
    const auto add = [&](bool visible, const QString& label, std::function<void()> localFn, std::function<void()> remoteFn) {
        if (!visible) {
            return;
        }
        auto body = remote ? std::move(remoteFn) : std::move(localFn);
        if (!body) {
            return;
        }
        connect(menu.addAction(label), &QAction::triggered, this, std::move(body));
    };
    const auto addSeparator = [&menu]() {
        if (!menu.isEmpty()) {
            menu.addSeparator();
        }
    };

    // ---- 传输：方向由面板决定（v1 语义：同一个词，两个方向）------------
    //
    // 远端只对单行显示：取回请求一次只允许一个在飞（协议层用"下一个应答
    // 就是我的"配对），多行会立刻被拒。这与 v1 的"逐项排队"是同一个限制，
    // 只是 v1 在任务窗口里排队，v2 让她去对照表的"下载到本地目录"那边排。
    if (remote) {
        add(singleEntry && !isPathLikeName(absPath),
            isDir ? QStringLiteral("传输（对端打包为 tar.xz 后下载）") : QStringLiteral("传输（下载到接收目录）"), nullptr,
            [this, absPath, isDir]() {
                emit remotePullRequested(peerId_, absPath, isDir);
                setStatus(isDir ? QStringLiteral("已请求对端打包 %1 …（可能要几秒）").arg(absPath)
                                : QStringLiteral("已请求取回 %1 …").arg(absPath));
            });
    } else {
        add(
            !sel.isEmpty(), QStringLiteral("传输（发送到当前通信对象）"),
            [this]() { emit localTransferRequested(selectedLocalPaths()); }, nullptr);
    }

    addSeparator();

    // ---- 文件系统变更 --------------------------------------------------
    add(
        !rootListing, QStringLiteral("新建文件夹…"), [this]() { onNewDir(); },
        [this]() {
            bool ok = false;
            const QString dirName =
                QInputDialog::getText(this, QStringLiteral("新建文件夹"), QStringLiteral("文件夹名"), QLineEdit::Normal, {}, &ok)
                    .trimmed();
            if (!ok || dirName.isEmpty()) {
                return;
            }
            // 与当前目录拼成对端绝对路径。空路径在协议里表示"根列表"，不是
            // "当前目录"，所以根列表下不给这个动作（上面已排除）。
            const QString target = relayer::app::joinRemotePath(currentPath_, dirName);
            setStatus(QStringLiteral("正在请求对端新建 %1 …").arg(target));
            emit remoteOpRequested(QStringLiteral("mkdir"), peerId_, target, QString(), {});
        });

    add(
        singleEntry, QStringLiteral("重命名…"), [this, sel]() { onRename(sel.at(0)); },
        [this, absPath, name]() {
            bool ok = false;
            const QString newName =
                QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("新名字"), QLineEdit::Normal, name, &ok)
                    .trimmed();
            if (!ok || newName.isEmpty() || newName == name) {
                return;
            }
            setStatus(QStringLiteral("正在请求对端重命名 %1 …").arg(name));
            emit remoteOpRequested(QStringLiteral("rename"), peerId_, absPath, newName, {});
        });

    add(
        !sel.isEmpty(), QStringLiteral("删除"), [this, sel]() { onDelete(sel); },
        [this, sel, remotePathOf]() {
            QStringList paths;
            paths.reserve(sel.size());
            for (int r : sel) {
                paths << remotePathOf(r);
            }
            const auto choice =
                QMessageBox::question(this, QStringLiteral("确认"),
                                      QStringLiteral("让对端删除这 %1 项？目录会连同内容一起删除，不可恢复。").arg(paths.size()),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (choice != QMessageBox::Yes) {
                return;
            }
            setStatus(QStringLiteral("正在请求对端删除 %1 项 …").arg(paths.size()));
            emit remoteOpRequested(QStringLiteral("delete"), peerId_, QString(), QString(), paths);
        });

    addSeparator();

    // ---- 校验与摘要 ----------------------------------------------------
    //
    // SHA256：本地读盘算，远端请对端算。两边都自动进剪贴板——用户下一步
    // 多半就是粘贴到别处比对。
    add(
        isFile, QStringLiteral("SHA256"),
        [this, absPath]() {
            // 后台算（v1 在 WorkerThread 里算，无大小上限）。结果放共享变量
            // 里带回：job 的返回值只表达成败，结论本身是那个十六进制串。
            auto hex = std::make_shared<QString>();
            runAsync(
                QStringLiteral("正在计算 SHA-256 …（大文件需要一会儿）"),
                [absPath, hex](QString& err) {
                    *hex = fileSha256(absPath, err);
                    return !hex->isEmpty();
                },
                [this, absPath, hex](bool ok, const QString& err) {
                    if (!ok) {
                        setStatus(QStringLiteral("SHA-256 计算失败：%1").arg(err), true);
                        return;
                    }
                    QApplication::clipboard()->setText(*hex);
                    setStatus(QStringLiteral("SHA-256（已复制到剪贴板）：%1").arg(*hex));
                    emit notice(QStringLiteral("[校验] %1\n  SHA-256 = %2").arg(absPath, *hex));
                });
        },
        [this, absPath]() {
            setStatus(QStringLiteral("已请求对端计算 SHA-256 …"));
            emit remoteOpRequested(QStringLiteral("sha256"), peerId_, absPath, QString(), {});
        });

    // 粗校验 / 时间戳校验：与**另一侧面板当前目录下的同名文件**比。
    // 方向无关——两端都是"本地目录/name vs 远端目录/name"，所以从哪一侧
    // 发起得到的都是同一个结论，执行体也就可以完全相同。
    add(
        isFile, QStringLiteral("粗校验（与另一侧同名文件比内容）"), [this, name]() { emit compareRequested(name, false); },
        [this, name]() { emit compareRequested(name, false); });
    add(
        isFile, QStringLiteral("时间戳校验（比修改时间）"), [this, name]() { emit compareRequested(name, true); },
        [this, name]() { emit compareRequested(name, true); });

    addSeparator();

    // ---- 压缩 / 解压缩 -------------------------------------------------
    //
    // 压缩只有本地：v2 没有"让对端压到对端某个路径"的入口（协议帧在，
    // 但没有真实用途——要拿对方的东西就打包取回，见 §11.10）。远端传
    // nullptr 即自动隐藏。
    add(
        !sel.isEmpty(), QStringLiteral("压缩为 tar.xz…"),
        [this]() {
            const QStringList paths = selectedLocalPaths();
            if (paths.isEmpty()) {
                return;
            }
            QString suggested = QStringLiteral("archive.tar.xz");
            if (paths.size() == 1) {
                suggested = QFileInfo(paths.first()).fileName() + QStringLiteral(".tar.xz");
            }
            const QString out = QFileDialog::getSaveFileName(
                this, QStringLiteral("压缩为"), QDir(currentPath()).filePath(suggested), QStringLiteral("tar.xz (*.tar.xz)"));
            if (out.isEmpty()) {
                return;
            }
            // 压缩是纯 CPU/IO 的重活（tar.xz 还要 LZMA 压），后台跑。
            std::vector<std::string> sources;
            sources.reserve(static_cast<std::size_t>(paths.size()));
            for (const auto& p : paths) {
                sources.push_back(p.toStdString());
            }
            const std::string archive = out.toStdString();
            runAsync(
                QStringLiteral("正在压缩 %1 项 …").arg(paths.size()),
                [sources, archive](QString& err) {
                    std::string detail;
                    if (!relayer::cli::Client::createArchive(sources, archive, detail)) {
                        err = QString::fromStdString(detail);
                        return false;
                    }
                    return true;
                },
                [this, out, count = paths.size()](bool ok, const QString& err) {
                    if (!ok) {
                        setStatus(QStringLiteral("压缩失败：%1").arg(err), true);
                        return;
                    }
                    setStatus(QStringLiteral("已压缩为 %1").arg(QFileInfo(out).fileName()));
                    emit notice(QStringLiteral("[压缩] %1（%2 项）").arg(out).arg(count));
                    enterPath(currentPath_);   // 刷新，让新包出现
                });
        },
        nullptr);

    add(
        isArchive, QStringLiteral("解压缩"),
        [this, absPath]() {
            const std::string archive = absPath.toStdString();
            auto dir = std::make_shared<std::string>();
            runAsync(
                QStringLiteral("正在解压 %1 …").arg(QFileInfo(absPath).fileName()),
                // false：手动压缩的包有顶层目录，解到父目录即可，再套一层会变成 a/a/。
                [archive, dir](QString& err) {
                    *dir = relayer::cli::Client::unpackArchive(archive, false);
                    if (dir->empty()) {
                        err = QStringLiteral("包可能已损坏");
                        return false;
                    }
                    return true;
                },
                [this, dir](bool ok, const QString& err) {
                    if (!ok) {
                        setStatus(QStringLiteral("解压失败（%1）").arg(err), true);
                        return;
                    }
                    const QString out = QString::fromStdString(*dir);
                    setStatus(QStringLiteral("已解压到 %1").arg(out));
                    emit notice(QStringLiteral("[解压] %1").arg(out));
                    enterPath(currentPath_);
                });
        },
        [this, absPath]() {
            setStatus(QStringLiteral("已请求对端解压 %1 …").arg(QFileInfo(absPath).fileName()));
            emit remoteOpRequested(QStringLiteral("unpack"), peerId_, absPath, QString(), {});
        });

    addSeparator();

    // ---- 只读动作 ------------------------------------------------------
    add(
        single, QStringLiteral("复制全路径"), [this, absPath]() { QApplication::clipboard()->setText(absPath); },
        [this, absPath]() { QApplication::clipboard()->setText(absPath); });

    // 数据直接取自 entries_，不重新 stat——显示的必须是"表格里那一条"，
    // 而不是"现在磁盘上那一条"（两者可能不同，用户会困惑）。
    add(
        single, QStringLiteral("详细信息"), [this, sel]() { showMetaInfo(sel.at(0)); },
        [this, sel]() { showMetaInfo(sel.at(0)); });

    // 「在资源管理器中打开」只对本地有意义：远端没有本机路径可打开。
    // 这正是"统一菜单里少数几个按端隐藏"的例子——远端传 nullptr。
    add(
        single, QStringLiteral("在资源管理器中打开"),
        [this, isDir, absPath]() {
            // 目录：打开它本身；文件：打开它所在目录（与 v1 一致——双击
            // 文件不该启动一个程序，那是"打开文件"不是"在资源管理器里看"）。
            QDesktopServices::openUrl(QUrl::fromLocalFile(isDir ? absPath : currentPath_));
        },
        nullptr);

    add(true, QStringLiteral("刷新"), [this]() { enterPath(currentPath_); }, [this]() { enterPath(currentPath_); });

    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void ExplorerControl::onRename(int row)
{
    const QString oldName = table_->item(row, toInt(ExpColumn::Name))->text();
    bool ok = false;
    const QString newName =
        QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("请输入新文件名"), QLineEdit::Normal, oldName, &ok)
            .trimmed();
    if (!ok || newName.isEmpty() || newName == oldName) {
        return;
    }
    const QDir dir(currentPath_);
    if (!QFile::rename(dir.filePath(oldName), dir.filePath(newName))) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("重命名失败（目标名已存在？权限不足？）"));
        return;
    }
    enterPath(currentPath_);   // 重列：简单且正确，不做手术式更新
}

void ExplorerControl::onDelete(const QVector<int>& rows)
{
    if (rows.isEmpty()) {
        return;
    }
    QStringList names;
    for (int row : rows) {
        names << table_->item(row, toInt(ExpColumn::Name))->text();
    }
    const auto choice = QMessageBox::question(
        this, QStringLiteral("确认"), QStringLiteral("删除选中的 %1 项？目录将连同内容一起删除，不可恢复。").arg(names.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }

    // 递归删除是"删一个几万文件的目录要几十秒"的操作，必须在后台。
    // 只传路径（按值），工作线程不碰本控件任何成员。
    const QDir dir(currentPath_);
    QStringList paths;
    paths.reserve(names.size());
    for (const QString& name : names) {
        paths << dir.filePath(name);
    }

    runAsync(
        QStringLiteral("正在删除 %1 项 …").arg(paths.size()),
        [paths](QString& err) {
            QStringList failed;
            for (const QString& path : paths) {
                const QFileInfo info(path);
                const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
                if (!ok) {
                    failed << info.fileName();
                }
            }
            if (!failed.isEmpty()) {
                err = failed.join(QStringLiteral("、"));
                return false;
            }
            return true;
        },
        [this](bool ok, const QString& err) {
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("提示"),
                                     QStringLiteral("以下项目删除失败（可能被占用或权限不足）：\n%1").arg(err));
            }
            enterPath(currentPath_);
        });
}

void ExplorerControl::onNewDir()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"), QStringLiteral("请输入文件夹名"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const QDir dir(currentPath_);
    if (!dir.mkdir(name)) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("新建失败（同名已存在？权限不足？）"));
        return;
    }
    enterPath(currentPath_);
}
