#pragma once

#include <QDialog>
#include <QList>
#include <QStringList>
#include <QVector>
#include <functional>

#include "OwnTableWidget.h"
#include "bridge/CoreService.h"

struct InfoDrop;

QT_BEGIN_NAMESPACE
namespace Ui {
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
//   1. 远端的写操作（改名 / 删除 / 新建目录 / 算 SHA-256 / 解压）走
//      PROTOCOL.md §11.12，经 MainWindow 转给 CoreService：本控件不碰网络，
//      也不判断路径合法（那只有对端知道）。与本地那套的区别是"谁执行"，
//      不是"能不能做"。
//   2. 右键菜单**只有一套**（onTableContextMenu）：每个动作声明 local /
//      remote 两个执行体，为空的表示这一端不支持、自动不显示——目前只有
//      「在资源管理器中打开」和「压缩为 tar.xz」是本地独有。这样"远端少了
//      哪项"由能力决定，而不是由两段独立的代码决定。
//   3. 远端的拖拽载荷是**对端绝对路径**而不是本地绝对路径，所以拖出时多打一个
//      标记 MIME，对照表的本地目录列据此拒收（见 InfoDrop.h）。
//   4. 远端路径是**对端绝对路径**（可能是另一个系统的路径规则），显示成
//      `peer:/rel`，且不可手输。
//
// 与 v1 的另一处差异：不用 WorkerThread/BaseAskDF/WaitDialog。本地列举同步
// 完成（本地盘足够快）；远端本身就是异步的，等信号即可，不需要等待窗口。
class ExplorerControl : public QDialog
{
    Q_OBJECT

public:
    // 数据源。与 v1 的 AskType 等价，只是 v2 只有两个。
    enum class Source {
        Local,
        Remote,
    };

    explicit ExplorerControl(QWidget* parent = nullptr);

    // 切到远端：browse 该对端的某个绝对路径。同一个控件，UI 不变（v1 语义）。
    //
    // relPath 非空时直接进那一层（对照表"访问远程目录"：那一行说的就是
    // peer:relDir，跳过去才看得出"到底传到了对方哪个目录"）。不能先
    // setRemotePeer 再 navigateTo——两次列举请求会撞在 Client 的
    // browseActive 上，第二个被拒。
    void setRemotePeer(const QString& peerId, const QString& peerName, const QString& relPath = QString());

    // 切回本地浏览。
    void useLocalSource();

    [[nodiscard]] Source source() const
    {
        return source_;
    }

    // 远端模式下当前浏览的对端 id（本地面板为空），以及当前所在路径
    // （两侧都是绝对路径，只是分属不同机器）。校验用它俩发列举请求。
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
    // rootListing = 这次是根列表（条目名是完整路径，双击时直接用）。
    void applyRemoteEntries(bool ok, bool rootListing, const QString& summary, const QList<relayer::app::RemoteRow>& rows);

    // 当前浏览的目录（空 = 尚未进入任何目录）。
    QString currentPath() const
    {
        return currentPath_;
    }

    // 拖出打包 InfoDrop 时报根目录；由 MainWindow 接拖入目标。
    ExpDropTable* table() const
    {
        return table_;
    }

    // 从**另一张浏览表**拖入时的回调（v1 的 peerDf 形状：一套代码两个方向）。
    //
    // 本控件只把载荷交出去：它不知道自己是左边还是右边、对面是谁、往哪落。
    // 方向由"谁收到了"决定——本地表收到 = 对方是远端 = 下载；远端表收到 =
    // 上传。这个判断只有 MainWindow 做得出来（它同时持有两个面板）。
    void setOnInfoDropped(std::function<void(const InfoDrop&)> onDropped)
    {
        table_->setOnInfoDropped(std::move(onDropped));
    }

    // 外部导航入口（配置表"访问本地目录"等）。进入失败经 notice 上报。
    void navigateTo(const QString& path)
    {
        enterPath(path);
    }

signals:
    // 目录浏览失败的提示（进入失败、删除失败等）。MainWindow 转发到日志。
    void notice(const QString& msg);

    // 远端模式下的列举请求（absPath 是对端的绝对路径，空 = 根列表）。
    // 本控件不碰网络：只发请求，结果由 applyRemoteEntries 送回。
    void remoteListRequested(const QString& peerId, const QString& relPath);

    // 远端模式下"下载到接收目录"（PROTOCOL.md §11.9）。本控件不碰网络：
    // 只把"从谁取、取哪个相对路径"交给 MainWindow。
    // asArchive=true 表示选中的是目录，请对端打包（PROTOCOL.md §11.10）。
    void remotePullRequested(const QString& peerId, const QString& relPath, bool asArchive);

    // 本地模式下右键「传输」：把选中项的**本地绝对路径**交给 MainWindow。
    // 本控件不碰网络与任务表；路径怎么变成任务项由 MainWindow 决定
    // （目录要递归展开、文件用"父目录 + 文件名"两种形状）。
    void localTransferRequested(const QStringList& paths);

    // 远端文件操作（PROTOCOL.md §11.12）：算 SHA-256 / 改名 / 新建目录 /
    // 删除 / 解压。本控件只把"对谁、对哪个路径、做什么"交出去——它不碰
    // 网络，也不知道自己是不是某个会话的一部分。
    //
    // kind："sha256" / "rename" / "mkdir" / "delete" / "unpack"。
    // newName 只有 rename 用；paths 只有 delete 用（可能是多项）。
    void remoteOpRequested(const QString& kind, const QString& peerId, const QString& absPath, const QString& newName,
                           const QStringList& paths);

    // 粗校验 / 时间戳校验（v1 的语义：与**另一个面板当前目录下的同名文件**
    // 比）。本控件不知道自己对面是谁——由 MainWindow 取另一侧路径并决定
    // 哪一边是远端。wantTime=false 比内容（采样指纹），true 比修改时间。
    //
    // 两端都能发起：结论与方向无关（都是"本地目录/name vs 远端目录/name"），
    // 所以远端菜单里这两项走的是同一个信号、同一段逻辑。
    void compareRequested(const QString& name, bool wantTime);

private:
    struct FileEntry {
        QString name;
        bool isDir = false;
        qint64 size = 0;         // 字节；目录无意义
        qint64 modifiedMs = 0;   // 最后修改时间（毫秒）
    };

    void initControl();
    void initSignals();

    // 列举 dir 下的条目到 entries；失败返回 false 并发 notice。
    bool listDir(const QString& dir, QVector<FileEntry>& entries) const;
    void enterPath(const QString& path);
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
    void setFileItem(const FileEntry& entry, int row);
    void onTableContextMenu(const QPoint& pos);

    // 右键动作（局部操作，成功后重列当前目录）。
    //
    // 没有 onOpenInExplorer / onCopyFullPath：v1 的"在资源管理器中打开当前
    // 目录"就是 v2 的「打开」（目录项 openUrl(dir)，文件项 openUrl(currentPath_)），
    // "复制全路径"是动作表里的一行内联代码。曾经在这里留过两个空声明，
    // 永远不会实现也永远不会报错——删掉。
    void onRename(int row);
    void onDelete(const QVector<int>& rows);
    void onNewDir();

    static bool lessThan(const FileEntry& a, const FileEntry& b, int column);

    // 数据源状态。remotePendingPath_ 记"这次请求要去的路径"——远端是异步的，
    // 回来之前 currentPath_ 不能改，否则中途失败会把界面停在一个没进去的目录上。
    Source source_ = Source::Local;
    QString peerId_;
    QString peerName_;
    QString remotePendingPath_;

    // 远端模式下把 relPath 显示成 `peer:/rel`（用户要能一眼看出这是远端）。
    [[nodiscard]] QString displayPath() const;

    // 当前这张远端表是不是"根列表"（盘符 + 家目录）。根列表的条目名是
    // 完整路径，双击/取回时不能当单段名去拼。
    bool remoteRootListing_ = false;

    // 把路径写进 cbPath。**不能用 setCurrentText**：远端模式 cbPath 不可编辑，
    // 非可编辑的 QComboBox 只会在已有项里查找，找不到就什么都不做——这正是
    // "点了浏览但控件毫无变化"的根因（路径框停在旧值，表格本来就是空的）。
    void showPath(const QString& text);

    // 当前选中的本地绝对路径（去重、保持表格顺序）。远端模式返回空。
    [[nodiscard]] QStringList selectedLocalPaths() const;

    // 当前所在路径。本地是本机绝对路径，远端是**对端**的绝对路径——由调用方
    // （MainWindow）按 source() 判断是哪一种。
    // 状态行。每一次列举都必须留下可见结果（读取中 / 共 N 项 / 失败原因），
    // 否则失败与"没反应"在界面上无法区分。
    void setStatus(const QString& text, bool error = false);

    // 按名字回源数据里找条目。
    // 表格里的大小/时间是**格式化后**的字符串（"1.2 KiB"、"2026-10-23 ..."），
    // 反解析既不可靠也多此一举——名字在同一个目录里唯一，直接回源。
    [[nodiscard]] const FileEntry* findEntry(const QString& name) const;

    // 右键「详细信息」：本地/远端共用同一个对话框（v1 也是同一个 Form）。
    void showMetaInfo(int row);

    // 后台跑一个本地重活（递归删除、压缩、解压、算 SHA-256）。
    //
    // v1 所有这些都走 WorkerThread + WaitDialog；v2 早先是 GUI 线程里同步跑，
    // 删一个几万文件的目录、解一个 2GB 的包、算一个 4GB 文件的 SHA-256 都会
    // 把界面冻住几十秒，用户唯一能做的就是等或杀进程。这里补回后台执行。
    //
    // 契约：
    //   - job 在**工作线程**执行，只能碰路径与标准库，不许碰任何 QWidget /
    //     本对象成员（要用的数据一律按值捕获）；
    //   - done 回到 GUI 线程执行，第二个参数是失败原因（成功时为空）；
    //   - 同一时刻只允许一个后台操作，重复请求被拒绝并在状态行说明。
    void runAsync(const QString& statusText, std::function<bool(QString&)> job, std::function<void(bool, const QString&)> done);
    [[nodiscard]] bool busy() const
    {
        return busy_;
    }

    Ui::ExplorerControl* ui_ = nullptr;
    ExpDropTable* table_ = nullptr;   // 表格本体由 initControl 动态创建，塞进 .ui 的占位 widget

    // 源数据与视图状态。
    QVector<FileEntry> entries_;
    QString currentPath_;
    int sortColumn_ = static_cast<int>(ExpColumn::Name);
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    QString filterName_;
    QStringList filterExts_;   // 小写后缀；含"(无后缀)"特殊标签；空 = 不筛选

    // 有后台本地操作在跑（见 runAsync）。
    bool busy_ = false;
};
