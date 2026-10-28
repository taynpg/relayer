#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// RFM1 帧协议（relayer frame message，第 1 版布局 / 第 2 版语义）。
//
// 完整的字节级说明在 docs/PROTOCOL.md。这里只放编译器需要的东西和
// 那些"改错了会静默出错"的约束说明。
//
// 本文件**不含任何 I/O**：Pack 只负责把结构变成字节，UnPack 只负责把字节
// 变成结构。谁读 socket、谁写 socket、什么时候断线，全都不在这里决定。
// 这条边界让协议层可以被纯粹的字节级测试覆盖，不需要起端口。

namespace relayer::protocol {

// ---------------------------------------------------------------------------
// 帧类型
// ---------------------------------------------------------------------------
//
// 类型号是**线格式的一部分**，一旦发布就不能改。分段是为了让接收方能
// 便宜地判断"这帧归哪个子系统处理"，不用维护一张大表：
//
//   0   .. 99    握手与认证      （连接建立、未认证时的唯一合法帧）
//   100 .. 199   控制与会话信令
//   200 .. 499   保留            （不得占用，留给未来的控制类扩展）
//   500 .. 549   文件传输会话管理
//   550 .. 599   服务端直连文件通道
//   600 .. 65535 分块数据与确认
//
// 新增类型时按段追加到**末尾**，不要复用已废弃的号：老客户端会把废弃号
// 当成自己认识的那个类型，行为错乱比直接断开更难排查。
enum class FrameType : std::uint16_t {
    // --- 握手 / 认证（0..99） ---
    //
    // 这三个是 v2 新增的。连接建立后，服务端发的第一帧必须是 kAuthChallenge；
    // 客户端在收到 kAuthResult(ok) 之前发的任何业务帧都应被丢弃。
    kAuthChallenge = 0,   // S->C: salt || nonceS
    kAuthResponse = 1,    // C->S: nonceC || proof
    kAuthResult = 2,      // S->C: status || serverProof
    kAuthAbort = 3,       // 双向: status（任一方放弃，随后关闭连接）

    // --- 控制与会话信令（100..199） ---
    kAskId = 100,
    kAnswerId = 101,
    kAskHome = 102,
    kAnswerHome = 103,
    kAskFileList = 104,
    kAnswerFileList = 105,
    kAskClientList = 106,
    kAnswerClientList = 107,
    kAskFileMeta = 108,
    kAnswerFileMeta = 109,
    kAskDelete = 110,
    kAnswerDelete = 111,
    kAskSha256 = 112,
    kAnswerSha256 = 113,
    kAskRename = 114,
    kAnswerRename = 115,
    kAskCreateDir = 116,
    kAnswerCreateDir = 117,
    kAskArchive = 118,
    kAnswerArchive = 119,
    kAskUnarchive = 120,
    kAnswerUnarchive = 121,
    kAskHomeAndDriver = 122,
    kAnswerHomeAndDriver = 123,
    kAskHeart = 124,
    kAnswerHeart = 125,
    kNotifyClientList = 126,
    kAnswerNotifyClientList = 127,
    kAskFileSamples = 128,
    kAnswerFileSamples = 129,
    kAskArchivePack = 130,
    kAnswerArchivePack = 131,
    kAskArchiveReady = 132,
    kAnswerArchiveReady = 133,

    // 客户端声明"我要叫这个 id"（PROTOCOL.md §11.7）。
    // 应答复用 kAnswerId 的 status + id 结构，不另开应答帧。
    kClaimId = 134,

    // 远端取回（PROTOCOL.md §11.9）：请对端把共享目录里的某个文件发给我。
    // 应答只带 status；真正的传输复用 500-509 那一套（对端以普通"推"的
    // 方式发起），所以这里不需要传输参数。
    kAskFilePull = 135,
    kAnswerFilePull = 136,

    // 目录取回（PROTOCOL.md §11.10）：请对端把某个**目录**打包成 tar.xz
    // 再发给我。应答复用 kAnswerFilePull（结论是同一类：接受/不存在/拒绝），
    // 包本身走普通推流程。
    kAskArchivePull = 137,

    // --- 文件传输会话管理（500..549） ---
    kFileRequestSend = 500,
    kFileAnswerSend = 501,
    // 502/503 是 v1 FileSession 模型的遗留，**v2 不使用**（PROTOCOL.md
    // §11.9.2 说明了为什么）：拉模式下接收方不知道 totalSize/chunkSize/
    // Merkle 根，最终仍要走一次 kFileRequestSend，加这两帧等于多一次往返
    // 换不到任何信息。编号保留，不要拿来复用。
    kFileRequestDown = 502,
    kFileAnswerDown = 503,
    kFileRequestComplete = 504,
    kFileAnswerComplete = 505,
    kFileRequestCancel = 506,
    kFileAnswerCancel = 507,
    kFileRequestStart = 508,
    kFileAnswerStart = 509,

    // --- 服务端直连文件通道（550..599） ---
    kDirectRequestId = 550,
    kDirectAnswerId = 551,

    // --- 分块数据与确认（600..） ---
    kChunkRequestAck = 600,
    kChunkAnswerAck = 601,
    kChunkRequestData = 602,
    kChunkAnswerData = 603,
};

// 分段边界。用 constexpr 而不是散落的魔法数字，是因为接收方要按段分派。
namespace type_range {
constexpr std::uint16_t kAuthBegin = 0;
constexpr std::uint16_t kAuthEnd = 99;
constexpr std::uint16_t kControlBegin = 100;
constexpr std::uint16_t kControlEnd = 199;
constexpr std::uint16_t kReservedBegin = 200;
constexpr std::uint16_t kReservedEnd = 499;
constexpr std::uint16_t kFileSessionBegin = 500;
constexpr std::uint16_t kFileSessionEnd = 549;
constexpr std::uint16_t kDirectBegin = 550;
constexpr std::uint16_t kDirectEnd = 599;
constexpr std::uint16_t kChunkBegin = 600;
}   // namespace type_range

[[nodiscard]] constexpr bool isAuthType(FrameType t) noexcept
{
    const auto v = static_cast<std::uint16_t>(t);
    return v <= type_range::kAuthEnd;
}

[[nodiscard]] constexpr bool isControlType(FrameType t) noexcept
{
    const auto v = static_cast<std::uint16_t>(t);
    return v >= type_range::kControlBegin && v <= type_range::kControlEnd;
}

[[nodiscard]] constexpr bool isReservedType(FrameType t) noexcept
{
    const auto v = static_cast<std::uint16_t>(t);
    return v >= type_range::kReservedBegin && v <= type_range::kReservedEnd;
}

[[nodiscard]] constexpr bool isFileSessionType(FrameType t) noexcept
{
    const auto v = static_cast<std::uint16_t>(t);
    return v >= type_range::kFileSessionBegin && v <= type_range::kFileSessionEnd;
}

[[nodiscard]] constexpr bool isDirectType(FrameType t) noexcept
{
    const auto v = static_cast<std::uint16_t>(t);
    return v >= type_range::kDirectBegin && v <= type_range::kDirectEnd;
}

[[nodiscard]] constexpr bool isChunkType(FrameType t) noexcept
{
    return static_cast<std::uint16_t>(t) >= type_range::kChunkBegin;
}

// 这一帧是否承载**大块负载**（会长时间占住 TCP 写队列）。
//
// 只有两种数据帧算：kChunkRequestData / kChunkAnswerData。同属块段的两个
// ACK 帧刻意**不算**——它们小、且是推进发送窗口的唯一手段，压在数据帧后面
// 会让整条管道停摆。分类错了不会报错，只会让所有帧一起排队，也就是退回
// 到没有优先级的状态，所以这里用精确的等值判断而不是范围判断。
//
// 传输层用它决定写队列的优先级：这个函数是"帧有多重"的唯一权威定义，
// 不要在 Connection 里另写一份。
[[nodiscard]] constexpr bool isBulkFrame(FrameType t) noexcept
{
    return t == FrameType::kChunkRequestData || t == FrameType::kChunkAnswerData;
}

// 类型号是否落在已定义区间内（含保留段）。用于 UnPack 时拒绝野值。
[[nodiscard]] bool isKnownType(std::uint16_t raw) noexcept;

// 日志用的稳定名称。未定义的类型返回 "unknown"。
[[nodiscard]] std::string_view toString(FrameType t) noexcept;

// ---------------------------------------------------------------------------
// 帧标志
// ---------------------------------------------------------------------------

// v1 把方向、加密、校验等信息挤在同一个 int16 的不同位上，导致"按 mark 是否
// 非零判方向"这类推断在加了加密位之后立即出错。v2 保留位布局但**禁止用
// 数值范围推断语义**：只用下面这些具名位。
enum FrameFlags : std::uint16_t {
    kFlagNone = 0,

    // 该帧由「接收文件数据的一侧」发出。
    // 发送侧与接收侧会各自维护一份会话槽，这个位决定去哪一侧找。
    kFlagFromReceiver = 1u << 0,

    // body 已加密，格式 nonce(12) || ciphertext || tag(16)。
    // 见 docs/PROTOCOL.md 的"加密载荷布局"一节。
    kFlagEncrypted = 1u << 1,

    // body 尾部附带 u32 CRC32。由 Pack 根据 body 长度自动决定：
    // body <= kBodyCrcLimit 时置位。大块数据不带——每 4MB 加 4 字节 CRC 的
    // 收益远不如分块 Merkle 校验，而且会让"帧长 = 头 + 载荷"的计算变复杂。
    kFlagBodyCrc = 1u << 2,
};

[[nodiscard]] constexpr bool hasFlag(std::uint16_t flags, FrameFlags f) noexcept
{
    return (flags & static_cast<std::uint16_t>(f)) != 0;
}

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

namespace frame {
// 'R','F','M','1' 按小端读成 u32
constexpr std::uint32_t kMagic = 0x314D4652;
constexpr std::uint16_t kVersion = 2;

constexpr std::size_t kFixedHeaderSize = 36;

// 单帧上限。32MB 的依据：分块传输的块大小是 4MB，留 8 倍余量给
// "一个帧里塞多个块 + 加密膨胀 + 包头"的极端情况；同时它小于常见的
// 16 位有符号 int 上限，避免把长度转 int 时溢出。
constexpr std::uint32_t kMaxFrameSize = 32u * 1024 * 1024;

// from / to / fuuid 单字段上限（长度前缀是 u8）
constexpr std::size_t kMaxRouteField = 255;

// body 小于等于这个值时附带 CRC32
constexpr std::uint32_t kBodyCrcLimit = 64u * 1024;

// 三个路由字段的长度前缀合计
constexpr std::size_t kRoutePrefixSize = 3;

// 帧的最小合法长度：固定头 + 三个空路由字段（各 1 字节长度前缀）
constexpr std::uint32_t kMinFrameSize = kFixedHeaderSize + kRoutePrefixSize;
}   // namespace frame

// ---------------------------------------------------------------------------
// 帧
// ---------------------------------------------------------------------------

struct Frame {
    FrameType type{};

    // 原始标志位。**不要**直接和数值比较，用 hasFlag()。
    std::uint16_t flags{kFlagNone};

    // 会话标识。同一个连接上可以并行多个会话，靠它区分。
    std::int64_t sessionId{};

    // 帧序号。语义由 type 决定：
    //   - 分块数据帧：块号
    //   - 请求/应答：请求号（应答与请求同号）
    //   - 其余：0
    std::int64_t index{};

    std::string from;    // 源端 id（服务端会强制覆写，见 PROTOCOL.md）
    std::string to;      // 目标端 id
    std::string fuuid;   // 文件/任务 uuid，控制类帧用；数据帧留空以省字节

    std::vector<char> body;

    [[nodiscard]] bool isFromReceiver() const noexcept
    {
        return hasFlag(flags, kFlagFromReceiver);
    }
    [[nodiscard]] bool isEncrypted() const noexcept
    {
        return hasFlag(flags, kFlagEncrypted);
    }
    [[nodiscard]] bool hasBodyCrc() const noexcept
    {
        return hasFlag(flags, kFlagBodyCrc);
    }
};

// ---------------------------------------------------------------------------
// 编解码
// ---------------------------------------------------------------------------

// Pack 的结果。
//
// 失败原因只有两种，都是调用方的编程错误（字段超长 / 帧太大），
// 不是运行时的正常分支，所以主接口用空 vector 表示失败就够了。
// 需要可诊断的日志时用下面的 packFrameChecked 拿到具体原因。
[[nodiscard]] std::vector<char> packFrame(const Frame& f);

// 同上，但把失败原因写进 out 的 Status / 错误信息里。
// 这个重载同时是 packFrame 的实现，两者行为保证一致。
[[nodiscard]] Status packFrameChecked(const Frame& f, std::vector<char>& out);

// CRC32（IEEE 802.3，多项式 0xEDB88320，初值与末值都取反）。
// 与 zlib 的 crc32() 结果一致。帧头与 body 校验都用它。
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t len) noexcept;

// ---------------------------------------------------------------------------
// 解包
// ---------------------------------------------------------------------------

enum class UnPackResult {
    kNeedMoreData,    // 输入还没收全，保留数据等下一次
    kOk,              // 解出一帧，out 有效，consumed 为消耗字节数
    kProtocolError,   // 流已不可恢复（版本不符、body CRC 错），调用方应断开
};

// 从 [data, data + len) 的**起始位置**尝试解出一帧。
//
// 语义约定（调用方必须按这个来用）：
//
//   1. 返回 kOk：out 填入帧，consumed 置为这一帧占用的字节数。
//      调用方把 consumed 之后的数据留给下一次调用。
//
//   2. 返回 kNeedMoreData：consumed 置为"可以安全丢弃的字节数"。
//      这通常是扫描跳过的前导垃圾；如果输入从头就是半帧，consumed 为 0。
//      调用方应当消费掉 consumed 字节、保留剩下的，然后等更多数据。
//      **不要**假设 consumed 恒为 0——垃圾数据要靠这个值清出去。
//
//   3. 返回 kProtocolError：流已不可恢复。consumed 可能非零（body CRC 失败时
//      会跳过整帧），但调用方应当按照"连接作废"处理，不必依赖这个值。
//
// 重同步：帧头里的 magic 可能在任意字节处出现（body 里恰好有 'RFM1' 很常见）。
// 因此单看 magic 会误判，算法是"找 magic -> 校验 frameLen 范围 -> 校验 headerCrc"，
// 任何一步不过就跳过 1 字节继续找。这样误判最多浪费一次 CRC 计算，不会把
// 解析器卡在一个虚假的超长帧长上。
UnPackResult unpackFrame(const char* data, std::size_t len, Frame& out, std::size_t& consumed);

}   // namespace relayer::protocol
