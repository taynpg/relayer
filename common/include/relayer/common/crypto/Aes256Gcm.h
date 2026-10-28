#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// AES-256-GCM（NIST SP 800-38D）。自包含实现，不依赖 OpenSSL。
//
// 为什么自己写：我们的目标平台里有一个只装了 MSYS2 运行时的场景，
// 引入 OpenSSL 要么静态链接 5MB+，要么给用户增加一个"装对了版本才能跑"
// 的部署前提。AES-GCM 的参考实现不到 600 行，测过 NIST 官方向量之后，
// 可控性比多一个动态依赖更高。
//
// !! 安全提示 !!
// 这里的 AES 用 T-table 实现，查表下标依赖密钥，属于**非**常量时间实现，
// 存在缓存计时侧信道。本项目的威胁模型是"防窃听、防篡改、防重放"，
// **不含**防同机旁路攻击——所以这个取舍是可接受的。如果你的部署场景里
// 攻击者能在同一台物理机上反复测量解密耗时，请换用 AES-NI 或 bitsliced 实现。

namespace relayer::crypto {

constexpr std::size_t kAesKeySize = 32;     // AES-256
constexpr std::size_t kGcmNonceSize = 12;   // GCM 标准 nonce 长度（96 位）
constexpr std::size_t kGcmTagSize = 16;     // 完整 128 位 tag

// 加密。
//
// key   : 32 字节
// nonce : 12 字节。**同一密钥下绝不可复用**，复用会直接泄露明文异或值。
//         GCM 的安全性建立在 nonce 唯一之上，这一点比密钥长度更重要。
// aad   : 附加认证数据，参与认证但不加密（因此可以放明文元数据）。
//         不需要时传空指针 + 长度 0。
// plain : 明文，长度可为 0
// cipher: 输出，与 plain 等长（可为空，当 len == 0 时）
// tag   : 输出 16 字节认证标签
//
// cipher 与 plain 允许指向同一块内存（就地加密）。
void aes256GcmEncrypt(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], const std::uint8_t* aad,
                      std::size_t aadLen, const std::uint8_t* plain, std::size_t len, std::uint8_t* cipher,
                      std::uint8_t tag[kGcmTagSize]);

// 解密并校验 tag。
//
// 返回 true 表示 tag 匹配，out 内容有效；返回 false 表示密钥错、nonce 错、
// AAD 不匹配或数据被篡改——这四种情况**无法区分**，这是 GCM 的设计（认证
// 失败不透露失败原因，避免给攻击者提供区分依据）。
//
// 返回 false 时 out 的内容未定义（可能是部分解密的明文），调用方**不得**使用。
bool aes256GcmDecrypt(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], const std::uint8_t* aad,
                      std::size_t aadLen, const std::uint8_t* cipher, std::size_t len, const std::uint8_t tag[kGcmTagSize],
                      std::uint8_t* out);

// ---------------------------------------------------------------------------
// std::string / vector 便捷重载
// ---------------------------------------------------------------------------

// 加密并返回 "cipher || tag" 拼接结果。省得每个调用点都自己拼。
[[nodiscard]] std::vector<std::uint8_t> aes256GcmSeal(const std::uint8_t key[kAesKeySize],
                                                      const std::uint8_t nonce[kGcmNonceSize], std::string_view aad,
                                                      std::string_view plain);

// 对应解封。sealed 需要至少 kGcmTagSize 字节。
// 失败时返回 false，plain 保持为空。
[[nodiscard]] bool aes256GcmOpen(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize],
                                 std::string_view aad, const std::uint8_t* sealed, std::size_t sealedLen,
                                 std::vector<std::uint8_t>& plain);

}   // namespace relayer::crypto
