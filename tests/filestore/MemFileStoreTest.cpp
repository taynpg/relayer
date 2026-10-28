// MemFileStore 的测试。
//
// 重点不在"它能不能存字符串"——那没什么可测的。
// 重点在**故障注入是否真的能模拟真实故障**，因为整个项目的测试策略
// 都依赖这一点：传输引擎的异常分支只有靠它才能被测到。
//
// 因此本文件对每个故障模式都做两件事：
//   1. 验证故障确实被触发；
//   2. 验证**同一份代码在 StdFileStore 上的行为**（用临时目录）与
//      MemFileStore 在正常路径下的行为一致。
// 第 2 点是关键——如果两个实现语义漂移，基于 MemFileStore 写的测试
// 就在验证一个不存在的东西。

#include "relayer/filestore/MemFileStore.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "relayer/filestore/StdFileStore.h"
#include "support/TempDir.h"

using relayer::filestore::DirEntry;
using relayer::filestore::ErrorCode;
using relayer::filestore::FaultInjection;
using relayer::filestore::IFileStore;
using relayer::filestore::MemFileStore;
using relayer::filestore::Status;
using relayer::filestore::StdFileStore;
using relayer::test::TempDir;
using relayer::test::writeFile;

namespace {

std::string readAll(IFileStore& fs, const std::string& path, Status& st)
{
    std::string out;
    auto stream = fs.openRead(path, st);
    if (!stream) {
        return out;
    }
    char buf[4096];
    for (;;) {
        const std::size_t n = stream->read(buf, sizeof(buf), st);
        out.append(buf, n);
        if (!st) {
            return {};
        }
        if (n == 0) {
            break;
        }
    }
    return out;
}

bool writeAll(IFileStore& fs, const std::string& path, const std::string& content, Status& st)
{
    auto w = fs.openWrite(path, true, st);
    if (!w) {
        return false;
    }
    st = w->write(content.data(), content.size());
    if (!st) {
        return false;
    }
    st = w->flush();
    return static_cast<bool>(st);
}

}   // namespace

// ---------------------------------------------------------------------------
// 基本读写
// ---------------------------------------------------------------------------

TEST_CASE("MemFileStore 读写往返", "[mem][io]")
{
    MemFileStore fs;
    Status st;
    REQUIRE(writeAll(fs, "/work/a.txt", "hello", st));
    REQUIRE(st);

    REQUIRE(fs.getFile("/work/a.txt") == "hello");
    REQUIRE(readAll(fs, "/work/a.txt", st) == "hello");
    REQUIRE(st);
}

TEST_CASE("MemFileStore 自动建父目录", "[mem][io]")
{
    // putFile 会自动建父目录。这个便利是为了让测试代码短，
    // 但它会掩盖"引擎忘记 mkdirs"的 bug，所以只用 putFile 准备数据，
    // 生产路径仍然走 openWrite + 显式 mkdirs。
    MemFileStore fs;
    fs.putFile("/a/b/c/d.txt", "x");
    REQUIRE(fs.stat("/a/b/c").isDir);
    REQUIRE(fs.has("/a/b/c/d.txt"));
}

TEST_CASE("MemFileStore 空文件", "[mem][io][edge]")
{
    MemFileStore fs;
    Status st;
    REQUIRE(writeAll(fs, "/e.bin", "", st));
    REQUIRE(fs.stat("/e.bin").exists);
    REQUIRE(fs.stat("/e.bin").size == 0);
    REQUIRE(readAll(fs, "/e.bin", st).empty());
    REQUIRE(st);
}

TEST_CASE("MemFileStore 读写含零字节的内容", "[mem][io][edge]")
{
    MemFileStore fs;
    Status st;
    const std::string content("a\0b\0c", 5);
    REQUIRE(writeAll(fs, "/bin.dat", content, st));
    REQUIRE(fs.getFile("/bin.dat").size() == 5);
    REQUIRE(readAll(fs, "/bin.dat", st) == content);
}

TEST_CASE("MemFileStore seek 与 tell", "[mem][io]")
{
    MemFileStore fs;
    fs.putFile("/s.bin", "0123456789");
    Status st;
    auto r = fs.openRead("/s.bin", st);
    REQUIRE(r);
    REQUIRE(r->seek(4));
    char buf[3] = {};
    REQUIRE(r->read(buf, 3, st) == 3);
    REQUIRE(std::string(buf, 3) == "456");
    REQUIRE(r->tell() == 7);
}

TEST_CASE("MemFileStore seek 越界被拒绝", "[mem][io][error]")
{
    // 越界 seek 静默返回 0 会让调用方以为"正常到文件尾"，
    // 传输引擎就可能把短文件当完整文件。必须显式失败。
    MemFileStore fs;
    fs.putFile("/s.bin", "abc");
    Status st;
    auto r = fs.openRead("/s.bin", st);
    REQUIRE(r);
    REQUIRE_FALSE(r->seek(100));
}

TEST_CASE("MemFileStore resize 截断与补零", "[mem][io]")
{
    MemFileStore fs;
    fs.putFile("/r.bin", "0123456789");

    REQUIRE(fs.resize("/r.bin", 4));
    REQUIRE(fs.getFile("/r.bin") == "0123");

    REQUIRE(fs.resize("/r.bin", 8));
    const auto out = fs.getFile("/r.bin");
    REQUIRE(out.size() == 8);
    REQUIRE(out.substr(0, 4) == "0123");
    REQUIRE(out[4] == '\0');
    REQUIRE(out[7] == '\0');
}

TEST_CASE("MemFileStore 打开不存在的文件返回 kNotFound", "[mem][io][error]")
{
    MemFileStore fs;
    Status st;
    REQUIRE_FALSE(fs.openRead("/nope", st));
    REQUIRE(st.code == ErrorCode::kNotFound);
}

TEST_CASE("MemFileStore 追加模式要求文件已存在", "[mem][io][error]")
{
    MemFileStore fs;
    Status st;
    REQUIRE_FALSE(fs.openWrite("/nope", false, st));
    REQUIRE(st.code == ErrorCode::kNotFound);
}

TEST_CASE("MemFileStore 不能把目录当文件写", "[mem][io][error]")
{
    MemFileStore fs;
    fs.putDir("/d");
    Status st;
    REQUIRE_FALSE(fs.openWrite("/d", true, st));
    REQUIRE(st.code == ErrorCode::kIsADirectory);
}

// ---------------------------------------------------------------------------
// 目录
// ---------------------------------------------------------------------------

TEST_CASE("MemFileStore list 非递归只给直接子项", "[mem][dir]")
{
    MemFileStore fs;
    fs.putFile("/d/a.txt", "1");
    fs.putFile("/d/sub/b.txt", "22");

    std::vector<DirEntry> out;
    REQUIRE(fs.list("/d", false, out));
    REQUIRE(out.size() == 2);   // a.txt 与 sub

    bool sawSub = false;
    bool sawA = false;
    for (const auto& e : out) {
        if (e.name == "sub") {
            sawSub = e.isDir;
        }
        if (e.name == "a.txt") {
            sawA = !e.isDir && e.size == 1;
        }
    }
    REQUIRE(sawSub);
    REQUIRE(sawA);
}

TEST_CASE("MemFileStore list 递归给全部后代", "[mem][dir]")
{
    MemFileStore fs;
    fs.putFile("/d/a.txt", "1");
    fs.putFile("/d/sub/b.txt", "22");
    fs.putFile("/d/sub/deep/c.txt", "333");

    std::vector<DirEntry> out;
    REQUIRE(fs.list("/d", true, out));
    // d/a.txt, d/sub, d/sub/b.txt, d/sub/deep, d/sub/deep/c.txt
    REQUIRE(out.size() == 5);
}

TEST_CASE("MemFileStore list 对文件返回 kNotADirectory", "[mem][dir][error]")
{
    MemFileStore fs;
    fs.putFile("/f.txt", "x");
    std::vector<DirEntry> out;
    const auto st = fs.list("/f.txt", false, out);
    REQUIRE_FALSE(st);
    REQUIRE(st.code == ErrorCode::kNotADirectory);
}

TEST_CASE("MemFileStore mkdirs 幂等且拒绝穿越文件", "[mem][dir]")
{
    MemFileStore fs;
    REQUIRE(fs.mkdirs("/x/y/z"));
    REQUIRE(fs.mkdirs("/x/y/z"));
    REQUIRE(fs.stat("/x/y/z").isDir);

    fs.putFile("/blocker", "f");
    REQUIRE_FALSE(fs.mkdirs("/blocker/sub"));
}

// ---------------------------------------------------------------------------
// 变更
// ---------------------------------------------------------------------------

TEST_CASE("MemFileStore atomicReplace 覆盖目标", "[mem][atomic]")
{
    MemFileStore fs;
    fs.putFile("/tmp/part", "new");
    fs.putFile("/final", "old");
    REQUIRE(fs.atomicReplace("/tmp/part", "/final"));
    REQUIRE(fs.getFile("/final") == "new");
    REQUIRE_FALSE(fs.has("/tmp/part"));
}

TEST_CASE("MemFileStore atomicReplace 源不存在则失败", "[mem][atomic][error]")
{
    MemFileStore fs;
    const auto st = fs.atomicReplace("/nope", "/final");
    REQUIRE_FALSE(st);
    REQUIRE(st.code == ErrorCode::kNotFound);
}

TEST_CASE("MemFileStore remove 不存在视为成功", "[mem][remove]")
{
    MemFileStore fs;
    REQUIRE(fs.remove("/never"));
}

TEST_CASE("MemFileStore remove 目录会连子项一起删", "[mem][remove]")
{
    MemFileStore fs;
    fs.putFile("/d/a.txt", "1");
    fs.putFile("/d/sub/b.txt", "2");
    REQUIRE(fs.remove("/d"));
    REQUIRE_FALSE(fs.has("/d/a.txt"));
    REQUIRE_FALSE(fs.has("/d/sub/b.txt"));
    REQUIRE_FALSE(fs.has("/d"));
}

// ---------------------------------------------------------------------------
// 指纹：必须与 StdFileStore 一致
// ---------------------------------------------------------------------------

TEST_CASE("两个实现的指纹算法一致", "[mem][fingerprint][compat]")
{
    // 这条是整份文件里最重要的一条。
    // 如果 MemFileStore 与 StdFileStore 的指纹不一致，所有基于内存实现
    // 写的续传测试都在验证一个假的东西。
    TempDir dir;
    StdFileStore real;
    MemFileStore mem;

    const std::string content = "the quick brown fox jumps over the lazy dog";
    const std::string realPath = dir.path("f.bin");
    REQUIRE(writeFile(realPath, content));
    mem.putFile("/f.bin", content);

    REQUIRE(real.quickFingerprint(realPath) == mem.quickFingerprint("/f.bin"));

    // 残片 + sizeOverride 的情形也要一致
    const std::string partPath = dir.path("f.relaypart");
    REQUIRE(writeFile(partPath, content.substr(0, 10)));
    mem.putFile("/f.relaypart", content.substr(0, 10));

    const auto expect = static_cast<std::int64_t>(content.size());
    REQUIRE(real.quickFingerprint(partPath, expect) == mem.quickFingerprint("/f.relaypart", expect));
}

TEST_CASE("MemFileStore 指纹区分内容与长度", "[mem][fingerprint]")
{
    MemFileStore fs;
    fs.putFile("/a", "abc");
    fs.putFile("/b", "abd");
    fs.putFile("/c", "abcd");
    REQUIRE(fs.quickFingerprint("/a") != fs.quickFingerprint("/b"));
    REQUIRE(fs.quickFingerprint("/a") != fs.quickFingerprint("/c"));
    REQUIRE(fs.quickFingerprint("/missing").empty());
}

// ---------------------------------------------------------------------------
// 故障注入
// ---------------------------------------------------------------------------

TEST_CASE("故障注入：无条件失败", "[mem][fault]")
{
    MemFileStore fs;
    fs.injectFault(FaultInjection{"/save/", 0, ErrorCode::kNoSpace, false});

    Status st;
    REQUIRE_FALSE(fs.openWrite("/save/out.bin", true, st));
    // openWrite 的故障在打开时抛出
    REQUIRE(st.code == ErrorCode::kNoSpace);
}

TEST_CASE("故障注入：按路径前缀隔离", "[mem][fault]")
{
    MemFileStore fs;
    fs.injectFault(FaultInjection{"/save/", 0, ErrorCode::kNoSpace, false});

    Status st;
    // 其它路径不受影响——这一点很重要，否则没法测"任务 A 失败不影响任务 B"
    REQUIRE(writeAll(fs, "/other/x.bin", "ok", st));
    REQUIRE(st);
    REQUIRE_FALSE(writeAll(fs, "/save/x.bin", "fail", st));
}

TEST_CASE("故障注入：只触发一次", "[mem][fault]")
{
    MemFileStore fs;
    fs.injectFault(FaultInjection{"/f", 0, ErrorCode::kBusy, true});

    Status st;
    REQUIRE_FALSE(fs.openWrite("/f", true, st));
    REQUIRE(st.code == ErrorCode::kBusy);

    // 第二次应当成功——模拟"文件被占用一会儿后释放"
    REQUIRE(writeAll(fs, "/f", "ok", st));
    REQUIRE(st);
}

TEST_CASE("故障注入：afterOps 模拟写到一半磁盘满", "[mem][fault][partial]")
{
    MemFileStore fs;
    // 前 2 次写操作成功，第 3 次开始失败。
    // 这是"写到一半磁盘满"的最小可复现模型。
    fs.injectFault(FaultInjection{"", 2, ErrorCode::kNoSpace, false});

    Status st;
    auto w = fs.openWrite("/half.bin", true, st);
    REQUIRE(w);

    REQUIRE(w->write("AAAA", 4));
    REQUIRE(w->write("BBBB", 4));
    const auto third = w->write("CCCC", 4);
    REQUIRE_FALSE(third);
    REQUIRE(third.code == ErrorCode::kNoSpace);

    // 失败后已写入的部分仍然在（真实磁盘也是这样，不会回滚）
    w.reset();
    REQUIRE(fs.getFile("/half.bin") == "AAAABBBB");
}

TEST_CASE("容量上限模拟磁盘满", "[mem][fault][capacity]")
{
    MemFileStore fs;
    fs.setCapacity(10);

    Status st;
    REQUIRE(writeAll(fs, "/a", "12345", st));
    // 还剩 5 字节
    REQUIRE(writeAll(fs, "/b", "67890", st));
    REQUIRE(fs.usedBytes() == 10);

    // 再写就超了
    REQUIRE_FALSE(writeAll(fs, "/c", "1", st));
    REQUIRE(st.code == ErrorCode::kNoSpace);
}

TEST_CASE("容量上限允许覆盖同一文件而不超限", "[mem][fault][capacity]")
{
    // 覆盖写不应把旧内容也算进容量，否则续传时会莫名其妙失败
    MemFileStore fs;
    fs.setCapacity(10);
    Status st;
    REQUIRE(writeAll(fs, "/a", "1234567890", st));
    REQUIRE(writeAll(fs, "/a", "abc", st));
    REQUIRE(fs.getFile("/a") == "abc");
}

TEST_CASE("关闭 flushOnClose 模拟进程崩溃丢缓冲", "[mem][fault]")
{
    MemFileStore fs;
    fs.setFlushOnClose(false);
    Status st;
    auto w = fs.openWrite("/lost.bin", true, st);
    REQUIRE(w);
    REQUIRE(w->write("data", 4));
    w.reset();   // 不 flush

    // 数据没落盘——模拟"写流还没 flush 进程就挂了"
    REQUIRE(fs.getFile("/lost.bin").empty());
}

// ---------------------------------------------------------------------------
// 与真实实现的行为对齐
// ---------------------------------------------------------------------------

TEST_CASE("两个实现对目录与缺失路径的判定一致", "[mem][compat]")
{
    TempDir dir;
    StdFileStore real;
    MemFileStore mem;

    const std::string realDir = dir.utf8() + "/d";
    REQUIRE(real.mkdirs(realDir));
    mem.putDir("/d");

    REQUIRE(real.stat(realDir).isDir == mem.stat("/d").isDir);
    REQUIRE_FALSE(real.stat(realDir + "/missing").exists);
    REQUIRE_FALSE(mem.stat("/d/missing").exists);

    std::vector<DirEntry> a;
    std::vector<DirEntry> b;
    REQUIRE(real.list(realDir, false, a));
    REQUIRE(mem.list("/d", false, b));
    REQUIRE(a.empty());
    REQUIRE(b.empty());
}

TEST_CASE("两个实现都拒绝把目录作文件读", "[mem][compat]")
{
    TempDir dir;
    StdFileStore real;
    MemFileStore mem;
    REQUIRE(real.mkdirs(dir.utf8() + "/d"));
    mem.putDir("/d");

    Status a;
    Status b;
    REQUIRE_FALSE(real.openRead(dir.utf8() + "/d", a));
    REQUIRE_FALSE(mem.openRead("/d", b));
    // 具体错误码允许有差异（平台相关），但都必须是失败
    REQUIRE_FALSE(a);
    REQUIRE_FALSE(b);
}

TEST_CASE("allFiles 返回排序后的文件列表", "[mem][helper]")
{
    MemFileStore fs;
    fs.putFile("/z.txt", "1");
    fs.putFile("/a.txt", "2");
    fs.putFile("/m/inner.txt", "3");
    // allFiles 来自 std::map 的键遍历，天然按字典序
    const std::vector<std::string> expected{"/a.txt", "/m/inner.txt", "/z.txt"};
    REQUIRE(fs.allFiles() == expected);
}
