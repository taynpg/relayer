#pragma once

#include <cstddef>

// 平台 CSPRNG。
//
// 为什么放在 common：它是**纯平台原语**——不认协议、不认认证，任何层都可能
// 需要（握手要 salt/nonce，载荷加密要 GCM nonce）。留在 core/auth 里会让
// core/protocol 为了取 12 字节随机数反向依赖 auth。
//
// 为什么不用 std::random_device：标准把质量留给实现，某些 MinGW 版本的
// std::random_device 是 mt19937 加固定种子——输出完全可预测。可预测的 GCM
// nonce 会直接毁掉 GCM 的保密性（同一密钥下 nonce 复用等于泄露明文异或），
// 可预测的握手 nonce 会让重放防护失效。

namespace relayer::crypto {

// 用系统 CSPRNG 填充。
//
// 返回 false（几乎只在极端环境下发生：没有 /dev/urandom、BCryptGenRandom
// 不可用），调用方**不要**退化成 rand()：那会让"加密/认证看起来成功了"，
// 但实际已经没有安全性。宁可失败。
[[nodiscard]] bool fillRandom(void* buf, std::size_t len) noexcept;

}   // namespace relayer::crypto
