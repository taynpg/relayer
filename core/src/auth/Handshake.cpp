#include "relayer/core/auth/Handshake.h"

#include <algorithm>
#include <cstring>

// 认证握手的公共部分：状态码、密钥派生、proof 计算、nonce 缓存、载荷编解码。
//
// 客户端与服务端两个状态机放在 HandshakeClient.cpp / HandshakeServer.cpp，
// 这样每个文件只讲一件事：这个文件讲"认证用到的纯计算"。

namespace relayer::core::auth
{

// ---------------------------------------------------------------------------
// 状态码
// ---------------------------------------------------------------------------

std::string_view toString(AuthStatus s) noexcept
{
    switch (s)
    {
    case AuthStatus::kOk:
        return "ok";
    case AuthStatus::kBadProof:
        return "bad_proof";
    case AuthStatus::kReplay:
        return "replay";
    case AuthStatus::kUnsupported:
        return "unsupported";
    case AuthStatus::kInternal:
        return "internal";
    }
    return "unknown";  // 不可达，但 clang 需要覆盖所有路径
}

AuthStatus authStatusFromByte(std::uint8_t raw, bool &ok) noexcept
{
    switch (raw)
    {
    case 0:
        ok = true;
        return AuthStatus::kOk;
    case 1:
        ok = true;
        return AuthStatus::kBadProof;
    case 2:
        ok = true;
        return AuthStatus::kReplay;
    case 3:
        ok = true;
        return AuthStatus::kUnsupported;
    case 4:
        ok = true;
        return AuthStatus::kInternal;
    default:
        ok = false;
        return AuthStatus::kInternal;
    }
}

Status toStatus(AuthStatus s, std::string_view context)
{
    if (s == AuthStatus::kOk)
    {
        return Status::Ok();
    }
    std::string msg;
    msg.reserve(context.size() + 32);
    msg.append(context);
    msg.append(": ");
    msg.append(toString(s));
    return Status::Fail(ErrorCode::kAuthFailed, std::move(msg));
}

// ---------------------------------------------------------------------------
// 密钥派生与 proof
// ---------------------------------------------------------------------------

SessionKey deriveKey(std::string_view password, const Salt &salt, std::uint32_t iterations)
{
    return crypto::pbkdf2HmacSha256(password, salt.data(), salt.size(), iterations);
}

namespace
{

// 把 tag || nonceA || nonceB 拼成一段连续内存再交给 HMAC。
//
// 为什么先拼接而不是分两次 update：hmacSha256 是一次性接口（见 Sha256.h 的
// 说明），它内部不做流式。最长 24 + 32 + 32 = 88 字节，栈上放得下。
Proof computeProof(const SessionKey &key, const Nonce &first, const Nonce &second)
{
    constexpr std::size_t kTagLen = params::kAuthTag.size();
    std::uint8_t msg[kTagLen + 2 * params::kNonceSize];

    std::memcpy(msg, params::kAuthTag.data(), kTagLen);
    std::memcpy(msg + kTagLen, first.data(), params::kNonceSize);
    std::memcpy(msg + kTagLen + params::kNonceSize, second.data(), params::kNonceSize);

    return crypto::hmacSha256(key.data(), key.size(), msg, sizeof(msg));
}

}  // namespace

Proof computeClientProof(const SessionKey &key, const Nonce &nonceS, const Nonce &nonceC)
{
    return computeProof(key, nonceS, nonceC);
}

Proof computeServerProof(const SessionKey &key, const Nonce &nonceS, const Nonce &nonceC)
{
    // 参数顺序故意和前一个相反（§7.4）。
    return computeProof(key, nonceC, nonceS);
}

// ---------------------------------------------------------------------------
// NonceCache
// ---------------------------------------------------------------------------

NonceCache::NonceCache(std::size_t limit)
    : limit_(limit == 0 ? 1 : limit)  // 0 会导致 checkAndInsert 每写入就淘汰，语义混乱
{
}

bool NonceCache::contains(const Nonce &n) const
{
    return std::find(order_.begin(), order_.end(), n) != order_.end();
}

bool NonceCache::seen(const Nonce &n) const
{
    return contains(n);
}

bool NonceCache::checkAndInsert(const Nonce &nonce)
{
    if (contains(nonce))
    {
        return false;
    }
    order_.push_back(nonce);
    // 超出上限时淘汰最旧的。淘汰后旧 nonce 可以被重放，但攻击者要利用这点，
    // 必须让对方在这么久之后仍然接受同一批字节——正常连接上握手只发生一次，
    // 而 4096 条的容量已经覆盖了单个进程生命周期内的全部连接。
    while (order_.size() > limit_)
    {
        order_.pop_front();
    }
    return true;
}

void NonceCache::clear() noexcept
{
    order_.clear();
}

// ---------------------------------------------------------------------------
// 载荷编解码
// ---------------------------------------------------------------------------

namespace
{

void appendBytes(std::vector<char> &out, const void *src, std::size_t len)
{
    const auto *p = static_cast<const char *>(src);
    out.insert(out.end(), p, p + len);
}

bool expectSize(const Frame &f, std::size_t expected, std::string_view what, std::string &err)
{
    if (f.body.size() == expected)
    {
        return true;
    }
    err.assign(what);
    err.append(" payload size mismatch: expected ");
    err.append(std::to_string(expected));
    err.append(", got ");
    err.append(std::to_string(f.body.size()));
    return false;
}

bool expectType(const Frame &f, FrameType expected, std::string &err)
{
    if (f.type == expected)
    {
        return true;
    }
    err.assign("unexpected frame type: expected ");
    err.append(protocol::toString(expected));
    err.append(", got ");
    err.append(protocol::toString(f.type));
    return false;
}

}  // namespace

std::vector<char> encodeChallenge(const Salt &salt, const Nonce &nonceS)
{
    std::vector<char> body;
    body.reserve(params::kChallengeSize);
    appendBytes(body, salt.data(), salt.size());
    appendBytes(body, nonceS.data(), nonceS.size());
    return body;
}

std::vector<char> encodeResponse(const Nonce &nonceC, const Proof &proof)
{
    std::vector<char> body;
    body.reserve(params::kResponseSize);
    appendBytes(body, nonceC.data(), nonceC.size());
    appendBytes(body, proof.data(), proof.size());
    return body;
}

std::vector<char> encodeResult(AuthStatus status, const Proof &serverProof)
{
    std::vector<char> body;
    body.reserve(params::kResultSize);
    body.push_back(static_cast<char>(static_cast<std::uint8_t>(status)));
    appendBytes(body, serverProof.data(), serverProof.size());
    return body;
}

std::vector<char> encodeAbort(AuthStatus status)
{
    return std::vector<char>{static_cast<char>(static_cast<std::uint8_t>(status))};
}

bool decodeChallenge(const Frame &f, Salt &salt, Nonce &nonceS, std::string &err)
{
    if (!expectType(f, FrameType::kAuthChallenge, err))
    {
        return false;
    }
    if (!expectSize(f, params::kChallengeSize, "challenge", err))
    {
        return false;
    }
    std::memcpy(salt.data(), f.body.data(), salt.size());
    std::memcpy(nonceS.data(), f.body.data() + salt.size(), nonceS.size());
    return true;
}

bool decodeResponse(const Frame &f, Nonce &nonceC, Proof &proof, std::string &err)
{
    if (!expectType(f, FrameType::kAuthResponse, err))
    {
        return false;
    }
    if (!expectSize(f, params::kResponseSize, "response", err))
    {
        return false;
    }
    std::memcpy(nonceC.data(), f.body.data(), nonceC.size());
    std::memcpy(proof.data(), f.body.data() + nonceC.size(), proof.size());
    return true;
}

bool decodeResult(const Frame &f, AuthStatus &status, Proof &serverProof, std::string &err)
{
    if (!expectType(f, FrameType::kAuthResult, err))
    {
        return false;
    }
    if (!expectSize(f, params::kResultSize, "result", err))
    {
        return false;
    }
    bool ok = false;
    const auto raw = static_cast<std::uint8_t>(f.body[0]);
    status = authStatusFromByte(raw, ok);
    if (!ok)
    {
        err.assign("unknown auth status byte: ");
        err.append(std::to_string(static_cast<unsigned>(raw)));
        return false;
    }
    std::memcpy(serverProof.data(), f.body.data() + 1, serverProof.size());
    return true;
}

bool decodeAbort(const Frame &f, AuthStatus &status, std::string &err)
{
    if (!expectType(f, FrameType::kAuthAbort, err))
    {
        return false;
    }
    if (!expectSize(f, params::kAbortSize, "abort", err))
    {
        return false;
    }
    bool ok = false;
    const auto raw = static_cast<std::uint8_t>(f.body[0]);
    status = authStatusFromByte(raw, ok);
    if (!ok)
    {
        err.assign("unknown auth abort status byte: ");
        err.append(std::to_string(static_cast<unsigned>(raw)));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 帧构造与身份绑定
// ---------------------------------------------------------------------------

Frame makeAuthFrame(FrameType type, std::vector<char> body)
{
    Frame f;
    f.type = type;
    f.flags = kFlagNone;
    f.sessionId = 0;
    f.index = 0;
    // from / to / fuuid 全部留空：握手阶段任何自称的身份都不可信（见头文件说明）。
    f.body = std::move(body);
    return f;
}

void bindSenderIdentity(Frame &f, std::string_view authenticatedId)
{
    f.from.assign(authenticatedId.data(), authenticatedId.size());
}

bool isAllowedBeforeAuth(FrameType t) noexcept
{
    // 只有 0..3 这四个认证帧。用 range 而不是逐个比较，是为了将来往认证段
    // 加类型时不会漏掉这里——但也要意识到那是放宽了这层校验，加之前想清楚。
    return protocol::isAuthType(t);
}

}  // namespace relayer::core::auth
