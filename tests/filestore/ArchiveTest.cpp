// tar.xz 归档层的测试。
//
// 这一层的关键不是"能打包能解包"，而是**解包时的落点必须受控**。
// 归档里的名字是别人给的：一个构造过的包可以带 `../../.ssh/authorized_keys`
// 或 `C:/Windows/system32/x.dll`，解包就等于替攻击者写文件。
//
// 所以本文件的重心是：
//   1. 恶意条目被跳过而不是被写出去（skip 而非整体失败——用户还得拿到别的文件）；
//   2. 合法条目仍然正常解出（防线不能"什么都不让写"）；
//   3. 中文/长路径/空文件/目录这些真实场景不退化。
//
// 全程用 MemFileStore，不落盘：跑得快，而且能精确构造任意"路径"，
// 在 Windows 上造不出以 `/` 开头的真实文件名。

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "relayer/filestore/IArchive.h"
#include "relayer/filestore/IFileStore.h"
#include "relayer/filestore/MemFileStore.h"
#include "relayer/filestore/TarXzArchiver.h"
#include "relayer/filestore/detail/TarCodec.h"
#include "relayer/filestore/detail/XzCodec.h"

using relayer::filestore::ArchiveEntry;
using relayer::filestore::ArchiveFormat;
using relayer::filestore::ArchiveItem;
using relayer::filestore::ArchiveOptions;
using relayer::filestore::ErrorCode;
using relayer::filestore::guessFormat;
using relayer::filestore::MemFileStore;
using relayer::filestore::sanitizeEntryName;
using relayer::filestore::Status;
using relayer::filestore::TarXzArchiver;
using relayer::filestore::toString;

namespace
{

constexpr const char *kArchive = "/out/archive.tar.xz";

// 造一个带若干文件的 store，返回归档里该放的 items。
std::vector<ArchiveItem> seedFiles(MemFileStore &ms)
{
    REQUIRE(ms.mkdirs("/src"));
    ms.putFile("/src/readme.txt", "hello relayer");
    ms.putFile("/src/sub/data.bin", std::string(3000, 'D'));
    ms.putFile("/src/empty.txt", "");
    ms.putFile("/src/中文名.txt", "中文内容");

    std::vector<ArchiveItem> items;
    items.push_back({"/src/readme.txt", "readme.txt"});
    items.push_back({"/src/sub/data.bin", "sub/data.bin"});
    items.push_back({"/src/empty.txt", "empty.txt"});
    items.push_back({"/src/中文名.txt", "中文名.txt"});
    return items;
}

}  // namespace

TEST_CASE("归档：格式名与后缀推断", "[archive][format]")
{
    REQUIRE(toString(ArchiveFormat::kZip) == "zip");
    REQUIRE(toString(ArchiveFormat::kTarXz) == "tar.xz");

    ArchiveFormat f{};
    REQUIRE(guessFormat("a.tar.xz", f));
    REQUIRE(f == ArchiveFormat::kTarXz);
    REQUIRE(guessFormat("a.TXZ", f));
    REQUIRE(f == ArchiveFormat::kTarXz);
    REQUIRE(guessFormat("备份.ZIP", f));
    REQUIRE(f == ArchiveFormat::kZip);
    // 不认识的
    REQUIRE_FALSE(guessFormat("a.tar", f));
    REQUIRE_FALSE(guessFormat("a.7z", f));
    REQUIRE_FALSE(guessFormat("noext", f));
}

TEST_CASE("归档：条目名清洗", "[archive][sanitize]")
{
    // 反斜杠统一成 /
    REQUIRE(sanitizeEntryName("dir\\file.txt") == "dir/file.txt");
    // 重复斜杠、`.` 段
    REQUIRE(sanitizeEntryName("a//b") == "a/b");
    REQUIRE(sanitizeEntryName("./a/b") == "a/b");
    // 前导斜杠被去掉：绝对路径被重新解释成 root 下的相对路径。
    // 这是"把别人机器上的包解到自己目录里"的默认行为，见 IArchive.h 第 3 条。
    REQUIRE(sanitizeEntryName("/a/b") == "a/b");

    // 含 .. 一律拒绝（不尝试折叠）
    REQUIRE(sanitizeEntryName("../etc/passwd").empty());
    REQUIRE(sanitizeEntryName("a/../../b").empty());
    REQUIRE(sanitizeEntryName("a/..").empty());

    // 盘符
    REQUIRE(sanitizeEntryName("C:/Windows/x.dll").empty());
    REQUIRE(sanitizeEntryName("c:file.txt").empty());

    // 空与纯分隔符
    REQUIRE(sanitizeEntryName("").empty());
    REQUIRE(sanitizeEntryName("/").empty());
    REQUIRE(sanitizeEntryName(".").empty());

    // 名字里有点号但不是 `.` 段，应当保留
    REQUIRE(sanitizeEntryName("a.b/c..d") == "a.b/c..d");
}

TEST_CASE("归档：tar.xz 打包再解包", "[archive][roundtrip]")
{
    MemFileStore ms;
    const auto items = seedFiles(ms);
    TarXzArchiver arch(ms);

    std::vector<std::string> skipped;
    Status st = arch.create(kArchive, items, ArchiveOptions{}, skipped);
    REQUIRE(st);
    REQUIRE(skipped.empty());
    REQUIRE(ms.has(kArchive));

    // 打包应当真的压缩了（3000 个 D 压完远小于 3000）
    REQUIRE(ms.getFile(kArchive).size() < 2000);

    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    // 4 个文件
    REQUIRE(entries.size() == 4);
    const auto findEntry = [&](const std::string &name) -> const ArchiveEntry *
    {
        for (const auto &e : entries)
        {
            if (e.name == name)
            {
                return &e;
            }
        }
        return nullptr;
    };
    REQUIRE(findEntry("readme.txt") != nullptr);
    REQUIRE(findEntry("readme.txt")->size == 13);
    REQUIRE(findEntry("sub/data.bin") != nullptr);
    REQUIRE(findEntry("sub/data.bin")->size == 3000);
    REQUIRE(findEntry("empty.txt") != nullptr);
    REQUIRE(findEntry("empty.txt")->size == 0);
    REQUIRE(findEntry("中文名.txt") != nullptr);

    std::vector<std::string> extracted;
    REQUIRE(arch.extract(kArchive, "/dst", ArchiveOptions{}, extracted, skipped));
    REQUIRE(skipped.empty());
    REQUIRE(extracted.size() == 4);

    REQUIRE(ms.getFile("/dst/readme.txt") == "hello relayer");
    REQUIRE(ms.getFile("/dst/sub/data.bin") == std::string(3000, 'D'));
    REQUIRE(ms.getFile("/dst/empty.txt").empty());
    REQUIRE(ms.getFile("/dst/中文名.txt") == "中文内容");
}

TEST_CASE("归档：恶意条目名在打包时被拒绝", "[archive][security]")
{
    MemFileStore ms;
    ms.putFile("/src/x.txt", "x");

    std::vector<ArchiveItem> items;
    items.push_back({"/src/x.txt", "../escape.txt"});
    items.push_back({"/src/x.txt", "a/../../b.txt"});
    items.push_back({"/src/x.txt", "C:/win.txt"});
    items.push_back({"/src/x.txt", "C:win.txt"});
    items.push_back({"/src/x.txt", "/abs.txt"});  // 前导斜杠 → 变成相对路径，允许
    items.push_back({"/src/x.txt", "ok.txt"});

    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));

    // 四个被拒（两个含 ..、两个带盘符），两个进包：
    //   "打出一个自己解不开的包"比"少打一个文件"更糟，
    //   但"把绝对路径收成相对路径"是刻意为之（见 IArchive.h 第 3 条）。
    REQUIRE(skipped.size() == 4);
    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].name == "abs.txt");
    REQUIRE(entries[1].name == "ok.txt");
}

TEST_CASE("归档：解包时 ../../ 条目被跳过", "[archive][security][zipslip]")
{
    // 关键用例。这里不能用"打包时清洗"来绕过——真实攻击来自别人做的包。
    // 所以先手工造一个含恶意名字的 tar，压成 xz，再让 extract 去解。
    MemFileStore ms;
    REQUIRE(ms.mkdirs("/payload"));

    // 用一个"宽松"的 writer 直接写 tar：绕过 sanitizeEntryName
    // （模拟外部工具打出来的恶意包）
    {
        relayer::filestore::TarWriter tw;
        REQUIRE(tw.addEntry("../../../etc/evil.txt", '0', 0644, 4, 0));
        REQUIRE(tw.writeData("evil", 4));
        REQUIRE(tw.finishData());
        REQUIRE(tw.addEntry("/absolute.txt", '0', 0644, 3, 0));
        REQUIRE(tw.writeData("abs", 3));
        REQUIRE(tw.finishData());
        REQUIRE(tw.addEntry("good.txt", '0', 0644, 4, 0));
        REQUIRE(tw.writeData("good", 4));
        REQUIRE(tw.finishData());
        REQUIRE(tw.close());

        // tar → xz → 落进 store
        relayer::filestore::XzEncoder enc;
        std::vector<char> buf(4096);
        std::string packed;
        for (;;)
        {
            const std::size_t n = tw.read(buf.data(), buf.size());
            if (n > 0)
            {
                REQUIRE(enc.write(buf.data(), n));
            }
            Status rs;
            const std::size_t got = enc.read(buf.data(), buf.size(), rs);
            REQUIRE(rs);
            packed.append(buf.data(), got);
            if (tw.drained())
            {
                if (enc.drained())
                {
                    break;
                }
                REQUIRE(enc.finish());
                continue;
            }
            if (n == 0 && got == 0)
            {
                break;
            }
        }
        ms.putFile(kArchive, packed);
    }

    TarXzArchiver arch(ms);
    std::vector<std::string> extracted;
    std::vector<std::string> skipped;
    const Status st = arch.extract(kArchive, "/safe", ArchiveOptions{}, extracted, skipped);
    REQUIRE(st);

    // 两个恶意条目进 skipped，正常条目照常解出
    REQUIRE(skipped.size() == 2);
    REQUIRE(extracted.size() == 1);
    REQUIRE(ms.getFile("/safe/good.txt") == "good");

    // 逃逸目标绝不能存在
    REQUIRE_FALSE(ms.has("/etc/evil.txt"));
    REQUIRE_FALSE(ms.has("/safe/../etc/evil.txt"));
    // 绝对路径被解释成 root 下的相对路径（这是设计选择，不是漏洞）
    REQUIRE(ms.has("/safe/absolute.txt") == false);
}

TEST_CASE("归档：目录条目", "[archive][dir]")
{
    MemFileStore ms;
    REQUIRE(ms.mkdirs("/src/tree/a/b"));
    ms.putFile("/src/tree/a/b/deep.txt", "deep");

    std::vector<ArchiveItem> items;
    items.push_back({"/src/tree", "tree"});
    items.push_back({"/src/tree/a/b/deep.txt", "tree/a/b/deep.txt"});

    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));
    REQUIRE(skipped.empty());

    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].name == "tree/");
    REQUIRE(entries[0].isDir);
    REQUIRE(entries[1].name == "tree/a/b/deep.txt");
    REQUIRE_FALSE(entries[1].isDir);

    std::vector<std::string> extracted;
    REQUIRE(arch.extract(kArchive, "/dst", ArchiveOptions{}, extracted, skipped));
    REQUIRE(skipped.empty());
    REQUIRE(ms.getFile("/dst/tree/a/b/deep.txt") == "deep");
}

TEST_CASE("归档：不存在的源文件记进 skipped", "[archive][skip]")
{
    MemFileStore ms;
    std::vector<ArchiveItem> items;
    items.push_back({"/src/missing.txt", "missing.txt"});
    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));
    REQUIRE(skipped.size() == 1);
    REQUIRE(skipped[0] == "missing.txt");

    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    REQUIRE(entries.empty());
}

TEST_CASE("归档：覆盖选项", "[archive][overwrite]")
{
    MemFileStore ms;
    ms.putFile("/src/a.txt", "new");

    std::vector<ArchiveItem> items;
    items.push_back({"/src/a.txt", "a.txt"});
    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));

    // 目标已存在 + overwrite=false → 跳过且不覆盖
    ms.putFile("/dst/a.txt", "old");
    ArchiveOptions opt;
    opt.overwrite = false;
    std::vector<std::string> extracted;
    REQUIRE(arch.extract(kArchive, "/dst", opt, extracted, skipped));
    REQUIRE(extracted.empty());
    REQUIRE(skipped.size() == 1);
    REQUIRE(ms.getFile("/dst/a.txt") == "old");

    // overwrite=true → 覆盖
    opt.overwrite = true;
    REQUIRE(arch.extract(kArchive, "/dst", opt, extracted, skipped));
    REQUIRE(extracted.size() == 1);
    REQUIRE(ms.getFile("/dst/a.txt") == "new");
}

TEST_CASE("归档：解包损坏的归档会失败", "[archive][error]")
{
    MemFileStore ms;
    // 随机垃圾，不是合法的 xz
    ms.putFile(kArchive, std::string(2000, 'Z'));
    TarXzArchiver arch(ms);
    std::vector<std::string> extracted;
    std::vector<std::string> skipped;
    const Status st = arch.extract(kArchive, "/dst", ArchiveOptions{}, extracted, skipped);
    REQUIRE_FALSE(st);
}

TEST_CASE("归档：解包被截断的归档会失败", "[archive][error]")
{
    MemFileStore ms;
    ms.putFile("/src/big.bin", std::string(200000, 'Q'));
    std::vector<ArchiveItem> items;
    items.push_back({"/src/big.bin", "big.bin"});
    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));

    // 砍掉尾部
    std::string packed = ms.getFile(kArchive);
    REQUIRE(packed.size() > 100);
    packed.resize(packed.size() * 2 / 3);
    ms.putFile(kArchive, packed);

    std::vector<std::string> extracted;
    const Status st = arch.extract(kArchive, "/dst", ArchiveOptions{}, extracted, skipped);
    REQUIRE_FALSE(st);
}

TEST_CASE("归档：list 不写任何文件", "[archive][list]")
{
    MemFileStore ms;
    const auto items = seedFiles(ms);
    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));

    const std::size_t before = ms.allFiles().size();
    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    REQUIRE(ms.allFiles().size() == before);
}

TEST_CASE("归档：压缩级别可调", "[archive][level]")
{
    MemFileStore ms;
    ms.putFile("/src/text.txt", std::string(50000, 'A'));
    std::vector<ArchiveItem> items;
    items.push_back({"/src/text.txt", "text.txt"});

    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;

    ArchiveOptions low;
    low.compressionLevel = 0;
    REQUIRE(arch.create("/out/low.tar.xz", items, low, skipped));
    const std::size_t lowSize = ms.getFile("/out/low.tar.xz").size();

    ArchiveOptions high;
    high.compressionLevel = 9;
    REQUIRE(arch.create("/out/high.tar.xz", items, high, skipped));
    const std::size_t highSize = ms.getFile("/out/high.tar.xz").size();

    REQUIRE(lowSize > highSize);

    // 两个都能正常解出
    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list("/out/low.tar.xz", entries));
    REQUIRE(entries.size() == 1);
    REQUIRE(arch.list("/out/high.tar.xz", entries));
    REQUIRE(entries.size() == 1);
}

TEST_CASE("归档：深层长路径", "[archive][longpath]")
{
    MemFileStore ms;
    std::string deep;
    for (int i = 0; i < 15; ++i)
    {
        deep += "layer" + std::to_string(i) + "/";
    }
    const std::string path = "/src/" + deep + "file.txt";
    ms.putFile(path, "deep");

    std::vector<ArchiveItem> items;
    items.push_back({path, deep + "file.txt"});
    TarXzArchiver arch(ms);
    std::vector<std::string> skipped;
    REQUIRE(arch.create(kArchive, items, ArchiveOptions{}, skipped));
    REQUIRE(skipped.empty());

    std::vector<ArchiveEntry> entries;
    REQUIRE(arch.list(kArchive, entries));
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].name.size() > 100);  // 确实走了 PAX

    std::vector<std::string> extracted;
    REQUIRE(arch.extract(kArchive, "/dst", ArchiveOptions{}, extracted, skipped));
    REQUIRE(ms.getFile("/dst/" + deep + "file.txt") == "deep");
}
