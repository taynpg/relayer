#include "relayer/core/transport/FrameReader.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "relayer/core/protocol/Frame.h"

using namespace relayer::core::transport;
using namespace relayer::protocol;

// Status 住在 relayer 下（跨模块共用），不在 transport 里
using relayer::Status;

namespace {

std::vector<char> makeWire(FrameType type, const std::string& body, std::int64_t sessionId = 0, std::int64_t index = 0)
{
    Frame f;
    f.type = type;
    f.sessionId = sessionId;
    f.index = index;
    f.from = "a";
    f.to = "b";
    f.body.assign(body.begin(), body.end());
    return packFrame(f);
}

// 把全部缓冲喂进去，把能解的帧都取出来。
std::vector<Frame> drain(FrameReader& r, Status& st)
{
    std::vector<Frame> out;
    Frame f;
    while (r.next(f, st)) {
        out.push_back(f);
    }
    return out;
}

}   // namespace

// ===========================================================================
// 基本
// ===========================================================================

TEST_CASE("reader decodes a complete frame", "[transport][framing]")
{
    const auto wire = makeWire(FrameType::kAskId, "hello");
    FrameReader r;
    Status st;

    REQUIRE(r.append(wire).ok());
    Frame f;
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(f.type == FrameType::kAskId);
    REQUIRE(std::string(f.body.begin(), f.body.end()) == "hello");
    REQUIRE(r.buffered() == 0);
    REQUIRE(r.framesRead() == 1);
    REQUIRE(r.garbageBytes() == 0);
}

TEST_CASE("reader returns false with no data", "[transport][framing]")
{
    FrameReader r;
    Status st;
    Frame f;
    REQUIRE_FALSE(r.next(f, st));
    REQUIRE(st.ok());
}

// ===========================================================================
// 半包：逐字节喂入
// ===========================================================================

TEST_CASE("reader handles one byte at a time", "[transport][framing]")
{
    // 这是最严格的分包测试：任何"假设一次 read 至少能拿到 N 字节"的实现
    // 都会在这里失败。
    const auto wire = makeWire(FrameType::kAnswerId, "payload");
    FrameReader r;
    Status st;

    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        REQUIRE(r.append(&wire[i], 1).ok());
        Frame f;
        REQUIRE_FALSE(r.next(f, st));   // 还没收全
        REQUIRE(st.ok());
        // 一个字节都没被当成垃圾丢掉
        REQUIRE(r.garbageBytes() == 0);
    }

    // 最后一个字节
    REQUIRE(r.append(&wire[wire.size() - 1], 1).ok());
    Frame f;
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(f.type == FrameType::kAnswerId);
    REQUIRE(r.buffered() == 0);
}

TEST_CASE("reader handles every possible split point", "[transport][framing]")
{
    // 对每一帧，在每一个位置切开，两段分别喂入。这能抓到"只在某个偏移
    // 上出错"的分支。
    const auto wire = makeWire(FrameType::kAskFileList, "0123456789");
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        FrameReader r;
        Status st;
        INFO("split=" << split);

        REQUIRE(r.append(wire.data(), split).ok());
        REQUIRE(r.append(wire.data() + split, wire.size() - split).ok());

        Frame f;
        REQUIRE(r.next(f, st));
        REQUIRE(st.ok());
        REQUIRE(f.type == FrameType::kAskFileList);
        REQUIRE(r.buffered() == 0);
    }
}

// ===========================================================================
// 粘包
// ===========================================================================

TEST_CASE("reader decodes multiple frames from one chunk", "[transport][framing]")
{
    std::vector<char> all;
    auto a = makeWire(FrameType::kAskId, "one");
    auto b = makeWire(FrameType::kAnswerId, "two");
    auto c = makeWire(FrameType::kAskHome, "three");
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    all.insert(all.end(), c.begin(), c.end());

    FrameReader r;
    Status st;
    REQUIRE(r.append(all).ok());

    const auto frames = drain(r, st);
    REQUIRE(st.ok());
    REQUIRE(frames.size() == 3);
    REQUIRE(frames[0].type == FrameType::kAskId);
    REQUIRE(frames[1].type == FrameType::kAnswerId);
    REQUIRE(frames[2].type == FrameType::kAskHome);
    REQUIRE(r.buffered() == 0);
    REQUIRE(r.framesRead() == 3);
}

TEST_CASE("reader keeps the trailing partial frame", "[transport][framing]")
{
    // 两个完整帧 + 第三个帧的前半段
    const auto a = makeWire(FrameType::kAskId, "aa");
    const auto b = makeWire(FrameType::kAskHome, "bb");
    const auto c = makeWire(FrameType::kAskHeart, "cc");

    std::vector<char> chunk;
    chunk.insert(chunk.end(), a.begin(), a.end());
    chunk.insert(chunk.end(), b.begin(), b.end());
    chunk.insert(chunk.end(), c.begin(), c.begin() + 10);   // 半个头

    FrameReader r;
    Status st;
    REQUIRE(r.append(chunk).ok());

    auto frames = drain(r, st);
    REQUIRE(st.ok());
    REQUIRE(frames.size() == 2);
    REQUIRE(r.buffered() == 10);   // 剩下的半个头留着

    // 补上剩余部分
    REQUIRE(r.append(c.data() + 10, c.size() - 10).ok());
    frames = drain(r, st);
    REQUIRE(st.ok());
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].type == FrameType::kAskHeart);
    REQUIRE(r.buffered() == 0);
}

// ===========================================================================
// 垃圾数据
// ===========================================================================

TEST_CASE("reader skips leading garbage", "[transport][framing]")
{
    std::vector<char> chunk = {'X', 'Y', 'Z', '!', '?'};
    const auto wire = makeWire(FrameType::kAskId, "clean");
    chunk.insert(chunk.end(), wire.begin(), wire.end());

    FrameReader r;
    Status st;
    REQUIRE(r.append(chunk).ok());

    Frame f;
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(f.type == FrameType::kAskId);
    REQUIRE(r.garbageBytes() == 5);
    REQUIRE(r.buffered() == 0);
}

TEST_CASE("reader skips garbage between frames", "[transport][framing]")
{
    std::vector<char> chunk;
    const auto a = makeWire(FrameType::kAskId, "1");
    const auto b = makeWire(FrameType::kAskHome, "2");
    chunk.insert(chunk.end(), a.begin(), a.end());
    chunk.push_back('\xFF');
    chunk.push_back('\x00');
    chunk.insert(chunk.end(), b.begin(), b.end());

    FrameReader r;
    Status st;
    REQUIRE(r.append(chunk).ok());

    const auto frames = drain(r, st);
    REQUIRE(st.ok());
    REQUIRE(frames.size() == 2);
    REQUIRE(r.garbageBytes() == 2);
}

TEST_CASE("reader treats a partial magic as pending, not garbage", "[transport][framing]")
{
    // 'R' 是 magic 的第一个字节。单独收到它时不能丢——可能是下一帧的开头。
    const auto wire = makeWire(FrameType::kAskId, "x");
    FrameReader r;
    Status st;

    REQUIRE(r.append(wire.data(), 1).ok());   // 只发 'R'
    Frame f;
    REQUIRE_FALSE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(r.buffered() == 1);   // 保留了，没被当垃圾丢掉

    REQUIRE(r.append(wire.data() + 1, wire.size() - 1).ok());
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(r.garbageBytes() == 0);
}

// ===========================================================================
// 错误
// ===========================================================================

TEST_CASE("reader reports protocol error on bad version", "[transport][framing][error]")
{
    auto wire = makeWire(FrameType::kAskId, "x");

    // 版本在偏移 4（magic 之后）。改成 99。
    //
    // 关键：必须同时重算头 CRC。unpackFrame 的顺序是"长度 sanity -> 头 CRC
    // -> 版本"，只改版本会让头 CRC 先失配，于是它跳 1 字节重同步，
    // 根本走不到版本检查。这样测出来的是"CRC 重同步"，不是"版本不符"。
    wire[4] = static_cast<char>(99);
    const std::uint32_t newCrc = crc32(wire.data(), 32);
    wire[32] = static_cast<char>(newCrc & 0xFF);
    wire[33] = static_cast<char>((newCrc >> 8) & 0xFF);
    wire[34] = static_cast<char>((newCrc >> 16) & 0xFF);
    wire[35] = static_cast<char>((newCrc >> 24) & 0xFF);

    FrameReader r;
    Status st;
    REQUIRE(r.append(wire).ok());

    Frame f;
    REQUIRE_FALSE(r.next(f, st));
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
    // 出错后缓冲清空，避免下次再解析同样的坏字节
    REQUIRE(r.buffered() == 0);
}

TEST_CASE("version mismatch without crc fix resyncs instead", "[transport][framing][error]")
{
    // 上一条的反面：只改版本、不改 CRC 时，走的应当是"跳 1 字节重同步"，
    // 而不是判协议错误。这两条一起把 unpackFrame 的检查顺序钉死。
    auto wire = makeWire(FrameType::kAskId, "x");
    wire[4] = static_cast<char>(99);

    FrameReader r;
    Status st;
    REQUIRE(r.append(wire).ok());

    Frame f;
    REQUIRE_FALSE(r.next(f, st));
    REQUIRE(st.ok());   // 不是错误，只是这一帧被当垃圾跳过了
    REQUIRE(r.garbageBytes() > 0);
}

TEST_CASE("reader reports protocol error on body crc mismatch", "[transport][framing][error]")
{
    auto wire = makeWire(FrameType::kAskId, "abcdef");
    wire.back() = static_cast<char>(wire.back() ^ 0xFF);   // 破坏 body 尾部

    FrameReader r;
    Status st;
    REQUIRE(r.append(wire).ok());

    Frame f;
    REQUIRE_FALSE(r.next(f, st));
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

TEST_CASE("reader enforces the buffer limit", "[transport][framing][error]")
{
    FrameReader r;
    Status st;

    SECTION("小批 append 正常累计")
    {
        std::vector<char> chunk(1024, 'Q');
        REQUIRE(r.append(chunk).ok());
        REQUIRE(r.buffered() == 1024);
    }

    SECTION("越界的那一次被拒，且缓冲不被破坏")
    {
        // 用一块接近上限的输入直接触发越界判断，不必真的灌满 64MB。
        // append 检查的是 buffer_.size() + len > kMaxBufferedBytes，
        // 所以先放 1 字节，再 append 一个 kMaxBufferedBytes 大小的声称长度
        // 就会越界——但那样会真的去读 64MB 内存。
        //
        // 折中：分块逼近，但块大、次数少，总拷贝量约 64MB，跑一次几十毫秒。
        const std::vector<char> chunk(4 * 1024 * 1024, 'Q');
        std::size_t appends = 0;
        while (r.buffered() + chunk.size() <= kMaxBufferedBytes) {
            REQUIRE(r.append(chunk).ok());
            ++appends;
        }
        REQUIRE(appends == 16);   // 64MB / 4MB
        REQUIRE(r.buffered() <= kMaxBufferedBytes);

        const std::size_t before = r.buffered();
        REQUIRE_FALSE(r.append(chunk).ok());
        REQUIRE(r.buffered() == before);   // 被拒时不改变已有内容
    }
}

TEST_CASE("reader resyncs past garbage instead of growing", "[transport][framing]")
{
    // 反向验证：持续喂垃圾时缓冲**不应该**无限增长——字节会被当作前导
    // 垃圾逐步丢弃。这条是上一条的补充：上限只是兜底，正常重同步不该走到上限。
    FrameReader r;
    std::vector<char> garbage(4096, 'Q');   // 'Q' 不是 magic 首字节

    for (int i = 0; i < 8; ++i) {
        REQUIRE(r.append(garbage).ok());
        Status st;
        Frame f;
        while (r.next(f, st)) {
        }
        REQUIRE(st.ok());
        // 重同步会把垃圾丢掉，缓冲不会累积
        REQUIRE(r.buffered() <= 3);   // 最多残留一个不完整的 magic 前缀
    }
    REQUIRE(r.garbageBytes() > 0);
}

TEST_CASE("reader rejects null pointer with non-zero length", "[transport][framing][error]")
{
    FrameReader r;
    REQUIRE_FALSE(r.append(nullptr, 8).ok());
    REQUIRE(r.append(nullptr, 0).ok());   // 长度为 0 允许
}

// ===========================================================================
// 与协议层的配合：帧 body 里含 magic
// ===========================================================================

TEST_CASE("reader is not confused by magic inside body", "[transport][framing]")
{
    // body 里塞一段看起来像帧头的数据。unpackFrame 靠"长度 + CRC"排除，
    // 而 reader 必须原样把整帧交出来。
    std::string tricky;
    const auto inner = makeWire(FrameType::kAskId, "inner");
    tricky.assign(inner.begin(), inner.end());

    const auto wire = makeWire(FrameType::kAnswerId, tricky);
    FrameReader r;
    Status st;
    REQUIRE(r.append(wire).ok());

    Frame f;
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(f.type == FrameType::kAnswerId);
    REQUIRE(f.body.size() == tricky.size());
    REQUIRE(std::equal(f.body.begin(), f.body.end(), tricky.begin()));
    REQUIRE(r.buffered() == 0);
}

TEST_CASE("reader statistics track activity", "[transport][framing]")
{
    FrameReader r;
    Status st;
    Frame f;

    REQUIRE(r.framesRead() == 0);
    REQUIRE(r.garbageBytes() == 0);
    REQUIRE(r.buffered() == 0);

    // 3 字节垃圾 + 1 帧
    std::vector<char> chunk = {'a', 'b', 'c'};
    const auto wire = makeWire(FrameType::kAskId, "x");
    chunk.insert(chunk.end(), wire.begin(), wire.end());
    REQUIRE(r.append(chunk).ok());
    REQUIRE(r.next(f, st));

    REQUIRE(r.framesRead() == 1);
    REQUIRE(r.garbageBytes() == 3);

    SECTION("clear 重置缓冲但不清统计")
    {
        r.clear();
        REQUIRE(r.buffered() == 0);
        REQUIRE(r.framesRead() == 1);   // 统计是累计的，用于诊断
    }
}

// ===========================================================================
// 大帧
// ===========================================================================

TEST_CASE("reader handles a large frame split across chunks", "[transport][framing]")
{
    // 200KB，超出 body CRC 阈值，走不带 CRC 的路径。分 5 段喂入。
    std::string body(200 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>(i % 251);
    }
    const auto wire = makeWire(FrameType::kAskFileSamples, body);
    REQUIRE(wire.size() > 200 * 1024);

    FrameReader r;
    Status st;
    const std::size_t chunkSize = wire.size() / 5 + 1;
    for (std::size_t off = 0; off < wire.size(); off += chunkSize) {
        const std::size_t n = std::min(chunkSize, wire.size() - off);
        REQUIRE(r.append(wire.data() + off, n).ok());
    }

    Frame f;
    REQUIRE(r.next(f, st));
    REQUIRE(st.ok());
    REQUIRE(f.body.size() == body.size());
    REQUIRE(std::equal(f.body.begin(), f.body.end(), body.begin()));
}
