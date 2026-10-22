#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "relayer/common/Payload.h"
#include "relayer/common/Status.h"
#include "relayer/core/transfer/MerkleTree.h"

// 传输层的载荷编解码（PROTOCOL.md §9.4 - §9.8）。
//
// 与协议层的分工：Frame.h 只管"帧怎么切成字节"，这里管"body 里的字段怎么
// 排"。本文件不碰 socket、不碰 StateMachine、不碰文件——纯粹的结构 <-> 字节。
//
// 为什么单独一个文件而不是塞进 TransferEngine：这些格式是**线上契约**，
// 要能被字节级测试直接覆盖，也该被文档一致性检查盯住。混进那个带定时器、
// 带文件句柄的引擎里之后，测一个字段顺序就是启动一次完整传输。

namespace relayer::core::transfer
{

// ---------------------------------------------------------------------------
// 传输会话描述（§9.4）
// ---------------------------------------------------------------------------

struct TransferDesc
{
    std::string displayName;  // 仅用于界面与日志
    std::uint64_t totalSize = 0;
    std::uint32_t chunkSize = kDefaultChunkSize;
    std::string sha256Hex;       // 整文件 SHA-256 小写十六进制（通常 64 字符）
    std::string fingerprintHex;  // IFileStore::quickFingerprint 的输出
    std::string root;            // 落点相对路径，相对接收方 root
};

// 校验描述是否自洽。
//
// 单独一个函数而不是在编解码里顺手判：发送方**构造**描述时也要判（避免
// 发一个自己都不合法的请求），接收方**收到**时也要判。两处共用一份规则，
// 否则总有一侧会先放松。
[[nodiscard]] Status validateDesc(const TransferDesc &d);

[[nodiscard]] Status encodeDesc(const TransferDesc &d, std::vector<char> &out);
[[nodiscard]] bool decodeDesc(const std::vector<char> &body, TransferDesc &out, std::string &err);

// ---------------------------------------------------------------------------
// 传输应答（§9.5）
// ---------------------------------------------------------------------------

// 应答状态码。数值是线格式的一部分。
enum class AnswerStatus : std::uint8_t
{
    kOk = 0,
    kTargetExists = 1,  // 目标已存在且内容不同
    kNoSpace = 2,
    kNotWritable = 3,
    kBadDesc = 4,  // 描述非法（块大小越界、总长与块数不匹配等）
    kDuplicateTask = 5,
    kTooLarge = 6,
};

[[nodiscard]] std::string_view toString(AnswerStatus s) noexcept;

struct TransferAnswer
{
    AnswerStatus status = AnswerStatus::kOk;
    std::uint64_t resumeOffset = 0;   // 必须是 chunkSize 的整数倍
    std::uint32_t chunkSizeHint = 0;  // 0 = 没意见
    std::string reason;               // status != 0 时的可读说明
};

// 校验应答自洽：成功时 resumeOffset 必须按块对齐（需要 chunkSize 才能判，
// 所以由调用方传进来；传 0 表示跳过这项检查）。
[[nodiscard]] Status validateAnswer(const TransferAnswer &a, std::uint32_t chunkSize);

[[nodiscard]] Status encodeAnswer(const TransferAnswer &a, std::vector<char> &out);
[[nodiscard]] bool decodeAnswer(const std::vector<char> &body, TransferAnswer &out, std::string &err);

// ---------------------------------------------------------------------------
// 分块数据（§9.6）
// ---------------------------------------------------------------------------

struct ChunkData
{
    std::uint32_t chunkIndex = 0;
    const char *payload = nullptr;  // 指向外部缓冲，不拥有
    std::uint32_t payloadLen = 0;
};

// 编码时会把 payload 复制进 out。
[[nodiscard]] Status encodeChunk(std::uint32_t chunkIndex, const void *data, std::size_t len, std::vector<char> &out);

// 解码后 payload 指向 body 内部，**不复制**。调用方不得让 body 先于使用失效。
[[nodiscard]] bool decodeChunk(const std::vector<char> &body, ChunkData &out, std::string &err);

// ---------------------------------------------------------------------------
// 分块确认（§9.7）
// ---------------------------------------------------------------------------

// 累积 ACK + SACK 位图。
//
// ackThrough 是**最长连续前缀长度**，不是"最后收到的块号 + 1"。这个区别在
// 乱序时是致命的：收到 0,1,5 时 ackThrough 必须是 2，若写成 6，发送方会
// 以为 2、3、4 都到了——数据丢失且不报错，只有最终哈希能发现。
struct ChunkAck
{
    std::uint32_t ackThrough = 0;
    std::vector<std::uint32_t> bitmap;  // 位 i = 块 ackThrough+i 已收到
};

// 位操作。位图中第 i 位在 bitmap[i/32] 的 1<<(i%32) 位（低位在前，与 §7 一致）。
[[nodiscard]] bool bitmapTest(const std::vector<std::uint32_t> &bm, std::uint32_t bit) noexcept;
void bitmapSet(std::vector<std::uint32_t> &bm, std::uint32_t bit);

// 从"已收到的块集合"构造 ACK：连续前缀 + 其后的空洞位图。
//
// totalChunks 用来裁掉尾部全零的字：位图只该描述"还有多远没收到"，
// 跟着一个巨大的全零尾巴纯属浪费带宽。
[[nodiscard]] ChunkAck buildAck(const std::vector<bool> &received, std::uint32_t totalChunks);

// 把 ACK 展开成"哪些块还需要重传"。totalChunks 之外的一律忽略。
[[nodiscard]] std::vector<std::uint32_t> missingChunks(const ChunkAck &ack, std::uint32_t totalChunks);

[[nodiscard]] Status encodeAck(const ChunkAck &a, std::vector<char> &out);
[[nodiscard]] bool decodeAck(const std::vector<char> &body, ChunkAck &out, std::string &err);

// ---------------------------------------------------------------------------
// 完成握手（§9.8）
// ---------------------------------------------------------------------------

enum class CompleteStatus : std::uint8_t
{
    kOk = 0,
    kRootMismatch = 1,   // 保留残片，可续传
    kWriteFailed = 2,    // 保留残片
    kShapeMismatch = 3,  // 块数/总长与描述不符，必须全量重传
};

[[nodiscard]] std::string_view toString(CompleteStatus s) noexcept;

// kFileRequestComplete
[[nodiscard]] Status encodeCompleteRequest(std::string_view merkleRootHex, std::uint64_t finalSize,
                                           std::vector<char> &out);
[[nodiscard]] bool decodeCompleteRequest(const std::vector<char> &body, std::string &rootHex, std::uint64_t &finalSize,
                                         std::string &err);

// kFileAnswerComplete
[[nodiscard]] Status encodeCompleteAnswer(CompleteStatus status, std::string_view merkleRootHex,
                                          std::vector<char> &out);
[[nodiscard]] bool decodeCompleteAnswer(const std::vector<char> &body, CompleteStatus &status, std::string &rootHex,
                                        std::string &err);

// ---------------------------------------------------------------------------
// 取消（kFileRequestCancel / kFileAnswerCancel）
// ---------------------------------------------------------------------------

// 取消原因。数值是线格式的一部分。
enum class CancelReason : std::uint8_t
{
    kUserCancelled = 0,
    kPeerGone = 1,
    kTimeout = 2,
    kLocalError = 3,
    kHashMismatch = 4,
};

[[nodiscard]] std::string_view toString(CancelReason r) noexcept;

// 取消帧的 body：`u8 reason || u16 msgLen || msg`。
//
// 带一句可读说明的代价很小（几十字节），收益是排查时不用去猜"对端为什么
// 走了"——日志里那句 "disk full" 比一个枚举号有用得多。
[[nodiscard]] Status encodeCancel(CancelReason reason, std::string_view message, std::vector<char> &out);
[[nodiscard]] bool decodeCancel(const std::vector<char> &body, CancelReason &reason, std::string &message,
                                std::string &err);

}  // namespace relayer::core::transfer
