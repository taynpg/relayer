#pragma once

#include <string>
#include <string_view>

// 路径规则。
//
// 单独抽出来的理由：
//
//   1. 路径折叠、拼接、分隔符是**纯字符串逻辑**，不需要访问磁盘，
//      也就没有"实现差异"——各个 FileStore 实现共用同一份即可，
//      没必要在每个实现里复制一遍。
//
//   2. 但"什么算绝对路径"是平台相关的（Windows 认 `C:/x` 和 `\\server\share`，
//      POSIX 只认 `/x`）。把这个平台差异收敛到 PathPolicy 的构造上，
//      而不是散落在各个 FileStore 里。
//
//   3. 路径相关的 bug（分隔符混用、`..` 没折叠、前缀比较没边界）在 v1 里
//      是最难排查的一类，集中到一处才好写测试、才好改。
//
// 所有路径参数与返回值都是 UTF-8 编码的 std::string，与 IFileStore 一致。

namespace relayer::filestore
{

// 路径按哪种规则解释。
//
// 默认取当前平台的规则；测试里可以显式指定，用来验证"在 Linux 上处理
// Windows 风格路径"这类跨平台逻辑。
enum class PathStyle
{
    kNative,  // 当前平台（Windows → kWindows，其它 → kPosix）
    kPosix,   // 只用 `/`，绝对路径以 `/` 开头
    kWindows  // `/` 与 `\` 都算分隔符；`C:/`、`C:\`、`\\server\share` 算绝对
};

class PathPolicy
{
public:
    explicit PathPolicy(PathStyle style = PathStyle::kNative);

    [[nodiscard]] PathStyle style() const noexcept
    {
        return style_;
    }

    // 统一分隔符为 `/`、折叠 `.` 与 `..`、去掉多余与末尾分隔符。
    // 不访问磁盘，对不存在的路径同样有效。
    // 空输入返回空串；根目录（`/`、`C:/`）保持原样。
    [[nodiscard]] std::string normalize(std::string_view path) const;

    // 拼路径。a 为空返回 b，b 为空返回 a，a 已带分隔符时不重复插入。
    // 注意：不折叠 `..`，需要折叠请再调 normalize。
    [[nodiscard]] std::string join(std::string_view a, std::string_view b) const;

    // 父目录。根目录的父目录仍返回根，返回空串会让调用方拼出相对路径。
    [[nodiscard]] std::string parent(std::string_view path) const;

    // 最后一段。以分隔符结尾时返回最后一段非空部分（`a/b/` → `b`）。
    [[nodiscard]] std::string fileName(std::string_view path) const;

    [[nodiscard]] bool isAbsolute(std::string_view path) const;

    // 把 child 解释为相对 root 的路径，并保证结果落在 root 之内。
    //
    // 这是 zip-slip / tar 路径穿越的唯一防线，所以单独成方法而不是让调用方
    // 自己 startsWith。返回空串表示"该条目必须被拒绝"。
    //
    // 拒绝的情形：child 是绝对路径、归一化后逃出 root、归一化后等于 root
    // 本身（一个条目不该指向根目录）、包含盘符切换（`C:` 出现在中间）。
    [[nodiscard]] std::string resolveWithin(std::string_view root, std::string_view child) const;

private:
    [[nodiscard]] bool isSeparator(char c) const noexcept;

    PathStyle style_;
};

}  // namespace relayer::filestore
