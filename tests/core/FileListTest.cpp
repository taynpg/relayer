#include "relayer/core/session/FileList.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::session;

namespace
{

RemoteEntry entry(const std::string &name, bool isDir, std::uint64_t size, std::int64_t mtime)
{
    RemoteEntry e;
    e.name = name;
    e.isDir = isDir;
    e.size = size;
    e.mtime = mtime;
    return e;
}

}  // namespace

// ===========================================================================
// kAskFileList
// ===========================================================================

TEST_CASE("ask file list roundtrip", "[filelist][codec]")
{
    const auto body = encodeAskFileList("docs/2026");
    // len(1) + path(9)
    REQUIRE(body.size() == 10);

    std::string path;
    std::string err;
    REQUIRE(decodeAskFileList(body, path, err));
    REQUIRE(path == "docs/2026");
}

TEST_CASE("ask file list with empty path means the share root", "[filelist][codec]")
{
    const auto body = encodeAskFileList("");
    REQUIRE(body.size() == 1);

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

TEST_CASE("ask file list truncates oversized paths instead of encoding garbage", "[filelist][codec]")
{
    // 路径超过 255 字节无法用 u8 长度表达。这里给空体（= 根目录）而不是
    // 静默截断——截断后的路径可能指向另一个目录，那比"列根目录"危险。
    const auto body = encodeAskFileList(std::string(300, 'x'));
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
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, in, body).ok());

    FileListStatus status = FileListStatus::kRejected;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, out, err));
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
    REQUIRE(encodeAnswerFileList(FileListStatus::kRejected, {entry("x", false, 1, 2)}, body).ok());

    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, out, err));
    REQUIRE(status == FileListStatus::kRejected);
    REQUIRE(out.empty());
}

TEST_CASE("answer file list reports truncation", "[filelist][codec]")
{
    // 截断仍然是成功（有内容），但状态必须如实告知，UI 才能提示
    // "只列出前 N 项"而不是假装目录就这么大。
    std::vector<char> body;
    REQUIRE(encodeAnswerFileList(FileListStatus::kTruncated, {entry("a", false, 1, 2)}, body).ok());

    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(body, status, out, err));
    REQUIRE(status == FileListStatus::kTruncated);
    REQUIRE(out.size() == 1);
}

TEST_CASE("answer file list rejects an absurd count", "[filelist][codec]")
{
    // 伪造的 count 会让 entries.reserve 直接吃光内存，必须先挡上限。
    // 结构：status(0) + count = 0xFFFFFFFF
    const std::vector<char> body = {0, '\xFF', '\xFF', '\xFF', '\xFF'};
    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(body, status, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer file list rejects truncated entries", "[filelist][codec]")
{
    std::vector<char> full;
    REQUIRE(encodeAnswerFileList(FileListStatus::kOk, {entry("abc", false, 7, 8)}, full).ok());

    // 把最后两个字节削掉：条目内部截断必须被判定为结构错误。
    full.pop_back();
    full.pop_back();

    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(full, status, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer file list refuses empty entry names", "[filelist][codec]")
{
    // 空名字在盘上不可能出现，发出来只可能是实现 bug；编码处直接失败，
    // 别让一个无名条目混进对端的列表。
    std::vector<char> body;
    const auto st = encodeAnswerFileList(FileListStatus::kOk, {entry("", false, 0, 0)}, body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

// ---------------------------------------------------------------------------
// 条目名必须是单段（防不可信对端把路径拼进来）
// ---------------------------------------------------------------------------

namespace
{

// 手工拼 body：encode 会拒绝非法名字，测解码侧只能自己造字节。
// 布局见 PROTOCOL.md §11.8：status(u8) count(u32) 后跟 count 个条目。
std::vector<char> rawAnswer(const std::vector<std::string> &names)
{
    std::vector<char> out;
    out.push_back(0);  // status = kOk
    const auto count = static_cast<std::uint32_t>(names.size());
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<char>((count >> (8 * i)) & 0xFF));  // 小端
    }
    for (const auto &n : names)
    {
        out.push_back(static_cast<char>(n.size()));
        out.insert(out.end(), n.begin(), n.end());
        out.push_back(0);  // type = 文件
        out.insert(out.end(), 8, 0);
        out.insert(out.end(), 8, 0);
    }
    return out;
}

}  // namespace

TEST_CASE("answer file list rejects a parent-directory entry name", "[filelist][codec]")
{
    // 名为 `..` 的条目一旦进列表，UI 双击就会把它拼进当前路径。
    // 合法对端不可能发出来，整份应答都不可信。
    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(rawAnswer({".."}), status, out, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(out.empty());
}

TEST_CASE("answer file list rejects an entry name containing a slash", "[filelist][codec]")
{
    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileList(rawAnswer({"a/b"}), status, out, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(out.empty());
}

TEST_CASE("answer file list keeps a backslash because it is a legal filename", "[filelist][codec]")
{
    // Unix 允许文件名含反斜杠。core 层只按协议判"是不是路径"，Windows 上
    // 它才是分隔符——那层平台判断在 UI（ExplorerControl 双击处）做，
    // 不能在这里把合法文件当成攻击拦掉。
    FileListStatus status = FileListStatus::kOk;
    std::vector<RemoteEntry> out;
    std::string err;
    REQUIRE(decodeAnswerFileList(rawAnswer({"a\\b.txt"}), status, out, err));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].name == "a\\b.txt");
}

TEST_CASE("encodeAnswerFileList refuses a path-like entry name", "[filelist][codec]")
{
    std::vector<char> body;
    const auto st = encodeAnswerFileList(FileListStatus::kOk, {entry("x/../y", false, 1, 2)}, body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}
