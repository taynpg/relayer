#include "RelayTaskControl.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <limits>

#include "model/PathText.h"
#include "model/ProgressTracker.h"

namespace relayer::app {

namespace {

// 同时进行的最大路数。
//
// 不是越大越好：每多一路就多一条连接和一次握手，而总吞吐由网络决定。
// 3 是"能重叠 RTT"与"别让每条流都变慢"之间的折中，也是对照表里常见的
// A/B/C 三目标场景的路数。
constexpr int kConcurrency = 3;

// 状态的显示文本与颜色集中在这里：新增状态只改这一处。
struct StatusStyle {
    QString text;
    QColor color;
};

StatusStyle statusStyle(RelayTaskControl::Status status)
{
    using S = RelayTaskControl::Status;
    switch (status) {
    case S::kTransing:
        return {QStringLiteral("传输中"), QColor(0, 102, 204)};
    case S::kDone:
        return {QStringLiteral("完成"), QColor(0, 153, 68)};
    case S::kFailed:
        return {QStringLiteral("失败"), QColor(204, 0, 0)};
    case S::kSkipped:
        return {QStringLiteral("已跳过"), QColor(153, 153, 0)};
    case S::kCancelled:
        return {QStringLiteral("已取消"), QColor(136, 136, 136)};
    case S::kWait:
    default:
        return {QStringLiteral("等待"), QColor(136, 136, 136)};
    }
}

// 列布局。与 v1 的"序号/名称/大小/状态/速度/用时"相比：速度与用时
// 合并进"进度"（v2 的单文件进度由 CoreService 100ms 一次直接给，
// 平均速度与用时在日志里，表格不重复展示），新增"对端"与"相对目录"
// ——v2 每行可指定对端与落点，这是对照表带来的新能力，得看得见。
enum Column {
    kColIndex = 0,
    kColName,
    kColPeer,
    kColRelDir,
    kColSize,
    kColStatus,
    kColProgress,
    kColCount,
};

}   // namespace

RelayTaskControl::RelayTaskControl(QWidget* parent) : QDialog(parent)
{
    buildLayout();
}

void RelayTaskControl::buildLayout()
{
    table_ = new QTableWidget(this);
    table_->setColumnCount(kColCount);
    table_->setHorizontalHeaderLabels({QStringLiteral("序号"), QStringLiteral("名称"), QStringLiteral("对端"),
                                       QStringLiteral("相对目录"), QStringLiteral("大小"), QStringLiteral("状态"),
                                       QStringLiteral("进度")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setColumnWidth(kColIndex, 50);
    table_->setColumnWidth(kColName, 220);
    table_->setColumnWidth(kColPeer, 100);
    table_->setColumnWidth(kColRelDir, 200);
    table_->setColumnWidth(kColSize, 90);
    table_->setColumnWidth(kColStatus, 70);
    table_->setColumnWidth(kColProgress, 150);
    table_->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setStretchLastSection(false);

    summaryLabel_ = new QLabel(QStringLiteral("共 0 项"), this);

    // 分页导航条（v1 RelayTask 的同款）：
    //   首页 / 上一页 / 第 x/y 页 ｜ 条目 a-b / 共 n / 下一页 / 末页 / 每页 N
    btnFirst_ = new QPushButton(QStringLiteral("首页"), this);
    btnPrev_ = new QPushButton(QStringLiteral("上一页"), this);
    btnNext_ = new QPushButton(QStringLiteral("下一页"), this);
    btnLast_ = new QPushButton(QStringLiteral("末页"), this);
    pageLabel_ = new QLabel(QStringLiteral("第 1/1 页"), this);
    pageLabel_->setAlignment(Qt::AlignCenter);
    pageSizeBox_ = new QComboBox(this);
    pageSizeBox_->addItems(
        {QStringLiteral("50"), QStringLiteral("100"), QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("全部")});
    pageSizeBox_->setCurrentText(QStringLiteral("100"));

    connect(btnFirst_, &QPushButton::clicked, this, [this]() { gotoPage(0); });
    connect(btnPrev_, &QPushButton::clicked, this, [this]() { gotoPage(page_ - 1); });
    connect(btnNext_, &QPushButton::clicked, this, [this]() { gotoPage(page_ + 1); });
    connect(btnLast_, &QPushButton::clicked, this, [this]() { gotoPage(pageCount() - 1); });
    connect(pageSizeBox_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        // "全部"给一个足够大的数而不是特判分支：pageCount / 取整 / 边界判断
        // 全都照常工作，少一处 if 就少一处漏判。
        pageSize_ = text == QStringLiteral("全部") ? std::numeric_limits<int>::max() : text.toInt();
        if (pageSize_ <= 0) {
            pageSize_ = 100;
        }
        page_ = 0;
        renderPage();
    });

    btnStop_ = new QPushButton(QStringLiteral("停止"), this);
    btnStop_->setEnabled(false);
    connect(btnStop_, &QPushButton::clicked, this, &RelayTaskControl::onStopClicked);

    btnRetryFailed_ = new QPushButton(QStringLiteral("重传失败项"), this);
    btnRetryFailed_->setEnabled(false);
    connect(btnRetryFailed_, &QPushButton::clicked, this, &RelayTaskControl::onRetryFailed);

    btnClear_ = new QPushButton(QStringLiteral("清空"), this);
    connect(btnClear_, &QPushButton::clicked, this, &RelayTaskControl::onClearClicked);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addWidget(summaryLabel_);
    btnLayout->addStretch();
    btnLayout->addWidget(btnFirst_);
    btnLayout->addWidget(btnPrev_);
    btnLayout->addWidget(pageLabel_);
    btnLayout->addWidget(btnNext_);
    btnLayout->addWidget(btnLast_);
    btnLayout->addSpacing(12);
    btnLayout->addWidget(new QLabel(QStringLiteral("每页"), this));
    btnLayout->addWidget(pageSizeBox_);
    btnLayout->addSpacing(12);
    btnLayout->addWidget(btnStop_);
    btnLayout->addWidget(btnRetryFailed_);
    btnLayout->addWidget(btnClear_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(table_);
    layout->addLayout(btnLayout);
    updatePageLabel();
}

void RelayTaskControl::attachCoreService(relayer::app::CoreService* core)
{
    core_ = core;
    // 批量发送走**并发**那条信号（带 taskId），不再用单文件发送的
    // sendFinished/progressChanged：并发时"这次进度是谁的"必须能区分，
    // 否则 B 的进度会画到 C 那一行上。
    connect(core_, &CoreService::sendTaskFinished, this, &RelayTaskControl::onSendTaskFinished);
    connect(core_, &CoreService::sendTaskProgress, this, &RelayTaskControl::onSendTaskProgress);
    connect(core_, &CoreService::operationFinished, this, &RelayTaskControl::onSessionEnded);
}

void RelayTaskControl::startTasks(const RelayTaskItems& items, const ConnInfo& conn, const QString& defaultPeerId)
{
    if (running_) {
        emit logMessage(QStringLiteral("[任务] 已有任务正在进行\n"));
        return;
    }
    conn_ = conn;

    // 目录行展开为文件序列：目录内容落到 relDir/name/ 之下（与 v1 的
    // 发送语义一致，展开逻辑自 MainWindow 临时执行器原样迁来）。
    rows_.clear();
    for (const auto& item : items) {
        // 对端列留空 = 发给连接面板右键选定的"当前通信对象"。在这里就地
        // 解析成具体 id（表格里显示实际目标，发送与重传不用再区分两种
        // 来源）；解析结果仍为空时才落"已跳过"。
        const QString peer = item.peerId.isEmpty() ? defaultPeerId : item.peerId;
        if (!item.isDir) {
            QString filePath = item.localDir;
            if (QFileInfo(filePath).isDir()) {
                filePath = QDir(filePath).filePath(item.name);
            }
            QFileInfo info(filePath);
            // 远端落点统一走 joinRemotePath：relDir 可能是手填的
            // `D://Download//` 或粘来的混合写法，自己拼会得到 `D:///name`。
            const QString rel = relayer::app::joinRemotePath(item.relDir, item.name);
            rows_.push_back({info.fileName(), filePath, peer, rel, static_cast<quint64>(info.size()), kWait});
            continue;
        }

        const QString rootRel = relayer::app::joinRemotePath(item.relDir, item.name);
        QDirIterator it(item.localDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString abs = it.next();
            const QString rel = QDir(item.localDir).relativeFilePath(abs);
            QFileInfo info(abs);
            rows_.push_back({info.fileName(), abs, peer, relayer::app::joinRemotePath(rootRel, rel),
                             static_cast<quint64>(info.size()), kWait});
        }
    }

    // 当前通信对象也没设的行直接标跳过：发送前必须补上，静默丢弃会让
    // 人以为发过了。
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].peerId.isEmpty()) {
            rows_[i].status = kSkipped;
        }
    }

    rebuildTable();
    updateSummary();

    if (rows_.isEmpty()) {
        emit logMessage(QStringLiteral("[任务] 没有可发送的文件（目录为空？）\n"));
        return;
    }

    // 有跳过行时给出可执行的指引：不是"对端没填"，而是"没设通信对象"。
    int skipped = 0;
    for (const auto& r : rows_) {
        skipped += (r.status == kSkipped) ? 1 : 0;
    }
    if (skipped > 0) {
        emit logMessage(QStringLiteral("[任务] %1 项被跳过：对端未填且连接面板未设「当前通信对象」"
                                       "（右键在线对端 → 设为通信对象）\n")
                            .arg(skipped));
    }

    running_ = true;
    stopping_ = false;
    active_.clear();
    trackers_.clear();
    progressText_.clear();
    updateButtons();
    emit logMessage(QStringLiteral("[任务] 开始：%1 个文件\n").arg(rows_.size()));
    fillWindow();
}

void RelayTaskControl::stopBatch()
{
    if (!running_) {
        return;
    }
    stopping_ = true;

    // 还没开始的行现在就标取消：它们不会被发送了。
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].status == kWait) {
            setStatus(i, kCancelled);
        }
    }
    updateSummary();

    // 在跑的那几路逐个硬取消（与 CLI abort 同语义）。它们的结论仍会各自经
    // sendTaskFinished 回来，那里看到 stopping_ 就把行标"已取消"——所以
    // 全部回来之前不结束批次，否则界面上会留下几个永远"传输中"的行。
    if (active_.isEmpty()) {
        finishBatch(false);
        return;
    }
    for (int row : active_) {
        if (core_) {
            core_->cancelSendTask(row + 1);
        }
    }
}

void RelayTaskControl::fillWindow()
{
    if (!running_) {
        return;
    }

    // 补满并发窗口：从前往后找还没开始的行。
    //
    // **同一对端的行串行**：对方的接收端一次只收一个文件（TransferPeer 是按
    // sessionId 解复用的单会话），同时给它发两个，第二个的 kFileRequestSend
    // 只会被拒或被忽略。所以并发的收益只在"发往不同对端"时存在——这恰好
    // 也是对照表的典型形状（A 行发 B、C 行发 C、D 行发 D）。
    while (active_.size() < kConcurrency) {
        int next = -1;
        for (int i = 0; i < rows_.size(); ++i) {
            if (rows_[i].status != kWait) {
                continue;
            }
            bool peerBusy = false;
            for (int a : active_) {
                if (rows_[a].peerId == rows_[i].peerId) {
                    peerBusy = true;
                    break;
                }
            }
            if (!peerBusy) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            break;   // 剩下的都在等同一批对端，等它们回来再补
        }

        const Row& t = rows_[next];
        setStatus(next, kTransing);
        trackers_.remove(next);   // 新的一路，进度从零算
        progressText_.remove(next);
        active_.insert(next);

        emit logMessage(
            QStringLiteral("[任务] %1/%2  %3 → %4:%5\n").arg(next + 1).arg(rows_.size()).arg(t.name, t.peerId, t.relDir));

        // taskId = 行号 + 1：0 保留给"单文件发送"（MainWindow 那条路径）。
        if (!core_->sendFileTask(next + 1, t.localPath, t.relDir, t.peerId, conn_)) {
            active_.remove(next);
            trackers_.remove(next);
            setStatus(next, kFailed);
            emit logMessage(QStringLiteral("[任务] 通道被占用，任务中止\n"));
            finishBatch(false);
            return;
        }
    }

    // 没有在跑的、也没有排队的 → 这一批结束了。
    bool hasWait = false;
    for (const auto& r : rows_) {
        if (r.status == kWait) {
            hasWait = true;
            break;
        }
    }
    if (active_.isEmpty() && !hasWait) {
        bool allOk = true;
        for (const auto& r : rows_) {
            if (r.status == kFailed) {
                allOk = false;
                break;
            }
        }
        finishBatch(allOk);
    }
}

void RelayTaskControl::onSendTaskFinished(qint64 taskId, bool ok, const QString& summary, const QString& detail)
{
    if (!running_) {
        return;
    }
    const int row = static_cast<int>(taskId) - 1;
    if (row < 0 || row >= rows_.size() || !active_.contains(row)) {
        return;   // 不是这一批的，或已经处理过（同一次任务不该有两个结论）
    }
    active_.remove(row);
    trackers_.remove(row);

    if (stopping_) {
        setStatus(row, kCancelled);
        if (active_.isEmpty()) {
            finishBatch(false);
        }
        return;
    }

    if (ok) {
        setStatus(row, kDone);
    } else {
        setStatus(row, kFailed);
        emit logMessage(QStringLiteral("[任务] %1 失败：%2\n").arg(rows_[row].name, detail.isEmpty() ? summary : detail));

        // 连接级失败（中继断开 / 对端失联）：只把**还在排队**的行标跳过。
        // 不像串行时代那样一刀切终止整批——其他并发路可能还连着，它们的
        // 结论会各自回来；真断了的话它们也会失败，不会漏。
        if (core_ && core_->lastFailureConnectionLost()) {
            for (int i = 0; i < rows_.size(); ++i) {
                if (rows_[i].status == kWait) {
                    setStatus(i, kSkipped);
                }
            }
        }
    }

    fillWindow();
}

void RelayTaskControl::onSessionEnded(bool ok, const QString& summary, const QString& detail)
{
    // 只有批次进行中才关心：空闲时的会话失败由 MainWindow 处理（重连/换 id）。
    if (!running_) {
        return;
    }
    Q_UNUSED(ok);

    // 结束的是**常驻接收会话**。并发发送各自有连接，理论上不受它影响，
    // 但中继整体挂了的话它们也会各自失败。所以这里只把排队的项标跳过，
    // 在跑的那几路等它们自己的结论——强行改它们的状态会让结论对不上。
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].status == kWait) {
            setStatus(i, kSkipped);
        }
    }
    emit logMessage(QStringLiteral("[任务] 常驻会话已结束（%1%2），在跑的项各自报结果\n")
                        .arg(summary, detail.isEmpty() ? QString() : QStringLiteral("：") + detail));
    if (active_.isEmpty()) {
        finishBatch(false);
    }
}

void RelayTaskControl::onSendTaskProgress(qint64 taskId, quint64 bytesDone, quint64 bytesTotal, double bytesPerSecond)
{
    if (!running_) {
        return;
    }
    const int row = static_cast<int>(taskId) - 1;
    if (row < 0 || row >= rows_.size() || !active_.contains(row)) {
        return;
    }
    // bytesTotal == 0 是"还没等到文件请求"的占位期，表格里保持空白
    // （日志侧由 MainWindow 提示"等待文件"，两边语义不同）。
    if (bytesTotal == 0) {
        return;
    }
    // 每行一个 tracker：并发时共用一份会把别的任务的速率画到这一行上。
    progressText_[row] = trackers_[row].update(bytesDone, bytesTotal, bytesPerSecond);
    const int viewRow = viewRowOf(row);
    if (viewRow < 0) {
        return;   // 不在当前页：文本已存，切页时 fillRow 会取
    }
    if (auto* it = table_->item(viewRow, kColProgress)) {
        it->setText(progressText_.value(row));
    }
}

void RelayTaskControl::onRetryFailed()
{
    if (running_) {
        return;
    }
    if (core_ && core_->sendInFlight()) {
        emit logMessage(QStringLiteral("[任务] 有文件正在发送，稍后再试\n"));
        return;
    }
    if (core_ && core_->busy() && !core_->canPreempt()) {
        emit logMessage(QStringLiteral("[任务] 通道被占用，稍后再试\n"));
        return;
    }
    if (conn_.host.isEmpty()) {
        emit logMessage(QStringLiteral("[任务] 未连接中继，请先连接\n"));
        return;
    }

    bool any = false;
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].status == kFailed) {
            setStatus(i, kWait);
            any = true;
        }
    }
    if (!any) {
        emit logMessage(QStringLiteral("[任务] 没有失败项可重试\n"));
        return;
    }

    running_ = true;
    stopping_ = false;
    active_.clear();
    trackers_.clear();
    progressText_.clear();
    updateButtons();
    emit logMessage(QStringLiteral("[任务] 重传失败项\n"));
    fillWindow();
}

void RelayTaskControl::onStopClicked()
{
    stopBatch();
}

void RelayTaskControl::onClearClicked()
{
    if (running_) {
        return;
    }
    rows_.clear();
    progressText_.clear();
    page_ = 0;
    renderPage();
    updateSummary();
}

void RelayTaskControl::finishBatch(bool allOk)
{
    const int total = static_cast<int>(rows_.size());
    running_ = false;
    stopping_ = false;
    active_.clear();
    trackers_.clear();
    updateButtons();
    updateSummary();
    if (allOk) {
        emit logMessage(QStringLiteral("[任务] 全部完成（%1 个文件）\n").arg(total));
    } else {
        int failed = 0;
        for (const auto& r : rows_) {
            failed += (r.status == kFailed) ? 1 : 0;
        }
        emit logMessage(QStringLiteral("[任务] 批次结束：失败 %1 个，可点「重传失败项」重试\n").arg(failed));
    }
    emit batchFinished(allOk);
}

int RelayTaskControl::pageCount() const
{
    const int total = static_cast<int>(rows_.size());
    if (total <= 0) {
        return 1;
    }
    const qint64 size = qMax(1, pageSize_);
    return static_cast<int>((static_cast<qint64>(total) + size - 1) / size);
}

int RelayTaskControl::viewRowOf(int modelRow) const
{
    if (modelRow < 0 || modelRow >= rows_.size()) {
        return -1;
    }
    // qint64：pageSize_ 在"全部"时是 INT_MAX，page_*pageSize_ 用 int 算会溢出。
    const qint64 start = static_cast<qint64>(page_) * pageSize_;
    const qint64 offset = static_cast<qint64>(modelRow) - start;
    if (offset < 0 || offset >= pageSize_) {
        return -1;
    }
    return static_cast<int>(offset);
}

void RelayTaskControl::fillRow(int viewRow, int modelRow)
{
    const Row& r = rows_[modelRow];
    auto mkItem = [](const QString& text) {
        auto* it = new QTableWidgetItem(text);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        return it;
    };
    table_->setItem(viewRow, kColIndex, mkItem(QString::number(modelRow + 1)));
    table_->setItem(viewRow, kColName, mkItem(r.name));
    table_->setItem(viewRow, kColPeer, mkItem(r.peerId));
    table_->setItem(viewRow, kColRelDir, mkItem(r.relDir));
    table_->setItem(viewRow, kColSize, mkItem(humanBytes(r.size)));

    const StatusStyle style = statusStyle(r.status);
    auto* statusItem = mkItem(style.text);
    statusItem->setForeground(style.color);
    table_->setItem(viewRow, kColStatus, statusItem);

    // 进度按模型行的文本恢复：翻到别的页再翻回来不该是空白，
    // 也不该等下一次 100ms 回调才填上。
    table_->setItem(viewRow, kColProgress, mkItem(progressText_.value(modelRow)));
}

void RelayTaskControl::renderPage()
{
    const int pages = pageCount();
    page_ = qBound(0, page_, pages - 1);

    const qint64 start = static_cast<qint64>(page_) * pageSize_;
    const int first = start < rows_.size() ? static_cast<int>(start) : -1;
    const int last = first < 0 ? -1 : static_cast<int>(qMin<qint64>(start + pageSize_, static_cast<qint64>(rows_.size())));

    table_->setUpdatesEnabled(false);
    table_->clearContents();
    table_->setRowCount(0);

    if (first >= 0 && last > first) {
        table_->setRowCount(last - first);
        for (int m = first; m < last; ++m) {
            fillRow(m - first, m);
        }
    }
    table_->setUpdatesEnabled(true);
    updatePageLabel();
}

void RelayTaskControl::gotoPage(int page)
{
    const int target = qBound(0, page, pageCount() - 1);
    if (target == page_) {
        return;
    }
    page_ = target;
    renderPage();
}

void RelayTaskControl::updatePageLabel()
{
    const int total = static_cast<int>(rows_.size());
    const int pages = pageCount();
    page_ = qBound(0, page_, pages - 1);

    const int begin = total == 0 ? 0 : static_cast<int>(static_cast<qint64>(page_) * pageSize_) + 1;
    const int end = total == 0 ? 0 : static_cast<int>(qMin<qint64>(static_cast<qint64>(page_ + 1) * pageSize_, total));
    pageLabel_->setText(
        QStringLiteral("第 %1/%2 页 ｜ 条目 %3-%4 / 共 %5").arg(page_ + 1).arg(pages).arg(begin).arg(end).arg(total));
    btnFirst_->setEnabled(page_ > 0);
    btnPrev_->setEnabled(page_ > 0);
    btnNext_->setEnabled(page_ < pages - 1);
    btnLast_->setEnabled(page_ < pages - 1);
}

void RelayTaskControl::rebuildTable()
{
    page_ = 0;
    renderPage();
}

void RelayTaskControl::setStatus(int row, Status st)
{
    if (row < 0 || row >= rows_.size()) {
        return;
    }
    rows_[row].status = st;
    refreshRowCells(row);
    if (st == kDone || st == kFailed) {
        updateSummary();
    }
}

void RelayTaskControl::refreshRowCells(int row)
{
    if (row < 0 || row >= rows_.size()) {
        return;
    }
    const int viewRow = viewRowOf(row);
    if (viewRow < 0) {
        return;   // 不在当前页：模型已改，切到那页时 fillRow 会带上
    }
    const StatusStyle style = statusStyle(rows_[row].status);
    if (auto* it = table_->item(viewRow, kColStatus)) {
        it->setText(style.text);
        it->setForeground(style.color);
    }
}

void RelayTaskControl::updateButtons()
{
    btnStop_->setEnabled(running_);
    btnRetryFailed_->setEnabled(!running_);
    btnClear_->setEnabled(!running_);
}

void RelayTaskControl::updateSummary()
{
    int done = 0, failed = 0, pending = 0;
    for (const auto& r : rows_) {
        switch (r.status) {
        case kDone:
            ++done;
            break;
        case kFailed:
            ++failed;
            break;
        case kWait:
        case kTransing:
            ++pending;
            break;
        default:
            break;
        }
    }
    summaryLabel_->setText(
        QStringLiteral("共 %1 项 · 完成 %2 · 失败 %3 · 待处理 %4").arg(rows_.size()).arg(done).arg(failed).arg(pending));
}

}   // namespace relayer::app
