#include <algorithm>
#include <cstring>

#include "relayer/core/auth/Handshake.h"

// 系统 CSPRNG。
//
// 三种平台各走一条路：
//   Windows  BCryptGenRandom（bcrypt.dll，Vista 起可用）
//   Linux    getrandom(2)（3.17 起），失败退 /dev/urandom
//   macOS    arc4random_buf（自带，无需种子）
//
// 为什么不用 std::random_device：标准把它的质量留给实现，某些 MinGW 版本
// 的 std::random_device 是 mt19937 加固定种子——输出完全可预测。握手用的
// salt 和 nonce 一旦可预测，攻击者可以预先算出 proof 表，重放防护也失去意义。

// 注意：这几个 include **有顺序依赖**，不能交给 clang-format 按字母序排。
// windows.h 必须先于 bcrypt.h——后者依赖前者定义的 LONG / ULONG / LPCWSTR
// 等基础类型，反过来 include 会得到一堆 "'LONG' does not name a type"。
// clang-format off
#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
// clang-format on

namespace relayer::core::auth
{

bool fillRandom(void *buf, std::size_t len) noexcept
{
    if (len == 0)
    {
        return true;
    }
    if (buf == nullptr)
    {
        return false;
    }

#if defined(_WIN32)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG：不需要打开算法句柄，直接取系统 RNG。
    // 它保证成功时一定填满 len 字节，返回值只有 STATUS_SUCCESS 一个成功码。
    const NTSTATUS st =
        ::BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st == 0;  // STATUS_SUCCESS
#elif defined(__APPLE__)
    ::arc4random_buf(buf, len);
    return true;
#else
    auto *p = static_cast<std::uint8_t *>(buf);
    std::size_t filled = 0;

    // getrandom 可能被信号打断，也可能在内核熵池尚未初始化时返回部分数据，
    // 所以必须循环直到填满。
    while (filled < len)
    {
        const ssize_t n = ::getrandom(p + filled, len - filled, 0);
        if (n > 0)
        {
            filled += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        break;  // ENOSYS 或其它错误：退到 /dev/urandom
    }
    if (filled == len)
    {
        return true;
    }

    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    while (filled < len)
    {
        const ssize_t n = ::read(fd, p + filled, len - filled);
        if (n > 0)
        {
            filled += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
#endif
}

bool randomSalt(Salt &out) noexcept
{
    return fillRandom(out.data(), out.size());
}

bool randomNonce(Nonce &out) noexcept
{
    return fillRandom(out.data(), out.size());
}

}  // namespace relayer::core::auth
