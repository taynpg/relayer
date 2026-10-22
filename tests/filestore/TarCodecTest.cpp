// ustar tar 编解码的测试。
//
// 这一层的错法都很隐蔽：头里某个字段少写一位、填充算错 512 的余数、
// 目录条目被当成文件读走 512 字节——表现都是"解出来的文件内容整体错位"，
// 而且要等归档里第二个条目之后才看得出来。所以下面把每个位置都钉一遍。
//
// 另外专门测 PAX 长路径：路径超过 100 字节是真实项目里天天遇到的情况
// （深层 node_modules、中文长文件名），而 ustar 的 155+100 拆分规则
// 有几种边界（正好 100、正好 155、没有斜杠可拆）。

#include "relayer/filestore/detail/TarCodec.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "relayer/filestore/IFileStore.h"

using relayer::filestore::ErrorCode;
using relayer::filestore::kTarBlock;
using relayer::filestore::Status;
using relayer::filestore::TarHeader;
using relayer::filestore::TarReader;
using relayer::filestore::TarWriter;

namespace
{

// 打包结果：整个归档的字节 + 每个条目的原始内容
struct Packed
{
    std::string bytes;
    std::vector<std::string> payloads;
    std::vector<std::string> names;
};

// 用 writer 打一个包。files 是 (name, content) 列表。
Packed pack(const std::vector<std::pair<std::string, std::string>> &files, std::uint16_t mode = 0644)
{
    TarWriter tw;
    Packed out;
    for (const auto &[name, content] : files)
    {
        Status st = tw.addEntry(name, '0', mode, content.size(), 1700000000);
        REQUIRE(st);
        st = tw.writeData(content.data(), content.size());
        REQUIRE(st);
        st = tw.finishData();
        REQUIRE(st);
        out.payloads.push_back(content);
        out.names.push_back(name);
    }
    REQUIRE(tw.close());

    std::vector<char> buf(97);  // 故意用非 512 的倍数，压出边界问题
    for (;;)
    {
        const std::size_t n = tw.read(buf.data(), buf.size());
        out.bytes.append(buf.data(), n);
        if (tw.drained())
        {
            break;
        }
    }
    return out;
}

// 逐个读条目，返回 (name, content) 列表。
// chunk 控制每次喂多少字节，用来模拟流式喂入。
std::vector<std::pair<std::string, std::string>> unpack(const std::string &bytes, std::size_t chunk = 0)
{
    TarReader tr;
    std::vector<std::pair<std::string, std::string>> out;

    if (chunk == 0)
    {
        REQUIRE(tr.write(bytes.data(), bytes.size()));
        tr.finishInput();
    }

    std::size_t fed = 0;
    auto feed = [&]() -> bool
    {
        // 返回 false 表示已经喂完
        if (chunk == 0 || fed >= bytes.size())
        {
            return false;
        }
        const std::size_t n = std::min(chunk, bytes.size() - fed);
        REQUIRE(tr.write(bytes.data() + fed, n));
        fed += n;
        if (fed >= bytes.size())
        {
            tr.finishInput();
        }
        return true;
    };
    // "要求更多输入"的统一处理：要么补喂成功，要么这是个真错误。
    // 用 REQUIRE 而不是静默跳出，可以避免"循环提前结束、测试却通过"。
    auto needMore = [&] { REQUIRE(feed()); };
    if (chunk != 0)
    {
        feed();
    }

    for (int guard = 0; guard < 100000; ++guard)
    {
        TarHeader h;
        Status st = tr.readHeader(h);
        while (st.code == ErrorCode::kUnsupported)
        {
            needMore();
            st = tr.readHeader(h);
        }
        REQUIRE(st);
        // 顺序很重要：PAX 条目的 name 也是空的，先排除它再判断结束标记，
        // 否则 PAX 头会被当成归档结束，后面的文件全部漏掉。
        if (h.isPax)
        {
            // 读完 PAX 负载，把路径喂回给下一个头
            std::vector<std::uint8_t> payload;
            Status pst = tr.readAllData(payload);
            while (pst.code == ErrorCode::kUnsupported)
            {
                needMore();
                pst = tr.readAllData(payload);
            }
            REQUIRE(pst);
            tr.applyPaxPayload(h.typeflag,
                               std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size()));
            continue;
        }
        if (h.name.empty())
        {
            return out;  // 结束标记
        }

        std::string content;
        std::vector<char> buf(100);
        for (;;)
        {
            Status rst;
            const std::size_t n = tr.readData(buf.data(), buf.size(), rst);
            if (rst.code == ErrorCode::kUnsupported)
            {
                needMore();
                continue;  // 补到了数据，重新读
            }
            REQUIRE(rst);
            content.append(buf.data(), n);
            if (n == 0)
            {
                break;
            }
        }
        out.emplace_back(h.name, content);
    }
    FAIL("too many entries");
    return out;
}

std::string makeContent(std::size_t n, unsigned seed = 7)
{
    std::string s;
    s.reserve(n);
    unsigned v = seed;
    for (std::size_t i = 0; i < n; ++i)
    {
        v = v * 1103515245u + 12345u;
        s.push_back(static_cast<char>((v >> 16) & 0xFF));
    }
    return s;
}

}  // namespace

TEST_CASE("tar 空归档只有结束标记", "[tar][roundtrip]")
{
    const Packed p = pack({});
    // 两个 512 的零块
    REQUIRE(p.bytes.size() == kTarBlock * 2);
    REQUIRE(p.bytes.find_first_not_of('\0') == std::string::npos);
    REQUIRE(unpack(p.bytes).empty());
}

TEST_CASE("tar 单文件往返", "[tar][roundtrip]")
{
    const Packed p = pack({{"hello.txt", "hello world"}});
    // 头 + 数据（不足 512 补齐）+ 两个零块
    REQUIRE(p.bytes.size() == kTarBlock + kTarBlock + kTarBlock * 2);

    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == "hello.txt");
    REQUIRE(out[0].second == "hello world");
}

TEST_CASE("tar 多个不同长度的文件", "[tar][roundtrip]")
{
    // 长度覆盖：0（不占数据块）、1、正好 512、跨块、明显多块
    const std::vector<std::size_t> sizes{0, 1, 511, 512, 513, 1024, 5000};
    std::vector<std::pair<std::string, std::string>> files;
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        files.emplace_back("f" + std::to_string(i) + ".bin", makeContent(sizes[i], static_cast<unsigned>(i + 1)));
    }
    const Packed p = pack(files);
    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == files.size());
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        INFO("entry " << i << " size " << sizes[i]);
        REQUIRE(out[i].first == files[i].first);
        REQUIRE(out[i].second == files[i].second);
    }
}

TEST_CASE("tar 每个条目都对齐到 512", "[tar][layout]")
{
    // 归档总长必须是 512 的整数倍，否则外部 tar 会报 "unexpected EOF"
    const Packed p = pack({{"a", "x"}, {"b", makeContent(1000)}});
    REQUIRE(p.bytes.size() % kTarBlock == 0);
}

TEST_CASE("tar 头的校验和合法", "[tar][layout]")
{
    // 手算第一个头的校验和，确保格式能被外部工具认。
    // 这里不调 liblzma 之外的任何东西，纯看字节。
    const Packed p = pack({{"a.txt", "x"}});
    REQUIRE(p.bytes.size() >= kTarBlock);

    const auto *raw = reinterpret_cast<const unsigned char *>(p.bytes.data());
    // magic 在偏移 257，应为 "ustar\0"
    REQUIRE(p.bytes.compare(257, 6, std::string("ustar\0", 6)) == 0);
    // version 在 263，应为 "00"
    REQUIRE(p.bytes.compare(263, 2, "00") == 0);
    // typeflag 在 156
    REQUIRE(p.bytes[156] == '0');
    // name 在 0..99
    REQUIRE(p.bytes.compare(0, 5, "a.txt") == 0);

    // 校验和：chksum 字段填空格后求和
    std::vector<unsigned char> copy(raw, raw + kTarBlock);
    for (std::size_t i = 148; i < 156; ++i)
    {
        copy[i] = ' ';
    }
    std::uint32_t sum = 0;
    for (unsigned char c : copy)
    {
        sum += c;
    }
    // 字段里存的是八进制
    const std::string field = p.bytes.substr(148, 6);
    const std::uint32_t stored = static_cast<std::uint32_t>(std::stoul(field, nullptr, 8));
    REQUIRE(stored == sum);
    // 第 6 位是 NUL，第 7 位是空格
    REQUIRE(p.bytes[154] == '\0');
    REQUIRE(p.bytes[155] == ' ');
}

TEST_CASE("tar PAX 长路径往返", "[tar][pax]")
{
    // 超过 100 字节、需要 PAX 的路径。分段拼出来，让前缀有斜杠可拆、
    // 又长到 ustar 的 prefix+name 装不下。
    std::string longName;
    for (int i = 0; i < 20; ++i)
    {
        longName += "segment" + std::to_string(i) + "/";
    }
    longName += "file.txt";
    // 198 字节：超过 name 字段（100），且没有能塞进 prefix(155) 的拆分点
    REQUIRE(longName.size() > 100);

    const Packed p = pack({{longName, "content of long path"}});
    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == longName);
    REQUIRE(out[0].second == "content of long path");
}

TEST_CASE("tar 长路径与短路径混排", "[tar][pax]")
{
    // PAX 头只作用于紧随其后的那个条目。混排能暴露"PAX 状态没清干净"
    // 的错法：长路径后面的短路径会被错误地改名。
    std::string longName(300, 'L');
    // 不加斜杠 → ustar 拆不开，只能走 PAX
    const Packed p = pack({{"short1.txt", "one"}, {longName, "two"}, {"short2.txt", "three"}});

    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].first == "short1.txt");
    REQUIRE(out[0].second == "one");
    REQUIRE(out[1].first == longName);
    REQUIRE(out[1].second == "two");
    REQUIRE(out[2].first == "short2.txt");
    REQUIRE(out[2].second == "three");
}

TEST_CASE("tar 名字正好 100 字节不用 PAX", "[tar][pax]")
{
    // 100 是 name 字段的容量（不满位，没有 NUL 结束符的空间），
    // 正好卡在边界上，最容易写错成 99。
    const std::string name100(100, 'n');
    const Packed p = pack({{name100, "boundary"}});
    // 没有 PAX 头 → 头 + 数据块 + 两个结束块 = 4 块
    REQUIRE(p.bytes.size() == kTarBlock * 4);
    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == name100);
}

TEST_CASE("tar 名字用 prefix 拆分而不是 PAX", "[tar][pax]")
{
    // 200 字节、含斜杠、且能塞进 prefix(155)+name(100) 的名字，
    // 应该走 ustar 的 prefix 字段，不产生 PAX 头。
    std::string name;
    name += std::string(80, 'a');  // 80
    name += "/";
    name += std::string(100, 'b');  // 100
    REQUIRE(name.size() == 181);

    const Packed p = pack({{name, "split"}});
    // 没有 PAX → 头 + 数据块 + 两个结束块 = 4 块
    REQUIRE(p.bytes.size() == kTarBlock * 4);
    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == name);
}

TEST_CASE("tar 中文与 emoji 路径", "[tar][pax][unicode]")
{
    // UTF-8 多字节字符按字节计长度。一个中文是 3 字节，
    // 所以"看起来 40 个字"可能已经 120 字节，需要 PAX。
    std::string name;
    for (int i = 0; i < 40; ++i)
    {
        name += "目录";
    }
    name += "/文件📦.txt";
    REQUIRE(name.size() > 100);

    const Packed p = pack({{name, "unicode ok"}});
    const auto out = unpack(p.bytes);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == name);
    REQUIRE(out[0].second == "unicode ok");
}

TEST_CASE("tar 流式分块喂入", "[tar][streaming]")
{
    // xz → tar 的管道里，tar 收到的是解压器挤出来的零碎字节。
    // 每次只喂 100 字节（远小于 512），头会被切断。
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("one.txt", makeContent(700));
    files.emplace_back(std::string(400, 'p'), makeContent(100));  // 长路径触发 PAX
    files.emplace_back("three.txt", "short");

    const Packed p = pack(files);
    const auto out = unpack(p.bytes, 100);
    REQUIRE(out.size() == files.size());
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        REQUIRE(out[i].first == files[i].first);
        REQUIRE(out[i].second == files[i].second);
    }
}

TEST_CASE("tar 目录条目", "[tar][dir]")
{
    TarWriter tw;
    REQUIRE(tw.addEntry("mydir/", '5', 0755, 0, 1700000000));
    REQUIRE(tw.finishData());
    REQUIRE(tw.addEntry("mydir/file.txt", '0', 0644, 4, 1700000000));
    REQUIRE(tw.writeData("data", 4));
    REQUIRE(tw.finishData());
    REQUIRE(tw.close());

    std::string bytes;
    std::vector<char> buf(256);
    for (;;)
    {
        const std::size_t n = tw.read(buf.data(), buf.size());
        bytes.append(buf.data(), n);
        if (tw.drained())
        {
            break;
        }
    }

    TarReader tr;
    REQUIRE(tr.write(bytes.data(), bytes.size()));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "mydir/");
    REQUIRE(h.typeflag == '5');
    REQUIRE(h.size == 0);
    REQUIRE(h.mode == 0755);
    REQUIRE(tr.skipData());

    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "mydir/file.txt");
    REQUIRE(h.typeflag == '0');
    REQUIRE(h.size == 4);
    REQUIRE(tr.skipData());

    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name.empty());  // 结束标记
}

TEST_CASE("tar 跳过条目数据后仍能继续读", "[tar][skip]")
{
    const Packed p = pack({{"a", makeContent(3000)}, {"b", "second"}});
    TarReader tr;
    REQUIRE(tr.write(p.bytes.data(), p.bytes.size()));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "a");
    // 不读数据直接跳（list 场景）
    REQUIRE(tr.skipData());

    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "b");
    REQUIRE(h.size == 6);
    REQUIRE(tr.skipData());
}

TEST_CASE("tar 校验和错误会被发现", "[tar][error]")
{
    Packed p = pack({{"a.txt", "content"}});
    // 篡改 name 字段但不改校验和
    p.bytes[0] = 'z';

    TarReader tr;
    REQUIRE(tr.write(p.bytes.data(), p.bytes.size()));
    tr.finishInput();

    TarHeader h;
    const Status st = tr.readHeader(h);
    REQUIRE_FALSE(st);
    REQUIRE(st.code == ErrorCode::kInvalidPath);
}

TEST_CASE("tar 输入被截断会报错", "[tar][error]")
{
    const Packed p = pack({{"a.txt", makeContent(2000)}});

    // 只给头 + 一半数据，然后声明输入结束
    TarReader tr;
    REQUIRE(tr.write(p.bytes.data(), kTarBlock + 100));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "a.txt");
    REQUIRE(h.size == 2000);

    std::vector<char> buf(1000);
    Status st;
    // 数据不够：要么返回 0 + 错误，要么消费完已有的后报错
    std::size_t total = 0;
    bool failed = false;
    for (int guard = 0; guard < 10; ++guard)
    {
        const std::size_t n = tr.readData(buf.data(), buf.size(), st);
        if (!st)
        {
            failed = true;
            break;
        }
        total += n;
        if (total >= 2000)
        {
            break;
        }
    }
    REQUIRE(failed);
}

TEST_CASE("tar 没有结束标记也能读出全部条目", "[tar][error]")
{
    // 有的老工具不打结束标记。读侧应在数据吃完后自然结束，
    // 而不是把"没见到零块"当成错误。
    const Packed p = pack({{"a.txt", "only"}});
    // 去掉最后两个零块
    const std::string noMarker = p.bytes.substr(0, p.bytes.size() - kTarBlock * 2);

    TarReader tr;
    REQUIRE(tr.write(noMarker.data(), noMarker.size()));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "a.txt");
    REQUIRE(tr.skipData());

    // 再读：输入已耗尽，应当是"截断"而不是"结束"
    const Status st = tr.readHeader(h);
    REQUIRE_FALSE(st);
}

TEST_CASE("tar PAX 负载里没有 path 时不影响后续条目", "[tar][pax][error]")
{
    // 伪造一个只带 mtime 记录的 PAX 头。名字应当仍取 ustar 头里的。
    TarWriter tw;
    // 用 PAX 触发条件：名字超长，但读侧会解析出空 path
    REQUIRE(tw.addEntry("normal.txt", '0', 0644, 2, 0));
    REQUIRE(tw.writeData("hi", 2));
    REQUIRE(tw.finishData());
    REQUIRE(tw.close());

    std::string bytes;
    std::vector<char> buf(512);
    for (;;)
    {
        const std::size_t n = tw.read(buf.data(), buf.size());
        bytes.append(buf.data(), n);
        if (tw.drained())
        {
            break;
        }
    }

    TarReader tr;
    REQUIRE(tr.write(bytes.data(), bytes.size()));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.name == "normal.txt");
}

TEST_CASE("tar 能读 GNU 的 'L' 长名字扩展", "[tar][pax][gnu]")
{
    // GNU tar 默认用 'L' 类型而不是 PAX 记录长文件名，负载是一段
    // 以 NUL 结尾的裸字符串。不认它的话，别人打的包在我们这里
    // 长路径会被静默截断到 100 字节——两个深路径塌成同一个名字、
    // 解包时互相覆盖，而且完全不报错。
    //
    // 手工构造一个 'L' 头 + 数据 + 常规头，模拟 GNU tar 的输出。
    const std::string longName = []
    {
        std::string s;
        for (int i = 0; i < 20; ++i)
        {
            s += "directory" + std::to_string(i) + "/";
        }
        s += "target.txt";
        return s;
    }();
    REQUIRE(longName.size() > 100);

    TarWriter tw;
    // GNU 的 'L' 条目：负载是 "路径\0"
    const std::string payload = longName + '\0';
    REQUIRE(tw.addEntry("././@LongLink", 'L', 0644, payload.size(), 0));
    REQUIRE(tw.writeData(payload.data(), payload.size()));
    REQUIRE(tw.finishData());
    // 常规头里只放截断的名字
    const std::string truncated = longName.substr(0, 100);
    REQUIRE(tw.addEntry(truncated, '0', 0644, 4, 0));
    REQUIRE(tw.writeData("data", 4));
    REQUIRE(tw.finishData());
    REQUIRE(tw.close());

    std::string bytes;
    std::vector<char> buf(512);
    for (;;)
    {
        const std::size_t n = tw.read(buf.data(), buf.size());
        bytes.append(buf.data(), n);
        if (tw.drained())
        {
            break;
        }
    }

    TarReader tr;
    REQUIRE(tr.write(bytes.data(), bytes.size()));
    tr.finishInput();

    TarHeader h;
    REQUIRE(tr.readHeader(h));
    REQUIRE(h.typeflag == 'L');
    REQUIRE(h.isPax);
    std::vector<std::uint8_t> got;
    REQUIRE(tr.readAllData(got));
    REQUIRE(got.size() == payload.size());
    const std::string_view sv(reinterpret_cast<const char *>(got.data()), got.size());
    tr.applyPaxPayload(h.typeflag, sv);

    REQUIRE(tr.readHeader(h));
    // 名字应当被 'L' 负载覆盖，而不是那个截断版本
    REQUIRE(h.name == longName);
    REQUIRE(h.name.size() > 100);
    REQUIRE(h.typeflag == '0');
}

TEST_CASE("tar writer 拒绝尺寸不符的写入", "[tar][error]")
{
    TarWriter tw;
    REQUIRE(tw.addEntry("a", '0', 0644, 10, 0));
    // 写超过声明长度
    REQUIRE_FALSE(tw.writeData("0123456789AB", 12));
    // 少写就 close：必须拒绝
    REQUIRE(tw.writeData("0123", 4));
    REQUIRE_FALSE(tw.close());
}

TEST_CASE("tar writer 拒绝空条目名", "[tar][error]")
{
    TarWriter tw;
    REQUIRE_FALSE(tw.addEntry("", '0', 0644, 0, 0));
}
