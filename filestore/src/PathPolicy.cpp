#include "relayer/filestore/PathPolicy.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace relayer::filestore {
namespace {

constexpr PathStyle kHostStyle =
#ifdef _WIN32
    PathStyle::kWindows;
#else
    PathStyle::kPosix;
#endif

// 把一个路径拆成 {前缀, 段列表}。
//
// 前缀是"根"的部分：POSIX 下是 "/"（可能有多个前导斜杠，折叠成一个），
// Windows 下是 "C:/"、"//server/share/" 或 ""（相对路径）。
//
// 拆段的规则：连续的 `..` 遇到非 `..` 段就抵消；遇到前缀就不能再往上退。
// 这与 std::filesystem::lexically_normal 的语义一致，但这里有三个好处：
//   1. 不依赖 <filesystem>，Windows 上不需要宽字符转换来回折腾；
//   2. 可以显式拒绝"逃出根"的情形（resolveWithin 需要这个能力）；
//   3. 行为完全可控，能写测试钉死。
struct SplitPath {
    std::string prefix;               // 含结尾分隔符（如果非空）
    std::vector<std::string> parts;   // 已折叠 . 与 ..

    [[nodiscard]] bool rooted() const noexcept
    {
        return !prefix.empty();
    }
};

SplitPath splitPath(std::string_view path, bool windowsSeps)
{
    const auto isSep = [windowsSeps](char c) { return c == '/' || (windowsSeps && c == '\\'); };

    SplitPath out;

    // ---- 前缀 ----
    std::size_t i = 0;
    const bool thirdIsSep = path.size() > 2 && isSep(path[2]);
    if (windowsSeps && path.size() >= 2 && isSep(path[0]) && isSep(path[1]) && !thirdIsSep) {
        // UNC：\\server\share\...
        // 前两段是主机与共享名，属于前缀，不算可折叠的普通段。
        //
        // `thirdIsSep` 这个条件不能省：`//a` 在 POSIX 语义下是"根目录"
        // （多余斜杠折叠），在 Windows 语义下也不构成合法 UNC（缺共享名）。
        // 不判的话会拼出 `///a`。
        std::size_t p = 2;
        int taken = 0;
        while (p < path.size() && taken < 2) {
            while (p < path.size() && !isSep(path[p])) {
                ++p;
            }
            ++taken;
            while (p < path.size() && isSep(path[p])) {
                ++p;
            }
        }
        out.prefix = "//";
        out.prefix.append(std::string(path.substr(2, p - 2)));
        for (auto& ch : out.prefix) {
            if (ch == '\\') {
                ch = '/';
            }
        }
        i = p;
    } else if (windowsSeps && path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':') {
        // 盘符：C:/ 或 C:\ 或 C:（C: 后面不带分隔符表示"该盘的当前目录"，
        // 语义含糊，这里按根处理，与 std::filesystem 一致）
        out.prefix.assign(1, static_cast<char>(std::toupper(static_cast<unsigned char>(path[0]))));
        out.prefix += ":/";
        i = 2;
        while (i < path.size() && isSep(path[i])) {
            ++i;
        }
    } else if (!path.empty() && isSep(path[0])) {
        // POSIX 绝对路径，或 Windows 下以单个分隔符开头的"当前盘根"路径
        out.prefix = "/";
        i = 1;
        while (i < path.size() && isSep(path[i])) {
            ++i;
        }
    }

    // ---- 段 ----
    auto apply = [&out](std::string_view seg) {
        if (seg.empty() || seg == ".") {
            return;
        }
        if (seg == "..") {
            if (!out.parts.empty() && out.parts.back() != "..") {
                out.parts.pop_back();
            } else if (!out.rooted()) {
                // 相对路径里的 .. 逃到根以外：保留，让 normalize 输出 ".."
                out.parts.emplace_back("..");
            }
            // 有了前缀还往上退 → 停在前缀，不产生 ".." 段
            return;
        }
        out.parts.emplace_back(seg);
    };

    std::string seg;
    for (; i < path.size(); ++i) {
        if (isSep(path[i])) {
            apply(seg);
            seg.clear();
        } else {
            seg.push_back(path[i]);
        }
    }
    apply(seg);

    return out;
}

std::string joinSplit(const SplitPath& sp)
{
    std::string out = sp.prefix;
    for (std::size_t k = 0; k < sp.parts.size(); ++k) {
        if (k != 0 || (!out.empty() && out.back() != '/')) {
            out.push_back('/');
        }
        out.append(sp.parts[k]);
    }
    return out;
}

}   // namespace

PathPolicy::PathPolicy(PathStyle style) : style_(style == PathStyle::kNative ? kHostStyle : style)
{
}

bool PathPolicy::isSeparator(char c) const noexcept
{
    return c == '/' || (style_ == PathStyle::kWindows && c == '\\');
}

std::string PathPolicy::normalize(std::string_view path) const
{
    if (path.empty()) {
        return {};
    }
    const SplitPath sp = splitPath(path, style_ == PathStyle::kWindows);
    std::string out = joinSplit(sp);
    // 相对路径归一化后若只剩空（例如 "." 或 "./"），返回 "."，
    // 与 std::filesystem 一致：表示"当前目录"而不是"什么都没有"。
    if (out.empty()) {
        return ".";
    }
    return out;
}

std::string PathPolicy::join(std::string_view a, std::string_view b) const
{
    if (a.empty()) {
        return std::string(b);
    }
    if (b.empty()) {
        return std::string(a);
    }
    std::string out(a);
    if (!isSeparator(out.back())) {
        out.push_back('/');
    }
    // b 以分隔符开头时不要拼出双分隔符（`a/` + `/b` → `a/b`）
    std::size_t skip = 0;
    while (skip < b.size() && isSeparator(b[skip])) {
        ++skip;
    }
    out.append(b.substr(skip));
    return out;
}

std::string PathPolicy::parent(std::string_view path) const
{
    if (path.empty()) {
        return {};
    }
    const SplitPath sp = splitPath(path, style_ == PathStyle::kWindows);
    if (sp.parts.empty()) {
        // 已经是根：返回根自身
        return sp.rooted() ? sp.prefix : std::string(path);
    }
    SplitPath up = sp;
    up.parts.pop_back();
    std::string out = joinSplit(up);
    if (out.empty()) {
        return ".";
    }
    return out;
}

std::string PathPolicy::fileName(std::string_view path) const
{
    if (path.empty()) {
        return {};
    }
    const SplitPath sp = splitPath(path, style_ == PathStyle::kWindows);
    if (sp.parts.empty()) {
        return sp.rooted() ? std::string{} : std::string(path);
    }
    return sp.parts.back();
}

bool PathPolicy::isAbsolute(std::string_view path) const
{
    if (path.empty()) {
        return false;
    }
    if (isSeparator(path.front())) {
        return true;
    }
    if (style_ == PathStyle::kWindows && path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
        path[1] == ':') {
        return true;
    }
    return false;
}

std::string PathPolicy::resolveWithin(std::string_view root, std::string_view child) const
{
    if (root.empty() || child.empty()) {
        return {};
    }
    // 绝对路径条目一律拒绝。归档条目必须是相对路径，否则它的落点
    // 由条目自己决定，与 root 完全无关。
    if (isAbsolute(child)) {
        return {};
    }
    // 带盘符的条目（`C:foo` 这种"盘相对"路径）在本策略下没有意义，一并拒绝。
    if (style_ == PathStyle::kWindows && child.size() >= 2 && std::isalpha(static_cast<unsigned char>(child[0])) != 0 &&
        child[1] == ':') {
        return {};
    }

    const bool win = style_ == PathStyle::kWindows;
    const SplitPath rootSplit = splitPath(root, win);
    const std::string rootNorm = joinSplit(rootSplit);

    // 关键：以"根 + 子"整条路径一起归一化，而不是单独归一化 child 再看它是否
    // 含 `..`。`sub/../file.txt` 含 `..` 但完全合法，单独看就会误杀。
    const SplitPath combined = splitPath(join(root, child), win);

    // 归一化后仍残留 `..` 段 → 说明退到了根以外。splitPath 对"已有前缀"
    // 的路径不会产生 `..`（退到根就停），所以这条只在相对根的情形下命中。
    if (std::find(combined.parts.begin(), combined.parts.end(), "..") != combined.parts.end()) {
        return {};
    }
    // 前缀必须一致：防止 `root=C:/a` 与 child 里塞的 UNC/盘符把结果带到别处。
    if (combined.prefix != rootSplit.prefix) {
        return {};
    }
    // 段数量必须大于根：等于根意味着这个条目指向根目录本身，不该落盘。
    if (combined.parts.size() <= rootSplit.parts.size()) {
        return {};
    }
    // 逐段比对根部分。用段比较而不是字符串前缀，天然带分隔符边界，
    // `/data/foo2` 不会被误判成在 `/data/foo` 之内。
    for (std::size_t k = 0; k < rootSplit.parts.size(); ++k) {
        if (combined.parts[k] != rootSplit.parts[k]) {
            return {};
        }
    }

    const std::string fullNorm = joinSplit(combined);
    if (fullNorm.empty() || fullNorm == rootNorm) {
        return {};
    }
    return fullNorm;
}

}   // namespace relayer::filestore
