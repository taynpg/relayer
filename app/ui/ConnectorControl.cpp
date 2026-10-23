#include "ConnectorControl.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <functional>

#include "Setting.h"
#include "SettingsKeys.h"
#include "ui_ConnectorControl.h"

using relayer::app::PeerRow;
using relayer::app::settings::kConnectorLast;
using relayer::app::settings::kConnectorNoAuth;
using relayer::app::settings::kConnectorRecent;

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
    // btnLock（"设定"）打开设置对话框（v1 同名按钮的行为）。模态 exec
    // 没问题：设置是纯配置操作，不占传输通道，也不会跑几分钟。
    connect(ui_->btnLock, &QPushButton::clicked, this,
            [this]()
            {
                Setting dlg(this);
                dlg.exec();
            });

    connect(ui_->btnConnect, &QPushButton::clicked, this, &ConnectorControl::onConnectClicked);
    connect(ui_->btnDisconnect, &QPushButton::clicked, this, &ConnectorControl::onDisconnectClicked);
    connect(ui_->btnRefresh, &QPushButton::clicked, this, &ConnectorControl::onRefreshClicked);
    connect(ui_->btnAboutQt, &QPushButton::clicked, this, []() { QApplication::aboutQt(); });

    updateConnectionButtons();
}

void ConnectorControl::updateConnectionButtons()
{
    // 【连接】在"已连上"或"正在连"时都禁用：
    //   - 已连上：再点一次会新开连接、消耗一个新 id 并顶掉常驻会话，
    //     正是文档里反复警告的"点一次刷新 id 涨一号"那类回归；
    //   - 正在连：连点会在握手中途再发一次探测，纯属添乱。
    // 换中继地址的路径是【断开】→【连接】，tooltip 里写明。
    const bool busy = core_ != nullptr && core_->busy();
    ui_->btnConnect->setEnabled(!connected_ && !busy);

    // 【断开】只看 connected_：它在会话活着时常驻可用，哪怕正有传输在跑
    // （中止就是用户的意图）。此前按 busy 禁用，而连接成功后 busy 恒为
    // true，等于永远点不动——那才是真问题。
    ui_->btnDisconnect->setEnabled(connected_);

    if (connected_)
    {
        ui_->btnConnect->setToolTip(
            QStringLiteral("已连接（id=%1）。刷新对端列表请用「刷新」；要换中继地址请先「断开」")
                .arg(ui_->lineEdit->text()));
    }
    else
    {
        ui_->btnConnect->setToolTip(QString());
    }
}

void ConnectorControl::initAuth()
{
    // "无认证"的勾选状态记忆下来（本地自测反复连同一个中继，每次重勾
    // 很烦）；口令框绝不持久化。勾选无认证时口令框禁用，表达"这个值
    // 不会被使用"，也防住"勾了无认证却填了口令"的歧义。
    QSettings settings;
    ui_->ckNoAuth->setChecked(settings.value(relayer::app::settings::kConnectorNoAuth, false).toBool());
    ui_->edPassword->setEnabled(!ui_->ckNoAuth->isChecked());

    connect(ui_->ckNoAuth, &QCheckBox::toggled, this,
            [this](bool checked)
            {
                ui_->edPassword->setEnabled(!checked);
                QSettings s;
                s.setValue(relayer::app::settings::kConnectorNoAuth, checked);
            });
}

void ConnectorControl::loadIpHistory()
{
    QSettings settings;
    ui_->cbIp->clear();
    const QStringList history = settings.value(kConnectorRecent).toStringList();
    ui_->cbIp->addItems(history);
    if (!history.isEmpty())
    {
        ui_->cbIp->setCurrentText(settings.value(kConnectorLast).toString());
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
    settings.setValue(kConnectorRecent, history);
    settings.setValue(kConnectorLast, ipPort);
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

    // 不在这里开连接。连接动作发给 MainWindow 汇聚：会话活着就复用
    // （在现有连接上刷列表，id 不变）；不在就建立常驻接收会话——认证
    // 后 Client 自动问列表（askListOnStart）。整个过程一条连接、一个 id。
    emit probeRequested(conn);
    saveIpHistory(text);
}

void ConnectorControl::clearPeerView()
{
    ui_->tableClients->setRowCount(0);
    ui_->edCurrentClient->setText(QString());
    ui_->lineEdit->setText(QString());
    ui_->lineEdit->setStyleSheet(QString());
    currentPeerId_.clear();
    onlineIds_.clear();
}

void ConnectorControl::onDisconnectClicked()
{
    if (!connected_)
    {
        return;
    }
    // 先落本地状态再发信号：MainWindow 关会话之后 operationFinished 会回来，
    // 那时 connected_ 已经 false，重复置位无害；反过来先发信号的话，中间
    // 那一小段时间按钮还是禁用态。
    connected_ = false;
    updateConnectionButtons();
    clearPeerView();

    // 会话的关闭交给 MainWindow：只有它知道自动接收的状态机（receiveWanted_）。
    // 在这里直接 cancelCurrent 而不清那个开关，断开后会立刻被自动重连回来。
    emit disconnectRequested();
}

void ConnectorControl::onRefreshClicked()
{
    // 与"连接"同一动作（见 onConnectClicked 的注释）：MainWindow 汇聚后
    // 会话活着就在现有连接上刷列表，id 保持稳定。
    onConnectClicked();
}

void ConnectorControl::onBusyChanged(bool busy)
{
    // 只影响按钮可用性，没有别的状态要维护（详见 updateConnectionButtons：
    // 【连接】按"已连接 / 正在连"禁用，【断开】按"已连接"启用）。
    Q_UNUSED(busy);
    updateConnectionButtons();
}

void ConnectorControl::onAssignedIdChanged(const QString &id)
{
    ui_->lineEdit->setText(id);
    ui_->lineEdit->setStyleSheet(QStringLiteral("color: black;"));

    // 空 id 是"认证刚过、服务端还没分配"的中间态（ClientConfig::onIdAssigned
    // 的契约：可能先给空串）。只有拿到真 id 才算连接成功。
    if (!id.isEmpty())
    {
        connected_ = true;
        updateConnectionButtons();
    }
}

void ConnectorControl::onOperationFinished(bool ok)
{
    // 任何一种"会话结束"（用户断开、中继挂了、认证被拒、一次性操作收尾）
    // 都会走到这里：常驻会话已经不在，【连接】恢复可用。
    Q_UNUSED(ok);
    if (connected_)
    {
        connected_ = false;
        updateConnectionButtons();
    }
}

void ConnectorControl::onPeerListReceived(const QList<PeerRow> &rows)
{
    onlineIds_.clear();
    for (const auto &r : rows)
    {
        onlineIds_.append(r.id);
    }

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
    // 会话结束（成功收尾/失败/用户断开）→ 连接按钮恢复可用。
    connect(core, &relayer::app::CoreService::operationFinished, this, &ConnectorControl::onOperationFinished);
    // operationFinished 的日志由 MainWindow 统一出口，这里不接（接了
    // 会把同一行结论打两遍）。
}
