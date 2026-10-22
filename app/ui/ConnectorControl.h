#pragma once

#include <QDialog>
#include <QString>

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
//     所以"连接"在 v2 里是**探测**（连上去拉一次在线列表，拿回自己的 id），
//     "断开"是**清除**（清空对端列表与选中对象）。
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

signals:
    // 右键"发送文件到此对端"。MainWindow 负责弹文件选择框并调 CoreService。
    void sendToPeerRequested(const QString &peerId, const QString &peerName);

    // 用户点了"连接"，参数校验通过。MainWindow 记住这三个值，供发送与
    // 自动接收复用（CoreService 一次一个操作，每次操作都要重新给参数）。
    void probeRequested(const QString &host, quint16 port, const QString &password);

    // 连接/探测的结论，交给日志面板显示。
    void logMessage(const QString &msg);

public:
    // 注入 CoreService 并接线。由 MainWindow 在构造完两者之后调用——
    // 构造函数里拿不到（MainWindow 先建 CoreService 再建本控件也行，
    // 但显式 attach 让依赖方向一眼可见，不依赖构造顺序约定）。
    void attachCoreService(relayer::app::CoreService *core);

private slots:
    void onConnectClicked();
    void onClearClicked();
    void onRefreshClicked();
    void onTableContextMenu(const QPoint &pos);

private:
    void initTable();
    void initButtons();
    void loadIpHistory();
    void saveIpHistory(const QString &ipPort);
    bool parseHostPort(const QString &text, QString &host, quint16 &port) const;

    // ---- CoreService 信号 ----
    void onBusyChanged(bool busy);
    void onAssignedIdChanged(const QString &id);
    void onPeerListReceived(const QList<relayer::app::PeerRow> &rows);
    void onOperationFinished(bool ok, const QString &summary, const QString &detail);

    Ui::ConnectorControl *ui_;
    relayer::app::CoreService *core_;
    QString currentPeerId_;
};
