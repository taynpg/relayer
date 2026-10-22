#include "ComparisonControl.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <functional>

#include "Base/MenuIcons.h"
#include "ui_ComparisonControl.h"

using relayer::app::RelayTaskItem;
using relayer::app::RelayTaskItems;

namespace
{

// 表头顺序与 ComColumn 一一对应，集中放一处供建表与取值共用。
QString headerOf(ComColumn col)
{
    switch (col)
    {
    case ComColumn::Id:
        return QStringLiteral("ID");
    case ComColumn::Name:
        return QStringLiteral("名称");
    case ComColumn::Type:
        return QStringLiteral("类型");
    case ComColumn::Mark:
        return QStringLiteral("标记");
    case ComColumn::LocalDir:
        return QStringLiteral("本地目录");
    case ComColumn::Peer:
        return QStringLiteral("对端");
    case ComColumn::RelDir:
        return QStringLiteral("相对目录");
    }
    return {};
}

constexpr int kColumnCount = 7;

// 用户可批量填充的列（只读列不列出）。与 v1 的差异：标记列由名称推导，
// 不再提供批量填充。
struct BatchColumn
{
    ComColumn col;
    const char *label;
};

const BatchColumn kBatchColumns[] = {
    {ComColumn::Name, "名称"},
    {ComColumn::LocalDir, "本地目录"},
    {ComColumn::Peer, "对端"},
    {ComColumn::RelDir, "相对目录"},
};

QString markForName(const QString &name, bool isDir)
{
    if (isDir)
    {
        return QStringLiteral("Dir");
    }
    const QString suffix = QFileInfo(name).suffix();
    return suffix.isEmpty() ? QStringLiteral("File") : suffix.toLower();
}

}  // namespace

ComparisonControl::ComparisonControl(QWidget *parent)
    : QWidget(parent)
    , ui_(new Ui::ComparisonControl)
{
    ui_->setupUi(this);
    initTable();
    initSignals();
    initConfigs();
}

void ComparisonControl::initTable()
{
    table_ = new ComDropTable(this);
    table_->setColumnCount(kColumnCount);
    QStringList headers;
    for (int i = 0; i < kColumnCount; ++i)
    {
        headers << headerOf(static_cast<ComColumn>(i));
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    table_->setColumnWidth(toInt(ComColumn::Id), 50);
    table_->setColumnWidth(toInt(ComColumn::Name), 200);
    table_->setColumnWidth(toInt(ComColumn::Type), 50);
    table_->setColumnWidth(toInt(ComColumn::Mark), 80);
    table_->setColumnWidth(toInt(ComColumn::Peer), 110);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::Id), QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::Type), QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::Mark), QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::Peer), QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::LocalDir), QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(toInt(ComColumn::RelDir), QHeaderView::Stretch);

    auto *layout = new QHBoxLayout();
    layout->addWidget(table_);
    layout->setContentsMargins(0, 0, 0, 0);
    ui_->widget->setLayout(layout);
}

void ComparisonControl::initSignals()
{
    connect(ui_->btnSave, &QPushButton::clicked, this, &ComparisonControl::saveConfig);
    connect(ui_->btnLoad, &QPushButton::clicked, this,
            [this]() { loadConfig(ui_->cbConfig->currentText().trimmed()); });
    connect(ui_->btnDel, &QPushButton::clicked, this, &ComparisonControl::deleteConfig);
    connect(ui_->btnNew, &QPushButton::clicked, this, &ComparisonControl::newConfig);
    connect(ui_->btnCopy, &QPushButton::clicked, this, &ComparisonControl::copyConfig);
    connect(ui_->btnReplace, &QPushButton::clicked, this, &ComparisonControl::onReplace);

    connect(ui_->cbConfig, &QComboBox::currentTextChanged, this,
            [this](const QString &text) { currentConfig_ = text.trimmed(); });

    connect(table_, &QTableWidget::customContextMenuRequested, this, &ComparisonControl::onTableContextMenu);
    connect(table_, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item)
            {
                if (!updating_)
                {
                    syncRowFromItem(item);
                }
            });

    connect(ui_->listWidget, &QListWidget::customContextMenuRequested, this, &ComparisonControl::onListContextMenu);
    connect(ui_->listWidget, &QListWidget::itemChanged, this, &ComparisonControl::onListItemToggled);
    connect(ui_->listWidget, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item)
            {
                if (item != nullptr)
                {
                    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
                }
            });
}

// ---------------------------------------------------------------------------
// 配置的加载与持久化（JSON 存储）
// ---------------------------------------------------------------------------

QString ComparisonControl::storePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/comparison.json");
}

void ComparisonControl::initConfigs()
{
    QFile file(storePath());
    QStringList names;
    QString last;
    if (file.open(QIODevice::ReadOnly))
    {
        const auto doc = QJsonDocument::fromJson(file.readAll());
        const auto obj = doc.object();
        last = obj.value(QStringLiteral("last")).toString();
        names = obj.value(QStringLiteral("configs")).toObject().keys();
        names.sort();
    }
    ui_->cbConfig->blockSignals(true);
    ui_->cbConfig->clear();
    ui_->cbConfig->addItems(names);
    const int idx = ui_->cbConfig->findText(last);
    ui_->cbConfig->setCurrentIndex(idx >= 0 ? idx : 0);
    ui_->cbConfig->blockSignals(false);

    currentConfig_ = ui_->cbConfig->currentText().trimmed();
    if (!currentConfig_.isEmpty())
    {
        loadConfig(currentConfig_);  // 启动即恢复上次使用的配置，免一次点击
    }
}

void ComparisonControl::writeStore()
{
    // 结构：{ "last": 当前配置名, "configs": { 配置名: [行对象...] } }
    // 原则：只重写磁盘上已有的配置 + 当前配置。其他配置的内容不经过本控件，
    // 不能凭内存里的空壳覆盖它们。
    QJsonObject configs;
    QFile file(storePath());
    if (file.open(QIODevice::ReadOnly))
    {
        configs = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("configs")).toObject();
    }

    QJsonArray rows;
    for (const auto &r : rows_)
    {
        QJsonObject o;
        o[QStringLiteral("name")] = r.name;
        o[QStringLiteral("type")] = r.type;
        o[QStringLiteral("mark")] = r.mark;
        o[QStringLiteral("localDir")] = r.localDir;
        o[QStringLiteral("peer")] = r.peer;
        o[QStringLiteral("relDir")] = r.relDir;
        rows.append(o);
    }
    if (!currentConfig_.isEmpty())
    {
        configs[currentConfig_] = rows;
    }

    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QSaveFile out(storePath());
    if (!out.open(QIODevice::WriteOnly))
    {
        emit logMessage(QStringLiteral("[配置] 保存失败：无法写入 %1").arg(storePath()));
        return;
    }
    QJsonObject root;
    root[QStringLiteral("last")] = currentConfig_;
    root[QStringLiteral("configs")] = configs;
    out.write(QJsonDocument(root).toJson());
    if (!out.commit())
    {
        emit logMessage(QStringLiteral("[配置] 保存失败：%1").arg(out.errorString()));
    }
}

void ComparisonControl::saveConfig()
{
    if (currentConfig_.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择或新建一个配置"));
        return;
    }
    writeStore();
    emit logMessage(QStringLiteral("[配置] 已保存「%1」（%2 行）").arg(currentConfig_).arg(rows_.size()));
}

void ComparisonControl::loadConfig(const QString &name)
{
    if (name.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("配置名称不能为空"));
        return;
    }
    QFile file(storePath());
    rows_.clear();
    visibleMarks_.clear();
    if (file.open(QIODevice::ReadOnly))
    {
        const auto rows = QJsonDocument::fromJson(file.readAll())
                              .object()
                              .value(QStringLiteral("configs"))
                              .toObject()
                              .value(name)
                              .toArray();
        for (const auto &v : rows)
        {
            const auto o = v.toObject();
            Row r;
            r.name = o.value(QStringLiteral("name")).toString();
            r.mark = o.value(QStringLiteral("mark")).toString();
            r.type = o.value(QStringLiteral("type")).toString();
            if (r.type.isEmpty())
            {
                r.type = r.mark == QStringLiteral("Dir") ? QStringLiteral("Dir") : QStringLiteral("File");
            }
            r.localDir = o.value(QStringLiteral("localDir")).toString();
            r.peer = o.value(QStringLiteral("peer")).toString();
            r.relDir = o.value(QStringLiteral("relDir")).toString();
            rows_.push_back(std::move(r));
        }
    }
    currentConfig_ = name;
    refreshMarkList();
    rebuildView();
    emit logMessage(QStringLiteral("[配置] 已加载「%1」（%2 行）").arg(name).arg(rows_.size()));
}

void ComparisonControl::deleteConfig()
{
    if (currentConfig_.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("配置名称不能为空"));
        return;
    }
    const auto choice = QMessageBox::question(
        this, QStringLiteral("确认"), QStringLiteral("确定要删除配置「%1」吗？该操作不可恢复。").arg(currentConfig_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    // 从存储中移除该配置（其余配置原样保留）。
    QJsonObject configs;
    QFile file(storePath());
    if (file.open(QIODevice::ReadOnly))
    {
        configs = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("configs")).toObject();
    }
    configs.remove(currentConfig_);

    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QSaveFile out(storePath());
    if (out.open(QIODevice::WriteOnly))
    {
        QJsonObject root;
        root[QStringLiteral("last")] = QString();
        root[QStringLiteral("configs")] = configs;
        out.write(QJsonDocument(root).toJson());
        out.commit();
    }

    rows_.clear();
    rebuildView();
    ui_->cbConfig->blockSignals(true);
    ui_->cbConfig->removeItem(ui_->cbConfig->findText(currentConfig_));
    ui_->cbConfig->blockSignals(false);
    currentConfig_ = ui_->cbConfig->currentText().trimmed();
    emit logMessage(QStringLiteral("[配置] 已删除「%1」").arg(currentConfig_));
}

void ComparisonControl::newConfig()
{
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("新建配置"), QStringLiteral("配置名："), QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty())
    {
        return;
    }
    const QString trimmed = name.trimmed();
    if (ui_->cbConfig->findText(trimmed) >= 0)
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("配置已存在"));
        return;
    }
    ui_->cbConfig->addItem(trimmed);
    ui_->cbConfig->setCurrentText(trimmed);
    currentConfig_ = trimmed;

    rows_.clear();
    refreshMarkList();
    rebuildView();
    writeStore();
    emit logMessage(QStringLiteral("[配置] 已新建「%1」").arg(trimmed));
}

void ComparisonControl::copyConfig()
{
    const QString source = ui_->cbConfig->currentText().trimmed();
    if (source.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请选择要复制的配置"));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("复制配置"), QStringLiteral("新配置名："),
                                               QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty())
    {
        return;
    }
    const QString trimmed = name.trimmed();
    if (ui_->cbConfig->findText(trimmed) >= 0)
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("配置已存在"));
        return;
    }

    // 先把当前表内容存到源配置，再加载源配置、改名保存——复制的是磁盘上
    // 的最新内容而不是内存里可能未保存的编辑。
    saveConfig();
    loadConfig(source);
    currentConfig_ = trimmed;
    writeStore();
    ui_->cbConfig->addItem(trimmed);
    emit logMessage(QStringLiteral("[配置] 已复制「%1」→「%2」").arg(source, trimmed));
}

// ---------------------------------------------------------------------------
// 视图投影（清单 #8：行号不再是状态）
// ---------------------------------------------------------------------------

void ComparisonControl::refreshMarkList()
{
    updating_ = true;
    ui_->listWidget->clear();
    QStringList marks;
    for (const auto &r : rows_)
    {
        if (!r.mark.isEmpty() && !marks.contains(r.mark))
        {
            marks.append(r.mark);
        }
    }
    marks.sort();
    for (const QString &mark : marks)
    {
        auto *item = new QListWidgetItem(mark, ui_->listWidget);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // 空集 = 全部可见：初始勾满，避免 v1"加载后表格空白，还得逐个勾"的体验。
        item->setCheckState(visibleMarks_.isEmpty() || visibleMarks_.contains(mark) ? Qt::Checked : Qt::Unchecked);
    }
    updating_ = false;
}

bool ComparisonControl::isMarkVisible(const QString &mark) const
{
    return visibleMarks_.isEmpty() || visibleMarks_.contains(mark);
}

void ComparisonControl::rebuildView()
{
    updating_ = true;
    table_->clearContents();
    table_->setRowCount(0);
    viewToRow_.clear();

    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const auto &r = rows_[i];
        if (!isMarkVisible(r.mark))
        {
            continue;
        }
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->ensureRowComplete(row);
        table_->setText(row, toInt(ComColumn::Id), r.id < 0 ? QString() : QString::number(r.id));
        table_->setText(row, toInt(ComColumn::Name), r.name);
        table_->setText(row, toInt(ComColumn::Type), r.type);
        table_->setText(row, toInt(ComColumn::Mark), r.mark);
        table_->setText(row, toInt(ComColumn::LocalDir), r.localDir);
        table_->setText(row, toInt(ComColumn::Peer), r.peer);
        table_->setText(row, toInt(ComColumn::RelDir), r.relDir);
        viewToRow_.push_back(static_cast<int>(i));
    }
    updating_ = false;
}

int ComparisonControl::appendRow(const Row &row)
{
    rows_.push_back(row);
    if (isMarkVisible(row.mark))
    {
        updating_ = true;
        const int viewRow = table_->rowCount();
        table_->insertRow(viewRow);
        table_->ensureRowComplete(viewRow);
        table_->setText(viewRow, toInt(ComColumn::Name), row.name);
        table_->setText(viewRow, toInt(ComColumn::Type), row.type);
        table_->setText(viewRow, toInt(ComColumn::Mark), row.mark);
        table_->setText(viewRow, toInt(ComColumn::LocalDir), row.localDir);
        table_->setText(viewRow, toInt(ComColumn::Peer), row.peer);
        table_->setText(viewRow, toInt(ComColumn::RelDir), row.relDir);
        updating_ = false;
        viewToRow_.push_back(static_cast<int>(rows_.size()) - 1);
        return viewRow;
    }
    return -1;
}

void ComparisonControl::syncRowFromItem(QTableWidgetItem *item)
{
    const int viewRow = item->row();
    const int col = item->column();
    if (viewRow < 0 || viewRow >= static_cast<int>(viewToRow_.size()))
    {
        return;
    }
    auto &r = rows_[static_cast<std::size_t>(viewToRow_[static_cast<std::size_t>(viewRow)])];
    const QString text = item->text();
    switch (static_cast<ComColumn>(col))
    {
    case ComColumn::Name:
        r.name = text;
        // 标记跟随名称推导；拖进来的目录行（mark=="Dir"）保持不变。
        if (r.mark != QStringLiteral("Dir"))
        {
            r.mark = markForName(r.name, false);
        }
        r.type = r.mark == QStringLiteral("Dir") ? QStringLiteral("Dir") : QStringLiteral("File");
        updating_ = true;
        table_->setText(viewRow, toInt(ComColumn::Mark), r.mark);
        table_->setText(viewRow, toInt(ComColumn::Type), r.type);
        updating_ = false;
        break;
    case ComColumn::LocalDir:
        r.localDir = text;
        break;
    case ComColumn::Peer:
        r.peer = text;
        break;
    case ComColumn::RelDir:
        r.relDir = text;
        break;
    default:
        break;  // Id/Type/Mark 只读
    }
}

// ---------------------------------------------------------------------------
// 筛选
// ---------------------------------------------------------------------------

void ComparisonControl::onListContextMenu(const QPoint &pos)
{
    QMenu menu(ui_->listWidget);
    QAction *all = menu.addAction(MenuIcons::selectAll(), QStringLiteral("全选"));
    QAction *none = menu.addAction(MenuIcons::unselectAll(), QStringLiteral("取消全选"));
    const QAction *ret = menu.exec(ui_->listWidget->mapToGlobal(pos));
    if (ret == nullptr)
    {
        return;
    }
    Qt::CheckState state = Qt::Unchecked;
    if (ret == all)
    {
        state = Qt::Checked;
    }
    else if (ret != none)
    {
        return;
    }
    updating_ = true;
    for (int i = 0; i < ui_->listWidget->count(); ++i)
    {
        ui_->listWidget->item(i)->setCheckState(state);
    }
    updating_ = false;
    onListItemToggled();
}

void ComparisonControl::onListItemToggled()
{
    if (updating_)
    {
        return;
    }
    visibleMarks_.clear();
    bool allChecked = true;
    for (int i = 0; i < ui_->listWidget->count(); ++i)
    {
        auto *item = ui_->listWidget->item(i);
        if (item->checkState() == Qt::Checked)
        {
            visibleMarks_.insert(item->text());
        }
        else
        {
            allChecked = false;
        }
    }
    if (allChecked)
    {
        visibleMarks_.clear();  // 全勾等价于"不筛选"，保留这一语义简化判断
    }
    rebuildView();
}

// ---------------------------------------------------------------------------
// 表格右键菜单（清单 #5：动作表注册式）
// ---------------------------------------------------------------------------

QList<QTableWidgetItem *> ComparisonControl::selectedRowItems() const
{
    // 每个选中行取其首格，保证"行"粒度而不依赖选中列。
    QList<QTableWidgetItem *> out;
    QSet<int> seen;
    const auto items = table_->selectedItems();
    for (auto *it : items)
    {
        if (!seen.contains(it->row()))
        {
            seen.insert(it->row());
            out.append(it);
        }
    }
    return out;
}

void ComparisonControl::onTableContextMenu(const QPoint &pos)
{
    const auto selected = selectedRowItems();
    QMenu menu(this);

    QAction *transAction = menu.addAction(MenuIcons::upload(), QStringLiteral("发送到对端"));
    transAction->setEnabled(!selected.isEmpty());
    QAction *newRowAction = menu.addAction(MenuIcons::newRow(), QStringLiteral("新行"));
    QAction *deleteAction = menu.addAction(MenuIcons::del(), QStringLiteral("删除选中行"));
    deleteAction->setEnabled(!selected.isEmpty());

    QMenu *batchMenu = nullptr;
    if (selected.size() > 1)
    {
        batchMenu = menu.addMenu(MenuIcons::fillColumn(), QStringLiteral("批量填充列"));
        for (const auto &bc : kBatchColumns)
        {
            // exec() 是模态的，动作在 exec 内部触发——直接用 triggered lambda
            // 分派，exec 返回后不需要再按指针识别是哪一项。
            connect(batchMenu->addAction(QString::fromUtf8(bc.label)), &QAction::triggered, this,
                    [this, col = toInt(bc.col), selected]()
                    {
                        bool ok = false;
                        const QString value = QInputDialog::getText(this, QStringLiteral("批量填充"),
                                                                    QStringLiteral("将选中行的该列统一设为："),
                                                                    QLineEdit::Normal, {}, &ok);
                        if (!ok)
                        {
                            return;
                        }
                        for (auto *it : selected)
                        {
                            table_->setText(it->row(), col, value);
                        }
                    });
        }
    }

    QAction *accessAction = nullptr;
    QAction *openAction = nullptr;
    if (selected.size() == 1)
    {
        accessAction = menu.addAction(MenuIcons::accessLocal(), QStringLiteral("访问本地目录"));
        openAction = menu.addAction(MenuIcons::openDir(), QStringLiteral("打开本地所在目录"));
    }

    const QAction *ret = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (ret == nullptr)
    {
        return;
    }
    if (ret == transAction)
    {
        onTrans();
        return;
    }
    if (ret == newRowAction)
    {
        appendRow(Row{});
        return;
    }
    if (ret == deleteAction)
    {
        // 倒序删：先删内存模型，再重建视图（视图行号在删除中会失效）。
        QList<int> modelIdx;
        for (auto *it : selected)
        {
            modelIdx.append(viewToRow_[static_cast<std::size_t>(it->row())]);
        }
        std::sort(modelIdx.begin(), modelIdx.end(), std::greater<int>());
        for (int idx : modelIdx)
        {
            rows_.erase(rows_.begin() + idx);
        }
        rebuildView();
        return;
    }
    if (ret == accessAction && !selected.isEmpty())
    {
        // 让浏览面板跳到该行的本地目录（v1 signalExplorerLocal 语义）。
        const QString dir = table_->item(selected.first()->row(), toInt(ComColumn::LocalDir))->text().trimmed();
        if (!dir.isEmpty())
        {
            emit exploreLocalRequested(dir);
        }
        return;
    }
    if (ret == openAction && !selected.isEmpty())
    {
        const QString dir = table_->item(selected.first()->row(), toInt(ComColumn::LocalDir))->text().trimmed();
        if (!dir.isEmpty())
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }
    }
}

// ---------------------------------------------------------------------------
// 搜索替换
// ---------------------------------------------------------------------------

void ComparisonControl::onReplace()
{
    const QString from = ui_->edFrom->text();
    const QString to = ui_->edTo->text();
    if (from.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("查找内容不能为空"));
        return;
    }
    const auto choice = QMessageBox::question(
        this, QStringLiteral("确认"), QStringLiteral("将所有行的可编辑列中的「%1」替换为「%2」？").arg(from, to),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
    {
        return;
    }

    // 简化说明：v1 的 ReplaceArea 可选作用列/大小写/正则；v2 目前作用于
    // 全部可编辑文本列、区分大小写的普通替换。作用域选择等有真实需求再补。
    int hits = 0;
    for (auto &r : rows_)
    {
        QString *fields[] = {&r.name, &r.localDir, &r.peer, &r.relDir};
        for (auto *f : fields)
        {
            if (f->contains(from))
            {
                hits += static_cast<int>(f->count(from));
                *f = f->replace(from, to);
            }
        }
    }
    for (auto &r : rows_)
    {
        r.type = r.mark == QStringLiteral("Dir") ? QStringLiteral("Dir") : QStringLiteral("File");
    }
    rebuildView();
    emit logMessage(QStringLiteral("[替换] 共替换 %1 处").arg(hits));
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void ComparisonControl::onTrans()
{
    const auto selected = selectedRowItems();
    if (selected.isEmpty())
    {
        return;
    }
    RelayTaskItems items;
    for (auto *it : selected)
    {
        const int row = it->row();
        const auto name = table_->item(row, toInt(ComColumn::Name))->text().trimmed();
        const auto type = table_->item(row, toInt(ComColumn::Type))->text().trimmed();
        const auto localDir = table_->item(row, toInt(ComColumn::LocalDir))->text().trimmed();
        const auto peer = table_->item(row, toInt(ComColumn::Peer))->text().trimmed();
        const auto relDir = table_->item(row, toInt(ComColumn::RelDir))->text().trimmed();

        if (name.isEmpty() || localDir.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("第 %1 行的名称或本地目录为空").arg(row + 1));
            return;
        }
        if (peer.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("第 %1 行没有填对端 id（在线列表里可右键复制）").arg(row + 1));
            return;
        }
        if (!QFileInfo::exists(localDir))
        {
            QMessageBox::warning(this, QStringLiteral("提示"),
                                 QStringLiteral("第 %1 行的本地目录不存在：%2").arg(row + 1).arg(localDir));
            return;
        }

        RelayTaskItem item;
        item.name = name;
        item.isDir = type == QStringLiteral("Dir");
        item.localDir = localDir;
        item.peerId = peer;
        item.relDir = relDir;
        items.append(item);
    }
    emit transTaskRun(items);
}
