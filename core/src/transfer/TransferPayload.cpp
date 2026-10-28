#include "relayer/core/transfer/TransferPayload.h"

#include <algorithm>
#include <string_view>

namespace relayer::core::transfer {

using relayer::common::PayloadReader;
using relayer::common::PayloadWriter;

namespace {

// 十六进制字符串统一按小写比较与传输（§9.4 的 sha256 字段格式）。
// 解码侧不强求大小写，但编码侧一定写小写——否则两端日志里同一个哈希
// 会有两种写法，grep 不到。
bool isHexOfLen(std::string_view s, std::size_t len) noexcept
{
    if (s.size() != len) {
        return false;
    }
    for (char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

std::string lowerHex(std::string_view s)
{
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// 会话描述
// ---------------------------------------------------------------------------

Status validateDesc(const TransferDesc& d)
{
    if (d.root.empty()) {
        return Status::Fail(ErrorCode::kProtocol, "desc: root path is empty");
    }

    // 块大小：范围 + 对齐（§9.4.1）。对齐是硬要求，因为"块号 → 文件偏移"
    // 的映射靠乘法，非对齐的块大小会让这个映射不再是简单乘法。
    if (d.chunkSize < kMinChunkSize || d.chunkSize > kMaxChunkSize) {
        return Status::Fail(ErrorCode::kProtocol, "desc: chunkSize out of range");
    }
    if (d.chunkSize % kChunkSizeAlignment != 0) {
        return Status::Fail(ErrorCode::kProtocol, "desc: chunkSize not aligned to 1024");
    }

    // 块数必须能用 u32 表示，否则线上的 chunkIndex 会溢出。
    const auto n = chunkCountFor(d.totalSize, d.chunkSize);
    if (n == 0xFFFFFFFFu && d.totalSize != 0) {
        // chunkCountFor 封顶到 UINT32_MAX 表示"超出可表示范围"。
        const std::uint64_t exact = d.totalSize / d.chunkSize + (d.totalSize % d.chunkSize != 0 ? 1 : 0);
        if (exact > 0xFFFFFFFFull) {
            return Status::Fail(ErrorCode::kProtocol, "desc: too many chunks for u32 index");
        }
    }

    // sha256 允许为空（上游可能没算），但给了就必须是合法十六进制。
    // 不接受空是更好的做法，可这里放宽：有的场景（比如 P2P 直连的小文件）
    // 会用 Merkle 根代替，语义上等价。
    if (!d.sha256Hex.empty() && !isHexOfLen(d.sha256Hex, 64)) {
        return Status::Fail(ErrorCode::kProtocol, "desc: sha256 must be 64 hex chars");
    }
    if (!d.fingerprintHex.empty() && !isHexOfLen(d.fingerprintHex, 64)) {
        return Status::Fail(ErrorCode::kProtocol, "desc: fingerprint must be 64 hex chars");
    }

    // 路由字段上限 255（§9.1）。超过就没法编码，早点说比在 encode 里失败好。
    if (d.root.size() > 0xFFFF) {
        return Status::Fail(ErrorCode::kProtocol, "desc: root path too long");
    }
    if (d.displayName.size() > 0xFFFF) {
        return Status::Fail(ErrorCode::kProtocol, "desc: display name too long");
    }

    return Status::Ok();
}

Status encodeDesc(const TransferDesc& d, std::vector<char>& out)
{
    const auto st = validateDesc(d);
    if (!st.ok()) {
        return st;
    }

    out.clear();
    PayloadWriter w(out);
    if (!w.str(d.displayName)) {
        return Status::Fail(ErrorCode::kProtocol, "desc: display name too long");
    }
    w.u64(d.totalSize);
    w.u32(d.chunkSize);
    if (!w.shortStr(lowerHex(d.sha256Hex))) {
        return Status::Fail(ErrorCode::kProtocol, "desc: sha256 too long");
    }
    if (!w.shortStr(lowerHex(d.fingerprintHex))) {
        return Status::Fail(ErrorCode::kProtocol, "desc: fingerprint too long");
    }
    if (!w.str(d.root)) {
        return Status::Fail(ErrorCode::kProtocol, "desc: root too long");
    }
    return Status::Ok();
}

bool decodeDesc(const std::vector<char>& body, TransferDesc& out, std::string& err)
{
    PayloadReader r(body);

    TransferDesc d;
    d.displayName = r.str();
    d.totalSize = r.u64();
    d.chunkSize = r.u32();
    d.sha256Hex = r.shortStr();
    d.fingerprintHex = r.shortStr();
    d.root = r.str();

    if (!r.ok()) {
        err = "desc: truncated";
        return false;
    }
    // 解完必须正好到末尾。多出来的字节意味着两端对字段表理解不同，
    // 而"多几个字节"最容易在后续版本里变成静默的字段错位。
    if (!r.atEnd()) {
        err = "desc: trailing bytes";
        return false;
    }

    const auto st = validateDesc(d);
    if (!st.ok()) {
        err = st.message;
        return false;
    }

    d.sha256Hex = lowerHex(d.sha256Hex);
    d.fingerprintHex = lowerHex(d.fingerprintHex);
    out = std::move(d);
    return true;
}

// ---------------------------------------------------------------------------
// 应答
// ---------------------------------------------------------------------------

std::string_view toString(AnswerStatus s) noexcept
{
    switch (s) {
    case AnswerStatus::kOk:
        return "ok";
    case AnswerStatus::kTargetExists:
        return "target_exists";
    case AnswerStatus::kNoSpace:
        return "no_space";
    case AnswerStatus::kNotWritable:
        return "not_writable";
    case AnswerStatus::kBadDesc:
        return "bad_desc";
    case AnswerStatus::kDuplicateTask:
        return "duplicate_task";
    case AnswerStatus::kTooLarge:
        return "too_large";
    }
    return "unknown";
}

Status validateAnswer(const TransferAnswer& a, std::uint32_t chunkSize)
{
    if (a.status != AnswerStatus::kOk) {
        // 失败应答里的 resumeOffset / hint 没有意义，忽略即可，不算错。
        return Status::Ok();
    }

    if (chunkSize != 0 && a.resumeOffset % chunkSize != 0) {
        // 非对齐的续传偏移会让"块号 → 偏移"的乘法映射失效（§9.5）。
        // 接收方发现对齐后的残片不足一块时应当直接给 0，而不是给个小数。
        return Status::Fail(ErrorCode::kProtocol, "answer: resumeOffset not chunk aligned");
    }
    return Status::Ok();
}

Status encodeAnswer(const TransferAnswer& a, std::vector<char>& out)
{
    out.clear();
    PayloadWriter w(out);
    w.u8(static_cast<std::uint8_t>(a.status));
    w.u64(a.resumeOffset);
    w.u32(a.chunkSizeHint);
    if (!w.shortStr(a.reason)) {
        return Status::Fail(ErrorCode::kProtocol, "answer: reason too long");
    }
    return Status::Ok();
}

bool decodeAnswer(const std::vector<char>& body, TransferAnswer& out, std::string& err)
{
    PayloadReader r(body);

    TransferAnswer a;
    const auto raw = r.u8();
    a.resumeOffset = r.u64();
    a.chunkSizeHint = r.u32();
    a.reason = r.shortStr();

    if (!r.ok()) {
        err = "answer: truncated";
        return false;
    }
    if (!r.atEnd()) {
        err = "answer: trailing bytes";
        return false;
    }
    if (raw > static_cast<std::uint8_t>(AnswerStatus::kTooLarge)) {
        // 未知状态码必须拒绝，不能当成 kOk——那样"拒绝"会被读成"接受"。
        err = "answer: unknown status " + std::to_string(raw);
        return false;
    }
    a.status = static_cast<AnswerStatus>(raw);

    out = std::move(a);
    return true;
}

// ---------------------------------------------------------------------------
// 分块数据
// ---------------------------------------------------------------------------

Status encodeChunk(std::uint32_t chunkIndex, const void* data, std::size_t len, std::vector<char>& out)
{
    if (len > kMaxChunkSize) {
        return Status::Fail(ErrorCode::kProtocol, "chunk: payload exceeds max chunk size");
    }
    if (len > 0 && data == nullptr) {
        return Status::Fail(ErrorCode::kProtocol, "chunk: null payload with non-zero length");
    }

    out.clear();
    out.reserve(8 + len);
    PayloadWriter w(out);
    w.u32(chunkIndex);
    w.u32(static_cast<std::uint32_t>(len));
    w.raw(data, len);
    return Status::Ok();
}

bool decodeChunk(const std::vector<char>& body, ChunkData& out, std::string& err)
{
    PayloadReader r(body);

    ChunkData c;
    c.chunkIndex = r.u32();
    c.payloadLen = r.u32();
    if (!r.ok()) {
        err = "chunk: truncated header";
        return false;
    }

    // payloadLen 必须先和剩余字节比对再取指针。若先取指针、后发现长度不符，
    // 就有一段"指针越界但没人用"的窗口，将来任何提前使用都是 UB。
    if (r.remaining() != c.payloadLen) {
        err = "chunk: payloadLen does not match remaining bytes";
        return false;
    }
    if (c.payloadLen > kMaxChunkSize) {
        err = "chunk: payloadLen exceeds max chunk size";
        return false;
    }
    c.payload = r.raw(c.payloadLen);
    if (c.payload == nullptr && c.payloadLen != 0) {
        err = "chunk: truncated payload";
        return false;
    }

    out = c;
    return true;
}

// ---------------------------------------------------------------------------
// 分块确认
// ---------------------------------------------------------------------------

bool bitmapTest(const std::vector<std::uint32_t>& bm, std::uint32_t bit) noexcept
{
    const auto word = bit / 32;
    if (word >= bm.size()) {
        return false;
    }
    return (bm[word] & (1u << (bit % 32))) != 0;
}

void bitmapSet(std::vector<std::uint32_t>& bm, std::uint32_t bit)
{
    const auto word = bit / 32;
    if (word >= bm.size()) {
        bm.resize(word + 1, 0);
    }
    bm[word] |= (1u << (bit % 32));
}

ChunkAck buildAck(const std::vector<bool>& received, std::uint32_t totalChunks)
{
    ChunkAck a;

    // 连续前缀：从头往后数到第一个没收到为止。
    // 注意收齐时 ackThrough 等于 totalChunks，此时位图为空。
    std::uint32_t i = 0;
    while (i < totalChunks && i < received.size() && received[i]) {
        ++i;
    }
    a.ackThrough = i;

    // 前缀之后的空洞才需要位图。
    std::uint32_t last = 0;
    bool any = false;
    for (std::uint32_t k = i; k < totalChunks && k < received.size(); ++k) {
        if (received[k]) {
            bitmapSet(a.bitmap, k - i);
            last = k - i;
            any = true;
        }
    }

    // 裁掉尾部全零的字。不裁的话一个"只差第 3 块"的 ACK 也会带上
    // 几万个零字，而它对发送方没有任何信息量。
    if (any) {
        a.bitmap.resize(last / 32 + 1);
    } else {
        a.bitmap.clear();
    }

    return a;
}

std::vector<std::uint32_t> missingChunks(const ChunkAck& ack, std::uint32_t totalChunks)
{
    std::vector<std::uint32_t> out;

    // ackThrough 超过 totalChunks 是对方的 bug，按 totalChunks 截断处理：
    // 这里的目标是"算出还要发什么"，不是校验对方。
    const auto through = std::min(ack.ackThrough, totalChunks);

    for (std::uint32_t k = through; k < totalChunks; ++k) {
        if (!bitmapTest(ack.bitmap, k - through)) {
            out.push_back(k);
        }
    }
    return out;
}

Status encodeAck(const ChunkAck& a, std::vector<char>& out)
{
    out.clear();
    PayloadWriter w(out);
    w.u32(a.ackThrough);
    w.u32(static_cast<std::uint32_t>(a.bitmap.size()));
    for (auto word : a.bitmap) {
        w.u32(word);
    }
    return Status::Ok();
}

bool decodeAck(const std::vector<char>& body, ChunkAck& out, std::string& err)
{
    PayloadReader r(body);

    ChunkAck a;
    a.ackThrough = r.u32();
    const auto words = r.u32();
    if (!r.ok()) {
        err = "ack: truncated header";
        return false;
    }

    // 先按声明长度校验剩余字节，再分配。
    //
    // 不校验就直接 resize(words) 是经典的"对端一个 u32 就让我们分配 16GB"。
    const std::uint64_t need = static_cast<std::uint64_t>(words) * 4;
    if (r.remaining() != need) {
        err = "ack: bitmap length mismatch";
        return false;
    }

    a.bitmap.reserve(words);
    for (std::uint32_t i = 0; i < words; ++i) {
        a.bitmap.push_back(r.u32());
    }
    if (!r.ok()) {
        err = "ack: truncated bitmap";
        return false;
    }

    out = std::move(a);
    return true;
}

// ---------------------------------------------------------------------------
// 完成握手
// ---------------------------------------------------------------------------

std::string_view toString(CompleteStatus s) noexcept
{
    switch (s) {
    case CompleteStatus::kOk:
        return "ok";
    case CompleteStatus::kRootMismatch:
        return "root_mismatch";
    case CompleteStatus::kWriteFailed:
        return "write_failed";
    case CompleteStatus::kShapeMismatch:
        return "shape_mismatch";
    }
    return "unknown";
}

Status encodeCompleteRequest(std::string_view merkleRootHex, std::uint64_t finalSize, std::vector<char>& out)
{
    if (!merkleRootHex.empty() && !isHexOfLen(merkleRootHex, 64)) {
        return Status::Fail(ErrorCode::kProtocol, "complete: merkle root must be 64 hex chars");
    }

    out.clear();
    PayloadWriter w(out);
    if (!w.shortStr(lowerHex(merkleRootHex))) {
        return Status::Fail(ErrorCode::kProtocol, "complete: merkle root too long");
    }
    w.u64(finalSize);
    return Status::Ok();
}

bool decodeCompleteRequest(const std::vector<char>& body, std::string& rootHex, std::uint64_t& finalSize, std::string& err)
{
    PayloadReader r(body);
    auto root = r.shortStr();
    const auto size = r.u64();
    if (!r.ok()) {
        err = "complete: truncated";
        return false;
    }
    if (!r.atEnd()) {
        err = "complete: trailing bytes";
        return false;
    }
    if (!root.empty() && !isHexOfLen(root, 64)) {
        err = "complete: merkle root must be 64 hex chars";
        return false;
    }

    rootHex = lowerHex(root);
    finalSize = size;
    return true;
}

Status encodeCompleteAnswer(CompleteStatus status, std::string_view merkleRootHex, std::vector<char>& out)
{
    if (!merkleRootHex.empty() && !isHexOfLen(merkleRootHex, 64)) {
        return Status::Fail(ErrorCode::kProtocol, "complete: merkle root must be 64 hex chars");
    }

    out.clear();
    PayloadWriter w(out);
    w.u8(static_cast<std::uint8_t>(status));
    if (!w.shortStr(lowerHex(merkleRootHex))) {
        return Status::Fail(ErrorCode::kProtocol, "complete: merkle root too long");
    }
    return Status::Ok();
}

bool decodeCompleteAnswer(const std::vector<char>& body, CompleteStatus& status, std::string& rootHex, std::string& err)
{
    PayloadReader r(body);
    const auto raw = r.u8();
    auto root = r.shortStr();
    if (!r.ok()) {
        err = "complete: truncated";
        return false;
    }
    if (!r.atEnd()) {
        err = "complete: trailing bytes";
        return false;
    }
    if (raw > static_cast<std::uint8_t>(CompleteStatus::kShapeMismatch)) {
        // 未知状态不能当成功。这是"以后加了个新状态，老客户端误判为通过"的
        // 唯一防线。
        err = "complete: unknown status " + std::to_string(raw);
        return false;
    }
    if (!root.empty() && !isHexOfLen(root, 64)) {
        err = "complete: merkle root must be 64 hex chars";
        return false;
    }

    status = static_cast<CompleteStatus>(raw);
    rootHex = lowerHex(root);
    return true;
}

// ---------------------------------------------------------------------------
// 取消
// ---------------------------------------------------------------------------

std::string_view toString(CancelReason r) noexcept
{
    switch (r) {
    case CancelReason::kUserCancelled:
        return "user_cancelled";
    case CancelReason::kPeerGone:
        return "peer_gone";
    case CancelReason::kTimeout:
        return "timeout";
    case CancelReason::kLocalError:
        return "local_error";
    case CancelReason::kHashMismatch:
        return "hash_mismatch";
    }
    return "unknown";
}

Status encodeCancel(CancelReason reason, std::string_view message, std::vector<char>& out)
{
    out.clear();
    PayloadWriter w(out);
    w.u8(static_cast<std::uint8_t>(reason));
    if (!w.str(message)) {
        return Status::Fail(ErrorCode::kProtocol, "cancel: message too long");
    }
    return Status::Ok();
}

bool decodeCancel(const std::vector<char>& body, CancelReason& reason, std::string& message, std::string& err)
{
    PayloadReader r(body);
    const auto raw = r.u8();
    auto msg = r.str();
    if (!r.ok()) {
        err = "cancel: truncated";
        return false;
    }
    if (!r.atEnd()) {
        err = "cancel: trailing bytes";
        return false;
    }
    if (raw > static_cast<std::uint8_t>(CancelReason::kHashMismatch)) {
        // 未知原因不该导致解析失败——取消本身是有效的，只是原因不认识。
        // 这里降级成 kLocalError 并保留原文，而不是拒绝整帧：
        // 拒绝会让双方都停在"我发了取消但对方没收到"的状态。
        reason = CancelReason::kLocalError;
        message = std::move(msg);
        return true;
    }
    reason = static_cast<CancelReason>(raw);
    message = std::move(msg);
    return true;
}

}   // namespace relayer::core::transfer
