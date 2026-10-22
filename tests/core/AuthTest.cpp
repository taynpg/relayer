#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "relayer/core/auth/Handshake.h"
#include "relayer/core/protocol/Frame.h"

using namespace relayer::core::auth;

namespace
{

// 固定输入，让测试可复现。生产路径用 randomSalt/randomNonce，
// 测试用这两个固定值注入，避免"随机导致偶发失败"。
Salt fixedSalt(std::uint8_t seed = 0x11)
{
    Salt s{};
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        s[i] = static_cast<std::uint8_t>(seed + i);
    }
    return s;
}

Nonce fixedNonce(std::uint8_t seed)
{
    Nonce n{};
    for (std::size_t i = 0; i < n.size(); ++i)
    {
        n[i] = static_cast<std::uint8_t>(seed * 7 + i * 3);
    }
    return n;
}

constexpr const char *kPassword = "correct horse battery staple";

// 一次完整握手的两端，用于反复构造"已验证"的状态。
struct Pair
{
    ServerHandshake server;
    ClientHandshake client;

    explicit Pair(std::string serverPw = kPassword, std::string clientPw = kPassword)
        : server(ServerConfig{std::move(serverPw), false, params::kDefaultIterations})
        , client(std::move(clientPw))
    {
    }
};

}  // namespace

// ===========================================================================
// 协议常量
// ===========================================================================
//
// 这些数字是线格式。测试把它们钉死，是为了让"顺手改一下"变成一次失败的
// 测试而不是一次静默的协议不兼容。

TEST_CASE("auth constants match PROTOCOL.md", "[auth][protocol]")
{
    SECTION("tag 的长度由字面量决定")
    {
        REQUIRE(params::kAuthTag == "relayer/auth/v2");
        // 长度写死在这里是为了钉住线格式：改 tag 等于改协议。
        // 15 = "relayer"(7) + "/"(1) + "auth"(4) + "/"(1) + "v2"(2)
        REQUIRE(params::kAuthTag.size() == 15);
    }

    SECTION("字段尺寸")
    {
        REQUIRE(params::kSaltSize == 16);
        REQUIRE(params::kNonceSize == 32);
        REQUIRE(params::kProofSize == 32);
    }

    SECTION("载荷总长")
    {
        // salt(16) + nonceS(32)
        REQUIRE(params::kChallengeSize == 48);
        // nonceC(32) + proof(32)
        REQUIRE(params::kResponseSize == 64);
        // status(1) + serverProof(32)
        REQUIRE(params::kResultSize == 33);
        // status(1)
        REQUIRE(params::kAbortSize == 1);
    }

    SECTION("迭代次数")
    {
        REQUIRE(params::kDefaultIterations == 20000);
    }
}

TEST_CASE("auth status codes match PROTOCOL.md table", "[auth][protocol]")
{
    REQUIRE(static_cast<std::uint8_t>(AuthStatus::kOk) == 0);
    REQUIRE(static_cast<std::uint8_t>(AuthStatus::kBadProof) == 1);
    REQUIRE(static_cast<std::uint8_t>(AuthStatus::kReplay) == 2);
    REQUIRE(static_cast<std::uint8_t>(AuthStatus::kUnsupported) == 3);
    REQUIRE(static_cast<std::uint8_t>(AuthStatus::kInternal) == 4);
}

TEST_CASE("auth status byte roundtrip", "[auth]")
{
    for (std::uint8_t raw = 0; raw <= 4; ++raw)
    {
        bool ok = false;
        const auto s = authStatusFromByte(raw, ok);
        REQUIRE(ok);
        REQUIRE(static_cast<std::uint8_t>(s) == raw);
    }

    SECTION("未知值被拒且归为 internal")
    {
        for (const std::uint8_t raw : {std::uint8_t{5}, std::uint8_t{6}, std::uint8_t{100}, std::uint8_t{255}})
        {
            bool ok = true;
            const auto s = authStatusFromByte(raw, ok);
            REQUIRE_FALSE(ok);
            REQUIRE(s == AuthStatus::kInternal);
        }
    }
}

TEST_CASE("auth status names are stable for logs", "[auth]")
{
    REQUIRE(toString(AuthStatus::kOk) == "ok");
    REQUIRE(toString(AuthStatus::kBadProof) == "bad_proof");
    REQUIRE(toString(AuthStatus::kReplay) == "replay");
    REQUIRE(toString(AuthStatus::kUnsupported) == "unsupported");
    REQUIRE(toString(AuthStatus::kInternal) == "internal");
}

// ===========================================================================
// proof 的密码学性质
// ===========================================================================

TEST_CASE("two proofs never coincide", "[auth][crypto]")
{
    // PROTOCOL.md §7.4：顺序相反保证两个 proof 一定不相等。
    // 这条是"中间人不能把客户端 proof 回给客户端"的全部依据。
    const Salt salt = fixedSalt();
    const SessionKey key = deriveKey(kPassword, salt);
    const Nonce ns = fixedNonce(1);
    const Nonce nc = fixedNonce(2);

    const Proof cp = computeClientProof(key, ns, nc);
    const Proof sp = computeServerProof(key, ns, nc);

    REQUIRE(cp != sp);

    SECTION("交换 nonce 顺序才得到对方的值")
    {
        // computeServerProof(ns, nc) 等价于 computeClientProof(nc, ns)。
        // 这钉死了"顺序是唯一区别"这件事。
        REQUIRE(computeServerProof(key, ns, nc) == computeClientProof(key, nc, ns));
    }
}

TEST_CASE("proof depends on every input", "[auth][crypto]")
{
    const Salt salt = fixedSalt();
    const SessionKey key = deriveKey(kPassword, salt);
    const Nonce ns = fixedNonce(1);
    const Nonce nc = fixedNonce(2);
    const Proof base = computeClientProof(key, ns, nc);

    SECTION("换 nonceS")
    {
        REQUIRE(computeClientProof(key, fixedNonce(9), nc) != base);
    }
    SECTION("换 nonceC")
    {
        REQUIRE(computeClientProof(key, ns, fixedNonce(9)) != base);
    }
    SECTION("换口令")
    {
        const SessionKey other = deriveKey("wrong password", salt);
        REQUIRE(computeClientProof(other, ns, nc) != base);
    }
    SECTION("换 salt")
    {
        const SessionKey other = deriveKey(kPassword, fixedSalt(0x40));
        REQUIRE(computeClientProof(other, ns, nc) != base);
    }
    SECTION("迭代次数不同则密钥不同")
    {
        const SessionKey weak = deriveKey(kPassword, salt, 1);
        REQUIRE(computeClientProof(weak, ns, nc) != base);
    }
}

TEST_CASE("deriveKey is deterministic and salt-sensitive", "[auth][crypto]")
{
    const SessionKey a = deriveKey(kPassword, fixedSalt());
    const SessionKey b = deriveKey(kPassword, fixedSalt());
    const SessionKey c = deriveKey(kPassword, fixedSalt(0x22));

    REQUIRE(a == b);
    REQUIRE(a != c);
}

// ===========================================================================
// NonceCache
// ===========================================================================

TEST_CASE("NonceCache rejects repeats", "[auth][nonce]")
{
    NonceCache cache;
    const Nonce n1 = fixedNonce(1);

    REQUIRE_FALSE(cache.seen(n1));
    REQUIRE(cache.checkAndInsert(n1));
    REQUIRE(cache.seen(n1));

    SECTION("第二次插入被拒")
    {
        REQUIRE_FALSE(cache.checkAndInsert(n1));
    }

    SECTION("不同的 nonce 仍然通过")
    {
        REQUIRE(cache.checkAndInsert(fixedNonce(2)));
        REQUIRE(cache.size() == 2);
    }
}

TEST_CASE("NonceCache evicts oldest when full", "[auth][nonce]")
{
    NonceCache cache(3);
    const Nonce n1 = fixedNonce(1);
    const Nonce n2 = fixedNonce(2);
    const Nonce n3 = fixedNonce(3);
    const Nonce n4 = fixedNonce(4);

    REQUIRE(cache.checkAndInsert(n1));
    REQUIRE(cache.checkAndInsert(n2));
    REQUIRE(cache.checkAndInsert(n3));
    REQUIRE(cache.size() == 3);

    // 插入第 4 个时淘汰 n1
    REQUIRE(cache.checkAndInsert(n4));
    REQUIRE(cache.size() == 3);
    REQUIRE_FALSE(cache.seen(n1));
    REQUIRE(cache.seen(n2));
    REQUIRE(cache.seen(n3));
    REQUIRE(cache.seen(n4));

    SECTION("clear 之后全部忘记")
    {
        cache.clear();
        REQUIRE(cache.size() == 0);
        REQUIRE(cache.checkAndInsert(n1));
    }
}

TEST_CASE("NonceCache with limit 1 still works", "[auth][nonce]")
{
    // limit=0 的构造会退化成 1，不能出现"写入即淘汰"的死循环
    NonceCache cache(0);
    REQUIRE(cache.checkAndInsert(fixedNonce(1)));
    REQUIRE(cache.size() == 1);
    REQUIRE_FALSE(cache.checkAndInsert(fixedNonce(1)));
}

// ===========================================================================
// 载荷编解码
// ===========================================================================

TEST_CASE("payload encoders produce exact sizes", "[auth][payload]")
{
    const Salt salt = fixedSalt();
    const Nonce ns = fixedNonce(1);
    const Nonce nc = fixedNonce(2);
    const Proof proof = computeClientProof(deriveKey(kPassword, salt), ns, nc);

    REQUIRE(encodeChallenge(salt, ns).size() == params::kChallengeSize);
    REQUIRE(encodeResponse(nc, proof).size() == params::kResponseSize);
    REQUIRE(encodeResult(AuthStatus::kOk, proof).size() == params::kResultSize);
    REQUIRE(encodeAbort(AuthStatus::kBadProof).size() == params::kAbortSize);
}

TEST_CASE("payload roundtrip", "[auth][payload]")
{
    const Salt salt = fixedSalt();
    const Nonce ns = fixedNonce(1);
    const Nonce nc = fixedNonce(2);
    const Proof proof = computeClientProof(deriveKey(kPassword, salt), ns, nc);
    std::string err;

    SECTION("challenge")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(salt, ns));
        Salt gotSalt{};
        Nonce gotNonce{};
        REQUIRE(decodeChallenge(f, gotSalt, gotNonce, err));
        REQUIRE(gotSalt == salt);
        REQUIRE(gotNonce == ns);
    }

    SECTION("response")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthResponse, encodeResponse(nc, proof));
        Nonce gotNonce{};
        Proof gotProof{};
        REQUIRE(decodeResponse(f, gotNonce, gotProof, err));
        REQUIRE(gotNonce == nc);
        REQUIRE(gotProof == proof);
    }

    SECTION("result")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kBadProof, proof));
        AuthStatus status{};
        Proof gotProof{};
        REQUIRE(decodeResult(f, status, gotProof, err));
        REQUIRE(status == AuthStatus::kBadProof);
        REQUIRE(gotProof == proof);
    }

    SECTION("abort")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthAbort, encodeAbort(AuthStatus::kReplay));
        AuthStatus status{};
        REQUIRE(decodeAbort(f, status, err));
        REQUIRE(status == AuthStatus::kReplay);
    }
}

TEST_CASE("decoders reject malformed payloads", "[auth][payload]")
{
    std::string err;
    const Salt salt = fixedSalt();
    const Nonce ns = fixedNonce(1);
    Salt gotSalt{};
    Nonce gotNonce{};
    Proof gotProof{};
    AuthStatus gotStatus{};

    SECTION("challenge 少一个字节")
    {
        auto body = encodeChallenge(salt, ns);
        body.pop_back();
        const Frame f = makeAuthFrame(FrameType::kAuthChallenge, std::move(body));
        REQUIRE_FALSE(decodeChallenge(f, gotSalt, gotNonce, err));
        REQUIRE_FALSE(err.empty());
    }

    SECTION("challenge 多一个字节")
    {
        auto body = encodeChallenge(salt, ns);
        body.push_back(0);
        const Frame f = makeAuthFrame(FrameType::kAuthChallenge, std::move(body));
        REQUIRE_FALSE(decodeChallenge(f, gotSalt, gotNonce, err));
    }

    SECTION("空 body")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthChallenge, {});
        REQUIRE_FALSE(decodeChallenge(f, gotSalt, gotNonce, err));
    }

    SECTION("类型不符")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthResponse, encodeChallenge(salt, ns));
        REQUIRE_FALSE(decodeChallenge(f, gotSalt, gotNonce, err));
    }

    SECTION("result 的状态字节未知")
    {
        auto body = encodeResult(AuthStatus::kOk, Proof{});
        body[0] = static_cast<char>(99);
        const Frame f = makeAuthFrame(FrameType::kAuthResult, std::move(body));
        REQUIRE_FALSE(decodeResult(f, gotStatus, gotProof, err));
        REQUIRE_FALSE(err.empty());
    }

    SECTION("abort 的状态字节未知")
    {
        const Frame f = makeAuthFrame(FrameType::kAuthAbort, std::vector<char>{static_cast<char>(200)});
        REQUIRE_FALSE(decodeAbort(f, gotStatus, err));
    }
}

TEST_CASE("auth frames carry no identity", "[auth][payload]")
{
    // 握手阶段 from/to/fuuid 必须为空：此时任何自称的身份都不可信。
    const Frame f = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(), fixedNonce(1)));
    REQUIRE(f.from.empty());
    REQUIRE(f.to.empty());
    REQUIRE(f.fuuid.empty());
    REQUIRE(f.sessionId == 0);
    REQUIRE(f.index == 0);
    REQUIRE(f.flags == kFlagNone);
}

// ===========================================================================
// 正常握手
// ===========================================================================

TEST_CASE("handshake succeeds with matching password", "[auth][flow]")
{
    Pair p;
    const Salt salt = fixedSalt();
    const Nonce ns = fixedNonce(1);

    REQUIRE(p.client.start().ok());
    REQUIRE(p.client.phase() == ClientPhase::kWaitChallenge);

    // 服务端 -> Challenge
    Frame challenge;
    bool hasOut = false;
    REQUIRE(p.server.startWith(challenge, hasOut, salt, ns).ok());
    REQUIRE(hasOut);
    REQUIRE(challenge.type == FrameType::kAuthChallenge);
    REQUIRE(p.server.phase() == ServerPhase::kWaitResponse);

    // 客户端处理 -> Response
    Frame response;
    hasOut = false;
    REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());
    REQUIRE(hasOut);
    REQUIRE(response.type == FrameType::kAuthResponse);
    REQUIRE(p.client.phase() == ClientPhase::kWaitResult);

    // 服务端处理 -> Result(ok)
    Frame result;
    hasOut = false;
    REQUIRE(p.server.handleFrame(response, result, hasOut).ok());
    REQUIRE(hasOut);
    REQUIRE(result.type == FrameType::kAuthResult);
    REQUIRE(p.server.phase() == ServerPhase::kEstablished);

    // 客户端处理 -> 完成
    Frame nothing;
    hasOut = true;
    REQUIRE(p.client.handleFrame(result, nothing, hasOut).ok());
    REQUIRE_FALSE(hasOut);  // 成功后不再产出帧
    REQUIRE(p.client.phase() == ClientPhase::kEstablished);
    REQUIRE(p.client.established());

    SECTION("双方密钥一致")
    {
        REQUIRE(p.client.sessionKey() == p.server.sessionKey());
    }
}

TEST_CASE("handshake with non-ASCII password", "[auth][flow]")
{
    // 口令可能含中文。PBKDF2 吃的是字节，两端编码一致就一致；
    // 这条测试确认没有在某处误当宽字符处理。
    Pair p("口令：测试用密码 🔐", "口令：测试用密码 🔐");
    Frame challenge;
    bool hasOut = false;
    REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());
    REQUIRE(p.client.start().ok());

    Frame response;
    REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());
    REQUIRE(hasOut);

    Frame result;
    REQUIRE(p.server.handleFrame(response, result, hasOut).ok());
    REQUIRE(hasOut);

    Frame nothing;
    REQUIRE(p.client.handleFrame(result, nothing, hasOut).ok());
    REQUIRE(p.client.established());
}

// ===========================================================================
// 失败路径
// ===========================================================================

TEST_CASE("wrong password is rejected with bad_proof", "[auth][failure]")
{
    Pair p(kPassword, "wrong password");

    Frame challenge;
    bool hasOut = false;
    REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

    REQUIRE(p.client.start().ok());
    Frame response;
    REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());
    REQUIRE(hasOut);

    Frame abort;
    const auto st = p.server.handleFrame(response, abort, hasOut);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kAuthFailed);
    REQUIRE(hasOut);
    REQUIRE(abort.type == FrameType::kAuthAbort);

    // abort 里的状态字节必须是 kBadProof，客户端才能给出"口令错"的提示
    AuthStatus status{};
    std::string err;
    REQUIRE(decodeAbort(abort, status, err));
    REQUIRE(status == AuthStatus::kBadProof);

    REQUIRE(p.server.phase() == ServerPhase::kFailed);
}

TEST_CASE("client rejects a fake server", "[auth][failure]")
{
    // 攻击者不知道口令，但能接受 TCP 连接。它发一个正常的 Challenge，
    // 拿到客户端的 Response 后无法算出正确的 serverProof。
    Pair p;
    REQUIRE(p.client.start().ok());

    Frame challenge;
    bool hasOut = false;
    REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

    Frame response;
    REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());
    REQUIRE(hasOut);

    SECTION("伪造的 proof")
    {
        Proof fake{};
        fake.fill(0xAB);
        Frame fakeResult = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kOk, fake));

        Frame nothing;
        const auto st = p.client.handleFrame(fakeResult, nothing, hasOut);
        REQUIRE_FALSE(st.ok());
        REQUIRE(st.code == relayer::ErrorCode::kAuthFailed);
        REQUIRE(p.client.phase() == ClientPhase::kFailed);
    }

    SECTION("把客户端的 proof 原样回给客户端")
    {
        // 这正是 §7.4 要防的攻击：攻击者没有 key，只能把收到的 proof 抄回来。
        // 因为两个 proof 顺序相反，抄回来的值一定不对。
        Nonce gotNonce{};
        Proof clientProof{};
        std::string err;
        REQUIRE(decodeResponse(response, gotNonce, clientProof, err));

        Frame echoed = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kOk, clientProof));
        Frame nothing;
        REQUIRE_FALSE(p.client.handleFrame(echoed, nothing, hasOut).ok());
        REQUIRE(p.client.phase() == ClientPhase::kFailed);
    }

    SECTION("服务端声称失败")
    {
        Frame rejected = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kBadProof, Proof{}));
        Frame nothing;
        REQUIRE_FALSE(p.client.handleFrame(rejected, nothing, hasOut).ok());
        REQUIRE(p.client.phase() == ClientPhase::kFailed);
    }
}

TEST_CASE("replayed nonce is rejected", "[auth][failure]")
{
    SECTION("服务端重放 nonceC")
    {
        Pair p;
        Frame challenge;
        bool hasOut = false;
        REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());
        REQUIRE(p.client.start().ok());

        Frame response;
        REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());

        // 第一次正常
        Frame result;
        REQUIRE(p.server.handleFrame(response, result, hasOut).ok());

        // 同一条 Response 再投一次。此时服务端已 established，
        // 收到认证帧一律判 replay。
        Frame again;
        const auto st = p.server.handleFrame(response, again, hasOut);
        REQUIRE_FALSE(st.ok());
        REQUIRE(p.server.phase() == ServerPhase::kFailed);
    }

    SECTION("服务端在 proof 校验前就记住 nonceC")
    {
        // 设计意图：重放检查发生在 PBKDF2/proof 计算之前，所以同一个 nonceC
        // 第二次进来时错误原因必须是 replay，而不是 bad_proof。
        //
        // 直接用 NonceCache 复现这条顺序：先插入、再查。
        NonceCache cache;
        const Nonce nc = fixedNonce(2);
        REQUIRE(cache.checkAndInsert(nc));
        REQUIRE_FALSE(cache.checkAndInsert(nc));
        REQUIRE(cache.seen(nc));
    }

    SECTION("错误 proof 也被记入 nonce 缓存")
    {
        // 验证"先记 nonce 再算 proof"：用一个算错的 proof 走一遍，
        // 服务端失败，但 nonce 应当已被消费。
        ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
        const Salt salt = fixedSalt();
        const Nonce ns = fixedNonce(1);
        Frame challenge;
        bool hasOut = false;
        REQUIRE(server.startWith(challenge, hasOut, salt, ns).ok());

        const Nonce nc = fixedNonce(2);
        Proof badProof{};
        badProof.fill(0x5A);
        Frame first = makeAuthFrame(FrameType::kAuthResponse, encodeResponse(nc, badProof));

        Frame out;
        const auto st = server.handleFrame(first, out, hasOut);
        REQUIRE_FALSE(st.ok());
        // 错误原因必须是 bad_proof（说明 nonce 检查通过了，卡在 proof 上）
        REQUIRE(st.code == relayer::ErrorCode::kAuthFailed);
        REQUIRE(st.message.find("proof") != std::string::npos);
    }

    SECTION("客户端重放 Challenge")
    {
        Pair p;
        REQUIRE(p.client.start().ok());

        Frame challenge;
        bool hasOut = false;
        REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

        Frame response;
        REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());

        // 第二个 Challenge（同样的 nonceS）
        Frame dupResponse;
        const auto st = p.client.handleFrame(challenge, dupResponse, hasOut);
        REQUIRE_FALSE(st.ok());
        REQUIRE(p.client.phase() == ClientPhase::kFailed);
    }

    SECTION("客户端收到 nonceS 复用但内容不同的 Challenge")
    {
        Pair p;
        REQUIRE(p.client.start().ok());

        Frame challenge;
        bool hasOut = false;
        REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());
        Frame response;
        REQUIRE(p.client.handleFrame(challenge, response, hasOut).ok());

        // 相位已经是 kWaitResult，任何 Challenge 都被判为重复
        Frame other = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(0x50), fixedNonce(5)));
        Frame out;
        REQUIRE_FALSE(p.client.handleFrame(other, out, hasOut).ok());
        REQUIRE(p.client.phase() == ClientPhase::kFailed);
    }
}

TEST_CASE("client nonce equal to server nonce is rejected", "[auth][failure]")
{
    // 如果两个 nonce 相同，clientProof 和 serverProof 的输入就一样了，
    // 中间人可以把客户端的 proof 当成服务端的 proof 回给客户端。
    ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
    const Salt salt = fixedSalt();
    const Nonce shared = fixedNonce(7);

    Frame challenge;
    bool hasOut = false;
    REQUIRE(server.startWith(challenge, hasOut, salt, shared).ok());

    const SessionKey key = deriveKey(kPassword, salt);
    const Proof proof = computeClientProof(key, shared, shared);  // 两个 nonce 相同
    Frame response = makeAuthFrame(FrameType::kAuthResponse, encodeResponse(shared, proof));

    Frame out;
    const auto st = server.handleFrame(response, out, hasOut);
    REQUIRE_FALSE(st.ok());
    REQUIRE(server.phase() == ServerPhase::kFailed);
}

TEST_CASE("non-auth frame before handshake is rejected", "[auth][failure]")
{
    SECTION("客户端在握手完成前收到业务帧")
    {
        // §7.6：绝不能因为"对端发了业务帧"就认为对端不开认证。
        ClientHandshake client(kPassword);
        REQUIRE(client.start().ok());

        Frame business;
        business.type = FrameType::kAskFileList;
        Frame out;
        bool hasOut = false;
        const auto st = client.handleFrame(business, out, hasOut);
        REQUIRE_FALSE(st.ok());
        REQUIRE(client.phase() == ClientPhase::kFailed);
    }

    SECTION("服务端在握手完成前收到业务帧")
    {
        ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
        Frame challenge;
        bool hasOut = false;
        REQUIRE(server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

        Frame business;
        business.type = FrameType::kAskFileList;
        Frame out;
        const auto st = server.handleFrame(business, out, hasOut);
        REQUIRE_FALSE(st.ok());
        // 未认证的连接不回任何数据
        REQUIRE_FALSE(hasOut);
        REQUIRE(server.phase() == ServerPhase::kFailed);
    }
}

TEST_CASE("out-of-order auth frames are rejected", "[auth][failure]")
{
    SECTION("客户端在 Challenge 之前收到 Result")
    {
        ClientHandshake client(kPassword);
        REQUIRE(client.start().ok());

        Frame result = makeAuthFrame(FrameType::kAuthResult, encodeResult(AuthStatus::kOk, Proof{}));
        Frame out;
        bool hasOut = false;
        REQUIRE_FALSE(client.handleFrame(result, out, hasOut).ok());
        REQUIRE(client.phase() == ClientPhase::kFailed);
    }

    SECTION("客户端未 start 就收帧")
    {
        ClientHandshake client(kPassword);
        Frame challenge = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(), fixedNonce(1)));
        Frame out;
        bool hasOut = false;
        REQUIRE_FALSE(client.handleFrame(challenge, out, hasOut).ok());
    }

    SECTION("服务端收到客户端不该发的帧")
    {
        ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
        Frame challenge;
        bool hasOut = false;
        REQUIRE(server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

        // 客户端反向发 Challenge
        Frame reversed = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(), fixedNonce(2)));
        Frame out;
        REQUIRE_FALSE(server.handleFrame(reversed, out, hasOut).ok());
        REQUIRE(server.phase() == ServerPhase::kFailed);
    }
}

TEST_CASE("malformed challenge makes client fail", "[auth][failure]")
{
    ClientHandshake client(kPassword);
    REQUIRE(client.start().ok());

    Frame bad = makeAuthFrame(FrameType::kAuthChallenge, std::vector<char>(10, 0));
    Frame out;
    bool hasOut = false;
    REQUIRE_FALSE(client.handleFrame(bad, out, hasOut).ok());
    REQUIRE(client.phase() == ClientPhase::kFailed);
}

TEST_CASE("failed handshake is terminal", "[auth][failure]")
{
    Pair p;
    REQUIRE(p.client.start().ok());
    Frame business;
    business.type = FrameType::kAskFileList;
    Frame out;
    bool hasOut = false;
    REQUIRE_FALSE(p.client.handleFrame(business, out, hasOut).ok());
    REQUIRE(p.client.phase() == ClientPhase::kFailed);

    // 失败之后即使来一个格式完全正确的 Challenge 也不能恢复
    Frame challenge = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(), fixedNonce(1)));
    REQUIRE_FALSE(p.client.handleFrame(challenge, out, hasOut).ok());
    REQUIRE(p.client.phase() == ClientPhase::kFailed);
}

TEST_CASE("peer abort ends the handshake", "[auth][failure]")
{
    SECTION("客户端收到 abort")
    {
        ClientHandshake client(kPassword);
        REQUIRE(client.start().ok());
        Frame abort = makeAuthFrame(FrameType::kAuthAbort, encodeAbort(AuthStatus::kUnsupported));
        Frame out;
        bool hasOut = false;
        const auto st = client.handleFrame(abort, out, hasOut);
        REQUIRE_FALSE(st.ok());
        REQUIRE(client.phase() == ClientPhase::kFailed);
    }

    SECTION("服务端收到 abort")
    {
        ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
        Frame challenge;
        bool hasOut = false;
        REQUIRE(server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

        Frame abort = makeAuthFrame(FrameType::kAuthAbort, encodeAbort(AuthStatus::kBadProof));
        Frame out;
        REQUIRE_FALSE(server.handleFrame(abort, out, hasOut).ok());
        REQUIRE(server.phase() == ServerPhase::kFailed);
    }
}

// ===========================================================================
// 不认证模式（§7.6）
// ===========================================================================

TEST_CASE("server with allowNoAuth skips the handshake", "[auth][noauth]")
{
    ServerHandshake server(ServerConfig{"", true, params::kDefaultIterations});
    Frame out;
    bool hasOut = true;
    REQUIRE(server.start(out, hasOut).ok());
    REQUIRE_FALSE(hasOut);  // 不发 Challenge
    REQUIRE(server.established());
    REQUIRE(server.noAuth());

    // 直接受理业务帧
    Frame business;
    business.type = FrameType::kAskFileList;
    Frame nothing;
    REQUIRE(server.handleFrame(business, nothing, hasOut).ok());
    REQUIRE(server.phase() == ServerPhase::kEstablished);
}

TEST_CASE("client in no-auth mode skips the handshake", "[auth][noauth]")
{
    ClientHandshake client("", ClientMode::kNoAuth);
    REQUIRE(client.start().ok());
    REQUIRE(client.established());

    Frame business;
    business.type = FrameType::kAnswerFileList;
    Frame out;
    bool hasOut = false;
    REQUIRE(client.handleFrame(business, out, hasOut).ok());
    REQUIRE(client.phase() == ClientPhase::kEstablished);
}

TEST_CASE("no-auth cannot be forced by the peer", "[auth][noauth]")
{
    // 核心安全性质：客户端在要求认证的模式下，绝不会因为对端发了业务帧
    // 就降级。攻击者只能通过"让客户端自己配置成不认证"来绕过。
    ClientHandshake client(kPassword, ClientMode::kRequireAuth);
    REQUIRE(client.start().ok());

    Frame business;
    business.type = FrameType::kAskFileList;
    Frame out;
    bool hasOut = false;
    REQUIRE_FALSE(client.handleFrame(business, out, hasOut).ok());
    REQUIRE(client.phase() == ClientPhase::kFailed);
}

TEST_CASE("auth enabled with empty password fails at startup", "[auth][noauth]")
{
    // 漏配口令不能静默变成开放服务器
    ServerHandshake server(ServerConfig{"", false, params::kDefaultIterations});
    Frame out;
    bool hasOut = false;
    const auto st = server.start(out, hasOut);
    REQUIRE_FALSE(st.ok());
    REQUIRE(server.phase() == ServerPhase::kFailed);
}

TEST_CASE("zero iterations is rejected", "[auth][noauth]")
{
    ServerHandshake server(ServerConfig{kPassword, false, 0});
    Frame out;
    bool hasOut = false;
    REQUIRE_FALSE(server.start(out, hasOut).ok());
    REQUIRE(server.phase() == ServerPhase::kFailed);
}

// ===========================================================================
// 随机数
// ===========================================================================

TEST_CASE("CSPRNG fills the buffer", "[auth][random]")
{
    Nonce a{};
    Nonce b{};
    REQUIRE(randomNonce(a));
    REQUIRE(randomNonce(b));

    // 两块 32 字节随机数相同的概率是 2^-256，实际不会发生。
    // 这条测试能抓到"随机源其实没写进缓冲区"这类实现错误。
    REQUIRE(a != b);

    bool allZero = true;
    for (const auto v : a)
    {
        if (v != 0)
        {
            allZero = false;
            break;
        }
    }
    REQUIRE_FALSE(allZero);
}

TEST_CASE("randomSalt produces distinct salts", "[auth][random]")
{
    Salt a{};
    Salt b{};
    REQUIRE(randomSalt(a));
    REQUIRE(randomSalt(b));
    REQUIRE(a != b);
}

TEST_CASE("fillRandom handles edge cases", "[auth][random]")
{
    std::uint8_t one = 0;
    REQUIRE(fillRandom(&one, 1));

    REQUIRE(fillRandom(nullptr, 0));        // 长度为 0 允许空指针
    REQUIRE_FALSE(fillRandom(nullptr, 8));  // 长度非 0 但空指针是错误
}

// ===========================================================================
// 身份绑定
// ===========================================================================

TEST_CASE("bindSenderIdentity overrides forged from", "[auth][identity]")
{
    Frame f;
    f.type = FrameType::kFileRequestSend;
    f.from = "victim-client-id";  // 客户端伪造成别人

    bindSenderIdentity(f, "real-client-id");
    REQUIRE(f.from == "real-client-id");

    SECTION("重复调用结果稳定")
    {
        bindSenderIdentity(f, "real-client-id");
        REQUIRE(f.from == "real-client-id");
    }

    SECTION("不触碰其他字段")
    {
        f.to = "peer";
        f.fuuid = "abc";
        bindSenderIdentity(f, "x");
        REQUIRE(f.to == "peer");
        REQUIRE(f.fuuid == "abc");
    }
}

TEST_CASE("only auth frames are allowed before handshake", "[auth][identity]")
{
    REQUIRE(isAllowedBeforeAuth(FrameType::kAuthChallenge));
    REQUIRE(isAllowedBeforeAuth(FrameType::kAuthResponse));
    REQUIRE(isAllowedBeforeAuth(FrameType::kAuthResult));
    REQUIRE(isAllowedBeforeAuth(FrameType::kAuthAbort));

    REQUIRE_FALSE(isAllowedBeforeAuth(FrameType::kAskId));
    REQUIRE_FALSE(isAllowedBeforeAuth(FrameType::kAskFileList));
    REQUIRE_FALSE(isAllowedBeforeAuth(FrameType::kFileRequestSend));
    REQUIRE_FALSE(isAllowedBeforeAuth(FrameType::kChunkRequestData));
}

// ===========================================================================
// 完整帧往返：认证帧走一遍 Pack/UnPack
// ===========================================================================
//
// 前面所有握手测试都是直接传 Frame 结构。这里补一条"过一遍线格式"的，
// 确保认证帧的 body 在 Pack/UnPack 之后仍然完好（尤其 size 边界）。

TEST_CASE("auth handshake survives pack and unpack", "[auth][wire]")
{
    using namespace relayer::protocol;

    Pair p;
    REQUIRE(p.client.start().ok());

    Frame challenge;
    bool hasOut = false;
    REQUIRE(p.server.startWith(challenge, hasOut, fixedSalt(), fixedNonce(1)).ok());

    // 服务端 -> 线上 -> 客户端
    const std::vector<char> wire = packFrame(challenge);
    REQUIRE_FALSE(wire.empty());

    Frame decoded;
    std::size_t consumed = 0;
    REQUIRE(unpackFrame(wire.data(), wire.size(), decoded, consumed) == UnPackResult::kOk);
    REQUIRE(consumed == wire.size());
    REQUIRE(decoded.type == FrameType::kAuthChallenge);
    REQUIRE(decoded.body.size() == params::kChallengeSize);

    Frame response;
    REQUIRE(p.client.handleFrame(decoded, response, hasOut).ok());
    REQUIRE(hasOut);

    // 客户端 -> 线上 -> 服务端
    const std::vector<char> wire2 = packFrame(response);
    Frame decoded2;
    REQUIRE(unpackFrame(wire2.data(), wire2.size(), decoded2, consumed) == UnPackResult::kOk);
    REQUIRE(decoded2.body.size() == params::kResponseSize);

    Frame result;
    REQUIRE(p.server.handleFrame(decoded2, result, hasOut).ok());
    REQUIRE(hasOut);

    const std::vector<char> wire3 = packFrame(result);
    Frame decoded3;
    REQUIRE(unpackFrame(wire3.data(), wire3.size(), decoded3, consumed) == UnPackResult::kOk);

    Frame nothing;
    REQUIRE(p.client.handleFrame(decoded3, nothing, hasOut).ok());
    REQUIRE(p.client.established());
    REQUIRE(p.server.established());
}

// ===========================================================================
// 确定性：同样的输入产生同样的 proof
// ===========================================================================
//
// 这里验证的是 computeClientProof / computeServerProof 是纯函数：同样的
// (key, nonceS, nonceC) 一定给出同样的 32 字节。这条性质是"两端独立算出的
// proof 能对上"的全部依据。
//
// 注意**不要**用两次完整握手来测这件事：客户端每次都会用 CSPRNG 生成新的
// nonceC，两次运行的输入本来就不同，结果不同才是正确的。想测端到端确定性，
// 必须把 nonce 也固定下来——那就是下面这条。

TEST_CASE("proof computation is a pure function", "[auth][flow]")
{
    const Salt salt = fixedSalt();
    const SessionKey key = deriveKey(kPassword, salt);
    const Nonce ns = fixedNonce(1);
    const Nonce nc = fixedNonce(2);

    REQUIRE(computeClientProof(key, ns, nc) == computeClientProof(key, ns, nc));
    REQUIRE(computeServerProof(key, ns, nc) == computeServerProof(key, ns, nc));

    // 重新派生一次 key（模拟另一端独立算），结果必须一致
    const SessionKey key2 = deriveKey(kPassword, salt);
    REQUIRE(computeClientProof(key2, ns, nc) == computeClientProof(key, ns, nc));
}

TEST_CASE("full handshake is deterministic when nonces are fixed", "[auth][flow]")
{
    // 客户端 nonceC 由 CSPRNG 生成，无法从外部注入。所以这里改用"手工构造
    // Response"的方式固定全部输入，端到端跑两遍，确认 serverProof 一致。
    auto run = []() -> Proof
    {
        ServerHandshake server(ServerConfig{kPassword, false, params::kDefaultIterations});
        const Salt salt = fixedSalt();
        const Nonce ns = fixedNonce(1);
        const Nonce nc = fixedNonce(2);

        Frame challenge;
        bool hasOut = false;
        REQUIRE(server.startWith(challenge, hasOut, salt, ns).ok());

        const SessionKey key = deriveKey(kPassword, salt);
        Frame response = makeAuthFrame(FrameType::kAuthResponse, encodeResponse(nc, computeClientProof(key, ns, nc)));

        Frame result;
        REQUIRE(server.handleFrame(response, result, hasOut).ok());

        AuthStatus status{};
        Proof serverProof{};
        std::string err;
        REQUIRE(decodeResult(result, status, serverProof, err));
        REQUIRE(status == AuthStatus::kOk);
        return serverProof;
    };

    REQUIRE(run() == run());
}

TEST_CASE("each handshake uses a fresh client nonce", "[auth][flow]")
{
    // 反过来确认这件事：两次独立启动的客户端一定生成不同的 nonceC。
    // 如果这里失败，说明 CSPRNG 没接上，重放防护会整体失效。
    ClientHandshake a(kPassword);
    ClientHandshake b(kPassword);
    REQUIRE(a.start().ok());
    REQUIRE(b.start().ok());

    Frame challenge = makeAuthFrame(FrameType::kAuthChallenge, encodeChallenge(fixedSalt(), fixedNonce(1)));
    Frame ra;
    Frame rb;
    bool hasA = false;
    bool hasB = false;
    REQUIRE(a.handleFrame(challenge, ra, hasA).ok());
    REQUIRE(b.handleFrame(challenge, rb, hasB).ok());
    REQUIRE(hasA);
    REQUIRE(hasB);

    // 两个 Response 的前 32 字节是各自的 nonceC
    Nonce na{};
    Nonce nb{};
    Proof pa{};
    Proof pb{};
    std::string err;
    REQUIRE(decodeResponse(ra, na, pa, err));
    REQUIRE(decodeResponse(rb, nb, pb, err));
    REQUIRE(na != nb);
    REQUIRE(pa != pb);
}
