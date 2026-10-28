#include "relayer/filestore/StdFileStore.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "support/TempDir.h"

using relayer::filestore::DirEntry;
using relayer::filestore::ErrorCode;
using relayer::filestore::FileStat;
using relayer::filestore::PathPolicy;
using relayer::filestore::Status;
using relayer::filestore::StdFileStore;
using relayer::test::TempDir;
using relayer::test::writeFile;

namespace {

std::vector<char> readAll(StdFileStore& fs, const std::string& path, Status& st)
{
    std::vector<char> out;
    auto stream = fs.openRead(path, st);
    if (!stream) {
        return out;
    }
    char buf[4096];
    for (;;) {
        const std::size_t n = stream->read(buf, sizeof(buf), st);
        out.insert(out.end(), buf, buf + n);
        if (!st) {
            return {};
        }
        if (n == 0) {
            break;
        }
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------

TEST_CASE("StdFileStore 的路径方法委托给 PathPolicy", "[filestore][path]")
{
    // 这里不做穷举：路径规则本身的测试全在 PathPolicyTest.cpp。
    // 本用例只验证"委托确实接上了"，防止实现被换掉后静默偏离。
    StdFileStore fs;
    REQUIRE(fs.normalize("a/./b/../c") == "a/c");
    REQUIRE(fs.join("a/", "/b") == "a/b");
    REQUIRE(fs.resolveWithin("/root", "../escape").empty());
    REQUIRE(fs.resolveWithin("/root", "/abs").empty());
    REQUIRE_FALSE(fs.resolveWithin("/root", "sub/file.txt").empty());
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

TEST_CASE("写入后读回内容一致", "[filestore][io]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string path = dir.path("hello.txt");
    // 刻意包含 \0，验证写入长度按 size 而非 strlen 计算
    const std::string content("relayer\0with\0nul", 16);

    Status st;
    auto w = fs.openWrite(path, true, st);
    REQUIRE(w);
    REQUIRE(w->write(content.data(), content.size()));
    REQUIRE(w->flush());
    REQUIRE(w->sync());
    w.reset();

    const auto back = readAll(fs, path, st);
    REQUIRE(st);
    REQUIRE(back.size() == content.size());
    REQUIRE(std::string(back.data(), back.size()) == content);
}

TEST_CASE("空文件可正常打开与读取", "[filestore][io][edge]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string path = dir.path("empty.bin");

    Status st;
    auto w = fs.openWrite(path, true, st);
    REQUIRE(w);
    w.reset();

    const auto back = readAll(fs, path, st);
    REQUIRE(st);
    REQUIRE(back.empty());
    REQUIRE(fs.stat(path).size == 0);
}

TEST_CASE("seek 定位后读到正确片段", "[filestore][io]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string path = dir.path("seek.bin");
    REQUIRE(writeFile(path, "0123456789"));

    Status st;
    auto r = fs.openRead(path, st);
    REQUIRE(r);
    REQUIRE(r->seek(3));
    char buf[4] = {};
    const std::size_t n = r->read(buf, 4, st);
    REQUIRE(st);
    REQUIRE(n == 4);
    REQUIRE(std::string(buf, 4) == "3456");
    REQUIRE(r->tell() == 7);
}

TEST_CASE("resize 截断与扩展", "[filestore][io]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string path = dir.path("resize.bin");
    REQUIRE(writeFile(path, "0123456789"));

    REQUIRE(fs.resize(path, 4));
    REQUIRE(fs.stat(path).size == 4);

    REQUIRE(fs.resize(path, 8));
    FileStat st = fs.stat(path);
    REQUIRE(st.size == 8);

    Status err;
    const auto back = readAll(fs, path, err);
    REQUIRE(err);
    REQUIRE(back.size() == 8);
    REQUIRE(std::string(back.data(), 4) == "0123");
    // 扩展部分是零字节
    REQUIRE(back[4] == '\0');
    REQUIRE(back[7] == '\0');
}

TEST_CASE("打开不存在的文件返回 kNotFound", "[filestore][io][error]")
{
    TempDir dir;
    StdFileStore fs;
    Status st;
    auto r = fs.openRead(dir.path("nope.bin"), st);
    REQUIRE_FALSE(r);
    REQUIRE(st.code == ErrorCode::kNotFound);
}

TEST_CASE("非 UTF-8 与中文路径都能正确落盘", "[filestore][io][unicode]")
{
    TempDir dir;
    StdFileStore fs;
    // 中文 + 空格 + emoji，覆盖 UTF-8 多字节与宽字符转换
    const std::string leaf = "测试 文件 \xF0\x9F\x93\x81.dat";
    const std::string path = dir.utf8() + "/" + leaf;
    REQUIRE(writeFile(path, "content"));

    FileStat st = fs.stat(path);
    REQUIRE(st.exists);
    REQUIRE(st.size == 7);
    REQUIRE(PathPolicy{}.fileName(path) == leaf);

    Status err;
    const auto back = readAll(fs, path, err);
    REQUIRE(err);
    REQUIRE(std::string(back.begin(), back.end()) == "content");
}

// ---------------------------------------------------------------------------
// 元信息与探测
// ---------------------------------------------------------------------------

TEST_CASE("stat 对不存在路径返回 exists=false 而非失败", "[filestore][stat]")
{
    TempDir dir;
    StdFileStore fs;
    const FileStat st = fs.stat(dir.path("absent"));
    REQUIRE_FALSE(st.exists);
}

TEST_CASE("stat 区分文件与目录", "[filestore][stat]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string sub = dir.path("sub");
    REQUIRE(fs.mkdirs(sub));
    REQUIRE(writeFile(dir.path("f.bin"), "abc"));

    REQUIRE(fs.stat(sub).exists);
    REQUIRE(fs.stat(sub).isDir);
    REQUIRE_FALSE(fs.stat(dir.path("f.bin")).isDir);
    REQUIRE(fs.stat(dir.path("f.bin")).size == 3);
}

TEST_CASE("probeWritable 对可写文件与缺失父目录给出不同结论", "[filestore][probe]")
{
    TempDir dir;
    StdFileStore fs;

    // 已存在的可写文件
    const std::string existing = dir.path("ok.bin");
    REQUIRE(writeFile(existing, "x"));
    REQUIRE(fs.probeWritable(existing));

    // 文件不存在但父目录在：可创建
    REQUIRE(fs.probeWritable(dir.path("new.bin")));

    // 父目录不存在
    const Status st = fs.probeWritable(dir.path("nodir/new.bin"));
    REQUIRE_FALSE(st);
    REQUIRE(st.code == ErrorCode::kNotFound);
}

// ---------------------------------------------------------------------------
// 目录
// ---------------------------------------------------------------------------

TEST_CASE("list 非递归只返回直接子项", "[filestore][dir]")
{
    TempDir dir;
    StdFileStore fs;
    REQUIRE(fs.mkdirs(dir.path("a")));
    REQUIRE(writeFile(dir.path("a/x.bin"), "1"));
    REQUIRE(writeFile(dir.path("y.bin"), "2"));

    std::vector<DirEntry> entries;
    REQUIRE(fs.list(dir.utf8(), false, entries));
    REQUIRE(entries.size() == 2);

    bool sawDir = false;
    bool sawFile = false;
    for (const auto& e : entries) {
        if (e.name == "a") {
            sawDir = e.isDir;
        }
        if (e.name == "y.bin") {
            sawFile = !e.isDir && e.size == 1;
        }
    }
    REQUIRE(sawDir);
    REQUIRE(sawFile);
}

TEST_CASE("list 递归返回全部后代", "[filestore][dir]")
{
    TempDir dir;
    StdFileStore fs;
    REQUIRE(fs.mkdirs(dir.path("a/b/c")));
    REQUIRE(writeFile(dir.path("a/b/c/deep.bin"), "123"));

    std::vector<DirEntry> entries;
    REQUIRE(fs.list(dir.utf8(), true, entries));
    // a、a/b、a/b/c、a/b/c/deep.bin
    REQUIRE(entries.size() == 4);
}

TEST_CASE("list 对文件路径返回 kNotADirectory", "[filestore][dir][error]")
{
    TempDir dir;
    StdFileStore fs;
    REQUIRE(writeFile(dir.path("f.bin"), "x"));
    std::vector<DirEntry> entries;
    const Status st = fs.list(dir.path("f.bin"), false, entries);
    REQUIRE_FALSE(st);
    REQUIRE(st.code == ErrorCode::kNotADirectory);
}

TEST_CASE("mkdirs 幂等", "[filestore][dir]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string deep = dir.path("x/y/z");
    REQUIRE(fs.mkdirs(deep));
    REQUIRE(fs.mkdirs(deep));
    REQUIRE(fs.stat(deep).isDir);
}

// ---------------------------------------------------------------------------
// 变更
// ---------------------------------------------------------------------------

TEST_CASE("atomicReplace 覆盖已存在的目标", "[filestore][atomic]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string from = dir.path("part.tmp");
    const std::string to = dir.path("final.bin");
    REQUIRE(writeFile(from, "new"));
    REQUIRE(writeFile(to, "old-content"));

    REQUIRE(fs.atomicReplace(from, to));
    REQUIRE_FALSE(fs.stat(from).exists);

    Status st;
    const auto back = readAll(fs, to, st);
    REQUIRE(st);
    REQUIRE(std::string(back.begin(), back.end()) == "new");
}

TEST_CASE("remove 对不存在的路径视为成功", "[filestore][remove]")
{
    TempDir dir;
    StdFileStore fs;
    REQUIRE(fs.remove(dir.path("never-existed")));
}

// ---------------------------------------------------------------------------
// 指纹
// ---------------------------------------------------------------------------

TEST_CASE("quickFingerprint 内容相同则一致、不同则不同", "[filestore][fingerprint]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string a = dir.path("a.bin");
    const std::string b = dir.path("b.bin");
    const std::string c = dir.path("c.bin");
    REQUIRE(writeFile(a, "same-content"));
    REQUIRE(writeFile(b, "same-content"));
    REQUIRE(writeFile(c, "diff-content"));

    const std::string fa = fs.quickFingerprint(a);
    REQUIRE_FALSE(fa.empty());
    REQUIRE(fa == fs.quickFingerprint(b));
    REQUIRE(fa != fs.quickFingerprint(c));
}

TEST_CASE("quickFingerprint 的空文件与非空文件指纹不同", "[filestore][fingerprint]")
{
    TempDir dir;
    StdFileStore fs;
    const std::string empty = dir.path("empty.bin");
    const std::string one = dir.path("one.bin");
    REQUIRE(writeFile(empty, ""));
    REQUIRE(writeFile(one, "a"));

    REQUIRE_FALSE(fs.quickFingerprint(empty).empty());
    REQUIRE(fs.quickFingerprint(empty) != fs.quickFingerprint(one));
}

TEST_CASE("quickFingerprint 用 sizeOverride 匹配残片归属", "[filestore][fingerprint][resume]")
{
    // 续传场景：残片只下了一半，自身长度不等于源文件长度。
    // 指纹把"源文件总长"和"已存在的有效内容长度"都混进哈希，因此
    //   1) 不带 sizeOverride → 总长不同，指纹必然不同；
    //   2) 带 sizeOverride  → 总长相同，指纹也仍然不同（valid 长度不同），
    //      但两者差异是**确定且可解释**的：内容前缀一致。
    // 真正的续传判断不该靠"指纹是否完全相等"，而是靠 size + 首段内容一致，
    // 接口注释里写的就是这个语义——这里验证的是接口行为符合文档。
    TempDir dir;
    StdFileStore fs;
    const std::string src = dir.path("src.bin");
    const std::string part = dir.path("src.bin.relaypart");
    const std::string content = "0123456789ABCDEF";
    REQUIRE(writeFile(src, content));
    REQUIRE(writeFile(part, content.substr(0, 6)));   // 只写了前 6 字节

    const std::string srcFp = fs.quickFingerprint(src);
    REQUIRE_FALSE(srcFp.empty());

    // 不带 override：残片按自己的长度（6）计算，与源文件不同
    REQUIRE(fs.quickFingerprint(part) != srcFp);

    // 带 override：总长一致，但已存在长度不同，指纹仍不同——
    // 说明指纹能区分"下完了"和"下了一半"，不会把残片误判成完整文件
    REQUIRE(fs.quickFingerprint(part, static_cast<std::int64_t>(content.size())) != srcFp);

    // 残片补全成完整内容后，指纹才与源文件一致
    REQUIRE(writeFile(part, content));
    REQUIRE(fs.quickFingerprint(part, static_cast<std::int64_t>(content.size())) == srcFp);
}

TEST_CASE("quickFingerprint 对不存在的文件返回空串", "[filestore][fingerprint][error]")
{
    TempDir dir;
    StdFileStore fs;
    REQUIRE(fs.quickFingerprint(dir.path("missing.bin")).empty());
}
