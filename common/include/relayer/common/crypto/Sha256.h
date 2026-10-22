#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace relayer::crypto
{

// 流式 SHA-256（FIPS 180-4）。
//
// 提供增量接口而非只有一次性函数，是因为完整文件校验无法一次把文件读进内存
// （1GB 文件会直接撑爆 32 位进程）。调用方按块 update，最后 digest。
//
// 线程安全：每个实例独占自己的状态，多实例可并发使用；单实例不可跨线程共享。
class Sha256
{
public:
    static constexpr std::size_t kDigestSize = 32;
    static constexpr std::size_t kBlockSize = 64;

    using Digest = std::array<std::uint8_t, kDigestSize>;

    Sha256();

    void update(const void *data, std::size_t len);
    [[nodiscard]] Digest digest();

    // 一次性便捷接口
    static Digest hash(const void *data, std::size_t len);
    static std::string hex(const Digest &digest);

private:
    void transform(const std::uint8_t block[kBlockSize]);

    std::uint32_t state_[8];
    std::uint64_t bitlen_;
    std::uint8_t buffer_[kBlockSize];
    std::uint32_t bufferLen_;
};

// ---------------------------------------------------------------------------
// HMAC-SHA256（RFC 2104）
// ---------------------------------------------------------------------------

using HmacKey = Sha256::Digest;  // 派生出来的密钥固定 32 字节

// 一次性 HMAC。key 可以是任意长度，内部会按 RFC 2104 处理：
// 超过块长（64 字节）先哈希，不足则右补零。
//
// 为什么不做流式：认证场景里被 MAC 的消息总是短小的定长结构
// （tag + 两个 nonce = 几十字节），没有分块的需求。加一个流式接口
// 只是徒增状态管理的出错面。
[[nodiscard]] Sha256::Digest hmacSha256(const void *key, std::size_t keyLen, const void *data, std::size_t dataLen);

// 便捷重载：std::string / 字符串字面量
[[nodiscard]] Sha256::Digest hmacSha256(std::string_view key, std::string_view data);

// 常量时间比较。
//
// 认证失败判定**必须**用它，不能用 memcmp：memcmp 在第一个不同字节处
// 就返回，攻击者能从中逐字节试出正确的 proof（计时攻击）。这里始终
// 比完全部字节，耗时只与长度有关。
[[nodiscard]] bool constantTimeEquals(const std::uint8_t *a, const std::uint8_t *b, std::size_t len) noexcept;

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256（RFC 8018 §5.2）
// ---------------------------------------------------------------------------

// 从口令派生 32 字节密钥。
//
// iterations 是安全与延迟的权衡：本项目的默认值见 auth 层的 kDefaultIterations
// （20000）。这个数字在 2010 年代够用，放在今天偏低，但它同时也决定了
// 老客户端能否连上新服务端——改它等于改协议，所以定在协议文档里而不是这里。
//
// 不提供"从外部注入迭代次数"的重载：那会让攻击者可以要求服务端用
// 1 次迭代来握手，等于绕过强度设计。迭代次数由协议版本钉死。
[[nodiscard]] HmacKey pbkdf2HmacSha256(std::string_view password, const std::uint8_t *salt, std::size_t saltLen,
                                       std::uint32_t iterations);

}  // namespace relayer::crypto
