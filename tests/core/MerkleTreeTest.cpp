#include "relayer/core/transfer/MerkleTree.h"

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "relayer/common/crypto/Sha256.h"

using namespace relayer::core::transfer;
using relayer::ErrorCode;
using relayer::Status;
using Digest = relayer::crypto::Sha256::Digest;

// Merkle 树的测试重点不在"哈希算得对不对"（那是 crypto 的事），而在
// **树的形状**。形状错了的表现是"两端都对、就是不一致"，而那种问题
// 在现场几乎无法定位——只能靠这里把规则一条条钉死。

namespace
{

// 独立于生产代码的参考实现。
//
// 刻意不调用 computeRoot / MerkleBuilder：那样测试就退化成"实现和自己相等"，
// 永远不会失败。这里按 PROTOCOL.md §8.3.1 的文字重新拼一遍。
Digest referenceRoot(const std::vector<std::string> &chunks, std::uint32_t chunkSize)
{
    if (chunks.empty())
    {
        return relayer::crypto::Sha256::hash("", 0);
    }

    std::vector<Digest> leaves;
    leaves.reserve(chunks.size());
    for (const auto &c : chunks)
    {
        std::vector<std::uint8_t> buf(c.begin(), c.end());
        buf.resize(chunkSize, 0);  // 补零到整块

        relayer::crypto::Sha256 h;
        const std::uint8_t prefix = 0x00;
        h.update(&prefix, 1);
        h.update(buf.data(), buf.size());
        leaves.push_back(h.digest());
    }

    while (leaves.size() > 1)
    {
        std::vector<Digest> next;
        for (std::size_t i = 0; i + 1 < leaves.size(); i += 2)
        {
            relayer::crypto::Sha256 h;
            const std::uint8_t prefix = 0x01;
            h.update(&prefix, 1);
            h.update(leaves[i].data(), 32);
            h.update(leaves[i + 1].data(), 32);
            next.push_back(h.digest());
        }
        if (leaves.size() % 2 == 1)
        {
            next.push_back(leaves.back());  // 单独上提
        }
        leaves.swap(next);
    }
    return leaves[0];
}

std::string makeChunk(std::size_t len, std::uint8_t seed)
{
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i)
    {
        s[i] = static_cast<char>((seed + i * 7) & 0xFF);
    }
    return s;
}

}  // namespace

// ===========================================================================
// 分块几何
// ===========================================================================

TEST_CASE("chunk count rounds up and treats empty as zero", "[transfer][merkle]")
{
    REQUIRE(chunkCountFor(0, 1024) == 0);
    REQUIRE(chunkCountFor(1, 1024) == 1);
    REQUIRE(chunkCountFor(1023, 1024) == 1);
    REQUIRE(chunkCountFor(1024, 1024) == 1);
    REQUIRE(chunkCountFor(1025, 1024) == 2);
    REQUIRE(chunkCountFor(4096, 1024) == 4);

    // chunkSize 为 0 是非法参数，返回 0 而不是崩。
    REQUIRE(chunkCountFor(100, 0) == 0);

    // 大文件不溢出：totalSize 接近 u64 上限时，先除后补零的做法不会
    // 因为 (a + b - 1) 而回绕。
    //
    // 块数在线上是 u32，所以结果封顶到 UINT32_MAX 而不是返回真值——
    // 截断成别的数会让发送方少传数据而不报错。调用方拿到的这个值
    // 本身就是"参数不自洽"的信号（1GB 文件配 1KB 块也有 100 万块，
    // 远没到上限；能撞上这个上限只可能是参数错了）。
    const auto huge = chunkCountFor(std::uint64_t{1} << 62, kMinChunkSize);
    REQUIRE(huge == 0xFFFFFFFFu);
}

TEST_CASE("chunk length is short only for the last block", "[transfer][merkle]")
{
    const std::uint64_t total = 2500;
    const std::uint32_t cs = 1024;

    REQUIRE(chunkLengthAt(0, total, cs) == 1024);
    REQUIRE(chunkLengthAt(1, total, cs) == 1024);
    REQUIRE(chunkLengthAt(2, total, cs) == 452);
    REQUIRE(chunkLengthAt(3, total, cs) == 0);  // 越界

    // 恰好整除时没有短块。
    REQUIRE(chunkLengthAt(1, 2048, 1024) == 1024);

    // 空文件任何块号都是 0。
    REQUIRE(chunkLengthAt(0, 0, 1024) == 0);

    // chunkSize 为 0 不能除零。
    REQUIRE(chunkLengthAt(0, 100, 0) == 0);
}

// ===========================================================================
// 空树与单叶
// ===========================================================================

TEST_CASE("empty input hashes to sha256 of empty string", "[transfer][merkle]")
{
    // 这个值是 SHA256("") 的公开已知值。写死它而不是调 crypto 现算：
    // 现算等于"实现和自己比"，而上游 crypto 已经单独测过了。
    const std::string expected = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    REQUIRE(toHex(emptyRoot()) == expected);

    // 三条路径都要给出同一个结论：直调、从空列表、从空缓冲。
    REQUIRE(toHex(computeRoot({})) == expected);
    REQUIRE(toHex(rootOfBuffer("", 0, 1024)) == expected);

    REQUIRE(toHex(MerkleBuilder(1024).root()) == expected);
}

TEST_CASE("single leaf root is the leaf itself", "[transfer][merkle]")
{
    // 只有一个块时不再套一层节点。套了的话根会变成 nodeHash(leaf, leaf)，
    // 与"两个相同块"的树撞车。
    const std::string data(1024, 'A');
    const auto leaf = leafHash(data.data(), data.size(), 1024);

    REQUIRE(toHex(rootOfBuffer(data.data(), data.size(), 1024)) == toHex(leaf));

    // 与"两个相同块"的树必须不同。
    const std::string twice = data + data;
    REQUIRE(toHex(rootOfBuffer(twice.data(), twice.size(), 1024)) != toHex(leaf));
}

TEST_CASE("root matches golden vectors computed independently", "[transfer][merkle]")
{
    // 金标准向量：用 Python hashlib 按 PROTOCOL.md §8.3.1 独立算出，
    // 与 referenceRoot（C++ 重写一遍）互为交叉验证。
    //
    // 为什么需要两份独立的参考实现：C++ 参考实现和被测实现由同一人写、
    // 可能共享同一个理解偏差；Python 那份是照着文档文字另起一遍，而且
    // hashlib 是外部依赖。两份都对上才说明"文档 → 实现"这一步没走偏。
    //
    // 数据构造与 merkle_ref.py 一致：第 i 块的第 j 字节 = (i*13+1+j*7) & 0xFF。
    const std::uint32_t cs = 1024;
    struct Vec
    {
        int n;
        const char *root;
    };
    const Vec vectors[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "48dfd6af5c6259d45acff0438826559cfe27fffd6058d1d6c944d46eeb58a60f"},
        {2, "42ec7d9c77c80cb92f01e436b334915dec332ef16d0dea97e980a71c852b6ced"},
        {3, "c7b204ef64f16f7663a9f4585ed543f4b25bbbe6caa5ab2ba12261bf095307f7"},
        {4, "9f6a179e2f398186dd3e439f04a3235972cfdcd2b5d4ef6138b8198862465dbe"},
        {5, "23dd165b9f5b7d1f478ef37e1f23c51f5f2a33cce50d09aedd3f5c67d9843618"},
        {6, "3c4fe32a1e809653b9ab2e0ed0bdec4d4089a5bc7b9901c621f178e6e398cfb8"},
        {7, "bb6f39426d554aca7d84706d59bec7fe98d736c9da41eeab567b3b181a1597e4"},
        {8, "57813639bc2ce225e7daf2d9465b8dbb8e9036ba52eeb29fedd8cc2aa9907e8f"},
        {9, "1f0ad93873ff3b200b2843675a0938f5af8c439ab6858647c3aad05a0b5d03fb"},
    };

    for (const auto &v : vectors)
    {
        std::string joined;
        MerkleBuilder b(cs);
        for (int i = 0; i < v.n; ++i)
        {
            std::string chunk(cs, '\0');
            for (std::size_t j = 0; j < cs; ++j)
            {
                chunk[j] = static_cast<char>((i * 13 + 1 + static_cast<int>(j) * 7) & 0xFF);
            }
            joined += chunk;
            REQUIRE(b.addChunk(chunk.data(), chunk.size()).ok());
        }

        INFO("block count = " << v.n);
        REQUIRE(toHex(b.root()) == v.root);
        // 一次性算也要给出一致结果。
        REQUIRE(toHex(rootOfBuffer(joined.data(), joined.size(), cs)) == v.root);
    }
}

// ===========================================================================
// 形状规则
// ===========================================================================

TEST_CASE("odd node is promoted, not duplicated", "[transfer][merkle]")
{
    // 这是本协议与比特币写法**唯一的差异**，也是最容易抄错的一条。
    //
    // [a,b,c] 与 [a,b,c,c] 必须是不同的根。若实现复制了最后一个节点，
    // 两者会相同——那意味着两种不同的文件内容通过了同一个校验。
    const auto a = makeChunk(1024, 1);
    const auto b = makeChunk(1024, 2);
    const auto c = makeChunk(1024, 3);

    std::vector<Digest> three{
        leafHash(a.data(), a.size(), 1024),
        leafHash(b.data(), b.size(), 1024),
        leafHash(c.data(), c.size(), 1024),
    };
    // 一次列出四个元素，而不是 `auto four = three; four.push_back(three[2])`。
    // 后者在 GCC 16 + `-O3 -Werror` 下会触发 `-Warray-bounds` 误报：编译器
    // 把"拷贝构造一个 3 元素 vector 再 push_back"内联成了对定长数组的越界
    // 访问（`array subscript 3 is outside array bounds of ... [3]`）。写法
    // 换掉之后告警消失，语义完全一样。
    std::vector<Digest> four{
        three[0],
        three[1],
        three[2],
        three[2],  // 复制最后一个
    };

    const auto r3 = computeRoot(three);
    const auto r4 = computeRoot(four);
    REQUIRE(toHex(r3) != toHex(r4));

    // 而且 r3 应当等于 node(node(a,b), c)。c 单独上提后与下层合并，
    // 所以第二层是 [node(a,b), c] 两个节点。
    const auto upper = nodeHash(nodeHash(three[0], three[1]), three[2]);
    REQUIRE(toHex(r3) == toHex(upper));
}

TEST_CASE("root matches an independent reference for many shapes", "[transfer][merkle]")
{
    // 覆盖 1..9 个块。9 个块会经过四层，奇数上提会在多个层级上发生，
    // 只测 3 个块不足以暴露"只在最后一层处理奇数"的半吊子实现。
    const std::uint32_t cs = 1024;
    for (int n = 1; n <= 9; ++n)
    {
        std::vector<std::string> chunks;
        for (int i = 0; i < n; ++i)
        {
            // 每块内容不同，否则"顺序错了"这件事测不出来。
            chunks.push_back(makeChunk(cs, static_cast<std::uint8_t>(i * 13 + 1)));
        }

        std::string joined;
        for (const auto &c : chunks)
        {
            joined += c;
        }

        const auto got = toHex(rootOfBuffer(joined.data(), joined.size(), cs));
        const auto want = toHex(referenceRoot(chunks, cs));
        INFO("block count = " << n);
        REQUIRE(got == want);
    }
}

TEST_CASE("block order affects the root", "[transfer][merkle]")
{
    // 若实现把叶子排序了（比如为了"规范化"），这个测试会失败。
    // 顺序必须由块号决定，不能由内容决定。
    const auto a = makeChunk(1024, 10);
    const auto b = makeChunk(1024, 20);

    const auto ab = a + b;
    const auto ba = b + a;

    REQUIRE(toHex(rootOfBuffer(ab.data(), ab.size(), 1024)) != toHex(rootOfBuffer(ba.data(), ba.size(), 1024)));
}

TEST_CASE("content change in any block changes the root", "[transfer][merkle]")
{
    // 分块 Merkle 的意义就在这里：任何一块被改，根就变。
    const std::uint32_t cs = 1024;
    std::string base(4096, 'x');
    const auto baseRoot = toHex(rootOfBuffer(base.data(), base.size(), cs));

    for (std::size_t pos : {std::size_t{0}, std::size_t{1500}, std::size_t{4095}})
    {
        std::string mutated = base;
        mutated[pos] = 'y';
        INFO("mutated byte at " << pos);
        REQUIRE(toHex(rootOfBuffer(mutated.data(), mutated.size(), cs)) != baseRoot);
    }
}

// ===========================================================================
// 补零规则
// ===========================================================================

TEST_CASE("last block is zero padded to chunkSize", "[transfer][merkle]")
{
    // 规格要求：最后一块补零到 chunkSize 后再哈希。所以"内容相同的短数据"
    // 在 offset 上补零后应当得到与"本来就带那些零"相同的叶哈希。
    const std::string shortData = "hello";
    std::string padded = shortData;
    padded.resize(1024, '\0');

    REQUIRE(toHex(leafHash(shortData.data(), shortData.size(), 1024)) ==
            toHex(leafHash(padded.data(), padded.size(), 1024)));
}

TEST_CASE("padded leaf is unaffected by buffer that already has trailing zeros", "[transfer][merkle]")
{
    // 反面：补零不该把**中间**的零也一起抹掉。这两条一起说明
    // "补零只发生在尾部且只补到 chunkSize"。
    std::string a(1024, 'a');
    std::string b = a;
    b[512] = '\0';

    REQUIRE(toHex(leafHash(a.data(), a.size(), 1024)) != toHex(leafHash(b.data(), b.size(), 1024)));
}

// ===========================================================================
// 增量构建器
// ===========================================================================

TEST_CASE("incremental builder agrees with one-shot root", "[transfer][merkle]")
{
    const std::uint32_t cs = 1024;
    for (int n = 0; n <= 9; ++n)
    {
        std::string joined;
        MerkleBuilder b(cs);
        REQUIRE(b.valid());

        for (int i = 0; i < n; ++i)
        {
            const auto chunk = makeChunk(cs, static_cast<std::uint8_t>(i * 5 + 3));
            joined += chunk;
            const auto st = b.addChunk(chunk.data(), chunk.size());
            REQUIRE(st.ok());
        }

        REQUIRE(b.count() == static_cast<std::uint32_t>(n));
        INFO("block count = " << n);
        REQUIRE(toHex(b.root()) == toHex(rootOfBuffer(joined.data(), joined.size(), cs)));
    }
}

TEST_CASE("builder rejects out-of-range chunk size", "[transfer][merkle]")
{
    // 构造不抛异常，越界时进入非法状态，addChunk 报错。
    // 这是"跨模块边界不用异常"的既定约定。
    REQUIRE_FALSE(MerkleBuilder(512).valid());                      // 小于下限
    REQUIRE_FALSE(MerkleBuilder(0).valid());                        //
    REQUIRE_FALSE(MerkleBuilder(4u * 1024 * 1024 + 1024).valid());  // 超过上限
    REQUIRE_FALSE(MerkleBuilder(1500).valid());                     // 未按 1024 对齐

    MerkleBuilder bad(512);
    REQUIRE_FALSE(bad.addChunk("x", 1).ok());

    // 边界值本身合法。
    REQUIRE(MerkleBuilder(kMinChunkSize).valid());
    REQUIRE(MerkleBuilder(kMaxChunkSize).valid());
}

TEST_CASE("builder rejects oversized chunk", "[transfer][merkle]")
{
    // 除最后一块外都必须恰好 chunkSize。超出说明调用方的块边界算错了，
    // 此时悄悄截断会让"根相同"变得毫无意义。
    MerkleBuilder b(1024);
    std::string tooBig(1025, 'z');
    REQUIRE_FALSE(b.addChunk(tooBig.data(), tooBig.size()).ok());
    REQUIRE(b.count() == 0);
}

TEST_CASE("builder accepts a short last chunk but not a short middle chunk", "[transfer][merkle]")
{
    // 短块只能是最后一块。构建器本身不知道"还有没有下一块"，所以它允许
    // 任何长度 <= chunkSize 的块；正确性由调用方按 totalSize 保证。
    // 这里固定住"允许"这个行为，免得以后有人加一条"必须等于 chunkSize"
    // 的检查而把所有单块小文件都拒掉。
    MerkleBuilder b(1024);
    REQUIRE(b.addChunk("abc", 3).ok());
    REQUIRE(b.count() == 1);
    REQUIRE(toHex(b.root()) == toHex(leafHash("abc", 3, 1024)));
}

TEST_CASE("root is stable across repeated calls", "[transfer][merkle]")
{
    // root() 声明为不修改状态。若实现为了"收尾"把层压平了，
    // 第二次调用就会得到不同结果。
    MerkleBuilder b(1024);
    const auto c1 = makeChunk(1024, 1);
    const auto c2 = makeChunk(1024, 2);
    const auto c3 = makeChunk(1024, 3);
    REQUIRE(b.addChunk(c1.data(), c1.size()).ok());
    REQUIRE(b.addChunk(c2.data(), c2.size()).ok());
    REQUIRE(b.addChunk(c3.data(), c3.size()).ok());

    const auto r1 = toHex(b.root());
    const auto r2 = toHex(b.root());
    REQUIRE(r1 == r2);

    // 而且后续还能继续喂块，根随内容变化。
    const auto c4 = makeChunk(1024, 4);
    REQUIRE(b.addChunk(c4.data(), c4.size()).ok());
    REQUIRE(toHex(b.root()) != r1);
}

// ===========================================================================
// 十六进制
// ===========================================================================

TEST_CASE("hex round trip and rejection", "[transfer][merkle]")
{
    const std::string data = "round trip";
    const auto d = relayer::crypto::Sha256::hash(data.data(), data.size());
    const auto hex = toHex(d);
    REQUIRE(hex.size() == 64);

    Digest back{};
    REQUIRE(fromHex(hex, back));
    REQUIRE(back == d);

    // 长度不对。
    REQUIRE_FALSE(fromHex("abcd", back));

    // 含非十六进制字符。
    std::string bad = hex;
    bad[10] = 'z';
    REQUIRE_FALSE(fromHex(bad, back));

    // 大写也接受（对照用的工具常输出大写）。
    std::string upper = hex;
    for (auto &c : upper)
    {
        if (c >= 'a' && c <= 'f')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    REQUIRE(fromHex(upper, back));
    REQUIRE(back == d);
}
