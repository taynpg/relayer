#pragma once

#include <QDialog>

namespace Ui
{
class ReplaceArea;
}

// 替换作用域（ReplaceArea 对话框的结论，v1 ReplcaeResult 的 v2 版）。
//
// 与 v1 的差异：
//   - 列勾选从「名称/本地/远端」改为 v2 行模型的四个可编辑字段
//     （名称/本地目录/对端/相对目录——v2 没有"远端目录"列）。
//   - v1 的 useCase/useRegex 是假选项（结果传进了替换 lambda 但从未
//     使用，实际恒为区分大小写的普通替换）；v2 把两个开关都做成真的。
struct ReplaceScope
{
    bool apply = false;         // true = 用户点了「确认」
    bool name = true;           // 替换「名称」列
    bool localDir = true;       // 替换「本地目录」列
    bool peer = true;           // 替换「对端」列
    bool relDir = true;         // 替换「相对目录」列
    bool caseSensitive = true;  // 区分大小写
    bool useRegex = false;      // 正则替换（from 是 pattern，to 是替换串）
};

// 替换区域对话框（对照 v1 Gui/Form/ReplaceArea 重写）。exec 后用
// result() 取结论；取消时 result().apply == false。
class ReplaceArea : public QDialog
{
    Q_OBJECT

public:
    explicit ReplaceArea(QWidget *parent = nullptr);
    ~ReplaceArea() override;

    ReplaceArea(const ReplaceArea &) = delete;
    ReplaceArea &operator=(const ReplaceArea &) = delete;

    [[nodiscard]] ReplaceScope result() const;

private:
    Ui::ReplaceArea *ui_;
    ReplaceScope result_;
};
