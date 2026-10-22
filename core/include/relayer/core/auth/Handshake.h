#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/common/crypto/Sha256.h"
#include "relayer/core/protocol/Frame.h"

// 认证握手（PROTOCOL.md §7）。
//
// 本文件同样**不含任何 I/O**：两个状态机都是"吃一帧，吐一帧"的纯函数式对象。
// 谁读 socket、谁写 socket、握手超时怎么算、超时后谁断开——全在 transport 层。
//
// 这样切的理由很实际：握手最容易出错的不是密码学，而是"收到不该收的帧时
// 该怎么办"。把状态转移写成纯逻辑，就能用单元测试把每一种错序都跑一遍，
// 不需要起端口，也不需要等超时。
//
// 时序（PROTOCOL.md §7.1）：
//
//   client                                    server
//     │◀── Challenge(salt[16], nonceS[32]) ────│
//     │── Response(nonceC[32], proof[32]) ────▶│
//     │◀── Result(ok, serverProof[32]) ────────│
//
// 双方状态机各自维护自己的相位（Phase）。相位用来拒绝乱序帧——比如客户端
// 在收到 Challenge 之前收到 Result，或者在已完成后又收到 Challenge。

namespace relayer::core::auth
{

// 协议层类型在本命名空间下的短名字。
//
// Frame 住在 relayer::protocol 而不是 relayer::core::protocol：protocol 是
// 整个通信库的地基（server 和 cli 都用），不该被绑在 core 这个"通信库"
// 概念下面。但 auth 自己的代码写 relayer::protocol::Frame 太啰嗦，这里
// 起别名。注意别名不改变身份——handler 收到的仍是同一个类型。
using protocol::Frame;
using protocol::FrameType;
using protocol::kFlagNone;

// ---------------------------------------------------------------------------
// 协议常量
// ---------------------------------------------------------------------------
//
// 这些数字是**线格式的一部分**，与 docs/PROTOCOL.md §7 一一对应。
// 改动其中任何一个都等于改协议版本：老客户端会算出不同的 proof，
// 表现为"口令正确却连不上"，排查成本很高。

namespace params
{
// 域分隔串（PROTOCOL.md §7.3）。作用是把本次 HMAC 与"任何其他用到同一
// 密钥的 HMAC"区分开：没有它，若将来某环节也用 key 做 MAC，攻击者可能
// 把两个场景的输出互换。
inline constexpr std::string_view kAuthTag = "relayer/auth/v2";  // 15 字节（不含 NUL）

inline constexpr std::size_t kSaltSize = 16;
inline constexpr std::size_t kNonceSize = 32;
inline constexpr std::size_t kProofSize = 32;  // = SHA-256 摘要长度

// PBKDF2 迭代次数。这个值同时决定服务端握手延迟和老客户端的兼容性，
// 所以钉在协议文档里，而不是做成可配置项：服务端私自调高会让所有客户端
// 算出不同的 key，握手的失败现象与"口令错"完全一样。
inline constexpr std::uint32_t kDefaultIterations = 20000;

// 载荷布局（PROTOCOL.md §7.1）：
//   kAuthChallenge = salt[16] || nonceS[32]           = 48
//   kAuthResponse  = nonceC[32] || proof[32]          = 64
//   kAuthResult    = status(u8) || serverProof[32]    = 33
//   kAuthAbort     = status(u8)                       = 1
inline constexpr std::size_t kChallengeSize = kSaltSize + kNonceSize;
inline constexpr std::size_t kResponseSize = kNonceSize + kProofSize;
inline constexpr std::size_t kResultSize = 1 + kProofSize;
inline constexpr std::size_t kAbortSize = 1;

// 已见 nonce 集合的容量上限。
//
// 为什么要上限：nonce 集合是"只进不出"的，长连接上不断握手会无限增长。
// 超过上限时淘汰最旧的——这里的安全前提是：攻击者要重放，必须让受害者
// 在"旧 nonce 已被淘汰"之后仍然接受它。而在一条正常连接上，握手只发生
// 一次；重放防护真正需要覆盖的是"攻击者在短时间内重复投递同一批字节"。
// 4096 个 32 字节 nonce 约 130KB 内存，足够覆盖一个进程生命周期内的
// 全部连接，同时不至于成为内存增长点。
inline constexpr std::size_t kNonceCacheLimit = 4096;
}  // namespace params

using Salt = std::array<std::uint8_t, params::kSaltSize>;
using Nonce = std::array<std::uint8_t, params::kNonceSize>;
using Proof = std::array<std::uint8_t, params::kProofSize>;
using SessionKey = crypto::HmacKey;  // 32 字节，与 proof 同长

// ---------------------------------------------------------------------------
// 认证结果码（PROTOCOL.md §7.2）
// ---------------------------------------------------------------------------
//
// 数值是线格式。与 ErrorCode 分开是因为这个码要发给**对端**，而 ErrorCode
// 是本进程内部诊断用的；两者混用会让"对端收到了什么"变得依赖本地实现。
//
// 认证失败时刻意不区分"口令错"和"对端不存在"，否则攻击者能靠错误码做
// 用户枚举。本项目只有一条共享口令，天然没有这个问题，但扩展时要守住。
enum class AuthStatus : std::uint8_t
{
    kOk = 0,
    kBadProof = 1,     // proof 不匹配（口令错）
    kReplay = 2,       // nonce 重复（重放攻击）
    kUnsupported = 3,  // 版本或算法不支持
    kInternal = 4,     // 服务端内部错误
};

[[nodiscard]] std::string_view toString(AuthStatus s) noexcept;

// 线上字节 -> 枚举。未知值返回 kInternal 并把 ok 置 false：
// 对端发来一个我们不认识的码，最接近的解释就是"对端比我们新/坏掉了"，
// 但绝不能当成 kOk。
[[nodiscard]] AuthStatus authStatusFromByte(std::uint8_t raw, bool &ok) noexcept;

// AuthStatus -> 上报给上层的 Status。kOk 返回成功。
[[nodiscard]] Status toStatus(AuthStatus s, std::string_view context);

// ---------------------------------------------------------------------------
// 密钥派生与 proof 计算
// ---------------------------------------------------------------------------

// key = PBKDF2-HMAC-SHA256(password, salt, kDefaultIterations, 32)
[[nodiscard]] SessionKey deriveKey(std::string_view password, const Salt &salt,
                                   std::uint32_t iterations = params::kDefaultIterations);

// clientProof = HMAC(key, tag || nonceS || nonceC)
[[nodiscard]] Proof computeClientProof(const SessionKey &key, const Nonce &nonceS, const Nonce &nonceC);

// serverProof = HMAC(key, tag || nonceC || nonceS)
//
// 顺序与 clientProof 相反，这不是笔误，也不是为了"看起来对称"：
// 顺序相同会让两个 proof 相等，中间人就能把客户端发来的 proof 原样回给
// 客户端冒充服务端（PROTOCOL.md §7.4）。
[[nodiscard]] Proof computeServerProof(const SessionKey &key, const Nonce &nonceS, const Nonce &nonceC);

// ---------------------------------------------------------------------------
// nonce 重放防护
// ---------------------------------------------------------------------------

// 已见 nonce 集合。
//
// 为什么不用时间窗口：攻击者可以在窗口内重放——窗口的意义只是"过了这么久
// 就当作新 nonce"，而那正好是攻击者需要的机会。维护一个集合才能真正拒掉
// 重复值（PROTOCOL.md §7.5）。
//
// 这不是线程安全的：它属于单条连接的状态机，而单条连接由单个线程驱动。
// 跨连接共享要不要加锁，是 transport 层的决定，不在这里预先加锁。
class NonceCache
{
public:
    explicit NonceCache(std::size_t limit = params::kNonceCacheLimit);

    // 首次见到返回 true 并记住；已见过返回 false（调用方应判为重放）。
    bool checkAndInsert(const Nonce &nonce);

    [[nodiscard]] bool seen(const Nonce &nonce) const;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return order_.size();
    }
    void clear() noexcept;

private:
    // 用 deque 记插入顺序，达到上限时从头部淘汰最旧的。
    // 为什么不用 unordered_set 配迭代器淘汰：迭代器失效规则容易写错，
    // 而这里的数据量（几千项）根本不值得为常数因子冒险。
    std::size_t limit_;
    std::deque<Nonce> order_;

    [[nodiscard]] bool contains(const Nonce &n) const;
};

// ---------------------------------------------------------------------------
// 载荷编解码
// ---------------------------------------------------------------------------
//
// 单独暴露是为了测试能直接构造畸形载荷（截断、超长），而不必伪造整个状态机。

[[nodiscard]] std::vector<char> encodeChallenge(const Salt &salt, const Nonce &nonceS);
[[nodiscard]] std::vector<char> encodeResponse(const Nonce &nonceC, const Proof &proof);
[[nodiscard]] std::vector<char> encodeResult(AuthStatus status, const Proof &serverProof);
[[nodiscard]] std::vector<char> encodeAbort(AuthStatus status);

// 解码失败一律返回 false 并把原因写进 err。上层应当把它当作协议错误，
// 而不是"再等等"：长度不对说明对端实现有问题，等下去不会变好。
[[nodiscard]] bool decodeChallenge(const Frame &f, Salt &salt, Nonce &nonceS, std::string &err);
[[nodiscard]] bool decodeResponse(const Frame &f, Nonce &nonceC, Proof &proof, std::string &err);
[[nodiscard]] bool decodeResult(const Frame &f, AuthStatus &status, Proof &serverProof, std::string &err);
[[nodiscard]] bool decodeAbort(const Frame &f, AuthStatus &status, std::string &err);

// ---------------------------------------------------------------------------
// 构造认证帧
// ---------------------------------------------------------------------------
//
// 认证帧一律：sessionId = 0、index = 0、from/to/fuuid 为空、不带任何标志位。
//
// 为什么 from/to 为空：握手发生在"身份尚未确认"的阶段，此时任何自称的
// 身份都不可信。把 from 留空，服务端就不可能在握手帧上读到伪造的身份；
// 身份只在 Result(ok) 之后由服务端自己分配（§7.6 的强制覆写规则）。

[[nodiscard]] Frame makeAuthFrame(FrameType type, std::vector<char> body);

// ---------------------------------------------------------------------------
// 客户端状态机
// ---------------------------------------------------------------------------

enum class ClientPhase
{
    kIdle,           // 还没开始（未调用 start）
    kWaitChallenge,  // 已发出开始信号，等 Challenge
    kWaitResult,     // 已发 Response，等 Result
    kEstablished,    // 已通过认证，可以走业务帧
    kFailed,         // 已失败（terminal），后续任何输入都被拒
};

// 服务端地址不可达时，客户端需要知道"该不该期待 Challenge"。
// 这个开关由**本地配置**决定，不由对端数据决定：如果允许"收到业务帧就
// 认为对方不开认证"，攻击者只要抢先发一个业务帧就能把认证降级掉（§7.6）。
enum class ClientMode
{
    kRequireAuth,  // 必须完成握手，收到非认证帧即报错
    kNoAuth,       // 明知服务端关闭了认证，跳过握手
};

class ClientHandshake
{
public:
    // password 在构造时拷贝一份内部副本。调用方之后可以清掉自己的字符串。
    // 不把口令留在成员里更好，但每次要算 key 都得有它，而重新派生又需要
    // salt——两害相权，保留到握手结束，随后由上层丢弃整个对象。
    ClientHandshake(std::string password, ClientMode mode = ClientMode::kRequireAuth);

    // 开始握手。kRequireAuth 模式下这一步不产出帧（等服务端先发 Challenge）。
    //
    // 注意协议上客户端**不主动**发任何东西：Challenge 由服务端发起，
    // 这样服务端可以通过"根本不发 Challenge"来表达"我不需要认证"。
    [[nodiscard]] Status start();

    // 吃一帧，产出要回给对端的帧（可能是空的）。
    //
    // 返回值语义：
    //   - Status 失败：握手失败，调用方应当发送 abort 帧（若 out 里已有）
    //     然后断开。phase() 会变成 kFailed。
    //   - Status 成功且 phase() == kEstablished：握手完成，可以走业务帧。
    //   - Status 成功且 phase() != kEstablished：还需要继续等。
    [[nodiscard]] Status handleFrame(const Frame &in, Frame &out, bool &hasOut);

    [[nodiscard]] ClientPhase phase() const noexcept
    {
        return phase_;
    }
    [[nodiscard]] bool established() const noexcept
    {
        return phase_ == ClientPhase::kEstablished;
    }

    // 握手成功后可用。用于后续对业务帧的可选加密（§6.2）。
    // 未完成握手时是未定义内容——上层只能在 established() 为真时读它。
    [[nodiscard]] const SessionKey &sessionKey() const noexcept
    {
        return key_;
    }

    // 服务端在 Result(ok) 里回的 proof，仅用于测试断言，业务不用。
    [[nodiscard]] const Proof &serverProof() const noexcept
    {
        return serverProof_;
    }

private:
    [[nodiscard]] Status fail(AuthStatus status, std::string message);

    std::string password_;
    ClientMode mode_;
    ClientPhase phase_ = ClientPhase::kIdle;

    SessionKey key_{};
    Nonce nonceC_{};
    Nonce nonceS_{};
    Proof serverProof_{};

    NonceCache seenNonces_;
};

// ---------------------------------------------------------------------------
// 服务端状态机
// ---------------------------------------------------------------------------

enum class ServerPhase
{
    kIdle,          // 还没开始
    kWaitResponse,  // 已发 Challenge，等 Response
    kEstablished,   // 已通过认证
    kFailed,        // 已失败（terminal）
};

// 服务端配置。
//
// allowNoAuth 就是 §7.6 里的 authEnabled=false。它是一个**显式配置项**，
// 不是"握手失败后的兜底"：状态机没有任何路径会因为"等不到 Response"而
// 自动降级。想关认证，必须在构造时明说。
struct ServerConfig
{
    std::string password;
    bool allowNoAuth = false;

    // 只接受这个范围内的迭代次数会更安全，但目前客户端不使用自定义值。
    // 留这个字段是为了让服务端在未来的协议版本里能拒绝过弱的派生参数。
    std::uint32_t iterations = params::kDefaultIterations;
};

class ServerHandshake
{
public:
    explicit ServerHandshake(ServerConfig config);

    // 开始握手，产出第一帧 Challenge。
    //
    // allowNoAuth 为真时不产出帧（hasOut=false）且直接进入 kEstablished：
    // 这正是"服务端可以配置成不认证，连接建立后直接受理业务帧"。
    //
    // salt 与 nonceS 用 CSPRNG 生成。为了让测试可复现，另外提供一个
    // 注入随机源的重载——生产代码**不要**用它。
    [[nodiscard]] Status start(Frame &out, bool &hasOut);
    [[nodiscard]] Status startWith(Frame &out, bool &hasOut, const Salt &salt, const Nonce &nonceS);

    [[nodiscard]] Status handleFrame(const Frame &in, Frame &out, bool &hasOut);

    [[nodiscard]] ServerPhase phase() const noexcept
    {
        return phase_;
    }
    [[nodiscard]] bool established() const noexcept
    {
        return phase_ == ServerPhase::kEstablished;
    }
    [[nodiscard]] bool noAuth() const noexcept
    {
        return noAuth_;
    }

    // 握手通过后身份就该绑定了。这里先返回空——真正的身份分配（客户端 id）
    // 属于会话层，握手层只负责"证明对方知道口令"。
    [[nodiscard]] const Salt &salt() const noexcept
    {
        return salt_;
    }
    [[nodiscard]] const Nonce &serverNonce() const noexcept
    {
        return nonceS_;
    }
    [[nodiscard]] const SessionKey &sessionKey() const noexcept
    {
        return key_;
    }

private:
    [[nodiscard]] Status fail(AuthStatus status, std::string message, Frame &out, bool &hasOut);

    ServerConfig config_;
    ServerPhase phase_ = ServerPhase::kIdle;
    bool noAuth_ = false;

    Salt salt_{};
    Nonce nonceS_{};
    SessionKey key_{};

    NonceCache seenNonces_;
};

// ---------------------------------------------------------------------------
// 随机数
// ---------------------------------------------------------------------------

// 用系统 CSPRNG 填充。失败返回 false（几乎只在极端环境下发生：没有
// /dev/urandom、BCryptGenRandom 不可用）。**不要**在失败时退化成 rand()：
// 可预测的 nonce 会让重放防护完全失效，而此时握手看起来是"成功的"。
[[nodiscard]] bool fillRandom(void *buf, std::size_t len) noexcept;

[[nodiscard]] bool randomSalt(Salt &out) noexcept;
[[nodiscard]] bool randomNonce(Nonce &out) noexcept;

// ---------------------------------------------------------------------------
// 身份绑定辅助
// ---------------------------------------------------------------------------

// 服务端转发业务帧前，强制把 from 覆写成认证身份（PROTOCOL.md §7.6）。
//
// 为什么必须做成独立函数而不是"记得写 f.from = id"：这条规则一旦漏掉一处，
// 就允许任何已认证客户端伪装成任意其他客户端给对方发文件——是本协议里
// 少数几个"漏写就是安全漏洞"的地方，值得单独一个名字。
void bindSenderIdentity(Frame &f, std::string_view authenticatedId);

// 该帧类型是否允许在认证完成前出现。
//
// 仅认证帧（0..3）合法。其余一律应丢弃或断开：未认证连接如果能发控制帧，
// 攻击者就能在不提供口令的情况下枚举文件列表。
[[nodiscard]] bool isAllowedBeforeAuth(FrameType t) noexcept;

}  // namespace relayer::core::auth
