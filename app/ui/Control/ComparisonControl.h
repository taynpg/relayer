#pragma once

#include <QSet>
#include <QWidget>
#include <vector>

#include "OwnTableWidget.h"
#include "model/TransferTask.h"

class QComboBox;

namespace Ui
{
class ComparisonControl;
}

// 传输配置表（v1 ComparisonControl 的 v2 重写，REWRITE-PLAN §3.7.2 #4-#6/#8/#9）。
//
// 行语义：「把 localDir 下的 name 发给 peer，落到对方接收根目录下的 relDir」。
// 与 v1 的差异：
//   - 「远程目录」列改为「对端 id + 相对目录」两列（用户认可的核心场景：
//     频繁把固定文件发到对方某个目录）。远端绝对路径语义需要远端目录浏览
//     协议支持，属阶段 F。
//   - 持久化用单个 JSON 文件（AppDataLocation/comparison.json）替代 v1 的
//     Sqlite——每配置一张表、ensureSchema 补列这些是结构性包袱，行数据
//     规模（几百行内）用 JSON 足够。
//   - 编辑直接同步进内存模型（v1 的表格是唯一真身、筛选重排会丢编辑，
//     v2 用 rows_ + 视图投影，清单 #8 的"行号不再是状态"）。
//   - 下载落到**每行自己的本地目录**（v1 语义），不扔进全局接收目录：
//     行说"name 属于 (localDir, peer, relDir)"，上下行因此对称。
class ComparisonControl : public QWidget
{
    Q_OBJECT

public:
    explicit ComparisonControl(QWidget *parent = nullptr);

signals:
    // 用户点了"发送到对端"。items 已过基本校验（名称/本地目录/对端非空），
    // 目录行的展开由消费者负责（E3.5 RelayTask / 暂由 MainWindow 顺序执行）。
    void transTaskRun(const relayer::app::RelayTaskItems &items);

    // 本表状态的一句话描述（保存/加载结果等），交给日志面板。
    void logMessage(const QString &msg);

    // 右键"访问本地目录"：让浏览面板跳到该行的本地目录。
    void exploreLocalRequested(const QString &path);

    // 右键"检查远端"：问对端"这些行要发的东西对方是不是已经有了"。
    // 本控件不碰网络（只做显示与转发），实际请求由 MainWindow 逐行发起。
    void remoteCheckRequested(const relayer::app::RelayTaskItems &items);

    // 右键"下载到本地"：把每行在**对端 relDir 下的 name** 取回，落到该行的
    // 本地目录。是"发送到对端"的逆操作，行语义因此是对称的：
    //   name 属于 (localDir, peer, relDir)，发送是 localDir → peer:relDir，
    //   下载是 peer:relDir → localDir。
    // 同样由 MainWindow 逐行发起（一次只能有一个取回请求在飞）。
    void downloadRequested(const relayer::app::RelayTaskItems &items);

private:
    // ---- 内存模型 ----
    struct Row
    {
        int id = -1;  // -1 = 尚未持久化；JSON 方案下仅用于会话内去重
        QString name;
        QString type;  // "Dir" / "File"（由 mark 推导）
        QString mark;
        QString localDir;
        QString peer;
        QString relDir;
    };

    void initTable();
    void initSignals();
    void initConfigs();

    // 视图 = rows_ 按 mark 筛选的投影。viewToRow_[表格行] → rows_ 下标。
    void rebuildView();
    void refreshMarkList();
    void syncRowFromItem(QTableWidgetItem *item);

    int appendRow(const Row &row);
    bool isMarkVisible(const QString &mark) const;

    // ---- 配置持久化 ----
    void saveConfig();
    void loadConfig(const QString &name);
    void deleteConfig();
    void newConfig();
    void copyConfig();
    void writeStore();
    [[nodiscard]] QString storePath() const;

    // ---- 表格动作 ----
    void onTableContextMenu(const QPoint &pos);
    void onListContextMenu(const QPoint &pos);
    void onListItemToggled();
    void onReplace();
    void onTrans();

    [[nodiscard]] QList<QTableWidgetItem *> selectedRowItems() const;

    // 把选中行读成任务项（onTrans 与"检查远端"共用同一份读取逻辑）。
    [[nodiscard]] relayer::app::RelayTaskItems selectedItems() const;

    Ui::ComparisonControl *ui_ = nullptr;
    ComDropTable *table_ = nullptr;

    std::vector<Row> rows_;       // 内存模型（当前配置的全部行）
    std::vector<int> viewToRow_;  // 表格行 → rows_ 下标
    QSet<QString> visibleMarks_;  // 空集 = 全部可见
    bool updating_ = false;       // 程序性 setText 时抑制 itemChanged 回写

    QString currentConfig_;  // 空 = 尚未选择配置
};
