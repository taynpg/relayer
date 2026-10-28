#include <utility>

#include "relayer/core/auth/Handshake.h"

// 服务端握手状态机。
//
// 相位转移：
//
//   kIdle ──start()──▶ kWaitResponse ──Response(正确)──▶ kEstablished
//         │                  │
//         └─(allowNoAuth)────┴── 任何错误 ──▶ kFailed
//
// 服务端比客户端多两个责任：
//
//   1. 生成 salt 与 nonceS（每次连接都新生成，不复用）
//   2. 分拣错误码：口令错是 kBadProof、nonce 重复是 kReplay。
//      这两个必须分开，否则客户端无法区分"我口令配错了"和"有人在重放"。

namespace relayer::core::auth {

ServerHandshake::ServerHandshake(ServerConfig config) : config_(std::move(config))
{
}

Status ServerHandshake::start(Frame& out, bool& hasOut)
{
    Salt salt{};
    Nonce nonceS{};
    if (!randomSalt(salt) || !randomNonce(nonceS)) {
        phase_ = ServerPhase::kFailed;
        return Status::Fail(ErrorCode::kAuthFailed, "server: CSPRNG unavailable, cannot start handshake");
    }
    return startWith(out, hasOut, salt, nonceS);
}

Status ServerHandshake::startWith(Frame& out, bool& hasOut, const Salt& salt, const Nonce& nonceS)
{
    hasOut = false;

    if (phase_ != ServerPhase::kIdle) {
        return Status::Fail(ErrorCode::kProtocol, "server handshake already started");
    }

    if (config_.allowNoAuth) {
        // §7.6：服务端配置成不认证时，不发 Challenge，直接受理业务帧。
        //
        // 这里刻意**不看** password 是否为空：如果写成"口令为空就算不认证"，
        // 那么一个漏配口令的部署会静默变成开放服务器。要关认证必须显式写
        // allowNoAuth = true。
        noAuth_ = true;
        phase_ = ServerPhase::kEstablished;
        return Status::Ok();
    }

    if (config_.password.empty()) {
        // 要认证却没有口令：这是配置错误，不能当成"不认证"继续跑。
        // 在启动阶段就失败，比等到第一个客户端连上来才失败好排查。
        phase_ = ServerPhase::kFailed;
        return Status::Fail(ErrorCode::kAuthFailed, "server: auth enabled but password is empty");
    }

    if (config_.iterations == 0) {
        // 0 次迭代等于明文存储口令。pbkdf2HmacSha256 会返回全零而不是崩溃，
        // 但那样"认证通过"的判定就变得毫无意义，所以在这里拦下。
        phase_ = ServerPhase::kFailed;
        return Status::Fail(ErrorCode::kAuthFailed, "server: iterations must be greater than zero");
    }

    salt_ = salt;
    nonceS_ = nonceS;

    // 把自己发的 nonce 也记进去：这样即使对端把 nonceS 原样当 nonceC 发回来，
    // 也会被下面是 checkAndInsert 拒掉（否则两个 nonce 相同会让两个 proof 相等）。
    if (!seenNonces_.checkAndInsert(nonceS_)) {
        phase_ = ServerPhase::kFailed;
        return Status::Fail(ErrorCode::kAuthFailed, "server: generated nonce collides with an earlier one");
    }

    out = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(salt_, nonceS_));
    hasOut = true;
    phase_ = ServerPhase::kWaitResponse;
    return Status::Ok();
}

Status ServerHandshake::fail(AuthStatus status, std::string message, Frame& out, bool& hasOut)
{
    phase_ = ServerPhase::kFailed;
    key_.fill(0);

    // 失败时回一个 abort 帧，让客户端知道原因而不是干等超时。
    // abort 的载荷只有一个状态字节，且**不含**任何 proof——失败路径上
    // 不能泄露"离正确 proof 差多少"这类信息。
    out = makeAuthFrame(FrameType::kAuthAbort, encodeAbort(status));
    hasOut = true;

    return toStatus(status, message);
}

Status ServerHandshake::handleFrame(const Frame& in, Frame& out, bool& hasOut)
{
    hasOut = false;

    switch (phase_) {
    case ServerPhase::kFailed:
        return Status::Fail(ErrorCode::kAuthFailed, "server handshake already failed");
    case ServerPhase::kIdle:
        return Status::Fail(ErrorCode::kProtocol, "server handshake not started");
    case ServerPhase::kEstablished:
        if (isAllowedBeforeAuth(in.type)) {
            // 认证完成后又收到认证帧：对端在重放，或实现错乱。
            return fail(AuthStatus::kReplay, "server: unexpected auth frame after handshake completed", out, hasOut);
        }
        return Status::Ok();
    case ServerPhase::kWaitResponse:
        break;
    }

    switch (in.type) {
    case FrameType::kAuthResponse: {
        Nonce nonceC{};
        Proof proof{};
        std::string err;
        if (!decodeResponse(in, nonceC, proof, err)) {
            return fail(AuthStatus::kUnsupported, "server: " + err, out, hasOut);
        }

        // 客户端的 nonceC 不能与本次的 nonceS 相同。如果相同，两个 proof
        // 的输入就变成一样了（§7.4 的对称性被打破），中间的"中介者攻击"
        // 就能让客户端把自己的 proof 当成服务端 proof 接受。
        if (nonceC == nonceS_) {
            return fail(AuthStatus::kReplay, "server: client nonce equals server nonce", out, hasOut);
        }

        // 重放检查必须在算 proof 之前：PBKDF2 很贵（20000 次迭代），
        // 让重放请求先通过一道便宜的过滤，避免被刷爆 CPU。
        if (!seenNonces_.checkAndInsert(nonceC)) {
            return fail(AuthStatus::kReplay, "server: client nonce reused", out, hasOut);
        }

        key_ = deriveKey(config_.password, salt_, config_.iterations);

        const Proof expected = computeClientProof(key_, nonceS_, nonceC);
        if (!crypto::constantTimeEquals(expected.data(), proof.data(), expected.size())) {
            return fail(AuthStatus::kBadProof, "server: client proof mismatch", out, hasOut);
        }

        // 通过：回 serverProof。它用的是相反的 nonce 顺序，所以一定不等于
        // 客户端刚发的那个 proof。
        const Proof serverProof = computeServerProof(key_, nonceS_, nonceC);
        out = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kOk, serverProof));
        hasOut = true;
        phase_ = ServerPhase::kEstablished;
        return Status::Ok();
    }

    case FrameType::kAuthChallenge:
    case FrameType::kAuthResult:
        // 客户端不该发这两种类型。收到说明对端把方向搞反了，或者有人在注入。
        return fail(AuthStatus::kUnsupported, "server: client sent a server-only auth frame", out, hasOut);

    case FrameType::kAuthAbort: {
        AuthStatus status{};
        std::string err;
        // 客户端主动放弃。这不是服务端的错误，但握手确实结束了。
        // 不再回 abort：对端已经在断开的路上了，回帧没有意义。
        phase_ = ServerPhase::kFailed;
        key_.fill(0);
        if (!decodeAbort(in, status, err)) {
            return toStatus(AuthStatus::kInternal, "server: peer aborted handshake (malformed)");
        }
        return toStatus(status == AuthStatus::kOk ? AuthStatus::kInternal : status, "server: peer aborted handshake");
    }

    default:
        break;
    }

    // 未认证连接发业务帧：直接断开，不回任何数据。
    //
    // 为什么连 abort 都不回：攻击者能借"你回了什么"来判断服务端是否在监听、
    // 版本几何。而未认证的连接本来就没有任何权利要求服务端回应。
    phase_ = ServerPhase::kFailed;
    key_.fill(0);
    return Status::Fail(ErrorCode::kProtocol, "server: non-auth frame before handshake completed");
}

}   // namespace relayer::core::auth
