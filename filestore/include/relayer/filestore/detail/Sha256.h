#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

}  // namespace relayer::crypto
