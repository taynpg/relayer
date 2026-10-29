#include "relayer/core/session/PathText.h"

#include <catch2/catch_test_macros.hpp>

using relayer::core::session::joinPathText;
using relayer::core::session::normalizePathText;
using relayer::core::session::parentPathText;

// 这批用例的来源是真实出现的显示 bug：`D:\/Download`。粘出来的路径是
// "盘符根（带反斜杠）+ 斜杠 + 名字"，两种分隔符各出现一次。它能在
// Windows 上打开，但显示出来一眼就是错的，而且再拼一次会继续叠加。

TEST_CASE("normalizePathText 把反斜杠统一成斜杠", "[pathtext]")
{
    REQUIRE(normalizePathText("D:\\Download") == "D:/Download");
    REQUIRE(normalizePathText("D:/Download") == "D:/Download");
    // 混合写法：这正是 UI 上出现过的那种。
    REQUIRE(normalizePathText("D:\\/Download") == "D:/Download");
    REQUIRE(normalizePathText("\\\\server\\share\\x") == "//server/share/x");
}

TEST_CASE("normalizePathText 合并重复分隔符与末尾分隔符", "[pathtext]")
{
    REQUIRE(normalizePathText("D:\\\\Download//a//") == "D:/Download/a");
    REQUIRE(normalizePathText("/home//me/") == "/home/me");
    // 根要保留尾部斜杠：它是"根"，不是"名字"。
    REQUIRE(normalizePathText("D:\\") == "D:/");
    REQUIRE(normalizePathText("D:") == "D:/");
    REQUIRE(normalizePathText("/") == "/");
}

TEST_CASE("normalizePathText 盘符统一成大写", "[pathtext]")
{
    // Windows 不区分大小写，但显示上两种写法混在一起会让人以为路径不同。
    REQUIRE(normalizePathText("d:/x") == "D:/x");
    REQUIRE(normalizePathText("c:") == "C:/");
}

TEST_CASE("normalizePathText 处理点段", "[pathtext]")
{
    REQUIRE(normalizePathText("D:/a/./b") == "D:/a/b");
    REQUIRE(normalizePathText("D:/a/b/../c") == "D:/a/c");
    // 退到根就停，不产生 "D:/.." 这种没有意义的东西。
    REQUIRE(normalizePathText("D:/../a") == "D:/a");
    REQUIRE(normalizePathText("/../a") == "/a");
}

TEST_CASE("normalizePathText 空输入返回空", "[pathtext]")
{
    // 空串在协议里是"根列表"，不能被归一成 "." 或 "/"。
    REQUIRE(normalizePathText("").empty());
    // 只有点段时按当前目录处理，与 std::filesystem / QDir::cleanPath 一致。
    REQUIRE(normalizePathText(".") == ".");
}

TEST_CASE("joinPathText 拼盘符根不会再产生混合作物", "[pathtext]")
{
    // 这一条就是那个 bug 的回归用例。
    REQUIRE(joinPathText("D:\\", "Download") == "D:/Download");
    REQUIRE(joinPathText("D:/", "Download") == "D:/Download");
    REQUIRE(joinPathText("D:/", "a") == "D:/a");
    REQUIRE(joinPathText("D:/Download", "a.txt") == "D:/Download/a.txt");
    REQUIRE(joinPathText("/", "home") == "/home");
    REQUIRE(joinPathText("/home", "me") == "/home/me");
}

TEST_CASE("joinPathText 空 base 表示根列表条目自带完整路径", "[pathtext]")
{
    REQUIRE(joinPathText("", "D:/x") == "D:/x");
    REQUIRE(joinPathText("", "/home/me") == "/home/me");
}

TEST_CASE("parentPathText 回到根列表时返回空", "[pathtext]")
{
    REQUIRE(parentPathText("D:/a/b") == "D:/a");
    REQUIRE(parentPathText("D:/a") == "D:/");
    // 根之上没有东西：调用方据此回到根列表（盘符 + 家目录）。
    REQUIRE(parentPathText("D:/").empty());
    REQUIRE(parentPathText("/").empty());
    REQUIRE(parentPathText("/a") == "/");
    REQUIRE(parentPathText("").empty());
}

TEST_CASE("parentPathText 用反斜杠输入的路径也走同一条规则", "[pathtext]")
{
    REQUIRE(parentPathText("D:\\a\\b") == "D:/a");
    REQUIRE(parentPathText("D:\\a") == "D:/");
}
