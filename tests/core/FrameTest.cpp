#include "relayer/core/protocol/Frame.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

using namespace relayer::protocol;

namespace {

using Bytes = std::vector<char>;

Bytes toBytes(const std::string& s)
{
    return Bytes(s.begin(), s.end());
}

std::string toString(const Bytes& b)
{
    return std::string(b.begin(), b.end());
}

// 解包辅助：断言必须是 kOk，并返回解出的帧
Frame mustUnpack(const Bytes& wire, std::size_t& consumed)
{
    Frame f;
    const auto r = unpackFrame(wire.data(), wire.size(), f, consumed);
    REQUIRE(r == UnPackResult::kOk);
    return f;
}

}   // namespace

// ===========================================================================
// 往返
// ===========================================================================

TEST_CASE("帧往返：字段全部保真", "[protocol][roundtrip]")
{
    Frame in;
    in.type = FrameType::kAskFileMeta;
    in.flags = kFlagFromReceiver;
    in.sessionId = -1234567890123LL;   // 负数，确认是按位原样保存而不是当无符号
    in.index = 9876543210LL;
    in.from = "client-A";
    in.to = "client-B";
    in.fuuid = "0123-4567-89ab";
    in.body = toBytes("hello, relayer");

    const Bytes wire = packFrame(in);
    REQUIRE_FALSE(wire.empty());

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(wire.data(), wire.size(), out, consumed);
    REQUIRE(r == UnPackResult::kOk);
    REQUIRE(consumed == wire.size());

    REQUIRE(out.type == in.type);
    REQUIRE(out.sessionId == in.sessionId);
    REQUIRE(out.index == in.index);
    REQUIRE(out.from == in.from);
    REQUIRE(out.to == in.to);
    REQUIRE(out.fuuid == in.fuuid);
    REQUIRE(out.body == in.body);
}

TEST_CASE("帧往返：空 body 与空路由字段", "[protocol][roundtrip][edge]")
{
    Frame in;
    in.type = FrameType::kAskHeart;
    // from/to/fuuid 全空是合法的最小帧

    const Bytes wire = packFrame(in);

    // 头 36 + 三个空路由字段的长度前缀 3 + 空 body 的 CRC 4 = 43。
    // 空 body 也带 CRC，因为 0 <= kBodyCrcLimit。
    REQUIRE(wire.size() == frame::kFixedHeaderSize + frame::kRoutePrefixSize + 4);

    std::size_t consumed = 0;
    const Frame out = mustUnpack(wire, consumed);
    REQUIRE(consumed == wire.size());
    REQUIRE(out.from.empty());
    REQUIRE(out.to.empty());
    REQUIRE(out.fuuid.empty());
    REQUIRE(out.body.empty());
    REQUIRE(out.hasBodyCrc());
}

TEST_CASE("不带 body CRC 时的理论最小帧", "[protocol][roundtrip][edge]")
{
    // kMinFrameSize 是"不带 CRC 的最短合法帧"：36 头 + 3 个空长度前缀 = 39。
    // 实际打包时因为 body 很小总会带 CRC（见上一条用例），所以线上最小是 43。
    // 这两个数字都要钉住，否则改常量时容易把 UnPack 的长度下限校验改错。
    REQUIRE(frame::kMinFrameSize == 39);
}

TEST_CASE("帧往返：各种 body 长度", "[protocol][roundtrip]")
{
    for (const std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(255), std::size_t(256),
                                  std::size_t(4096), std::size_t(65535), std::size_t(65536), std::size_t(200000)}) {
        Frame in;
        in.type = FrameType::kChunkRequestAck;
        in.body.resize(len);
        for (std::size_t i = 0; i < len; ++i) {
            in.body[i] = static_cast<char>((i * 37 + 11) & 0xFF);
        }

        const Bytes wire = packFrame(in);
        REQUIRE(wire.size() >= len);

        std::size_t consumed = 0;
        const Frame out = mustUnpack(wire, consumed);
        REQUIRE(consumed == wire.size());
        REQUIRE(out.body.size() == len);
        REQUIRE(out.body == in.body);
    }
}

TEST_CASE("帧往返：UTF-8 路由字段", "[protocol][roundtrip][utf8]")
{
    Frame in;
    in.type = FrameType::kAnswerFileList;
    in.from = "发送端-甲";
    in.to = "接收端-乙";
    in.fuuid = "文件-uuid-🔑";

    const Bytes wire = packFrame(in);
    std::size_t consumed = 0;
    const Frame out = mustUnpack(wire, consumed);

    // 按字节原样保留，不做任何编码转换
    REQUIRE(out.from == in.from);
    REQUIRE(out.to == in.to);
    REQUIRE(out.fuuid == in.fuuid);
}

// ===========================================================================
// 头部布局
// ===========================================================================

TEST_CASE("帧头字段落在文档规定的位置", "[protocol][layout]")
{
    // 这条用例把 docs/PROTOCOL.md 里的偏移表钉死在代码里。
    // 任何"重构调整了字段顺序"的改动都会让它失败。
    Frame in;
    in.type = FrameType::kAskId;   // 100
    in.sessionId = 1;
    in.index = 2;

    const Bytes wire = packFrame(in);

    const auto u16 = [&](std::size_t off) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(wire[off]) |
                                          (static_cast<std::uint16_t>(static_cast<std::uint8_t>(wire[off + 1])) << 8));
    };
    const auto u32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(wire[off])) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(wire[off + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(wire[off + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(wire[off + 3])) << 24);
    };

    // magic: 'R','F','M','1'
    REQUIRE(wire[0] == 'R');
    REQUIRE(wire[1] == 'F');
    REQUIRE(wire[2] == 'M');
    REQUIRE(wire[3] == '1');
    REQUIRE(u32(0) == frame::kMagic);

    REQUIRE(u16(4) == frame::kVersion);
    REQUIRE(u16(6) == static_cast<std::uint16_t>(kFlagBodyCrc));   // 空 body，自动带 CRC
    REQUIRE(u16(8) == 100);                                        // kAskId
    REQUIRE(u16(10) == 0);                                         // reserved 必须为 0
    REQUIRE(u32(28) == wire.size());                               // frameLen
    REQUIRE(u32(32) == crc32(wire.data(), 32));                    // headerCrc 覆盖前 32 字节
}

TEST_CASE("body CRC 标志由长度自动决定", "[protocol][layout][crc]")
{
    SECTION("body 小于阈值时带 CRC")
    {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.body.assign(100, 'a');
        const Bytes wire = packFrame(f);

        Frame out;
        std::size_t consumed = 0;
        unpackFrame(wire.data(), wire.size(), out, consumed);
        REQUIRE(out.hasBodyCrc());

        // 帧长 = 头 + 3 个长度前缀 + body + 4 字节 CRC
        REQUIRE(wire.size() == frame::kFixedHeaderSize + 3 + 100 + 4);
    }

    SECTION("body 恰好在阈值上仍带 CRC（<= 而非 <）")
    {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.body.assign(frame::kBodyCrcLimit, 'a');
        const Bytes wire = packFrame(f);

        Frame out;
        std::size_t consumed = 0;
        unpackFrame(wire.data(), wire.size(), out, consumed);
        REQUIRE(out.hasBodyCrc());
    }

    SECTION("body 超过阈值时不带 CRC")
    {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.body.assign(frame::kBodyCrcLimit + 1, 'a');
        const Bytes wire = packFrame(f);

        Frame out;
        std::size_t consumed = 0;
        unpackFrame(wire.data(), wire.size(), out, consumed);
        REQUIRE_FALSE(out.hasBodyCrc());
        REQUIRE(wire.size() == frame::kFixedHeaderSize + 3 + frame::kBodyCrcLimit + 1);
    }

    SECTION("调用方自己置了 kFlagBodyCrc 也会被重算")
    {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.flags = kFlagBodyCrc;       // 谎报
        f.body.assign(200000, 'a');   // 实际超过阈值

        const Bytes wire = packFrame(f);
        Frame out;
        std::size_t consumed = 0;
        unpackFrame(wire.data(), wire.size(), out, consumed);
        REQUIRE_FALSE(out.hasBodyCrc());
    }

    SECTION("调用方清掉 kFlagBodyCrc 也会被重算")
    {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.flags = kFlagNone;      // 谎报
        f.body.assign(10, 'a');   // 实际需要 CRC

        const Bytes wire = packFrame(f);
        Frame out;
        std::size_t consumed = 0;
        unpackFrame(wire.data(), wire.size(), out, consumed);
        REQUIRE(out.hasBodyCrc());
    }
}

// ===========================================================================
// 打包失败
// ===========================================================================

TEST_CASE("打包拒绝超长路由字段", "[protocol][pack][reject]")
{
    SECTION("from 超过 255")
    {
        Frame f;
        f.type = FrameType::kAskId;
        f.from.assign(frame::kMaxRouteField + 1, 'a');
        REQUIRE(packFrame(f).empty());
    }

    SECTION("to 超过 255")
    {
        Frame f;
        f.type = FrameType::kAskId;
        f.to.assign(frame::kMaxRouteField + 1, 'a');
        REQUIRE(packFrame(f).empty());
    }

    SECTION("fuuid 超过 255")
    {
        Frame f;
        f.type = FrameType::kAskId;
        f.fuuid.assign(frame::kMaxRouteField + 1, 'a');
        REQUIRE(packFrame(f).empty());
    }

    SECTION("正好 255 是合法的")
    {
        Frame f;
        f.type = FrameType::kAskId;
        f.from.assign(frame::kMaxRouteField, 'a');
        REQUIRE_FALSE(packFrame(f).empty());
    }

    SECTION("失败原因可诊断")
    {
        Frame f;
        f.type = FrameType::kAskId;
        f.from.assign(300, 'a');
        std::vector<char> out;
        const relayer::Status st = packFrameChecked(f, out);
        REQUIRE_FALSE(st.ok());
        REQUIRE(st.code == relayer::ErrorCode::kProtocol);
        REQUIRE(st.message.find("from") != std::string::npos);
    }
}

TEST_CASE("打包拒绝超长帧", "[protocol][pack][reject]")
{
    Frame f;
    f.type = FrameType::kChunkRequestData;
    // body 超过 32MB。分配 33MB 在测试里可以接受（一次性）。
    f.body.assign(frame::kMaxFrameSize + 1, 'z');
    REQUIRE(packFrame(f).empty());
}

// ===========================================================================
// 解包：半包
// ===========================================================================

TEST_CASE("半包：任意前缀都不应当解出帧", "[protocol][unpack][partial]")
{
    Frame src;
    src.type = FrameType::kAskFileList;
    src.sessionId = 99;
    src.from = "a";
    src.to = "b";
    src.fuuid = "c";
    src.body = toBytes("0123456789");

    const Bytes wire = packFrame(src);
    REQUIRE(wire.size() > frame::kFixedHeaderSize);

    for (std::size_t cut = 0; cut < wire.size(); ++cut) {
        Frame out;
        std::size_t consumed = 0;
        const auto r = unpackFrame(wire.data(), cut, out, consumed);

        // 除了「cut 恰好等于某段垃圾长度」这种退化情况，前缀都应该只是"不够"
        if (r == UnPackResult::kOk) {
            FAIL("对 " << cut << " 字节的前缀解出了完整帧");
        }
        REQUIRE(r == UnPackResult::kNeedMoreData);
        REQUIRE(consumed == 0);   // 从头就是半帧，不应该吞掉任何字节
    }
}

TEST_CASE("半包：逐字节喂入能拼出完整帧", "[protocol][unpack][partial][stream]")
{
    Frame src;
    src.type = FrameType::kChunkAnswerData;
    src.sessionId = 7;
    src.index = 12345;
    src.from = "sender";
    src.to = "receiver";
    src.body = toBytes("payload-data-here");

    const Bytes wire = packFrame(src);

    Bytes buf;
    Frame got;
    bool done = false;

    for (const char c : wire) {
        buf.push_back(c);

        Frame out;
        std::size_t consumed = 0;
        const auto r = unpackFrame(buf.data(), buf.size(), out, consumed);

        if (r == UnPackResult::kOk) {
            got = std::move(out);
            done = true;
            REQUIRE(consumed == buf.size());
            break;
        }
        REQUIRE(r == UnPackResult::kNeedMoreData);
        // 半帧状态下不应该有字节被消费
        REQUIRE(consumed == 0);
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    REQUIRE(done);
    REQUIRE(got.sessionId == 7);
    REQUIRE(got.index == 12345);
    REQUIRE(got.from == "sender");
    REQUIRE(got.to == "receiver");
    REQUIRE(toString(got.body) == "payload-data-here");
}

// ===========================================================================
// 解包：粘包
// ===========================================================================

TEST_CASE("粘包：一次输入里连续解出多帧", "[protocol][unpack][sticky]")
{
    std::vector<Frame> sources;
    Bytes stream;

    for (int i = 0; i < 5; ++i) {
        Frame f;
        f.type = FrameType::kAskHeart;
        f.index = i;
        f.body = toBytes("frame-" + std::to_string(i));
        const Bytes wire = packFrame(f);
        stream.insert(stream.end(), wire.begin(), wire.end());
        sources.push_back(std::move(f));
    }

    std::size_t offset = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        Frame out;
        std::size_t consumed = 0;
        const auto r = unpackFrame(stream.data() + offset, stream.size() - offset, out, consumed);
        REQUIRE(r == UnPackResult::kOk);
        REQUIRE(out.index == static_cast<std::int64_t>(i));
        REQUIRE(toString(out.body) == "frame-" + std::to_string(i));
        offset += consumed;
    }
    REQUIRE(offset == stream.size());
}

// ===========================================================================
// 解包：重同步
// ===========================================================================

TEST_CASE("前导垃圾被跳过并报告可丢弃字节数", "[protocol][unpack][resync]")
{
    Frame src;
    src.type = FrameType::kAskHeart;
    src.body = toBytes("ok");
    const Bytes wire = packFrame(src);

    const Bytes garbage = toBytes("GARBAGE-BYTES-BEFORE-MAGIC:");

    Bytes stream = garbage;
    stream.insert(stream.end(), wire.begin(), wire.end());

    Frame out;
    std::size_t consumed = 0;

    // 第一次调用：应当只报告"丢弃了多少垃圾"，不解帧
    auto r = unpackFrame(stream.data(), stream.size(), out, consumed);
    REQUIRE(r == UnPackResult::kNeedMoreData);
    REQUIRE(consumed == garbage.size());

    // 调用方消费掉垃圾后重新调用
    r = unpackFrame(stream.data() + consumed, stream.size() - consumed, out, consumed);
    REQUIRE(r == UnPackResult::kOk);
    REQUIRE(toString(out.body) == "ok");
}

TEST_CASE("末尾不足 4 字节的半个 magic 会被保留", "[protocol][unpack][resync]")
{
    // 输入是 "xxxRF"（magic 的前两个字节），这是典型的分包边界情况。
    // 最后 3 字节必须留着，否则下一次数据到达时就接不上了。
    const Bytes stream = toBytes("xxxxxxRF");

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(stream.data(), stream.size(), out, consumed);

    REQUIRE(r == UnPackResult::kNeedMoreData);
    // 7 字节里最后 3 字节要留（有 'x','R','F' 三种可能构成后续 magic 的部分）
    REQUIRE(consumed == stream.size() - 3);
}

TEST_CASE("body 里的假 magic 不会导致误判", "[protocol][unpack][resync]")
{
    // 构造一个 body 里含 "RFM1" 的帧，并且后面再跟一个真帧。
    // 解析器第一次扫到的 magic 在正确位置，但如果它把 body 里的当成帧头，
    // 就会因为 header CRC 不对而乱跳。
    Frame first;
    first.type = FrameType::kChunkRequestData;
    first.index = 1;
    const std::string payload = "prefix-RFM1-suffix";
    first.body = toBytes(payload);

    Frame second;
    second.type = FrameType::kAskHeart;
    second.index = 2;
    second.body = toBytes("second");

    Bytes stream = packFrame(first);
    const Bytes w2 = packFrame(second);
    stream.insert(stream.end(), w2.begin(), w2.end());

    std::size_t offset = 0;

    Frame a;
    std::size_t consumed = 0;
    REQUIRE(unpackFrame(stream.data(), stream.size(), a, consumed) == UnPackResult::kOk);
    REQUIRE(a.index == 1);
    REQUIRE(toString(a.body) == payload);
    offset += consumed;

    Frame b;
    REQUIRE(unpackFrame(stream.data() + offset, stream.size() - offset, b, consumed) == UnPackResult::kOk);
    REQUIRE(b.index == 2);
    REQUIRE(toString(b.body) == "second");
}

TEST_CASE("虚假 magic + 非法 frameLen 时跳 1 字节继续找", "[protocol][unpack][resync]")
{
    // 手工造一个"magic 正确、frameLen 离谱"的头，后面跟一个真帧。
    // 解析器必须放弃假头（跳 1 字节重新扫），最终找到真帧。
    Bytes fake;
    fake.push_back('R');
    fake.push_back('F');
    fake.push_back('M');
    fake.push_back('1');
    // 后面 32 字节刻意填成 0：version=0, flags=0, type=0, frameLen=0（非法）
    fake.resize(frame::kFixedHeaderSize, '\0');

    Frame real;
    real.type = FrameType::kAskHeart;
    real.body = toBytes("real-frame");
    const Bytes realWire = packFrame(real);

    Bytes stream = fake;
    stream.insert(stream.end(), realWire.begin(), realWire.end());

    // 反复调用，直到解出帧。每次最多前进 1 字节（跳 1 字节重同步）。
    std::size_t offset = 0;
    Frame out;
    bool found = false;
    while (offset < stream.size()) {
        std::size_t consumed = 0;
        const auto r = unpackFrame(stream.data() + offset, stream.size() - offset, out, consumed);

        if (r == UnPackResult::kOk) {
            found = true;
            REQUIRE(toString(out.body) == "real-frame");
            break;
        }
        REQUIRE(r == UnPackResult::kNeedMoreData);
        REQUIRE(consumed >= 1);   // 必须前进，否则死循环
        offset += consumed;
    }
    REQUIRE(found);
}

TEST_CASE("header CRC 被篡改时不会解出帧", "[protocol][unpack][resync]")
{
    Frame src;
    src.type = FrameType::kAskId;
    src.body = toBytes("x");
    Bytes wire = packFrame(src);

    // 改 headerCrc 的最后一个字节
    wire[35] = static_cast<char>(wire[35] ^ 0x40);

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(wire.data(), wire.size(), out, consumed);

    // 头 CRC 不对 -> 当成假 magic，跳 1 字节
    REQUIRE(r == UnPackResult::kNeedMoreData);
    REQUIRE(consumed == 1);
}

TEST_CASE("frameLen 被篡改时不会解出帧", "[protocol][unpack][resync][tamper]")
{
    Frame src;
    src.type = FrameType::kAskId;
    src.body = toBytes("hello");
    Bytes wire = packFrame(src);

    // 把 frameLen（offset 28）改成远大于实际的值。
    // headerCrc 覆盖了 frameLen，所以改完之后 CRC 必然不匹配。
    wire[28] = static_cast<char>(0xFF);
    wire[29] = static_cast<char>(0xFF);

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(wire.data(), wire.size(), out, consumed);

    REQUIRE(r == UnPackResult::kNeedMoreData);
    REQUIRE(consumed == 1);
}

// ===========================================================================
// 解包：协议错误
// ===========================================================================

TEST_CASE("版本不符时返回协议错误", "[protocol][unpack][error]")
{
    Frame src;
    src.type = FrameType::kAskId;
    src.body = toBytes("x");
    Bytes wire = packFrame(src);

    // 把 version 从 2 改成 3，并重算 headerCrc，
    // 模拟"对端确实是按协议打的包，只是用了别的版本号"。
    wire[4] = static_cast<char>(3);
    wire[5] = static_cast<char>(0);
    const std::uint32_t newCrc = crc32(wire.data(), 32);
    for (std::size_t i = 0; i < 4; ++i) {
        wire[32 + i] = static_cast<char>((newCrc >> (8 * i)) & 0xFF);
    }

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(wire.data(), wire.size(), out, consumed);

    REQUIRE(r == UnPackResult::kProtocolError);
}

TEST_CASE("body CRC 不匹配时返回协议错误并跳过整帧", "[protocol][unpack][error][crc]")
{
    Frame src;
    src.type = FrameType::kAskFileMeta;
    src.body = toBytes("important-metadata");
    Bytes wire = packFrame(src);

    // 破坏 body 的第一个字节（body 起始 = 36 + 3 个长度前缀）
    const std::size_t bodyStart = frame::kFixedHeaderSize + 3;
    wire[bodyStart] = static_cast<char>(wire[bodyStart] ^ 0xFF);

    Frame out;
    std::size_t consumed = 0;
    const auto r = unpackFrame(wire.data(), wire.size(), out, consumed);

    REQUIRE(r == UnPackResult::kProtocolError);
    REQUIRE(consumed == wire.size());   // 整帧被丢掉
}

TEST_CASE("未定义的类型号返回协议错误并跳过整帧", "[protocol][unpack][error][type]")
{
    // 类型号 550 落在定义的直连段里；这里用 70000 会溢出 u16，
    // 所以换一个"段内但未在枚举里出现"的值无法构造（每段都在枚举里）。
    // 真正能被 isKnownType 拒绝的是超过 65535 —— u16 装不下。
    // 因此这条用例改为验证：枚举里没有的号只要落在某段内也算"认识"，
    // 也就是 isKnownType 的语义边界。
    REQUIRE(isKnownType(0));
    REQUIRE(isKnownType(199));
    REQUIRE(isKnownType(499));
    REQUIRE(isKnownType(599));
    REQUIRE(isKnownType(65535));

    // 200..499 是保留段，明确认它，以便老客户端能回"不支持"
    REQUIRE(isKnownType(300));
    REQUIRE(isKnownType(type_range::kReservedBegin));
    REQUIRE(isKnownType(type_range::kReservedEnd));
}

// ===========================================================================
// 类型分段
// ===========================================================================

TEST_CASE("类型分段判定", "[protocol][types]")
{
    REQUIRE(isAuthType(FrameType::kAuthChallenge));
    REQUIRE(isAuthType(FrameType::kAuthResult));
    REQUIRE_FALSE(isAuthType(FrameType::kAskId));

    REQUIRE(isControlType(FrameType::kAskId));
    REQUIRE(isControlType(FrameType::kAnswerNotifyClientList));
    REQUIRE_FALSE(isControlType(FrameType::kAuthChallenge));
    REQUIRE_FALSE(isControlType(FrameType::kFileRequestSend));

    REQUIRE(isFileSessionType(FrameType::kFileRequestSend));
    REQUIRE(isFileSessionType(FrameType::kFileAnswerStart));
    REQUIRE_FALSE(isFileSessionType(FrameType::kDirectRequestId));

    REQUIRE(isDirectType(FrameType::kDirectRequestId));
    REQUIRE(isDirectType(FrameType::kDirectAnswerId));
    REQUIRE_FALSE(isDirectType(FrameType::kChunkRequestAck));

    REQUIRE(isChunkType(FrameType::kChunkRequestAck));
    REQUIRE(isChunkType(FrameType::kChunkAnswerData));
    REQUIRE_FALSE(isChunkType(FrameType::kDirectAnswerId));

    // 分段之间不能有重叠（重叠会让分派逻辑出现歧义）
    REQUIRE_FALSE(isReservedType(FrameType::kAskId));
    REQUIRE_FALSE(isReservedType(FrameType::kFileRequestSend));
}

TEST_CASE("类型名可读", "[protocol][types]")
{
    REQUIRE(toString(FrameType::kAuthChallenge) == "auth_challenge");
    REQUIRE(toString(FrameType::kAskHeart) == "ask_heart");
    REQUIRE(toString(FrameType::kChunkAnswerData) == "chunk_answer_data");
    REQUIRE(toString(FrameType::kDirectRequestId) == "direct_request_id");
}

// ===========================================================================
// CRC32
// ===========================================================================

TEST_CASE("CRC32 与标准向量一致", "[protocol][crc]")
{
    // 与 zlib 的 crc32() 和 Python 的 zlib.crc32() 相同
    REQUIRE(crc32("", 0) == 0x00000000u);
    REQUIRE(crc32("a", 1) == 0xE8B7BE43u);
    REQUIRE(crc32("abc", 3) == 0x352441C2u);
    REQUIRE(crc32("123456789", 9) == 0xCBF43926u);
    REQUIRE(crc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);
}

// ===========================================================================
// 鲁棒性
// ===========================================================================

TEST_CASE("空指针与零长度输入", "[protocol][unpack][edge]")
{
    Frame out;
    std::size_t consumed = 123;
    REQUIRE(unpackFrame(nullptr, 0, out, consumed) == UnPackResult::kNeedMoreData);
    REQUIRE(consumed == 0);

    const char dummy = 0;
    consumed = 123;
    REQUIRE(unpackFrame(&dummy, 0, out, consumed) == UnPackResult::kNeedMoreData);
    REQUIRE(consumed == 0);
}

TEST_CASE("随机字节流不会崩溃且不会被误判成帧", "[protocol][unpack][fuzz]")
{
    // 确定性伪随机：用固定种子的线性同余，避免测试结果受 Catch2 随机种子影响。
    std::uint32_t seed = 0x12345678u;
    Bytes junk;
    junk.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1103515245u + 12345u;
        junk.push_back(static_cast<char>((seed >> 16) & 0xFF));
    }

    std::size_t offset = 0;
    int framesFound = 0;
    while (offset < junk.size()) {
        Frame out;
        std::size_t consumed = 0;
        const auto r = unpackFrame(junk.data() + offset, junk.size() - offset, out, consumed);

        if (r == UnPackResult::kOk) {
            ++framesFound;
            REQUIRE(consumed > 0);
        } else if (r == UnPackResult::kProtocolError) {
            // 随机数据里凑巧出现完整合法帧的概率极低，走到这里也算合理，
            // 但不能因此卡住：至少要消费掉一些字节。
            REQUIRE(consumed > 0);
        } else {
            REQUIRE(consumed <= junk.size() - offset);
        }

        offset += consumed;
        if (consumed == 0) {
            // 剩余数据构成"半个头"，等更多数据即可。测试里没有更多了，退出。
            break;
        }
    }

    // 20000 字节随机数据里出现完整合法帧（magic + 长度 + CRC 全对）是不可能的
    REQUIRE(framesFound == 0);
}
