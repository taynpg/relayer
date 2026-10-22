#include "relayer/core/transfer/TransferPayload.h"

#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::transfer;
using relayer::ErrorCode;
using relayer::Status;

// 这一层的测试建立在一条信念上：**载荷格式的 bug 不会在功能测试里暴露**。
// 字段顺序换一下、长度前缀用 u8 而不是 u16，两端如果都是同一份实现，
// 照样跑得通；只有跨版本或跨实现时才炸，而那时已经是现场了。
//
// 所以这里除了往返测试，还要逐字节核对固定向量，并覆盖所有"畸形输入"
// 路径——那些路径是攻击面，也是"老版本遇到新版本"的第一道防线。

namespace
{

const std::string kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kFp = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

TransferDesc makeDesc()
{
    TransferDesc d;
    d.displayName = "report.pdf";
    d.totalSize = 4u * 1024 * 1024 + 123;
    d.chunkSize = 4u * 1024 * 1024;
    d.sha256Hex = kSha;
    d.fingerprintHex = kFp;
    d.root = "inbox/report.pdf";
    return d;
}

}  // namespace

// ===========================================================================
// 会话描述
// ===========================================================================

TEST_CASE("transfer desc round trips", "[transfer][payload]")
{
    const auto d = makeDesc();

    std::vector<char> buf;
    REQUIRE(encodeDesc(d, buf).ok());

    TransferDesc back;
    std::string err;
    REQUIRE(decodeDesc(buf, back, err));
    REQUIRE(back.displayName == d.displayName);
    REQUIRE(back.totalSize == d.totalSize);
    REQUIRE(back.chunkSize == d.chunkSize);
    REQUIRE(back.sha256Hex == d.sha256Hex);
    REQUIRE(back.fingerprintHex == d.fingerprintHex);
    REQUIRE(back.root == d.root);
}

TEST_CASE("transfer desc has a stable byte layout", "[transfer][payload]")
{
    // 逐字节核对字段顺序与长度前缀宽度。这条测试的价值在于：只要有人
    // 调整了 encodeDesc 的字段顺序，即使往返测试仍然通过，这里也会红。
    //
    // 规格（§9.4）：
    //   u16 nameLen || name || u64 totalSize || u32 chunkSize
    //   || u8 shaLen || sha || u8 fpLen || fp || u16 rootLen || root
    TransferDesc d;
    d.displayName = "ab";
    // totalSize 必须与 chunkSize 自洽：块数要落在 u32 内，否则 validateDesc
    // 会拒掉。用 0x000102030405 这样的值既能检查 u64 的 8 字节小端布局，
    // 又不会让块数溢出（约 1.1e12 / 1024 = 1.07e9 块，仍在 u32 内）。
    d.totalSize = 0x0000010203040506ull;
    d.chunkSize = 1024;
    d.sha256Hex = std::string(64, 'a');
    d.fingerprintHex = std::string(64, 'b');
    d.root = "x";

    std::vector<char> buf;
    REQUIRE(encodeDesc(d, buf).ok());

    // 手工拼出期望字节，逐段比对。
    std::vector<char> want;
    want.push_back(2);
    want.push_back(0);
    want.push_back('a');
    want.push_back('b');
    // u64 小端
    for (int i = 0; i < 8; ++i)
    {
        want.push_back(static_cast<char>((d.totalSize >> (8 * i)) & 0xFF));
    }
    // u32 小端 = 1024 = 0x00000400
    want.push_back(0x00);
    want.push_back(0x04);
    want.push_back(0x00);
    want.push_back(0x00);
    want.push_back(64);
    for (int i = 0; i < 64; ++i)
    {
        want.push_back('a');
    }
    want.push_back(64);
    for (int i = 0; i < 64; ++i)
    {
        want.push_back('b');
    }
    want.push_back(1);
    want.push_back(0);
    want.push_back('x');

    REQUIRE(buf == want);
}

TEST_CASE("desc validation rejects bad shapes", "[transfer][payload]")
{
    auto d = makeDesc();

    SECTION("empty root")
    {
        d.root.clear();
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("chunk size below minimum")
    {
        d.chunkSize = 512;
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("chunk size above maximum")
    {
        d.chunkSize = 8u * 1024 * 1024;
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("chunk size misaligned")
    {
        d.chunkSize = 1536;  // 1.5 KiB，在范围内但没按 1024 对齐
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("sha256 wrong length")
    {
        d.sha256Hex = "abc";
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("sha256 not hex")
    {
        d.sha256Hex = std::string(64, 'z');
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("fingerprint not hex")
    {
        d.fingerprintHex = std::string(64, 'g');
        REQUIRE_FALSE(validateDesc(d).ok());
    }
    SECTION("empty hashes are allowed")
    {
        // 有的场景用 Merkle 根代替整文件哈希，语义等价，不该被拒。
        d.sha256Hex.clear();
        d.fingerprintHex.clear();
        REQUIRE(validateDesc(d).ok());
    }
}

TEST_CASE("desc validation rejects u32 chunk index overflow", "[transfer][payload]")
{
    // 块数必须能塞进 u32，否则 line 上的 chunkIndex 会绕回，
    // 表现为"第 0 块被反复覆盖"，而总数对得上。
    auto d = makeDesc();
    d.chunkSize = kMinChunkSize;           // 1 KiB
    d.totalSize = std::uint64_t{1} << 50;  // 2^50 / 2^10 = 2^40 块
    REQUIRE_FALSE(validateDesc(d).ok());

    // 边界内的最大值可以接受。
    d.chunkSize = kMaxChunkSize;
    d.totalSize = std::uint64_t{0xFFFFFFFFull} * kMaxChunkSize;
    REQUIRE(validateDesc(d).ok());
}

TEST_CASE("desc decode rejects truncated and trailing input", "[transfer][payload]")
{
    const auto d = makeDesc();
    std::vector<char> buf;
    REQUIRE(encodeDesc(d, buf).ok());

    SECTION("every truncation fails")
    {
        // 逐字节截断：任何一个截断点都不该解出"看起来正常"的描述。
        for (std::size_t cut = 0; cut < buf.size(); ++cut)
        {
            std::vector<char> partial(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cut));
            TransferDesc out;
            std::string err;
            INFO("cut at " << cut);
            REQUIRE_FALSE(decodeDesc(partial, out, err));
            REQUIRE_FALSE(err.empty());
        }
    }

    SECTION("trailing bytes rejected")
    {
        auto extra = buf;
        extra.push_back('\x7f');
        TransferDesc out;
        std::string err;
        REQUIRE_FALSE(decodeDesc(extra, out, err));
        REQUIRE(err.find("trailing") != std::string::npos);
    }
}

TEST_CASE("desc hashes are normalized to lower case", "[transfer][payload]")
{
    // 大小写不统一会让两端日志里的同一个哈希 grep 不到一起。
    auto d = makeDesc();
    for (auto &c : d.sha256Hex)
    {
        if (c >= 'a' && c <= 'f')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }

    std::vector<char> buf;
    REQUIRE(encodeDesc(d, buf).ok());

    TransferDesc back;
    std::string err;
    REQUIRE(decodeDesc(buf, back, err));
    REQUIRE(back.sha256Hex == kSha);  // 解出来是小写
}

// ===========================================================================
// 应答
// ===========================================================================

TEST_CASE("transfer answer round trips for all statuses", "[transfer][payload]")
{
    using S = AnswerStatus;
    for (auto s :
         {S::kOk, S::kTargetExists, S::kNoSpace, S::kNotWritable, S::kBadDesc, S::kDuplicateTask, S::kTooLarge})
    {
        TransferAnswer a;
        a.status = s;
        a.resumeOffset = s == S::kOk ? 8192 : 0;
        a.chunkSizeHint = s == S::kOk ? 1024 : 0;
        a.reason = s == S::kOk ? std::string{} : "some reason";

        std::vector<char> buf;
        REQUIRE(encodeAnswer(a, buf).ok());

        TransferAnswer back;
        std::string err;
        REQUIRE(decodeAnswer(buf, back, err));
        INFO("status = " << toString(s));
        REQUIRE(back.status == s);
        REQUIRE(back.resumeOffset == a.resumeOffset);
        REQUIRE(back.chunkSizeHint == a.chunkSizeHint);
        REQUIRE(back.reason == a.reason);
    }
}

TEST_CASE("answer rejects unknown status code", "[transfer][payload]")
{
    // 这是"以后加了新状态，老版本误当成功"的唯一防线。
    std::vector<char> buf;
    buf.push_back(static_cast<char>(99));  // 未知状态
    for (int i = 0; i < 8; ++i)
    {
        buf.push_back(0);
    }
    for (int i = 0; i < 4; ++i)
    {
        buf.push_back(0);
    }
    buf.push_back(0);  // reason 长度 0

    TransferAnswer a;
    std::string err;
    REQUIRE_FALSE(decodeAnswer(buf, a, err));
    REQUIRE(err.find("unknown") != std::string::npos);
}

TEST_CASE("answer validation requires chunk-aligned resume offset", "[transfer][payload]")
{
    TransferAnswer a;
    a.status = AnswerStatus::kOk;
    a.resumeOffset = 1024;
    REQUIRE(validateAnswer(a, 1024).ok());

    // 非对齐：块号 → 偏移的乘法映射会失效。
    a.resumeOffset = 1000;
    REQUIRE_FALSE(validateAnswer(a, 1024).ok());

    // chunkSize 传 0 表示跳过这项检查（比如尚未协商时）。
    REQUIRE(validateAnswer(a, 0).ok());

    // 失败应答里的 offset 无意义，不校验。
    a.status = AnswerStatus::kNoSpace;
    a.resumeOffset = 12345;
    REQUIRE(validateAnswer(a, 1024).ok());
}

// ===========================================================================
// 分块数据
// ===========================================================================

TEST_CASE("chunk data round trips", "[transfer][payload]")
{
    const std::string payload = "the quick brown fox jumps over the lazy dog";
    std::vector<char> buf;
    REQUIRE(encodeChunk(7, payload.data(), payload.size(), buf).ok());

    ChunkData c;
    std::string err;
    REQUIRE(decodeChunk(buf, c, err));
    REQUIRE(c.chunkIndex == 7);
    REQUIRE(c.payloadLen == payload.size());
    REQUIRE(std::string(c.payload, c.payloadLen) == payload);
}

TEST_CASE("zero-length chunk is valid", "[transfer][payload]")
{
    // 空文件只有 0 块，但"最后一块长度为 0"不该在编码层被拒——
    // 那是上层按 totalSize 判断的事。
    std::vector<char> buf;
    REQUIRE(encodeChunk(0, nullptr, 0, buf).ok());

    ChunkData c;
    std::string err;
    REQUIRE(decodeChunk(buf, c, err));
    REQUIRE(c.chunkIndex == 0);
    REQUIRE(c.payloadLen == 0);
}

TEST_CASE("chunk decode rejects length mismatch", "[transfer][payload]")
{
    std::vector<char> buf;
    REQUIRE(encodeChunk(1, "abcdef", 6, buf).ok());

    SECTION("payloadLen claims more than present")
    {
        auto bad = buf;
        bad[4] = 100;  // payloadLen 的低字节改成 100
        ChunkData c;
        std::string err;
        REQUIRE_FALSE(decodeChunk(bad, c, err));
        REQUIRE(err.find("payloadLen") != std::string::npos);
    }

    SECTION("payloadLen claims less than present")
    {
        auto bad = buf;
        bad[4] = 3;
        ChunkData c;
        std::string err;
        REQUIRE_FALSE(decodeChunk(bad, c, err));
    }

    SECTION("truncated header")
    {
        std::vector<char> shortBuf(buf.begin(), buf.begin() + 5);
        ChunkData c;
        std::string err;
        REQUIRE_FALSE(decodeChunk(shortBuf, c, err));
    }
}

TEST_CASE("chunk encode rejects oversized or null payload", "[transfer][payload]")
{
    std::vector<char> buf;
    std::string huge(static_cast<std::size_t>(kMaxChunkSize) + 1, 'x');
    REQUIRE_FALSE(encodeChunk(0, huge.data(), huge.size(), buf).ok());
    REQUIRE_FALSE(encodeChunk(0, nullptr, 10, buf).ok());
}

// ===========================================================================
// 分块确认
// ===========================================================================

TEST_CASE("bitmap bit order is low bit first", "[transfer][payload]")
{
    // 与 §7 的位标志约定一致。写反了的话 SACK 会重传完全错误的块，
    // 而"重传了不该重传的块"不会报错，只是慢——非常难发现。
    std::vector<std::uint32_t> bm;
    bitmapSet(bm, 0);
    bitmapSet(bm, 31);
    bitmapSet(bm, 32);

    REQUIRE(bm.size() == 2);
    REQUIRE(bm[0] == 0x80000001u);
    REQUIRE(bm[1] == 0x00000001u);

    REQUIRE(bitmapTest(bm, 0));
    REQUIRE(bitmapTest(bm, 31));
    REQUIRE(bitmapTest(bm, 32));
    REQUIRE_FALSE(bitmapTest(bm, 1));
    REQUIRE_FALSE(bitmapTest(bm, 33));  // 越界返回 false
}

TEST_CASE("ack through is the longest contiguous prefix", "[transfer][payload]")
{
    // 这是整个 SACK 里最容易写错的一处。收到 0,1,5 时必须报 ackThrough=2，
    // 若写成"最后收到的 + 1"= 6，发送方会以为 2、3、4 都到了。
    const std::vector<bool> received = {true, true, false, false, false, true};
    const auto ack = buildAck(received, 6);

    REQUIRE(ack.ackThrough == 2);
    REQUIRE(bitmapTest(ack.bitmap, 3));  // 块 5 = ackThrough + 3
    REQUIRE_FALSE(bitmapTest(ack.bitmap, 0));
    REQUIRE_FALSE(bitmapTest(ack.bitmap, 1));
    REQUIRE_FALSE(bitmapTest(ack.bitmap, 2));

    const auto missing = missingChunks(ack, 6);
    REQUIRE(missing == std::vector<std::uint32_t>{2, 3, 4});
}

TEST_CASE("ack for complete transfer has empty bitmap", "[transfer][payload]")
{
    const std::vector<bool> received(10, true);
    const auto ack = buildAck(received, 10);

    REQUIRE(ack.ackThrough == 10);
    REQUIRE(ack.bitmap.empty());
    REQUIRE(missingChunks(ack, 10).empty());
}

TEST_CASE("ack for nothing received has empty bitmap", "[transfer][payload]")
{
    const std::vector<bool> received(10, false);
    const auto ack = buildAck(received, 10);

    REQUIRE(ack.ackThrough == 0);
    REQUIRE(ack.bitmap.empty());
    REQUIRE(missingChunks(ack, 10).size() == 10);
}

TEST_CASE("ack for empty file", "[transfer][payload]")
{
    // 空文件 0 块：ackThrough=0，位图为空，没有缺失。
    const std::vector<bool> received;
    const auto ack = buildAck(received, 0);
    REQUIRE(ack.ackThrough == 0);
    REQUIRE(ack.bitmap.empty());
    REQUIRE(missingChunks(ack, 0).empty());
}

TEST_CASE("ack bitmap truncates trailing zero words", "[transfer][payload]")
{
    // 只差第 2 块时不该带上几万个零字。
    std::vector<bool> received(10000, true);
    received[2] = false;
    const auto ack = buildAck(received, 10000);

    REQUIRE(ack.ackThrough == 2);
    // 块 2 之后的 9997 个块全部置位，最后一个是块 9999，位下标 9999-2 = 9997，
    // 落在第 312 个字（9997/32 = 312），所以需要 313 个字。
    //
    // 这条断言不是"随手写的数字"：它同时在验证"截断只砍到最后一个置位位
    // 所在的字"。若实现按 totalChunks 一路补零，这里会远大于 313。
    REQUIRE(ack.bitmap.size() == 313);
    const auto missing = missingChunks(ack, 10000);
    REQUIRE(missing == std::vector<std::uint32_t>{2});
}

TEST_CASE("ack handles only the last chunk missing", "[transfer][payload]")
{
    std::vector<bool> received(64, true);
    received[63] = false;
    const auto ack = buildAck(received, 64);

    REQUIRE(ack.ackThrough == 63);
    REQUIRE(missingChunks(ack, 64) == std::vector<std::uint32_t>{63});
}

TEST_CASE("ack round trips through encode/decode", "[transfer][payload]")
{
    std::vector<bool> received(200, true);
    received[5] = false;
    received[100] = false;
    received[199] = false;
    const auto ack = buildAck(received, 200);

    std::vector<char> buf;
    REQUIRE(encodeAck(ack, buf).ok());

    ChunkAck back;
    std::string err;
    REQUIRE(decodeAck(buf, back, err));
    REQUIRE(back.ackThrough == ack.ackThrough);
    REQUIRE(back.bitmap == ack.bitmap);
    REQUIRE(missingChunks(back, 200) == std::vector<std::uint32_t>{5, 100, 199});
}

TEST_CASE("ack decode rejects a bitmap length that disagrees with body", "[transfer][payload]")
{
    // "对端一个 u32 就让我们分配 16GB" 的经典防御。
    std::vector<char> buf;
    ChunkAck a;
    a.ackThrough = 0;
    REQUIRE(encodeAck(a, buf).ok());

    SECTION("declared words exceed actual bytes")
    {
        auto bad = buf;
        // 把 wordCount 改成 0xFFFFFFFF，body 里却没有那么多字节。
        bad[4] = static_cast<char>(0xFF);
        bad[5] = static_cast<char>(0xFF);
        bad[6] = static_cast<char>(0xFF);
        bad[7] = static_cast<char>(0xFF);

        ChunkAck out;
        std::string err;
        REQUIRE_FALSE(decodeAck(bad, out, err));
        REQUIRE(err.find("mismatch") != std::string::npos);
    }

    SECTION("declared words fewer than actual bytes")
    {
        ChunkAck withBits;
        withBits.ackThrough = 0;
        withBits.bitmap = {0xFFFFFFFFu};
        std::vector<char> buf2;
        REQUIRE(encodeAck(withBits, buf2).ok());
        // 声称 0 个字，实际还有 4 字节。u32 的四个字节都要清零。
        buf2[4] = 0;
        buf2[5] = 0;
        buf2[6] = 0;
        buf2[7] = 0;

        ChunkAck out;
        std::string err;
        REQUIRE_FALSE(decodeAck(buf2, out, err));
    }
}

TEST_CASE("missing chunks tolerates ack through beyond total", "[transfer][payload]")
{
    // 对方多报了不该导致我们越界读：按 totalChunks 截断，剩下的都算缺失。
    // 这里的目标是"算出发什么"，不是校验对方。
    ChunkAck ack;
    ack.ackThrough = 100;
    REQUIRE(missingChunks(ack, 10).empty());

    ack.ackThrough = 5;
    REQUIRE(missingChunks(ack, 10).size() == 5);
}

// ===========================================================================
// 完成握手
// ===========================================================================

TEST_CASE("complete request round trips", "[transfer][payload]")
{
    std::vector<char> buf;
    REQUIRE(encodeCompleteRequest(kSha, 12345, buf).ok());

    std::string root;
    std::uint64_t size = 0;
    std::string err;
    REQUIRE(decodeCompleteRequest(buf, root, size, err));
    REQUIRE(root == kSha);
    REQUIRE(size == 12345);
}

TEST_CASE("complete answer round trips for all statuses", "[transfer][payload]")
{
    using S = CompleteStatus;
    for (auto s : {S::kOk, S::kRootMismatch, S::kWriteFailed, S::kShapeMismatch})
    {
        std::vector<char> buf;
        // 失败时带本端算出的根，便于对端定位是哪一块错了。
        const std::string root = (s == S::kOk) ? std::string{} : kSha;
        REQUIRE(encodeCompleteAnswer(s, root, buf).ok());

        CompleteStatus back;
        std::string rootBack;
        std::string err;
        REQUIRE(decodeCompleteAnswer(buf, back, rootBack, err));
        INFO("status = " << toString(s));
        REQUIRE(back == s);
        REQUIRE(rootBack == root);
    }
}

TEST_CASE("complete answer rejects unknown status", "[transfer][payload]")
{
    std::vector<char> buf;
    buf.push_back(9);  // 未知
    buf.push_back(0);  // root 长度 0

    CompleteStatus s;
    std::string root;
    std::string err;
    REQUIRE_FALSE(decodeCompleteAnswer(buf, s, root, err));
    REQUIRE(err.find("unknown") != std::string::npos);
}

TEST_CASE("complete request rejects non-hex root", "[transfer][payload]")
{
    std::vector<char> buf;
    REQUIRE_FALSE(encodeCompleteRequest(std::string(64, 'z'), 0, buf).ok());

    // 解码侧也要拒，不能只靠发送方自觉。
    std::vector<char> bad;
    bad.push_back(64);
    for (int i = 0; i < 64; ++i)
    {
        bad.push_back('z');
    }
    for (int i = 0; i < 8; ++i)
    {
        bad.push_back(0);
    }

    std::string root;
    std::uint64_t size = 0;
    std::string err;
    REQUIRE_FALSE(decodeCompleteRequest(bad, root, size, err));
}

// ===========================================================================
// 取消
// ===========================================================================

TEST_CASE("cancel round trips", "[transfer][payload]")
{
    std::vector<char> buf;
    REQUIRE(encodeCancel(CancelReason::kHashMismatch, "chunk 42 corrupt", buf).ok());

    CancelReason r{};
    std::string msg;
    std::string err;
    REQUIRE(decodeCancel(buf, r, msg, err));
    REQUIRE(r == CancelReason::kHashMismatch);
    REQUIRE(msg == "chunk 42 corrupt");
}

TEST_CASE("unknown cancel reason degrades instead of failing", "[transfer][payload]")
{
    // 取消本身是有效动作。因为"不认识原因"而拒绝整帧，会让双方卡在
    // "我发了取消但对方没收到"的状态——比失去原因信息糟糕得多。
    //
    // body 格式：u8 reason || u16 msgLen || msg（§9.1 的通用字符串约定，
    // 这里用的是 u16 长度前缀，不是路由字段那种 u8）。
    std::vector<char> buf;
    buf.push_back(99);  // 未知原因
    buf.push_back(2);   // msgLen 低字节
    buf.push_back(0);   // msgLen 高字节
    buf.push_back('h');
    buf.push_back('i');

    CancelReason r{};
    std::string msg;
    std::string err;
    REQUIRE(decodeCancel(buf, r, msg, err));
    REQUIRE(r == CancelReason::kLocalError);
    REQUIRE(msg == "hi");
}

TEST_CASE("cancel rejects truncation", "[transfer][payload]")
{
    std::vector<char> buf;
    REQUIRE(encodeCancel(CancelReason::kTimeout, "too slow", buf).ok());

    for (std::size_t cut = 0; cut < buf.size(); ++cut)
    {
        std::vector<char> partial(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cut));
        CancelReason r{};
        std::string msg;
        std::string err;
        INFO("cut at " << cut);
        REQUIRE_FALSE(decodeCancel(partial, r, msg, err));
    }
}

// ===========================================================================
// 状态名
// ===========================================================================

TEST_CASE("status names are stable strings", "[transfer][payload]")
{
    // 日志与文档比对都靠这些字符串，改名会让已有的日志解析脚本失效。
    REQUIRE(toString(AnswerStatus::kOk) == "ok");
    REQUIRE(toString(AnswerStatus::kTargetExists) == "target_exists");
    REQUIRE(toString(AnswerStatus::kDuplicateTask) == "duplicate_task");
    REQUIRE(toString(CompleteStatus::kRootMismatch) == "root_mismatch");
    REQUIRE(toString(CompleteStatus::kShapeMismatch) == "shape_mismatch");
    REQUIRE(toString(CancelReason::kPeerGone) == "peer_gone");
}
