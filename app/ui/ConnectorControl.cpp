#include "ConnectorControl.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <functional>

#include "ui_ConnectorControl.h"

using relayer::app::PeerRow;

namespace
{
constexpr int kColId = 0;
constexpr int kColAddress = 1;
}  // namespace

ConnectorControl::ConnectorControl(QWidget *parent)
    : QDialog(parent)
    , ui_(new Ui::ConnectorControl)
    , core_(nullptr)
{
    ui_->setupUi(this);
    setMaximumWidth(350);
    initTable();
    initButtons();
    initAuth();
    loadIpHistory();
}

ConnectorControl::~ConnectorControl()
{
    delete ui_;
}

void ConnectorControl::initTable()
{
    QTableWidget *table = ui_->tableClients;
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("地址")});
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setColumnWidth(0, 150);
    table->setColumnWidth(1, 150);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->horizontalHeader()->setSectionResizeMode(kColId, QHeaderView::Fixed);
    table->horizontalHeader()->setSectionResizeMode(kColAddress, QHeaderView::Stretch);

    connect(table, &QTableWidget::customContextMenuRequested, this, &ConnectorControl::onTableContextMenu);
}

void ConnectorControl::initButtons()
{
    // btnLock（"设定"）在 Setting 对话框迁移进来之前不可用。
    // 禁用而不是隐藏：布局与 v1 一致，用户能看到功能在计划内。
    ui_->btnLock->setEnabled(false);
    ui_->btnLock->setToolTip(QStringLiteral("设置面板将在后续版本提供"));

    connect(ui_->btnConnect, &QPushButton::clicked, this, &ConnectorControl::onConnectClicked);
    connect(ui_->btnDisconnect, &QPushButton::clicked, this, &ConnectorControl::onClearClicked);
    connect(ui_->btnRefresh, &QPushButton::clicked, this, &ConnectorControl::onRefreshClicked);
    connect(ui_->btnAboutQt, &QPushButton::clicked, this, []() { QApplication::aboutQt(); });
}

void ConnectorControl::initAuth()
{
    // "无认证"的勾选状态记忆下来（本地自测反复连同一个中继，每次重勾
    // 很烦）；口令框绝不持久化。勾选无认证时口令框禁用，表达"这个值
    // 不会被使用"，也防住"勾了无认证却填了口令"的歧义。
    QSettings settings;
    ui_->ckNoAuth->setChecked(settings.value("connector/noAuth", false).toBool());
    ui_->edPassword->setEnabled(!ui_->ckNoAuth->isChecked());

    connect(ui_->ckNoAuth, &QCheckBox::toggled, this,
            [this](bool checked)
            {
                ui_->edPassword->setEnabled(!checked);
                QSettings s;
                s.setValue("connector/noAuth", checked);
            });
}

void ConnectorControl::loadIpHistory()
{
    QSettings settings;
    ui_->cbIp->clear();
    const QStringList history = settings.value("connector/recent").toStringList();
    ui_->cbIp->addItems(history);
    if (!history.isEmpty())
    {
        ui_->cbIp->setCurrentText(settings.value("connector/last").toString());
    }
    ui_->cbIp->setEditable(true);
}

void ConnectorControl::saveIpHistory(const QString &ipPort)
{
    QSettings settings;
    QStringList history = settings.value("connector/recent").toStringList();
    history.removeAll(ipPort);
    history.prepend(ipPort);
    while (history.size() > 10)
    {
        history.removeLast();
    }
    settings.setValue("connector/recent", history);
    settings.setValue("connector/last", ipPort);
    loadIpHistory();
}

// v1 的 parseIpPort 只认 IPv4。中继地址还可能是域名与 IPv6，所以 v2
// 放宽为 "host:port"——host 不含冒号即可（IPv6 字面量需写方括号，暂不支持，
// 报错文案里说明）。端口校验是硬性的，host 只做非空与无冒号检查。
bool ConnectorControl::parseHostPort(const QString &text, QString &host, quint16 &port) const
{
    static const QRegularExpression re(QStringLiteral("^\\s*([^:\\s]+)\\s*:\\s*(\\d{1,5})\\s*$"));
    const auto m = re.match(text);
    if (!m.hasMatch())
    {
        return false;
    }
    bool ok = false;
    const int n = m.captured(2).toInt(&ok);
    if (!ok || n <= 0 || n > 65535)
    {
        return false;
    }
    host = m.captured(1);
    port = static_cast<quint16>(n);
    return true;
}

void ConnectorControl::onConnectClicked()
{
    const QString text = ui_->cbIp->currentText().trimmed();
    QString host;
    quint16 port = 0;
    if (!parseHostPort(text, host, port))
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("地址格式应为 主机:端口，例如 127.0.0.1:9527"));
        return;
    }

    // 口令与认证方式都在面板上（内联而非弹窗）：状态始终可见，也不存在
    // "弹窗被主窗口挡住"这类无法自查的问题。口令不持久化——持久化的
    // 口令等于把钥匙贴在锁上。
    const bool noAuth = ui_->ckNoAuth->isChecked();
    const QString password = ui_->edPassword->text();
    if (!noAuth && password.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请填写认证口令；若中继以 --allow-no-auth 启动，请勾选「无认证」"));
        return;
    }

    relayer::app::ConnInfo conn;
    conn.host = host;
    conn.port = port;
    conn.password = password;
    conn.noAuth = noAuth;

    if (core_ && !core_->listPeers(conn))
    {
        // 只有真在传输才会到这（接收挂起会被 CoreService 自动抢占）。
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("有传输正在进行，请等它结束"));
        return;
    }
    emit probeRequested(conn);
    saveIpHistory(text);
}

void ConnectorControl::onClearClicked()
{
    ui_->tableClients->setRowCount(0);
    ui_->edCurrentClient->setText(QString());
    ui_->lineEdit->setText(QString());
    currentPeerId_.clear();
}

void ConnectorControl::onRefreshClicked()
{
    // 与"连接"同一动作：v2 每次操作一条连接，刷新就是重新探测一遍。
    onConnectClicked();
}

void ConnectorControl::onBusyChanged(bool busy)
{
    // 连接/刷新按钮**不再随 busy 禁用**：v2 的"在线"由自动接收循环维持，
    // 接收挂起期间 busy 恒为 true，禁用会让用户永远点不了刷新（实测如此）。
    // CoreService 已能抢占挂起的接收，传输中再点会收到明确提示。
    // 断开按钮保留 busy 禁用：它要动对端表，探测中途清表会跟结果渲染打架。
    ui_->btnDisconnect->setEnabled(!busy);
}

void ConnectorControl::onAssignedIdChanged(const QString &id)
{
    ui_->lineEdit->setText(id);
    ui_->lineEdit->setStyleSheet(QStringLiteral("color: black;"));
}

void ConnectorControl::onPeerListReceived(const QList<PeerRow> &rows)
{
    QTableWidget *table = ui_->tableClients;
    table->clearContents();
    table->setRowCount(static_cast<int>(rows.size()));
    for (int r = 0; r < rows.size(); ++r)
    {
        auto *idItem = new QTableWidgetItem(rows[r].id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        auto *addrItem = new QTableWidgetItem(rows[r].address);
        addrItem->setFlags(addrItem->flags() & ~Qt::ItemIsEditable);

        // 自己标红（同 v1）：发的文件"发给自己"是合法用法（本机自测），
        // 但容易误操作，标出来。
        if (rows[r].id == ui_->lineEdit->text())
        {
            idItem->setForeground(Qt::red);
            addrItem->setForeground(Qt::red);
        }
        table->setItem(r, kColId, idItem);
        table->setItem(r, kColAddress, addrItem);
    }
}

void ConnectorControl::onTableContextMenu(const QPoint &pos)
{
    auto *item = ui_->tableClients->itemAt(pos);
    if (!item)
    {
        return;
    }
    const int row = item->row();
    const QString id = ui_->tableClients->item(row, kColId)->text();
    const QString address = ui_->tableClients->item(row, kColAddress)->text();

    QMenu menu(this);

    // 动作表：{文字, 回调}。选中即执行，不需要 exec 返回值比较。
    const struct
    {
        QString label;
        std::function<void()> run;
    } actions[] = {
        {QStringLiteral("设为通信对象"),
         [this, id, address]()
         {
             currentPeerId_ = id;
             ui_->edCurrentClient->setText(QStringLiteral("%1（%2）").arg(id, address));
         }},
        {QStringLiteral("发送文件到此对端"), [this, id, address]() { emit sendToPeerRequested(id, address); }},
    };

    for (const auto &a : actions)
    {
        connect(menu.addAction(a.label), &QAction::triggered, this, a.run);
    }
    menu.exec(ui_->tableClients->viewport()->mapToGlobal(pos));
}

void ConnectorControl::attachCoreService(relayer::app::CoreService *core)
{
    core_ = core;
    if (!core)
    {
        return;
    }
    connect(core, &relayer::app::CoreService::busyChanged, this, &ConnectorControl::onBusyChanged);
    connect(core, &relayer::app::CoreService::assignedIdChanged, this, &ConnectorControl::onAssignedIdChanged);
    connect(core, &relayer::app::CoreService::peerListReceived, this, &ConnectorControl::onPeerListReceived);
    // operationFinished 的日志由 MainWindow 统一出口，这里不接（接了
    // 会把同一行结论打两遍）。
}
