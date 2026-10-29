#include "relayer/core/session/FileOp.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using relayer::core::session::decodeAnswerDelete;
using relayer::core::session::decodeAnswerFileOp;
using relayer::core::session::decodeAnswerSha256;
using relayer::core::session::decodeAnswerUnarchive;
using relayer::core::session::decodeAskDelete;
using relayer::core::session::decodeAskPath;
using relayer::core::session::decodeAskRename;
using relayer::core::session::encodeAnswerDelete;
using relayer::core::session::encodeAnswerFileOp;
using relayer::core::session::encodeAnswerSha256;
using relayer::core::session::encodeAnswerUnarchive;
using relayer::core::session::encodeAskDelete;
using relayer::core::session::encodeAskPath;
using relayer::core::session::encodeAskRename;
using relayer::core::session::FileOpStatus;
using relayer::core::session::kMaxOpPathBytes;
using relayer::core::session::kMaxOpPaths;

// 这批用例的目标不是"字节一模一样"，而是三条性质：
//   1. 往返保真（编码→解码得到原文）；
//   2. 结构坏了必须**判失败**而不是返回半截数据（半截路径比失败更危险：
//      界面会拿着它去删/改名一个别的文件）；
//   3. 非法输入在编码侧就挡掉（空体 → 解码侧立刻失败）。

TEST_CASE("单路径请求往返保真", "[fileop]")
{
    for (const std::string& path : {std::string("D:/照片/2026/a.jpg"), std::string("/home/me/doc.txt"),
                                    std::string("//server/share/x"), std::string("文件.txt")}) {
        const auto body = encodeAskPath(path);
        REQUIRE_FALSE(body.empty());

        std::string got;
        std::string err;
        REQUIRE(decodeAskPath(body, got, err));
        REQUIRE(got == path);
    }
}

TEST_CASE("单路径请求拒绝空路径与超长路径", "[fileop]")
{
    // 空体是"构造失败"的统一表示：解码侧立刻判失败，调用方不会发出坏帧。
    REQUIRE(encodeAskPath("").empty());
    REQUIRE(encodeAskPath(std::string(kMaxOpPathBytes + 1, 'x')).empty());

    // 恰好到上限仍然合法（边界不能差一）。
    REQUIRE_FALSE(encodeAskPath(std::string(kMaxOpPathBytes, 'x')).empty());
}

TEST_CASE("单路径请求解码拒绝截断与空路径", "[fileop]")
{
    std::string path;
    std::string err;

    REQUIRE_FALSE(decodeAskPath({}, path, err));

    // 长度说有 8 字节，实际只有 3 字节。
    std::vector<char> truncated = {8, 0, 'a', 'b', 'c'};
    REQUIRE_FALSE(decodeAskPath(truncated, path, err));

    // 长度 0：协议里"空路径"没有意义（空是列举专用的"根列表"语义）。
    std::vector<char> emptyPath = {0, 0};
    REQUIRE_FALSE(decodeAskPath(emptyPath, path, err));
}

TEST_CASE("重命名请求往返保真", "[fileop]")
{
    const auto body = encodeAskRename("D:/a/old.txt", "D:/a/new.txt");
    REQUIRE_FALSE(body.empty());

    std::string from;
    std::string to;
    std::string err;
    REQUIRE(decodeAskRename(body, from, to, err));
    REQUIRE(from == "D:/a/old.txt");
    REQUIRE(to == "D:/a/new.txt");
}

TEST_CASE("重命名请求缺目标时判失败", "[fileop]")
{
    // 只有源路径：解码必须在读目标时失败，而不是把 to 留空让调用方去撞南墙。
    const auto body = encodeAskPath("D:/a/old.txt");
    std::string from;
    std::string to;
    std::string err;
    REQUIRE_FALSE(decodeAskRename(body, from, to, err));
    REQUIRE(err.find("target") != std::string::npos);
}

TEST_CASE("批量删除请求往返保真", "[fileop]")
{
    const std::vector<std::string> paths = {"D:/a.txt", "D:/b/c.txt", "/home/me/x"};
    const auto body = encodeAskDelete(paths);
    REQUIRE_FALSE(body.empty());

    std::vector<std::string> got;
    std::string err;
    REQUIRE(decodeAskDelete(body, got, err));
    REQUIRE(got == paths);
}

TEST_CASE("批量删除请求的边界", "[fileop]")
{
    // 空列表没有意义（"删 0 项"要么是调用方的 bug，要么是想探什么）。
    REQUIRE(encodeAskDelete({}).empty());

    // 超过上限在编码侧就挡掉：服务端是逐个删，条数越多阻塞越久。
    std::vector<std::string> tooMany(kMaxOpPaths + 1, "x");
    REQUIRE(encodeAskDelete(tooMany).empty());

    // 恰好到上限合法。
    std::vector<std::string> atLimit(kMaxOpPaths, "x");
    REQUIRE_FALSE(encodeAskDelete(atLimit).empty());

    // 列表里混一个空路径，整份请求作废（不能"跳过它继续"，否则调用方
    // 以为删了 N 项，实际只删了 N-1 项）。
    REQUIRE(encodeAskDelete({"ok.txt", ""}).empty());
}

TEST_CASE("只带状态的应答往返保真", "[fileop]")
{
    for (const auto status : {FileOpStatus::kOk, FileOpStatus::kNotFound, FileOpStatus::kExists, FileOpStatus::kDenied,
                              FileOpStatus::kRejected, FileOpStatus::kFailed}) {
        std::vector<char> body;
        REQUIRE(encodeAnswerFileOp(status, body).ok());

        FileOpStatus got = FileOpStatus::kOk;
        std::string err;
        REQUIRE(decodeAnswerFileOp(body, got, err));
        REQUIRE(got == status);
    }
}

TEST_CASE("缺状态字节的应答判失败", "[fileop]")
{
    FileOpStatus got = FileOpStatus::kOk;
    std::string err;
    REQUIRE_FALSE(decodeAnswerFileOp({}, got, err));
}

TEST_CASE("删除应答带失败清单", "[fileop]")
{
    const std::vector<std::string> failed = {"D:/busy.txt", "D:/locked/x"};
    std::vector<char> body;
    REQUIRE(encodeAnswerDelete(FileOpStatus::kDenied, failed, body).ok());

    FileOpStatus status = FileOpStatus::kOk;
    std::vector<std::string> gotFailed;
    std::string err;
    REQUIRE(decodeAnswerDelete(body, status, gotFailed, err));
    REQUIRE(status == FileOpStatus::kDenied);
    REQUIRE(gotFailed == failed);
}

TEST_CASE("删除应答允许空失败清单", "[fileop]")
{
    std::vector<char> body;
    REQUIRE(encodeAnswerDelete(FileOpStatus::kOk, {}, body).ok());

    FileOpStatus status = FileOpStatus::kFailed;
    std::vector<std::string> failed = {"stale"};
    std::string err;
    REQUIRE(decodeAnswerDelete(body, status, failed, err));
    REQUIRE(status == FileOpStatus::kOk);
    REQUIRE(failed.empty());
}

TEST_CASE("SHA-256 应答往返保真", "[fileop]")
{
    std::vector<char> digest(32);
    for (int i = 0; i < 32; ++i) {
        digest[static_cast<std::size_t>(i)] = static_cast<char>(i + 1);
    }

    std::vector<char> body;
    REQUIRE(encodeAnswerSha256(FileOpStatus::kOk, digest, body).ok());

    FileOpStatus status = FileOpStatus::kFailed;
    std::vector<char> got;
    std::string err;
    REQUIRE(decodeAnswerSha256(body, status, got, err));
    REQUIRE(status == FileOpStatus::kOk);
    REQUIRE(got == digest);
}

TEST_CASE("SHA-256 成功但摘要不是 32 字节时必须拒绝编码", "[fileop]")
{
    // 半截摘要在界面上会显示成一个"看起来正常"的短串，比明确失败更糟。
    std::vector<char> body;
    REQUIRE_FALSE(encodeAnswerSha256(FileOpStatus::kOk, std::vector<char>(16, 'x'), body).ok());
    REQUIRE(body.empty());
}

TEST_CASE("SHA-256 失败应答不带摘要", "[fileop]")
{
    std::vector<char> body;
    REQUIRE(encodeAnswerSha256(FileOpStatus::kNotFound, {}, body).ok());
    REQUIRE(body.size() == 1);   // 只有状态字节

    FileOpStatus status = FileOpStatus::kOk;
    std::vector<char> digest = {'s', 't', 'a', 'l', 'e'};
    std::string err;
    REQUIRE(decodeAnswerSha256(body, status, digest, err));
    REQUIRE(status == FileOpStatus::kNotFound);
    REQUIRE(digest.empty());
}

TEST_CASE("解压应答往返保真", "[fileop]")
{
    std::vector<char> body;
    REQUIRE(encodeAnswerUnarchive(FileOpStatus::kOk, "D:/Download/archive", body).ok());

    FileOpStatus status = FileOpStatus::kFailed;
    std::string dir;
    std::string err;
    REQUIRE(decodeAnswerUnarchive(body, status, dir, err));
    REQUIRE(status == FileOpStatus::kOk);
    REQUIRE(dir == "D:/Download/archive");
}

TEST_CASE("解压失败时应答目录为空", "[fileop]")
{
    // 传了目录也写不进去：失败就是失败，别让界面拿着一半的信息去刷新。
    std::vector<char> body;
    REQUIRE(encodeAnswerUnarchive(FileOpStatus::kFailed, "D:/ignored", body).ok());

    FileOpStatus status = FileOpStatus::kOk;
    std::string dir = "stale";
    std::string err;
    REQUIRE(decodeAnswerUnarchive(body, status, dir, err));
    REQUIRE(status == FileOpStatus::kFailed);
    REQUIRE(dir.empty());
}

TEST_CASE("状态文案覆盖全部枚举值", "[fileop]")
{
    // 界面与日志共用这一份文案；新增枚举忘了加分支会落到"未知状态"，
    // 这条用例就是那个提醒。
    for (const auto status : {FileOpStatus::kOk, FileOpStatus::kNotFound, FileOpStatus::kExists, FileOpStatus::kDenied,
                              FileOpStatus::kRejected, FileOpStatus::kFailed}) {
        const std::string text = relayer::core::session::fileOpStatusText(status);
        REQUIRE_FALSE(text.empty());
        REQUIRE(text != "未知状态");
    }
}
