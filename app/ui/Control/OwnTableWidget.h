#pragma once

#include <QPoint>
#include <QTableWidget>
#include <functional>

struct InfoDrop;

// ComparisonControl 动作表的列语义。v1 里列号 0-5 以裸数字散落在拖拽、
// 补列、读写各处（清单 #2 的病灶）；v2 全部收敛到这个 enum，落点判定用
// isPathColumn()，可编辑性用 isReadOnlyColumn()，不再逐处传布尔。
//
// v2 的行语义是「把本地目录里的 name 发给 peer，落到对方接收根目录下的
// relDir」——对应用户认可的核心场景"频繁把固定文件发到对方某个目录"。
// v1 的「本地目录 ↔ 远程目录」映射需要远端目录浏览协议，属阶段 F。
enum class ComColumn
{
    Id = 0,        // 行标识（保存配置用，用户不可见地维护）
    Name = 1,      // 文件/目录名
    Type = 2,      // "Dir" / "File"
    Mark = 3,      // 后缀标记（目录为 "Dir"）
    LocalDir = 4,  // 本地根目录（拖拽目标）
    Peer = 5,      // 目标对端 id（发送用）
    RelDir = 6,    // 对方接收根目录之下的相对目录（可空 = 落根）
};

template <typename E> constexpr int toInt(E e)
{
    return static_cast<int>(e);
}

// ExplorerControl 文件浏览表的列（与 initControl 的表头一一对应）。
// v1 的 `items.size() / 5` 与 `items[i * 5 + 1]` 把列数写死在算术里
// （清单 #4/#7 的同源问题）；v2 用这里的常量与 selectedRows()。
enum class ExpColumn
{
    Icon = 0,
    Name = 1,
    Modified = 2,
    Type = 3,  // "Dir" / "File"
    Size = 4,
};

// ComparisonControl 的拖拽表：接受 InfoDrop 拖入本地目录列，
// 逐行落格子。可编辑性由 isReadOnlyColumn 统一定义（清单 #1）。
class ComDropTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit ComDropTable(QWidget *parent = nullptr);

    // 拖拽的合法落点：只有两个路径列。
    static bool isPathColumn(int col);
    // 只读列：Id / Type / Mark（用户手填没有意义，由拖拽与程序维护）。
    static bool isReadOnlyColumn(int col);

    // 清单 #1：v1 的 setItemData 一肩挑三件事且"建时设只读、改时不设"，
    // 拆成三个正交操作：
    QTableWidgetItem *ensureItem(int row, int col);       // 没有就建（可编辑性按列配置）
    void setText(int row, int col, const QString &text);  // 只改已有 item 的文本
    void setReadOnly(int row, int col, bool readOnly);
    // 清单 #3：一行内逐列补齐缺失 item（新行、或旧配置缺列）。
    // 补齐避免保存/读取时 item(row, n) 取到空指针闪退。
    void ensureRowComplete(int row);

protected:
    void dropEvent(QDropEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;

private:
    QString markFor(const QString &fileName, bool isDir) const;
};

// ExplorerControl 的文件浏览表：自身可拖出（打包成 InfoDrop），拖入时
// 交给所属 ExplorerControl 发起跨端传输；自拖（同一张表内）被忽略。
class ExpDropTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit ExpDropTable(QWidget *parent = nullptr);

    // 拖出的 InfoDrop::from 取值（本表浏览的是哪个根目录）。
    void setGetOwnRoot(std::function<QString()> getOwnRoot);
    // 从另一张浏览表（本地→远端或反向）放下时的回调。
    void setOnInfoDropped(std::function<void(const InfoDrop &)> onDropped);

    // 本表浏览的是远端（相对共享根）。只影响拖出：载荷里额外打一个标记，
    // 让对照表的本地目录列**拒收**——远端相对路径不是本地路径，落进去
    // 会把"发到哪"写成一句谁都不认的路径。
    //
    // 不往 InfoDrop 里加字段：那个序列化格式是与 v1 的契约（InfoDrop.h），
    // 加字段会让旧数据读不动。标记走独立的 MIME，载荷本身一字不改。
    void setRemoteSource(bool remote);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;

private:
    std::function<QString()> getOwnRoot_;
    std::function<void(const InfoDrop &)> onInfoDropped_;
    QPoint dragStart_;
    bool remoteSource_ = false;
};
