#include "RelayTaskControl.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace relayer::app
{

namespace
{

QString humanBytes(quint64 n)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        ++i;
    }
    return QString::number(v, 'f', 1) + QLatin1String(units[i]);
}

// 状态的显示文本与颜色集中在这里：新增状态只改这一处。
struct StatusStyle
{
    QString text;
    QColor color;
};

StatusStyle statusStyle(RelayTaskControl::Status status)
{
    using S = RelayTaskControl::Status;
    switch (status)
    {
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
enum Column
{
    kColIndex = 0,
    kColName,
    kColPeer,
    kColRelDir,
    kColSize,
    kColStatus,
    kColProgress,
    kColCount,
};

}  // namespace

RelayTaskControl::RelayTaskControl(QWidget *parent)
    : QDialog(parent)
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
    table_->setColumnWidth(kColProgress, 70);
    table_->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Stretch);
    table_->horizontalHeader()->setStretchLastSection(false);

    summaryLabel_ = new QLabel(QStringLiteral("共 0 项"), this);

    btnStop_ = new QPushButton(QStringLiteral("停止"), this);
    btnStop_->setEnabled(false);
    connect(btnStop_, &QPushButton::clicked, this, &RelayTaskControl::onStopClicked);

    btnRetryFailed_ = new QPushButton(QStringLiteral("重传失败项"), this);
    btnRetryFailed_->setEnabled(false);
    connect(btnRetryFailed_, &QPushButton::clicked, this, &RelayTaskControl::onRetryFailed);

    btnClear_ = new QPushButton(QStringLiteral("清空"), this);
    connect(btnClear_, &QPushButton::clicked, this, &RelayTaskControl::onClearClicked);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addWidget(summaryLabel_);
    btnLayout->addStretch();
    btnLayout->addWidget(btnStop_);
    btnLayout->addWidget(btnRetryFailed_);
    btnLayout->addWidget(btnClear_);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(table_);
    layout->addLayout(btnLayout);
}

void RelayTaskControl::attachCoreService(relayer::app::CoreService *core)
{
    core_ = core;
    connect(core_, &CoreService::sendFinished, this, &RelayTaskControl::onSendFinished);
    connect(core_, &CoreService::operationFinished, this, &RelayTaskControl::onSessionEnded);
    connect(core_, &CoreService::progressChanged, this, &RelayTaskControl::onProgress);
}

void RelayTaskControl::startTasks(const RelayTaskItems &items, const ConnInfo &conn, const QString &defaultPeerId)
{
    if (running_)
    {
        emit logMessage(QStringLiteral("[任务] 已有任务正在进行\n"));
        return;
    }
    conn_ = conn;

    // 目录行展开为文件序列：目录内容落到 relDir/name/ 之下（与 v1 的
    // 发送语义一致，展开逻辑自 MainWindow 临时执行器原样迁来）。
    rows_.clear();
    for (const auto &item : items)
    {
        // 对端列留空 = 发给连接面板右键选定的"当前通信对象"。在这里就地
        // 解析成具体 id（表格里显示实际目标，发送与重传不用再区分两种
        // 来源）；解析结果仍为空时才落"已跳过"。
        const QString peer = item.peerId.isEmpty() ? defaultPeerId : item.peerId;
        if (!item.isDir)
        {
            QString filePath = item.localDir;
            if (QFileInfo(filePath).isDir())
            {
                filePath = QDir(filePath).filePath(item.name);
            }
            QFileInfo info(filePath);
            const QString rel = item.relDir.isEmpty() ? item.name : item.relDir + QStringLiteral("/") + item.name;
            rows_.push_back({info.fileName(), filePath, peer, rel, static_cast<quint64>(info.size()), kWait});
            continue;
        }

        const QString rootRel = item.relDir.isEmpty() ? item.name : item.relDir + QStringLiteral("/") + item.name;
        QDirIterator it(item.localDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString abs = it.next();
            const QString rel = QDir(item.localDir).relativeFilePath(abs);
            QFileInfo info(abs);
            rows_.push_back({info.fileName(), abs, peer, rootRel + QStringLiteral("/") + rel,
                             static_cast<quint64>(info.size()), kWait});
        }
    }

    // 当前通信对象也没设的行直接标跳过：发送前必须补上，静默丢弃会让
    // 人以为发过了。
    for (int i = 0; i < rows_.size(); ++i)
    {
        if (rows_[i].peerId.isEmpty())
        {
            rows_[i].status = kSkipped;
        }
    }

    rebuildTable();
    updateSummary();

    if (rows_.isEmpty())
    {
        emit logMessage(QStringLiteral("[任务] 没有可发送的文件（目录为空？）\n"));
        return;
    }

    // 有跳过行时给出可执行的指引：不是"对端没填"，而是"没设通信对象"。
    int skipped = 0;
    for (const auto &r : rows_)
    {
        skipped += (r.status == kSkipped) ? 1 : 0;
    }
    if (skipped > 0)
    {
        emit logMessage(QStringLiteral("[任务] %1 项被跳过：对端未填且连接面板未设「当前通信对象」"
                                       "（右键在线对端 → 设为通信对象）\n")
                            .arg(skipped));
    }

    running_ = true;
    stopping_ = false;
    current_ = -1;
    updateButtons();
    emit logMessage(QStringLiteral("[任务] 开始：%1 个文件\n").arg(rows_.size()));
    startNext();
}

void RelayTaskControl::stopBatch()
{
    if (!running_)
    {
        return;
    }
    stopping_ = true;
    // 剩余等待项现在就标取消：它们永远不会被发送了。
    for (int i = current_ + 1; i < rows_.size(); ++i)
    {
        if (rows_[i].status == kWait)
        {
            setStatus(i, kCancelled);
        }
    }
    updateSummary();
    // 当前项走硬取消（与 CLI abort 同语义），结论由 operationFinished 带回。
    if (core_)
    {
        core_->cancelCurrent();
    }
}

void RelayTaskControl::startNext()
{
    if (!running_)
    {
        return;
    }

    // 跳过无需发送的行（未指定对端）。
    while (current_ + 1 < rows_.size() && rows_[current_ + 1].status == kSkipped)
    {
        ++current_;
    }

    if (current_ + 1 >= rows_.size())
    {
        bool allOk = true;
        for (const auto &r : rows_)
        {
            if (r.status == kFailed)
            {
                allOk = false;
                break;
            }
        }
        finishBatch(allOk);
        return;
    }

    ++current_;
    const Row &t = rows_[current_];
    setStatus(current_, kTransing);
    emit logMessage(QStringLiteral("[任务] %1/%2  %3 → %4:%5\n")
                        .arg(current_ + 1)
                        .arg(rows_.size())
                        .arg(t.name, t.peerId, t.relDir));

    // 防御：入口已挡过真忙。到这还 sendFile 失败说明通道被意外占住，
    // 宁可中止批次也不能卡在"等一个永远不来的 operationFinished"。
    if (!core_->sendFile(t.localPath, t.relDir, t.peerId, conn_))
    {
        emit logMessage(QStringLiteral("[任务] 通道被占用，任务中止\n"));
        setStatus(current_, kFailed);
        finishBatch(false);
    }
}

void RelayTaskControl::onSendFinished(bool ok, const QString &summary, const QString &detail)
{
    if (!running_ || current_ < 0 || current_ >= rows_.size())
    {
        return;
    }

    if (stopping_)
    {
        setStatus(current_, kCancelled);
        finishBatch(false);
        return;
    }

    if (ok)
    {
        setStatus(current_, kDone);
    }
    else
    {
        setStatus(current_, kFailed);
        emit logMessage(
            QStringLiteral("[任务] %1 失败：%2\n").arg(rows_[current_].name, detail.isEmpty() ? summary : detail));

        // 连接级失败（中继断开/对端失联）终止批次：链路没了，剩余文件
        // 只会逐个等满 answerTimeout。文件级失败（目标已存在、写盘失败、
        // 校验不过）继续下一个——v1 的"单个文件失败不中断整批"。
        if (core_ && core_->lastFailureConnectionLost())
        {
            emit logMessage(QStringLiteral("[任务] 连接已断开，剩余项不再传输（可连接后点「重传失败项」）\n"));
            for (int i = current_ + 1; i < rows_.size(); ++i)
            {
                if (rows_[i].status == kWait)
                {
                    setStatus(i, kSkipped);
                }
            }
            finishBatch(false);
            return;
        }
    }

    startNext();
}

void RelayTaskControl::onSessionEnded(bool ok, const QString &summary, const QString &detail)
{
    // 只有批次进行中才关心：空闲时的会话失败由 MainWindow 处理（重连/换 id）。
    if (!running_)
    {
        return;
    }
    Q_UNUSED(ok);

    if (current_ >= 0 && current_ < rows_.size())
    {
        setStatus(current_, kFailed);
    }
    emit logMessage(QStringLiteral("[任务] 会话已结束（%1%2），批次中止\n")
                        .arg(summary, detail.isEmpty() ? QString() : QStringLiteral("：") + detail));
    for (int i = current_ + 1; i < rows_.size(); ++i)
    {
        if (rows_[i].status == kWait)
        {
            setStatus(i, kSkipped);
        }
    }
    finishBatch(false);
}

void RelayTaskControl::onProgress(quint64 bytesDone, quint64 bytesTotal, double /*bytesPerSecond*/)
{
    if (!running_ || current_ < 0 || current_ >= rows_.size())
    {
        return;
    }
    const int pct =
        bytesTotal > 0 ? static_cast<int>(100.0 * static_cast<double>(bytesDone) / static_cast<double>(bytesTotal)) : 0;
    if (auto *it = table_->item(current_, kColProgress))
    {
        it->setText(QStringLiteral("%1%").arg(pct));
    }
}

void RelayTaskControl::onRetryFailed()
{
    if (running_)
    {
        return;
    }
    if (core_ && core_->sendInFlight())
    {
        emit logMessage(QStringLiteral("[任务] 有文件正在发送，稍后再试\n"));
        return;
    }
    if (core_ && core_->busy() && !core_->canPreempt())
    {
        emit logMessage(QStringLiteral("[任务] 通道被占用，稍后再试\n"));
        return;
    }
    if (conn_.host.isEmpty())
    {
        emit logMessage(QStringLiteral("[任务] 未连接中继，请先连接\n"));
        return;
    }

    bool any = false;
    for (int i = 0; i < rows_.size(); ++i)
    {
        if (rows_[i].status == kFailed)
        {
            setStatus(i, kWait);
            any = true;
        }
    }
    if (!any)
    {
        emit logMessage(QStringLiteral("[任务] 没有失败项可重试\n"));
        return;
    }

    running_ = true;
    stopping_ = false;
    current_ = -1;
    updateButtons();
    emit logMessage(QStringLiteral("[任务] 重传失败项\n"));
    startNext();
}

void RelayTaskControl::onStopClicked()
{
    stopBatch();
}

void RelayTaskControl::onClearClicked()
{
    if (running_)
    {
        return;
    }
    rows_.clear();
    table_->setRowCount(0);
    updateSummary();
}

void RelayTaskControl::finishBatch(bool allOk)
{
    const int total = static_cast<int>(rows_.size());
    running_ = false;
    stopping_ = false;
    current_ = -1;
    updateButtons();
    updateSummary();
    if (allOk)
    {
        emit logMessage(QStringLiteral("[任务] 全部完成（%1 个文件）\n").arg(total));
    }
    else
    {
        int failed = 0;
        for (const auto &r : rows_)
        {
            failed += (r.status == kFailed) ? 1 : 0;
        }
        emit logMessage(QStringLiteral("[任务] 批次结束：失败 %1 个，可点「重传失败项」重试\n").arg(failed));
    }
    emit batchFinished(allOk);
}

void RelayTaskControl::rebuildTable()
{
    table_->setRowCount(static_cast<int>(rows_.size()));
    for (int i = 0; i < rows_.size(); ++i)
    {
        const Row &r = rows_[i];
        auto mkItem = [](const QString &text)
        {
            auto *it = new QTableWidgetItem(text);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            return it;
        };
        table_->setItem(i, kColIndex, mkItem(QString::number(i + 1)));
        table_->setItem(i, kColName, mkItem(r.name));
        table_->setItem(i, kColPeer, mkItem(r.peerId));
        table_->setItem(i, kColRelDir, mkItem(r.relDir));
        table_->setItem(i, kColSize, mkItem(humanBytes(r.size)));
        table_->setItem(i, kColStatus, mkItem(QString()));
        table_->setItem(i, kColProgress, mkItem(QString()));
        refreshRowCells(i);
    }
}

void RelayTaskControl::setStatus(int row, Status st)
{
    if (row < 0 || row >= rows_.size())
    {
        return;
    }
    rows_[row].status = st;
    refreshRowCells(row);
    if (st == kDone || st == kFailed)
    {
        updateSummary();
    }
}

void RelayTaskControl::refreshRowCells(int row)
{
    if (row < 0 || row >= rows_.size())
    {
        return;
    }
    const StatusStyle style = statusStyle(rows_[row].status);
    if (auto *it = table_->item(row, kColStatus))
    {
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
    for (const auto &r : rows_)
    {
        switch (r.status)
        {
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
    summaryLabel_->setText(QStringLiteral("共 %1 项 · 完成 %2 · 失败 %3 · 待处理 %4")
                               .arg(rows_.size())
                               .arg(done)
                               .arg(failed)
                               .arg(pending));
}

}  // namespace relayer::app
