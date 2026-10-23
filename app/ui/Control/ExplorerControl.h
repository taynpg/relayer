#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>
#include <QVector>

#include "OwnTableWidget.h"
#include "bridge/CoreService.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class ExplorerControl;
}
QT_END_NAMESPACE

class QComboBox;

// 文件浏览面板——**一套 UI，两种数据源**（v1 ExplorerControl 的形状）。
//
// v1 用 `askType_` + `BaseAskDF::Create(AskType)` 区分本地/远端，UI 与交互
// 完全共用（连跨面板拖拽都是同一套代码，见 v1 ExplorerControl.cpp 的 peerDf）。
// v2 沿用这个结构，只把"数据源"换成同步/异步两条路：
//
//   - Local ：直接读盘（QDir），同步返回；
//   - Remote：发 kAskFileList（PROTOCOL.md §11.8），结果经
//             applyRemoteEntries() 异步送回。
//
// 两条路共用同一份表格、排序、筛选、导航逻辑——这正是"只换子类、不换 UI"
// 的意义。差异只在三处，都有明确理由：
//   1. 远端是**只读**：重命名/删除/新建文件夹都是本地文件系统操作，远端没有
//      对应帧（要做得先有协议）；远端模式不显示这些动作。
//   2. 远端不拖出：拖拽载荷是"本地路径"，远端条目没有本地路径。要支持得
//      先做下载，那是另一件事。
//   3. 远端路径是**相对共享根**（可能是另一个系统的路径规则），显示成
//      `peer:/rel`，且不可手输。
//
// 与 v1 的另一处差异：不用 WorkerThread/BaseAskDF/WaitDialog。本地列举同步
// 完成（本地盘足够快）；远端本身就是异步的，等信号即可，不需要等待窗口。
class ExplorerControl : public QDialog
{
    Q_OBJECT

public:
    // 数据源。与 v1 的 AskType 等价，只是 v2 只有两个。
    enum class Source
    {
        Local,
        Remote,
    };

    explicit ExplorerControl(QWidget *parent = nullptr);

    // 切到远端：browse 该对端的共享根。同一个控件，UI 不变（v1 语义）。
    void setRemotePeer(const QString &peerId, const QString &peerName);

    // 切回本地浏览。
    void useLocalSource();

    [[nodiscard]] Source source() const
    {
        return source_;
    }

    // 远端模式下当前浏览的对端 id（本地面板为空），以及当前所在路径
    // （本地为绝对路径、远端为相对共享根）。校验用它俩发列举请求。
    [[nodiscard]] QString remotePeerId() const
    {
        return peerId_;
    }
    [[nodiscard]] QString currentPathForCompare() const
    {
        return currentPath_;
    }

    // 远端列举的结果（MainWindow 从 CoreService 送回来）。
    // ok=false 时 summary 说明原因（未开共享 / 目录不存在 / 会话不可用）。
    void applyRemoteEntries(bool ok, const QString &summary, const QList<relayer::app::RemoteRow> &rows);

    // 当前浏览的目录（空 = 尚未进入任何目录）。
    QString currentPath() const
    {
        return currentPath_;
    }

    // 拖出打包 InfoDrop 时报根目录；由 MainWindow 接拖入目标。
    ExpDropTable *table() const
    {
        return table_;
    }

    // 外部导航入口（配置表"访问本地目录"等）。进入失败经 notice 上报。
    void navigateTo(const QString &path)
    {
        enterPath(path);
    }

signals:
    // 目录浏览失败的提示（进入失败、删除失败等）。MainWindow 转发到日志。
    void notice(const QString &msg);

    // 远端模式下的列举请求（relPath 相对对端共享根，空 = 根）。
    // 本控件不碰网络：只发请求，结果由 applyRemoteEntries 送回。
    void remoteListRequested(const QString &peerId, const QString &relPath);

    // 远端模式下"下载到接收目录"（PROTOCOL.md §11.9）。本控件不碰网络：
    // 只把"从谁取、取哪个相对路径"交给 MainWindow。
    // asArchive=true 表示选中的是目录，请对端打包（PROTOCOL.md §11.10）。
    void remotePullRequested(const QString &peerId, const QString &relPath, bool asArchive);

    // 本地模式下右键「传输」：把选中项的**本地绝对路径**交给 MainWindow。
    // 本控件不碰网络与任务表；路径怎么变成任务项由 MainWindow 决定
    // （目录要递归展开、文件用"父目录 + 文件名"两种形状）。
    void localTransferRequested(const QStringList &paths);

    // 粗校验 / 时间戳校验（v1 的语义：与**另一个面板当前目录下的同名文件**
    // 比）。本控件不知道自己对面是谁——由 MainWindow 取另一侧路径并决定
    // 哪一边是远端。wantTime=false 比大小，true 比修改时间。
    void compareRequested(const QString &name, bool wantTime);

private:
    struct FileEntry
    {
        QString name;
        bool isDir = false;
        qint64 size = 0;        // 字节；目录无意义
        qint64 modifiedMs = 0;  // 最后修改时间（毫秒）
    };

    void initControl();
    void initSignals();

    // 列举 dir 下的条目到 entries；失败返回 false 并发 notice。
    bool listDir(const QString &dir, QVector<FileEntry> &entries) const;
    void enterPath(const QString &path);
    void onEnter();
    void onHome();
    void onUp();
    void onDoubleClick(int row);
    void onHeaderClicked(int index);
    void onFilterChanged();
    void onToggleFilter(bool checked);
    void onResetFilter();
    void updateExtOptions();
    void rebuildView();
    void setFileItem(const FileEntry &entry, int row);
    void onTableContextMenu(const QPoint &pos);

    // 右键动作（局部操作，成功后重列当前目录）。
    void onOpenInExplorer();
    void onCopyFullPath();
    void onRename(int row);
    void onDelete(const QVector<int> &rows);
    void onNewDir();

    static bool lessThan(const FileEntry &a, const FileEntry &b, int column);

    // 数据源状态。remotePendingPath_ 记"这次请求要去的路径"——远端是异步的，
    // 回来之前 currentPath_ 不能改，否则中途失败会把界面停在一个没进去的目录上。
    Source source_ = Source::Local;
    QString peerId_;
    QString peerName_;
    QString remotePendingPath_;

    // 远端模式下把 relPath 显示成 `peer:/rel`（用户要能一眼看出这是远端）。
    [[nodiscard]] QString displayPath() const;

    // 把路径写进 cbPath。**不能用 setCurrentText**：远端模式 cbPath 不可编辑，
    // 非可编辑的 QComboBox 只会在已有项里查找，找不到就什么都不做——这正是
    // "点了浏览但控件毫无变化"的根因（路径框停在旧值，表格本来就是空的）。
    void showPath(const QString &text);

    // 当前选中的本地绝对路径（去重、保持表格顺序）。远端模式返回空。
    [[nodiscard]] QStringList selectedLocalPaths() const;

    // 当前所在路径。本地是绝对路径，远端是相对共享根——由调用方
    // （MainWindow）按 source() 判断是哪一种。
    // 状态行。每一次列举都必须留下可见结果（读取中 / 共 N 项 / 失败原因），
    // 否则失败与"没反应"在界面上无法区分。
    void setStatus(const QString &text, bool error = false);

    Ui::ExplorerControl *ui_ = nullptr;
    ExpDropTable *table_ = nullptr;  // 表格本体由 initControl 动态创建，塞进 .ui 的占位 widget

    // 源数据与视图状态。
    QVector<FileEntry> entries_;
    QString currentPath_;
    int sortColumn_ = static_cast<int>(ExpColumn::Name);
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    QString filterName_;
    QStringList filterExts_;  // 小写后缀；含"(无后缀)"特殊标签；空 = 不筛选
};
