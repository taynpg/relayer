#include "relayer/core/session/FileList.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "relayer/core/session/PathMeta.h"

using namespace relayer::core::session;

namespace {

RemoteEntry entry(const std::string& name, bool isDir, std::uint64_t size, std::int64_t mtime)
{
    RemoteEntry e;
    e.name = name;
    e.isDir = isDir;
    e.size = size;
    e.mtime = mtime;
    return e;
}

}   // namespace

// ===========================================================================
// kAskFileList
// ===========================================================================

TEST_CASE("ask file list roundtrip", "[filelist][codec]")
{
    const auto body = encodeAskFileList("D:/docs/2026");
    // len(u16 两个字节) + path(12)
    REQUIRE(body.size() == 14);

    std::string path;
    std::string err;
    REQUIRE(decodeAskFileList(body, path, err));
    REQUIRE(path == "D:/docs/2026");
}

TEST_CASE("ask file list with empty path means the root list", "[filelist][codec]")
{
    // 空路径 = 要根列表（盘符 + 家目录）。长度字段是 u16，所以空路径也
    // 带两个字节。
    const auto body = encodeAskFileList("");
    REQUIRE(body.size() == 2);

    std::string path = "stale";
    std::string err;
    REQUIRE(decodeAskFileList(body, path, err));
    REQUIRE(path.empty());
}

TEST_CASE("ask file list refuses malformed payloads", "[filelist][codec]")
{
    std::string path;
    std::string err;

    // 空体：连长度字节都没有
    REQUIRE_FALSE(decodeAskFileList({}, path, err));
    // 长度说 5 只给 3 字节
    REQUIRE_FALSE(decodeAskFileList({5, 'a', 'b', 'c'}, path, err));
    // 长度说 3 却给了 5 字节：多出来的字节说明不是按这张表编码的
    REQUIRE_FALSE(decodeAskFileList({3, 'a', 'b', 'c', 'd', 'e'}, path, err));
}

TEST_CASE("ask file list accepts long absolute paths", "[filelist][codec]")
{
    // 路径长度前缀是 u16：绝对路径轻易超过 255 字节（Windows 的 MAX_PATH
    // 就是 260），用 u8 会把长路径整批判成"根目录"。
    const auto body = encodeAskFileList(std::string(1000, 'x'));
    REQUIRE_FALSE(body.empty());

    std::string path;
    std::string err;
    REQUIRE(decodeAskFileList(body, path, err));
    REQUIRE(path.size() == 1000);
}

TEST_CASE("ask file list rejects paths beyond u16 length", "[filelist][codec]")
{
    // 超过 65535 字节无法用 u16 表达：给空体（判失败），绝不静默截断——
    // 截断后的路径指向的是另一个目录。
    const auto body = encodeAskFileList(std::string(70000, 'x'));
    REQUIRE(body.empty());
}

// ===========================================================================
// kAnswerFileList
// ===========================================================================

TEST_CASE("answer file list roundtrip preserves entries", "[filelist][codec]")
{
    const std::vector<RemoteEntry> in = {
        entry("docs", true, 0, 1700000000),
        entry("b.bin", false, 1234567890123ULL, 1700000001),
    };

    std::vector<char> body;
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, false, in, body).ok());

    FileListStatus status = FileListStatus::kRejected;
    bool roots = true;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, roots, out, err));
    REQUIRE_FALSE(roots);
    REQUIRE(status == FileListStatus::kOk);
    REQUIRE(out.size() == 2);

    REQUIRE(out[0].name == "docs");
    REQUIRE(out[0].isDir);
    REQUIRE(out[0].mtime == 1700000000);

    // 大文件与 u64 边界：用 32 位中间量的实现会在这里错。
    REQUIRE(out[1].name == "b.bin");
    REQUIRE_FALSE(out[1].isDir);
    REQUIRE(out[1].size == 1234567890123ULL);
}

TEST_CASE("answer file list rejection carries no entries", "[filelist][codec]")
{
    // 拒绝时即使调用方塞了条目也不发出去：带了会让客户端以为"只是部分结果"。
    std::vector<char> body;
    REQUIRE(encodeAnswerFileList(FileListStatus::kRejected, false, {entry("x", false, 1, 2)}, body).ok());

    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, roots, out, err));
    REQUIRE(status == FileListStatus::kRejected);
    REQUIRE(out.empty());
}

TEST_CASE("answer file list reports truncation", "[filelist][codec]")
{
    // 截断仍然是成功（有内容），但状态必须如实告知，UI 才能提示
    // "只列出前 N 项"而不是假装目录就这么大。
    std::vector<char> body;
    REQUIRE(encodeAnswerFileList(FileListStatus::kTruncated, false, {entry("a", false, 1, 2)}, body).ok());

    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, roots, out, err));
    REQUIRE(status == FileListStatus::kTruncated);
    REQUIRE(out.size() == 1);
}

TEST_CASE("answer file list rejects an absurd count", "[filelist][codec]")
{
    // 伪造的 count 会让 entries.reserve 直接吃光内存，必须先挡上限。
    // 结构：status(0) + count = 0xFFFFFFFF
    const std::vector<char> body = {0, '\xFF', '\xFF', '\xFF', '\xFF'};
    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(body, status, roots, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer file list rejects truncated entries", "[filelist][codec]")
{
    std::vector<char> full;
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, false, {entry("abc", false, 7, 8)}, full).ok());

    // 把最后两个字节削掉：条目内部截断必须被判定为结构错误。
    full.pop_back();
    full.pop_back();

    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(full, status, roots, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer file list refuses empty entry names", "[filelist][codec]")
{
    // 空名字在盘上不可能出现，发出来只可能是实现 bug；编码处直接失败，
    // 别让一个无名条目混进对端的列表。
    std::vector<char> body;
    const auto st = encodeAnswerFileList(FileListStatus::kOk, false, {entry("", false, 0, 0)}, body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

// ---------------------------------------------------------------------------
// 条目名必须是单段（防不可信对端把路径拼进来）
// ---------------------------------------------------------------------------

namespace {

// 手工拼 body：encode 会拒绝非法名字，测解码侧只能自己造字节。
// 布局见 PROTOCOL.md §11.8：status(u8) count(u32) 后跟 count 个条目。
std::vector<char> rawAnswer(const std::vector<std::string>& names)
{
    std::vector<char> out;
    out.push_back(0);   // status = kOk
    out.push_back(0);   // flags：普通列表（非根列表）
    const auto count = static_cast<std::uint32_t>(names.size());
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((count >> (8 * i)) & 0xFF));   // 小端
    }
    for (const auto& n : names) {
        out.push_back(static_cast<char>(n.size()));
        out.insert(out.end(), n.begin(), n.end());
        out.push_back(0);   // type = 文件
        out.insert(out.end(), 8, 0);
        out.insert(out.end(), 8, 0);
    }
    return out;
}

}   // namespace

TEST_CASE("answer file list rejects a parent-directory entry name", "[filelist][codec]")
{
    // 名为 `..` 的条目一旦进列表，UI 双击就会把它拼进当前路径。
    // 合法对端不可能发出来，整份应答都不可信。
    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(rawAnswer({".."}), status, roots, out, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(out.empty());
}

TEST_CASE("answer file list rejects an entry name containing a slash", "[filelist][codec]")
{
    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(rawAnswer({"a/b"}), status, roots, out, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(out.empty());
}

TEST_CASE("answer file list keeps a backslash because it is a legal filename", "[filelist][codec]")
{
    // Unix 允许文件名含反斜杠。core 层只按协议判"是不是路径"，Windows 上
    // 它才是分隔符——那层平台判断在 UI（ExplorerControl 双击处）做，
    // 不能在这里把合法文件当成攻击拦掉。
    FileListStatus status = FileListStatus::kOk;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(rawAnswer({"a\\b.txt"}), status, roots, out, err));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].name == "a\\b.txt");
}

TEST_CASE("encodeAnswerFileList refuses a path-like entry name", "[filelist][codec]")
{
    std::vector<char> body;
    const auto st = encodeAnswerFileList(FileListStatus::kOk, false, {entry("x/../y", false, 1, 2)}, body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

// ---------------------------------------------------------------------------
// 根列表（绝对路径模型的起点）
// ---------------------------------------------------------------------------

TEST_CASE("answer file list allows full paths when flagged as a root listing", "[filelist][codec]")
{
    // 根列表的条目名天生带分隔符（`C:/`、`/home/me`）。校验规则由应答头
    // 的 flags.bit0 显式切换，而不是靠"名字里有没有分隔符"去猜——后者
    // 对端只要发一个叫 `../../etc` 的条目就能绕过单段名校验。
    std::vector<char> body;
    const std::vector<RemoteEntry> in = {
        entry("C://", true, 0, 1700000000),
        entry("/home/me", true, 0, 1700000001),
    };
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, true, in, body).ok());

    FileListStatus status = FileListStatus::kRejected;
    bool roots = false;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, roots, out, err));
    REQUIRE(roots);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].name == "C://");
    REQUIRE(out[1].name == "/home/me");
}

TEST_CASE("answer file list rejects a path-like name when not a root listing", "[filelist][codec]")
{
    // 同一份内容，flags 不置位就必须判错：根列表的放宽不能变成普通列表
    // 的漏洞。
    std::vector<char> body;
    const std::vector<RemoteEntry> in = {entry("/home/me", true, 0, 1)};
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, false, in, body).ok() == false);
}

// ===========================================================================
// kAskPathMeta / kAnswerPathMeta（§11.11）
// ===========================================================================

TEST_CASE("ask path meta roundtrip keeps paths in order", "[pathmeta][codec]")
{
    const std::vector<std::string> in = {"D:/photos/a.jpg", "/home/me/b.txt", "C:/x"};
    const auto body = encodeAskPathMeta(in);
    REQUIRE_FALSE(body.empty());

    std::vector<std::string> out;
    std::string err;
    REQUIRE(decodeAskPathMeta(body, out, err));
    REQUIRE(out == in);
}

TEST_CASE("ask path meta refuses empty and oversized batches", "[pathmeta][codec]")
{
    // 空路径没有意义；超过上限会让对端逐个 stat + 读 64KB，变成一次长阻塞。
    REQUIRE(encodeAskPathMeta({""}).empty());

    std::vector<std::string> tooMany(kMaxMetaPaths + 1, "C:/x");
    REQUIRE(encodeAskPathMeta(tooMany).empty());
}

TEST_CASE("ask path meta rejects an absurd count", "[pathmeta][codec]")
{
    // 伪造的 count 会让 reserve 吃光内存，必须先挡上限。
    const std::vector<char> body = {'\xFF', '\xFF', '\xFF', '\xFF'};
    std::vector<std::string> out;
    std::string err;
    REQUIRE_FALSE(decodeAskPathMeta(body, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer path meta roundtrip carries existence size mtime and fingerprint", "[pathmeta][codec]")
{
    std::vector<RemoteMeta> in(3);
    in[0].exists = true;
    in[0].size = 1234567890123ULL;
    in[0].mtime = 1700000000;
    in[0].fingerprint = "deadbeef";
    in[1].exists = true;
    in[1].isDir = true;
    in[1].mtime = -1;   // 取不到时间时的边界值，必须原样带回
    // in[2]：不存在，全零

    std::vector<char> body;
    REQUIRE(encodeAnswerPathMeta(in, body).ok());

    std::vector<RemoteMeta> out;
    std::string err;
    REQUIRE(decodeAnswerPathMeta(body, out, err));
    REQUIRE(out.size() == 3);

    REQUIRE(out[0].exists);
    REQUIRE_FALSE(out[0].isDir);
    REQUIRE(out[0].size == 1234567890123ULL);
    REQUIRE(out[0].mtime == 1700000000);
    REQUIRE(out[0].fingerprint == "deadbeef");

    REQUIRE(out[1].exists);
    REQUIRE(out[1].isDir);
    REQUIRE(out[1].mtime == -1);

    REQUIRE_FALSE(out[2].exists);
    REQUIRE_FALSE(out[2].isDir);
    REQUIRE(out[2].size == 0);
    REQUIRE(out[2].fingerprint.empty());
}

TEST_CASE("answer path meta tolerates trailing bytes from a newer peer", "[pathmeta][codec]")
{
    std::vector<RemoteMeta> in(1);
    in[0].exists = true;
    std::vector<char> body;
    REQUIRE(encodeAnswerPathMeta(in, body).ok());
    body.push_back('\x7F');   // 未来加的字段

    std::vector<RemoteMeta> out;
    std::string err;
    REQUIRE(decodeAnswerPathMeta(body, out, err));
    REQUIRE(out.size() == 1);
}

TEST_CASE("answer path meta rejects a truncated entry", "[pathmeta][codec]")
{
    std::vector<RemoteMeta> in(1);
    in[0].exists = true;
    in[0].fingerprint = "abc";
    std::vector<char> body;
    REQUIRE(encodeAnswerPathMeta(in, body).ok());
    body.pop_back();

    std::vector<RemoteMeta> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerPathMeta(body, out, err));
}
