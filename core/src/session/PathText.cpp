#include "relayer/core/session/PathText.h"

#include <vector>

namespace relayer::core::session {

namespace {

constexpr bool isSep(char c) noexcept
{
    return c == '/' || c == '\\';
}

// 拆出"前缀"（盘符 / UNC）与"段"。前缀是唯一允许出现连续分隔符的地方。
struct Split {
    std::string prefix;   // "" / "/" / "D:/" / "//server/share/"
    std::vector<std::string> segments;
};

Split splitPath(std::string_view path)
{
    Split out;
    std::size_t i = 0;

    // UNC：`\\server\share\...` 或 `//server/share/...`
    // 判据是"前两个字符是分隔符且第三个不是"——`//a` 在 POSIX 语义下
    // 只是"根 + 一个斜杠"，不构成 UNC（缺共享名）。
    const bool leadingDoubleSep = path.size() >= 2 && isSep(path[0]) && isSep(path[1]) && (path.size() == 2 || !isSep(path[2]));
    if (leadingDoubleSep) {
        std::string body(path.substr(2));
        std::string host;
        std::string share;
        std::size_t pos = 0;
        auto takeSegment = [&body, &pos](std::string& dst) {
            const auto start = pos;
            while (pos < body.size() && !isSep(body[pos])) {
                ++pos;
            }
            dst = body.substr(start, pos - start);
            while (pos < body.size() && isSep(body[pos])) {
                ++pos;
            }
        };
        takeSegment(host);
        takeSegment(share);
        if (!host.empty()) {
            out.prefix = "//" + host + "/";
            if (!share.empty()) {
                out.prefix += share + "/";
            }
            i = 2 + pos;
        }
    } else if (path.size() >= 2 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        // 盘符：`C:`、`C:\x`、`C:/x`。`C:` 后面不带分隔符时按根处理
        // （Windows 上它的本义是"该盘的当前目录"，对本工具没有意义）。
        char drive = path[0];
        if (drive >= 'a' && drive <= 'z') {
            drive = static_cast<char>(drive - 'a' + 'A');
        }
        out.prefix = std::string(1, drive) + ":/";
        i = 2;
    } else if (!path.empty() && isSep(path[0])) {
        out.prefix = "/";
        i = 1;
    }

    std::string cur;
    for (; i < path.size(); ++i) {
        if (isSep(path[i])) {
            if (!cur.empty()) {
                out.segments.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(path[i]);
    }
    if (!cur.empty()) {
        out.segments.push_back(std::move(cur));
    }
    return out;
}

std::string assemble(const Split& sp)
{
    std::string out = sp.prefix;
    for (std::size_t k = 0; k < sp.segments.size(); ++k) {
        out += sp.segments[k];
        if (k + 1 < sp.segments.size()) {
            out.push_back('/');
        }
    }
    // "D:/" 与 "/" 保持带尾部分隔符的形式：它们是根，拼子项时直接接名字。
    return out;
}

}   // namespace

std::string normalizePathText(std::string_view path)
{
    if (path.empty()) {
        return {};
    }

    Split sp = splitPath(path);

    // `.` 与 `..`：`.` 丢掉；`..` 退一级，退到前缀就停（根之上还是根）。
    std::vector<std::string> resolved;
    resolved.reserve(sp.segments.size());
    for (const auto& seg : sp.segments) {
        if (seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (!resolved.empty()) {
                resolved.pop_back();
            }
            continue;
        }
        resolved.push_back(seg);
    }
    sp.segments = std::move(resolved);

    std::string out = assemble(sp);

    // 没有前缀也没有段（输入是 "." 或 ".."）：按当前目录处理，与
    // std::filesystem / QDir::cleanPath 一致。
    if (out.empty()) {
        return ".";
    }
    return out;
}

std::string joinPathText(std::string_view base, std::string_view name)
{
    const std::string b = normalizePathText(base);
    const std::string n = normalizePathText(name);

    if (b.empty()) {
        return n;   // base 为空 = 根列表条目本身就是完整路径
    }
    if (b == ".") {
        // base 是"当前目录"这种空洞值，没法和它拼出有意义的东西，
        // 直接返回 name（它至少可能是一条根列表路径）。
        return n.empty() ? b : n;
    }
    if (n.empty()) {
        return b;
    }

    // 根已经带尾部分隔符（`D:/`、`/`、`//server/share/`），直接接名字。
    if (b.back() == '/') {
        return normalizePathText(b + n);
    }
    return normalizePathText(b + "/" + n);
}

std::string parentPathText(std::string_view path)
{
    const std::string p = normalizePathText(path);
    if (p.empty() || p == ".") {
        return {};
    }

    Split sp = splitPath(p);
    if (sp.segments.empty()) {
        // 已经在根（`/`、`D:/`、UNC 共享名）：上面是根列表。
        return {};
    }
    sp.segments.pop_back();
    if (sp.segments.empty() && sp.prefix.empty()) {
        return {};
    }
    return assemble(sp);
}

}   // namespace relayer::core::session
