#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

#include "OwnTableWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class ExplorerControl;
}
QT_END_NAMESPACE

class QComboBox;

// 本地文件浏览面板（v1 ExplorerControl 的 v2 版）。
//
// **范围决定**（E3.3）：只做本地目录浏览。v1 的"远端面板"靠其私有协议的
// 目录列举能力，v2 协议当前没有对应帧（PROTOCOL.md 26 帧里没有 list-dir），
// 远端浏览属阶段 F / 协议扩展。因此：
//   - 保留：导航（访问/家目录/上一级/双击进入）、表头排序（目录恒置顶）、
//     名称模糊筛选 + 后缀多选筛选（清单同 v1）、拖出打包 InfoDrop、
//     右键菜单（动作表写法，清单 #5）：打开/复制全路径/重命名/删除/新建文件夹
//   - 未做（如实标注，不是已完成）：SHA256/粗校验/时间戳校验/压缩/解压/
//     详细信息/远端面板——依赖协议扩展或 RelayTask，后续增量补
//
// 与 v1 的结构差异：
//   - 不用 WorkerThread/BaseAskDF/WaitDialog：本地目录列举同步完成（本地盘
//     足够快；网络盘慢的场景等阶段 F 换 QtConcurrent，不预造）
//   - 不用 v1 协议的 FileMeta：本地浏览是 GUI 的事，用自己的轻量 FileEntry
//   - 表格行 = 源数据的投影：currentEntries_ 保存源数据，表格按筛选/排序
//     条件重建（rebuildView），行号不作为持久状态
class ExplorerControl : public QDialog
{
    Q_OBJECT

public:
    explicit ExplorerControl(QWidget *parent = nullptr);

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
