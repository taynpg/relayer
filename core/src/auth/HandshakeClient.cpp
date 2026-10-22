#include <utility>

#include "relayer/core/auth/Handshake.h"

// 客户端握手状态机。
//
// 相位转移：
//
//   kIdle ──start()──▶ kWaitChallenge ──Challenge──▶ kWaitResult ──Result(ok)──▶ kEstablished
//         │                  │                          │
//         └──────────────────┴── 任何错误 ───────────────┴─────────────────────▶ kFailed
//
// kFailed 是终态：一旦进入，后续任何帧都被拒。这避免了"失败之后靠收到某些
// 字节恢复"的路径——那条路径永远是最容易被绕过的。

namespace relayer::core::auth
{

ClientHandshake::ClientHandshake(std::string password, ClientMode mode)
    : password_(std::move(password))
    , mode_(mode)
{
}

Status ClientHandshake::fail(AuthStatus status, std::string message)
{
    phase_ = ClientPhase::kFailed;
    // 失败时把密钥抹掉：握手没成功，这个 key 不应对任何人有价值，
    // 但留着它就是一份多余的口令派生结果。
    key_.fill(0);
    return toStatus(status, message);
}

Status ClientHandshake::start()
{
    if (phase_ != ClientPhase::kIdle)
    {
        return Status::Fail(ErrorCode::kProtocol, "client handshake already started");
    }

    if (mode_ != ClientMode::kRequireAuth)
    {
        // 明知服务端关了认证：不发任何帧，直接可用。
        // 这个分支只由本地配置进入，绝不会因为"对端发来了什么"而进入。
        phase_ = ClientPhase::kEstablished;
        return Status::Ok();
    }

    // 协议上客户端不主动发东西：Challenge 由服务端发起（§7.1）。客户端只需
    // 生成自己的 nonceC 并等着。提前生成而不是收到 Challenge 时再生成，
    // 是为了让 nonce 的生成时机与对端行为解耦——对端的字节不该影响我们的随机数。
    if (!randomNonce(nonceC_))
    {
        return fail(AuthStatus::kInternal, "client: CSPRNG unavailable, cannot generate nonce");
    }

    phase_ = ClientPhase::kWaitChallenge;
    return Status::Ok();
}

Status ClientHandshake::handleFrame(const Frame &in, Frame &out, bool &hasOut)
{
    hasOut = false;

    switch (phase_)
    {
    case ClientPhase::kFailed:
        return Status::Fail(ErrorCode::kAuthFailed, "client handshake already failed");
    case ClientPhase::kIdle:
        return Status::Fail(ErrorCode::kProtocol, "client handshake not started");
    case ClientPhase::kEstablished:
        // 已经通过认证之后又来一个认证帧：要么对端在重放，要么对端实现错乱。
        // 两种情况都不该继续用它，所以直接判失败而不是静默忽略。
        if (isAllowedBeforeAuth(in.type))
        {
            return fail(AuthStatus::kReplay, "client: unexpected auth frame after handshake completed");
        }
        // 业务帧在完成后是正常的，交给上层。
        return Status::Ok();
    case ClientPhase::kWaitChallenge:
    case ClientPhase::kWaitResult:
        break;
    }

    switch (in.type)
    {
    case FrameType::kAuthChallenge:
    {
        if (phase_ != ClientPhase::kWaitChallenge)
        {
            // kWaitResult 时收到第二个 Challenge：对端在重放第一个，或者发了
            // 两个。前者正是重放攻击的形态，直接拒。
            return fail(AuthStatus::kReplay, "client: duplicate challenge");
        }

        Salt salt{};
        Nonce nonceS{};
        std::string err;
        if (!decodeChallenge(in, salt, nonceS, err))
        {
            return fail(AuthStatus::kUnsupported, "client: " + err);
        }

        // 服务端 nonce 必须没见过。客户端自己发的 nonceC 由服务端检查——
        // 双方各守一边，各自拒掉"自己发出的那一半"被复用。
        if (!seenNonces_.checkAndInsert(nonceS))
        {
            return fail(AuthStatus::kReplay, "client: server nonce reused");
        }
        nonceS_ = nonceS;

        key_ = deriveKey(password_, salt);
        const Proof proof = computeClientProof(key_, nonceS_, nonceC_);

        out = makeAuthFrame(FrameType::kAuthResponse, encodeResponse(nonceC_, proof));
        hasOut = true;
        phase_ = ClientPhase::kWaitResult;
        return Status::Ok();
    }

    case FrameType::kAuthResult:
    {
        if (phase_ != ClientPhase::kWaitResult)
        {
            // kWaitChallenge 时收到 Result：对端跳过了 Challenge。没有 salt 就
            // 算不出 key，也就无从校验 serverProof。
            return fail(AuthStatus::kUnsupported, "client: result before challenge");
        }

        AuthStatus status{};
        Proof serverProof{};
        std::string err;
        if (!decodeResult(in, status, serverProof, err))
        {
            return fail(AuthStatus::kUnsupported, "client: " + err);
        }

        if (status != AuthStatus::kOk)
        {
            return fail(status, "client: server rejected credentials");
        }

        // 这一步是客户端识别假服务端的唯一手段：只有真正知道 key 的一方
        // 才能算出正确的 serverProof。少了它，任何能接受 TCP 连接的进程
        // 都能冒充服务端，把客户端引导到恶意文件源。
        const Proof expected = computeServerProof(key_, nonceS_, nonceC_);
        if (!crypto::constantTimeEquals(expected.data(), serverProof.data(), expected.size()))
        {
            return fail(AuthStatus::kBadProof, "client: server proof mismatch (possible impersonation)");
        }

        serverProof_ = serverProof;
        phase_ = ClientPhase::kEstablished;
        return Status::Ok();
    }

    case FrameType::kAuthAbort:
    {
        AuthStatus status{};
        std::string err;
        // 载荷坏了也照样失败，只是拿不到精确原因——把它归为内部错误。
        if (!decodeAbort(in, status, err))
        {
            return fail(AuthStatus::kInternal, "client: peer aborted handshake");
        }
        return fail(status == AuthStatus::kOk ? AuthStatus::kInternal : status, "client: peer aborted handshake");
    }

    default:
        break;
    }

    // 走到这里说明收到的是业务帧，但握手还没完成。
    //
    // 为什么不"宽容地"把它当成对端不支持认证：那正是 §7.6 里禁止的静默降级。
    // 攻击者只要抢先发一个业务帧，就能让客户端放弃认证。
    return fail(AuthStatus::kUnsupported, "client: non-auth frame before handshake completed");
}

}  // namespace relayer::core::auth
