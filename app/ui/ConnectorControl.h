#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include "bridge/CoreService.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class ConnectorControl;
}
QT_END_NAMESPACE

// 连接面板：服务器地址、在线对端表、本机标识。
//
// 与 v1 的语义差异（这是 v2 结构性决定，不是偷懒）：
//   - v1 有一条常驻控制连接，"连接/断开"管着它的生死；v2 的 Client 是
//     一次操作一条连接（服务端无状态、可水平扩展，见 REWRITE-PLAN 6.1）。
//     GUI 的"连接"= 建立**常驻接收会话**（拿到 id 后一直挂着等文件）。
//   - 连接成功后【连接】按钮禁用（用户要求）：会话已经是常驻的，再点一次
//     只会白耗一个服务端 id 并顶掉当前会话。要换中继地址必须先【断开】。
//     【刷新】不受影响——它在现有会话上问一次列表，id 不变。
//   - 【断开】= 真的结束会话（经 disconnectRequested 交给 MainWindow 处理：
//     它持有自动接收状态机，本控件不知道"断开后该不该重连"）。
//   - 自动接收由 MainWindow 负责（收到 assignedId 后挂起等文件），本控件
//     只负责显示与选择。
//
// §3.7.2 清单落实：右键菜单用动作表注册式写法（对应清单 #5），不写
// exec 返回值平铺比较。
class ConnectorControl : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectorControl(QWidget *parent = nullptr);
    ~ConnectorControl() override;

    ConnectorControl(const ConnectorControl &) = delete;
    ConnectorControl &operator=(const ConnectorControl &) = delete;

    // 当前通信对象（对端表格右键选定）。空表示未选。
    QString currentPeerId() const
    {
        return currentPeerId_;
    }

    // 直接把某个对端设为当前通信对象。给"意图已明确指向某对端"的入口用
    // （跨面板拖拽：拖到谁的面板就是发给谁）。
    //
    // 不在这里做在线校验：调用方（MainWindow）有自己的提示口径，而且
    // 列表可能是旧的。address 可空（有些入口只知道 id）。
    void setCurrentPeer(const QString &id, const QString &address = QString());

    // 最近一次在线列表里有没有这个 id。列表为空（还没刷新过）时返回
    // false——调用方据此区分"确定不在线"与"不知道"，别把后者当错误拦下。
    bool isPeerOnline(const QString &id) const
    {
        return onlineIds_.contains(id);
    }

signals:
    // 右键"发送文件到此对端"。MainWindow 负责弹文件选择框并调 CoreService。
    void sendToPeerRequested(const QString &peerId, const QString &peerName);

    // 右键"浏览此对端的目录"（PROTOCOL.md §11.8）。窗口由 MainWindow 建，
    // 因为浏览要走 CoreService 的常驻会话。
    void browsePeerRequested(const QString &peerId, const QString &peerName);

    // 用户点了"连接"，参数校验通过。MainWindow 记住这组参数，供发送与
    // 自动接收复用（CoreService 一次一个操作，每次操作都要重新给参数）。
    void probeRequested(const relayer::app::ConnInfo &conn);

    // 用户点了"断开"：请求结束常驻会话。由 MainWindow 落手，因为只有它
    // 知道自动接收的状态机（receiveWanted_），直接在这里取消会被自动
    // 重连回来。
    void disconnectRequested();

    // 连接/探测的结论，交给日志面板显示。
    void logMessage(const QString &msg);

public:
    // 注入 CoreService 并接线。由 MainWindow 在构造完两者之后调用——
    // 构造函数里拿不到（MainWindow 先建 CoreService 再建本控件也行，
    // 但显式 attach 让依赖方向一眼可见，不依赖构造顺序约定）。
    void attachCoreService(relayer::app::CoreService *core);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onRefreshClicked();
    void onTableContextMenu(const QPoint &pos);

private:
    void initTable();
    void initButtons();
    void initAuth();
    void loadIpHistory();
    void saveIpHistory(const QString &ipPort);
    bool parseHostPort(const QString &text, QString &host, quint16 &port) const;

    // 只用对端表/选中对象，不动会话（断开时清显示）。
    void clearPeerView();

    // 【连接】/【断开】的可用状态唯一出口：任何影响 connected_/busy_ 的
    // 事件之后都调它，避免状态散成几处各改一个按钮。
    void updateConnectionButtons();

    // ---- CoreService 信号 ----
    void onBusyChanged(bool busy);
    void onAssignedIdChanged(const QString &id);
    void onOperationFinished(bool ok);
    void onPeerListReceived(const QList<relayer::app::PeerRow> &rows);

    Ui::ConnectorControl *ui_;
    relayer::app::CoreService *core_;
    QString currentPeerId_;

    // 常驻会话已建立（拿到非空 id）。空 id 是"认证刚过、id 还没分配"的
    // 中间态，不算连接成功。
    bool connected_ = false;

    // 最近一次 kAnswerClientList 的 id 集合（含自己）。任务入口用它做
    // "填了对端但它不在线"的提示。
    QStringList onlineIds_;
};
