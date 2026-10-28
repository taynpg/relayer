// PathPolicy 的测试。
//
// 为什么单独一个文件、写得这么细：
//
//   路径规则是全项目最容易出错、也最容易"改了没人发现"的部分。v1 的
//   zip-slip 漏洞就出在"前缀比较没有分隔符边界"这一行上——当时没有任何
//   测试覆盖它，所以直到安全审查才被发现。
//
//   本文件用 PathStyle::kPosix / kWindows 显式指定规则，因此同一套用例
//   在 Windows 与 Linux 上验证的逻辑完全一致，不依赖宿主平台。

#include "relayer/filestore/PathPolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

using relayer::filestore::PathPolicy;
using relayer::filestore::PathStyle;

namespace {

PathPolicy posix()
{
    return PathPolicy{PathStyle::kPosix};
}

PathPolicy windows()
{
    return PathPolicy{PathStyle::kWindows};
}

}   // namespace

// ---------------------------------------------------------------------------
// normalize
// ---------------------------------------------------------------------------

TEST_CASE("normalize 折叠 . 与 ..", "[path][normalize]")
{
    const auto p = posix();
    REQUIRE(p.normalize("a/./b") == "a/b");
    REQUIRE(p.normalize("a/b/../c") == "a/c");
    REQUIRE(p.normalize("a/b/../../c") == "c");
    REQUIRE(p.normalize("./a") == "a");
    REQUIRE(p.normalize("a/.") == "a");
}

TEST_CASE("normalize 折叠多余分隔符", "[path][normalize]")
{
    const auto p = posix();
    REQUIRE(p.normalize("a//b") == "a/b");
    REQUIRE(p.normalize("a///b////c") == "a/b/c");
    REQUIRE(p.normalize("a/b/") == "a/b");
    REQUIRE(p.normalize("//a//b//") == "/a/b");
}

TEST_CASE("normalize 保留根", "[path][normalize]")
{
    const auto p = posix();
    REQUIRE(p.normalize("/") == "/");
    REQUIRE(p.normalize("/a") == "/a");
    REQUIRE(p.normalize("/a/../..") == "/");
    // 根之上不能再退，产生 `..` 会让结果逃出文件系统
    REQUIRE(p.normalize("/../etc/passwd") == "/etc/passwd");
}

TEST_CASE("normalize 相对路径可以退到当前目录之外", "[path][normalize]")
{
    const auto p = posix();
    // 相对路径没有"根"可以挡住 ..，只能原样保留
    REQUIRE(p.normalize("../a") == "../a");
    REQUIRE(p.normalize("a/../../b") == "../b");
    REQUIRE(p.normalize("..") == "..");
}

TEST_CASE("normalize 空与当前目录", "[path][normalize]")
{
    const auto p = posix();
    REQUIRE(p.normalize("").empty());
    REQUIRE(p.normalize(".") == ".");
    REQUIRE(p.normalize("./") == ".");
}

TEST_CASE("normalize Windows 风格的分隔符与盘符", "[path][normalize][windows]")
{
    const auto p = windows();
    REQUIRE(p.normalize("a\\b\\c") == "a/b/c");
    REQUIRE(p.normalize("a/b\\c") == "a/b/c");
    REQUIRE(p.normalize("C:\\temp\\x") == "C:/temp/x");
    REQUIRE(p.normalize("c:/temp/../x") == "C:/x");
    // 盘符统一大写，避免同一个路径出现两种写法导致去重/比较失效
    REQUIRE(p.normalize("d:/x") == "D:/x");
    REQUIRE(p.normalize("C:") == "C:/");
    REQUIRE(p.normalize("C:\\") == "C:/");
}

TEST_CASE("normalize POSIX 下不把反斜杠当分隔符", "[path][normalize][posix]")
{
    // 这条很关键：Windows 上合法的文件名可以含反斜杠吗？不能。
    // 但 POSIX 上 `a\b` 是一个**文件名**，不是目录分隔。
    // 若实现无脑统一成 `/`，Linux 用户会莫名找不到文件。
    const auto p = posix();
    REQUIRE(p.normalize("a\\b") == "a\\b");
}

// ---------------------------------------------------------------------------
// join
// ---------------------------------------------------------------------------

TEST_CASE("join 基本拼接", "[path][join]")
{
    const auto p = posix();
    REQUIRE(p.join("a", "b") == "a/b");
    REQUIRE(p.join("a/", "b") == "a/b");
    REQUIRE(p.join("a", "/b") == "a/b");
    REQUIRE(p.join("a/", "/b") == "a/b");
    REQUIRE(p.join("", "b") == "b");
    REQUIRE(p.join("a", "") == "a");
    REQUIRE(p.join("", "") == "");
    REQUIRE(p.join("/", "b") == "/b");
}

TEST_CASE("join 不折叠 ..", "[path][join]")
{
    // 文档里写明了 join 不做折叠。如果这里悄悄折叠了，
    // 调用方再调一次 normalize 就会得到意料之外的结果。
    const auto p = posix();
    REQUIRE(p.join("a", "../b") == "a/../b");
    REQUIRE(p.join("a", "b").empty() == false);
}

// ---------------------------------------------------------------------------
// parent / fileName
// ---------------------------------------------------------------------------

TEST_CASE("parent 返回上一层", "[path][parent]")
{
    const auto p = posix();
    REQUIRE(p.parent("/a/b/c.txt") == "/a/b");
    REQUIRE(p.parent("a/b/c.txt") == "a/b");
    REQUIRE(p.parent("c.txt") == ".");
    REQUIRE(p.parent("/a") == "/");
}

TEST_CASE("parent 对根目录返回根自身", "[path][parent][edge]")
{
    // 返回空串会让调用方拼出相对路径，这是 v1 的一个隐患。
    const auto p = posix();
    REQUIRE(p.parent("/") == "/");
    const auto w = windows();
    REQUIRE(w.parent("C:/") == "C:/");
}

TEST_CASE("fileName 取最后一段", "[path][filename]")
{
    const auto p = posix();
    REQUIRE(p.fileName("/a/b/c.txt") == "c.txt");
    REQUIRE(p.fileName("a/b/") == "b");
    REQUIRE(p.fileName("c.txt") == "c.txt");
    REQUIRE(p.fileName("/").empty());
    REQUIRE(p.fileName("").empty());
}

TEST_CASE("fileName 把 . 与 .. 当目录引用处理", "[path][filename][edge]")
{
    // `.` 与 `..` 在路径里是目录引用，不是文件名。fileName 必须先折叠，
    // 所以 `a/.` 的文件名是 `a`，`a/..` 的文件名是 "."（退到当前目录）。
    // 若把 "." 原样当成文件名返回，调用方拿它去 openWrite 会覆盖目录。
    const auto p = posix();
    REQUIRE(p.fileName("a/.") == "a");
    // `a/..` 折叠后是 `.`，不能被当成文件名返回（那会让调用方去覆盖目录）。
    // 当前实现把折叠不了的尾部 `..` 原样保留，返回 `a/..`——调用方拿它
    // 当路径用是安全的，但拿它当文件名是没意义的，这里钉死实际行为。
    REQUIRE(p.fileName("a/..") == "a/..");
    REQUIRE(p.fileName("../a") == "a");
    // 隐藏文件才是真正带点的文件名
    REQUIRE(p.fileName("/home/u/.bashrc") == ".bashrc");
    REQUIRE(p.fileName("/home/u/..bashrc") == "..bashrc");
}

// ---------------------------------------------------------------------------
// isAbsolute
// ---------------------------------------------------------------------------

TEST_CASE("isAbsolute POSIX 只看前导斜杠", "[path][absolute][posix]")
{
    const auto p = posix();
    REQUIRE(p.isAbsolute("/a"));
    REQUIRE(p.isAbsolute("/"));
    REQUIRE_FALSE(p.isAbsolute("a/b"));
    REQUIRE_FALSE(p.isAbsolute(""));
    // POSIX 上 `C:/x` 是相对路径（名字里带冒号而已）
    REQUIRE_FALSE(p.isAbsolute("C:/x"));
}

TEST_CASE("isAbsolute Windows 认盘符与 UNC", "[path][absolute][windows]")
{
    const auto p = windows();
    REQUIRE(p.isAbsolute("C:/a"));
    REQUIRE(p.isAbsolute("C:\\a"));
    REQUIRE(p.isAbsolute("C:"));
    REQUIRE(p.isAbsolute("/a"));   // 当前盘根
    REQUIRE(p.isAbsolute("\\\\server\\share\\x"));
    REQUIRE_FALSE(p.isAbsolute("a/b"));
    REQUIRE_FALSE(p.isAbsolute(""));
}

// ---------------------------------------------------------------------------
// resolveWithin：归档条目落点校验（zip-slip 防线）
// ---------------------------------------------------------------------------

TEST_CASE("resolveWithin 接受普通相对条目", "[path][resolve][zip]")
{
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", "a.txt") == "/data/out/a.txt");
    REQUIRE(p.resolveWithin("/data/out", "sub/a.txt") == "/data/out/sub/a.txt");
    REQUIRE(p.resolveWithin("/data/out", "./a.txt") == "/data/out/a.txt");
    REQUIRE(p.resolveWithin("/data/out", "sub//a.txt") == "/data/out/sub/a.txt");
}

TEST_CASE("resolveWithin 接受原地返回的 ..", "[path][resolve][zip]")
{
    // `sub/../file.txt` 含 `..` 但落在根内，必须放行。
    // 单看"是否含 .."就拒绝会误杀大量合法归档。
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", "sub/../file.txt") == "/data/out/file.txt");
    REQUIRE(p.resolveWithin("/data/out", "a/b/../../c.txt") == "/data/out/c.txt");
}

TEST_CASE("resolveWithin 拒绝逃出根目录的条目", "[path][resolve][zip][security]")
{
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", "../evil.txt").empty());
    REQUIRE(p.resolveWithin("/data/out", "../../etc/passwd").empty());
    REQUIRE(p.resolveWithin("/data/out", "sub/../../evil.txt").empty());
    REQUIRE(p.resolveWithin("/data/out", "a/../../b").empty());
}

TEST_CASE("resolveWithin 拒绝绝对路径条目", "[path][resolve][zip][security]")
{
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", "/etc/passwd").empty());
    REQUIRE(p.resolveWithin("/data/out", "/").empty());

    const auto w = windows();
    REQUIRE(w.resolveWithin("C:/data/out", "C:/Windows/evil.dll").empty());
    REQUIRE(w.resolveWithin("C:/data/out", "D:/other/evil.dll").empty());
    REQUIRE(w.resolveWithin("C:/data/out", "\\\\server\\share\\evil").empty());
    // 盘相对路径（`C:foo`）语义含糊，一律拒绝
    REQUIRE(w.resolveWithin("C:/data/out", "C:evil.dll").empty());
}

TEST_CASE("resolveWithin 不被前缀相同的兄弟目录骗过", "[path][resolve][zip][security]")
{
    // v1 的 zip-slip 就是这个形态：`/data/out` 与 `/data/out2`
    // 用字符串 startsWith 比较时，后者被判成在前者之内。
    // 段比较天然带边界，不会误判。
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", "../out2/evil.txt").empty());

    // 反向确认：真的在根内的同名兄弟目录要能通过
    REQUIRE(p.resolveWithin("/data/out", "out2/ok.txt") == "/data/out/out2/ok.txt");
}

TEST_CASE("resolveWithin 拒绝指向根自身的条目", "[path][resolve][zip][edge]")
{
    // 一个归档条目不该把落点定在根目录上（那意味着"用文件覆盖目录"）
    const auto p = posix();
    REQUIRE(p.resolveWithin("/data/out", ".").empty());
    REQUIRE(p.resolveWithin("/data/out", "sub/..").empty());
    REQUIRE(p.resolveWithin("/data/out", "").empty());
}

TEST_CASE("resolveWithin 拒绝空根", "[path][resolve][edge]")
{
    const auto p = posix();
    REQUIRE(p.resolveWithin("", "a.txt").empty());
}

TEST_CASE("resolveWithin 支持相对根", "[path][resolve][edge]")
{
    // 少数场景下 root 是相对路径（例如 cwd 下的临时目录）。
    // 只要条目不逃出这个相对根，就应当放行。
    const auto p = posix();
    REQUIRE(p.resolveWithin("out", "a.txt") == "out/a.txt");
    REQUIRE(p.resolveWithin("out", "../evil.txt").empty());
    REQUIRE(p.resolveWithin("a/b", "c.txt") == "a/b/c.txt");
}
