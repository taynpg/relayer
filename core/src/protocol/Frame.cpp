#include "relayer/core/protocol/Frame.h"

#include <algorithm>
#include <array>
#include <string>

namespace relayer::protocol
{
namespace
{

// ---------------------------------------------------------------------------
// 小端读写
// ---------------------------------------------------------------------------
//
// 全部手写而不是 memcpy + 字节序判断：协议要求"无论主机字节序如何，线上
// 字节序列固定"，手写移位是唯一能一眼看明白的实现方式，而且对这些长度
// （最多 8 字节）编译器会优化成一条 load/store。

inline std::uint16_t readU16(const char *p) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0]) |
                                      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8));
}

inline std::uint32_t readU32(const char *p) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24);
}

inline std::uint64_t readU64(const char *p) noexcept
{
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
    {
        v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    }
    return v;
}

inline void writeU16(std::vector<char> &out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

inline void writeU32(std::vector<char> &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

inline void writeU64(std::vector<char> &out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

// 路由字段：u8 长度 + 内容。超长返回 false，**不静默截断**——
// 静默截断会让两个不同的 id 变成同一个，比直接失败危险得多。
bool writeRouteField(std::vector<char> &out, std::string_view s)
{
    if (s.size() > frame::kMaxRouteField)
    {
        return false;
    }
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(s.size())));
    out.insert(out.end(), s.begin(), s.end());
    return true;
}

// 读路由字段。avail 是"从 p 起还剩多少字节"。
bool readRouteField(const char *p, std::size_t avail, std::string &out, std::size_t &used)
{
    if (avail < 1)
    {
        return false;
    }
    const auto len = static_cast<std::size_t>(static_cast<std::uint8_t>(p[0]));
    if (avail < 1 + len)
    {
        return false;
    }
    out.assign(p + 1, len);
    used = 1 + len;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

std::uint32_t crc32(const void *data, std::size_t len) noexcept
{
    // 表在首次调用时构建，之后只读。C++11 起函数内 static 的初始化是
    // 线程安全的，所以并发首次调用也没问题。
    static const auto kTable = []() -> std::array<std::uint32_t, 256>
    {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = ((c & 1u) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    std::uint32_t crc = 0xFFFFFFFFu;
    const auto *p = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < len; ++i)
    {
        crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// 类型辅助
// ---------------------------------------------------------------------------

bool isKnownType(std::uint16_t raw) noexcept
{
    // 逐段判断是否"落在已定义号段内"。保留段 200..499 也认，
    // 这样老版本收到新版本发来的控制类扩展帧时能明确回"不支持"，
    // 而不是当成野值直接断线。
    if (raw <= type_range::kAuthEnd)
    {
        return true;
    }
    if (raw >= type_range::kControlBegin && raw <= type_range::kControlEnd)
    {
        return true;
    }
    if (raw >= type_range::kReservedBegin && raw <= type_range::kReservedEnd)
    {
        return true;
    }
    if (raw >= type_range::kFileSessionBegin && raw <= type_range::kFileSessionEnd)
    {
        return true;
    }
    if (raw >= type_range::kDirectBegin && raw <= type_range::kDirectEnd)
    {
        return true;
    }
    if (raw >= type_range::kChunkBegin)
    {
        return true;
    }
    return false;
}

std::string_view toString(FrameType t) noexcept
{
    switch (t)
    {
    case FrameType::kAuthChallenge:
        return "auth_challenge";
    case FrameType::kAuthResponse:
        return "auth_response";
    case FrameType::kAuthResult:
        return "auth_result";
    case FrameType::kAuthAbort:
        return "auth_abort";

    case FrameType::kAskId:
        return "ask_id";
    case FrameType::kAnswerId:
        return "answer_id";
    case FrameType::kAskHome:
        return "ask_home";
    case FrameType::kAnswerHome:
        return "answer_home";
    case FrameType::kAskFileList:
        return "ask_file_list";
    case FrameType::kAnswerFileList:
        return "answer_file_list";
    case FrameType::kAskClientList:
        return "ask_client_list";
    case FrameType::kAnswerClientList:
        return "answer_client_list";
    case FrameType::kAskFileMeta:
        return "ask_file_meta";
    case FrameType::kAnswerFileMeta:
        return "answer_file_meta";
    case FrameType::kAskDelete:
        return "ask_delete";
    case FrameType::kAnswerDelete:
        return "answer_delete";
    case FrameType::kAskSha256:
        return "ask_sha256";
    case FrameType::kAnswerSha256:
        return "answer_sha256";
    case FrameType::kAskRename:
        return "ask_rename";
    case FrameType::kAnswerRename:
        return "answer_rename";
    case FrameType::kAskCreateDir:
        return "ask_create_dir";
    case FrameType::kAnswerCreateDir:
        return "answer_create_dir";
    case FrameType::kAskArchive:
        return "ask_archive";
    case FrameType::kAnswerArchive:
        return "answer_archive";
    case FrameType::kAskUnarchive:
        return "ask_unarchive";
    case FrameType::kAnswerUnarchive:
        return "answer_unarchive";
    case FrameType::kAskHomeAndDriver:
        return "ask_home_and_driver";
    case FrameType::kAnswerHomeAndDriver:
        return "answer_home_and_driver";
    case FrameType::kAskHeart:
        return "ask_heart";
    case FrameType::kAnswerHeart:
        return "answer_heart";
    case FrameType::kNotifyClientList:
        return "notify_client_list";
    case FrameType::kAnswerNotifyClientList:
        return "answer_notify_client_list";
    case FrameType::kAskFileSamples:
        return "ask_file_samples";
    case FrameType::kAnswerFileSamples:
        return "answer_file_samples";
    case FrameType::kAskArchivePack:
        return "ask_archive_pack";
    case FrameType::kAnswerArchivePack:
        return "answer_archive_pack";
    case FrameType::kAskArchiveReady:
        return "ask_archive_ready";
    case FrameType::kAnswerArchiveReady:
        return "answer_archive_ready";
    case FrameType::kClaimId:
        return "claim_id";
    case FrameType::kAskFilePull:
        return "ask_file_pull";
    case FrameType::kAnswerFilePull:
        return "answer_file_pull";

    case FrameType::kFileRequestSend:
        return "file_request_send";
    case FrameType::kFileAnswerSend:
        return "file_answer_send";
    case FrameType::kFileRequestDown:
        return "file_request_down";
    case FrameType::kFileAnswerDown:
        return "file_answer_down";
    case FrameType::kFileRequestComplete:
        return "file_request_complete";
    case FrameType::kFileAnswerComplete:
        return "file_answer_complete";
    case FrameType::kFileRequestCancel:
        return "file_request_cancel";
    case FrameType::kFileAnswerCancel:
        return "file_answer_cancel";
    case FrameType::kFileRequestStart:
        return "file_request_start";
    case FrameType::kFileAnswerStart:
        return "file_answer_start";

    case FrameType::kDirectRequestId:
        return "direct_request_id";
    case FrameType::kDirectAnswerId:
        return "direct_answer_id";

    case FrameType::kChunkRequestAck:
        return "chunk_request_ack";
    case FrameType::kChunkAnswerAck:
        return "chunk_answer_ack";
    case FrameType::kChunkRequestData:
        return "chunk_request_data";
    case FrameType::kChunkAnswerData:
        return "chunk_answer_data";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Pack
// ---------------------------------------------------------------------------

Status packFrameChecked(const Frame &f, std::vector<char> &out)
{
    out.clear();

    if (f.from.size() > frame::kMaxRouteField)
    {
        return Status::Fail(ErrorCode::kProtocol, "pack: from field exceeds 255 bytes");
    }
    if (f.to.size() > frame::kMaxRouteField)
    {
        return Status::Fail(ErrorCode::kProtocol, "pack: to field exceeds 255 bytes");
    }
    if (f.fuuid.size() > frame::kMaxRouteField)
    {
        return Status::Fail(ErrorCode::kProtocol, "pack: fuuid field exceeds 255 bytes");
    }
    if (!isKnownType(static_cast<std::uint16_t>(f.type)))
    {
        return Status::Fail(ErrorCode::kProtocol, "pack: frame type out of defined ranges");
    }

    // body CRC 由长度自动决定，调用方不能自己置位——否则可能出现
    // "flag 说有 CRC 但实际没写"这种自相矛盾的帧。这里先把标志位算出来，
    // 调用方传进来的 kFlagBodyCrc 会被覆盖。
    const bool withBodyCrc = f.body.size() <= frame::kBodyCrcLimit;
    std::uint16_t flags = f.flags;
    if (withBodyCrc)
    {
        flags = static_cast<std::uint16_t>(flags | kFlagBodyCrc);
    }
    else
    {
        flags = static_cast<std::uint16_t>(flags & ~static_cast<std::uint16_t>(kFlagBodyCrc));
    }

    out.reserve(frame::kFixedHeaderSize + frame::kRoutePrefixSize + f.from.size() + f.to.size() + f.fuuid.size() +
                f.body.size() + 4);

    writeU32(out, frame::kMagic);
    writeU16(out, frame::kVersion);
    writeU16(out, flags);
    writeU16(out, static_cast<std::uint16_t>(f.type));
    writeU16(out, 0);  // reserved，必须为 0；将来若要启用需要走版本号变更
    writeU64(out, static_cast<std::uint64_t>(f.sessionId));
    writeU64(out, static_cast<std::uint64_t>(f.index));
    writeU32(out, 0);  // frameLen，先占位
    writeU32(out, 0);  // headerCrc，先占位

    writeRouteField(out, f.from);
    writeRouteField(out, f.to);
    writeRouteField(out, f.fuuid);

    out.insert(out.end(), f.body.begin(), f.body.end());
    if (withBodyCrc)
    {
        writeU32(out, crc32(f.body.data(), f.body.size()));
    }

    const auto frameLen = out.size();
    if (frameLen > frame::kMaxFrameSize)
    {
        out.clear();
        return Status::Fail(ErrorCode::kProtocol, "pack: frame exceeds 32MB limit");
    }

    // 回填 frameLen（offset 28）与 headerCrc（offset 32）。
    // headerCrc 覆盖前 32 字节，也就是"到 frameLen 为止但不含 headerCrc 自身"。
    const auto len32 = static_cast<std::uint32_t>(frameLen);
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[28 + i] = static_cast<char>((len32 >> (8 * i)) & 0xFF);
    }
    const std::uint32_t headerCrc = crc32(out.data(), 32);
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[32 + i] = static_cast<char>((headerCrc >> (8 * i)) & 0xFF);
    }

    return Status::Ok();
}

std::vector<char> packFrame(const Frame &f)
{
    std::vector<char> out;
    if (!packFrameChecked(f, out).ok())
    {
        return {};
    }
    return out;
}

// ---------------------------------------------------------------------------
// UnPack
// ---------------------------------------------------------------------------

UnPackResult unpackFrame(const char *data, std::size_t len, Frame &out, std::size_t &consumed)
{
    consumed = 0;

    if (data == nullptr)
    {
        return UnPackResult::kNeedMoreData;
    }

    const std::size_t avail = len;

    // --- 第一步：找 magic ---
    //
    // 逐字节前进而不是用 memchr：magic 是 4 字节且要按小端解释，memchr 只能
    // 找单个字节，找到 'R' 之后仍要验证后三个字节，省不了多少。
    std::size_t offset = 0;
    while (offset + 4 <= avail && readU32(data + offset) != frame::kMagic)
    {
        ++offset;
    }

    if (offset + 4 > avail)
    {
        // 没找到完整的 magic。末尾最多 3 字节可能是半个 magic，必须留着。
        consumed = avail > 3 ? avail - 3 : 0;
        return UnPackResult::kNeedMoreData;
    }

    if (offset > 0)
    {
        // 前导垃圾丢掉，让调用方从 magic 处重新调用。
        consumed = offset;
        return UnPackResult::kNeedMoreData;
    }

    // --- 第二步：固定头是否收全 ---
    if (avail < frame::kFixedHeaderSize)
    {
        consumed = 0;
        return UnPackResult::kNeedMoreData;
    }

    const char *h = data;
    const std::uint32_t frameLen = readU32(h + 28);
    const std::uint32_t headerCrc = readU32(h + 32);

    // --- 第三步：头部 sanity + CRC ---
    //
    // magic 对上了不代表这是帧头：body 里刚好出现 'RFM1' 的四个字节非常常见
    // （尤其是压缩数据或随机二进制）。所以必须再验长度范围与头 CRC。
    // 任何一步不过都只跳过 1 字节——这样最多损失一次 CRC 计算，
    // 而不会把一个虚假的超长 frameLen 当成真的去等 32MB。
    if (frameLen < frame::kMinFrameSize || frameLen > frame::kMaxFrameSize || crc32(h, 32) != headerCrc)
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }

    // 头部 CRC 正确却版本不符：这是真的协议不匹配，继续等只会永远等下去。
    if (readU16(h + 4) != frame::kVersion)
    {
        consumed = 0;
        return UnPackResult::kProtocolError;
    }

    // --- 第四步：整帧是否收全 ---
    if (avail < frameLen)
    {
        consumed = 0;
        return UnPackResult::kNeedMoreData;
    }

    const std::uint16_t flags = readU16(h + 6);
    const bool withBodyCrc = hasFlag(flags, kFlagBodyCrc);
    const std::size_t payloadLen = frameLen - frame::kFixedHeaderSize;

    // 载荷至少要装下三个长度前缀 +（可选）4 字节 body CRC
    const std::size_t minPayload = frame::kRoutePrefixSize + (withBodyCrc ? 4u : 0u);
    if (payloadLen < minPayload)
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }

    // --- 第五步：拆路由字段 ---
    std::size_t p = frame::kFixedHeaderSize;
    std::string from;
    std::string to;
    std::string fuuid;
    std::size_t used = 0;

    if (!readRouteField(h + p, frameLen - p, from, used))
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }
    p += used;

    if (!readRouteField(h + p, frameLen - p, to, used))
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }
    p += used;

    if (!readRouteField(h + p, frameLen - p, fuuid, used))
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }
    p += used;

    // --- 第六步：body 与可选 CRC ---
    const std::size_t bodyEnd = static_cast<std::size_t>(frameLen) - (withBodyCrc ? 4u : 0u);
    if (p > bodyEnd)
    {
        consumed = 1;
        return UnPackResult::kNeedMoreData;
    }

    const std::size_t bodyLen = bodyEnd - p;
    if (withBodyCrc)
    {
        const std::uint32_t expect = readU32(h + bodyEnd);
        if (crc32(h + p, bodyLen) != expect)
        {
            // body 损坏。TCP 保证不丢字节，所以这里只可能是对端实现有 bug
            // 或者内存被写坏。丢掉整帧并报协议错误——继续用这条连接传下去
            // 已经不可能保证数据正确了。
            consumed = frameLen;
            return UnPackResult::kProtocolError;
        }
    }

    // --- 第七步：填帧 ---
    const std::uint16_t rawType = readU16(h + 8);
    if (!isKnownType(rawType))
    {
        // 类型号落在未定义区间。头部 CRC 是对的，说明对端确实按协议打包了，
        // 只是它认识的类型号我们还不认识——这通常意味着对端版本更新。
        // 跳过整帧继续（帧长可信），而不是整条连接作废。
        consumed = frameLen;
        return UnPackResult::kProtocolError;
    }

    out.type = static_cast<FrameType>(rawType);
    out.flags = flags;
    out.sessionId = static_cast<std::int64_t>(readU64(h + 12));
    out.index = static_cast<std::int64_t>(readU64(h + 20));
    out.from = std::move(from);
    out.to = std::move(to);
    out.fuuid = std::move(fuuid);

    if (bodyLen > 0)
    {
        out.body.assign(h + p, h + p + bodyLen);
    }
    else
    {
        out.body.clear();
    }

    consumed = frameLen;
    return UnPackResult::kOk;
}

}  // namespace relayer::protocol
