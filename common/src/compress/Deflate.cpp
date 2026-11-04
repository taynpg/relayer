#include "relayer/common/compress/Deflate.h"

#include <algorithm>
#include <limits>

#include "miniz.h"

namespace relayer::compression {

namespace {

std::size_t boundOf(std::size_t len)
{
    // mz_compressBound 是 uLong 的接口。超大输入直接拒绝：单块上限由协议
    // 钉死在 4 MiB（PROTOCOL §9.4），这里只是不想让调用方传进一个
    // size_t 溢出的值。
    constexpr std::size_t kMax = 64u * 1024 * 1024;
    return std::min(len, kMax);
}

}   // namespace

std::vector<char> deflateBlock(const char* data, std::size_t len, int level)
{
    if (data == nullptr || len == 0) {
        return {};
    }
    const auto in = boundOf(len);
    if (in != len) {
        return {};   // 超过单块上限：按原样发
    }

    mz_ulong destLen = mz_compressBound(static_cast<mz_ulong>(len));
    std::vector<char> out(destLen);

    const int rc = mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &destLen,
                                reinterpret_cast<const unsigned char*>(data), static_cast<mz_ulong>(len), level);
    if (rc != MZ_OK) {
        return {};   // 压缩失败不是错误：按原样发就是了
    }

    // 没省到 kWorthItRatio 就不值得：多一次解压、多一层"这块是压缩的"
    // 分支，换几字节回来没意义。
    if (static_cast<double>(destLen) > static_cast<double>(len) * kWorthItRatio) {
        return {};
    }

    out.resize(destLen);
    return out;
}

bool inflateBlock(const char* data, std::size_t len, std::size_t expected, std::vector<char>& out)
{
    if (data == nullptr || len == 0 || expected == 0) {
        return false;
    }

    out.assign(expected, '\0');
    mz_ulong destLen = static_cast<mz_ulong>(expected);

    const int rc = mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &destLen,
                                 reinterpret_cast<const unsigned char*>(data), static_cast<mz_ulong>(len));

    // 只认"恰好解出 expected 字节"：
    //   - MZ_BUF_ERROR：输出装不下（压缩炸弹，或 expected 给错了）；
    //   - 解出来比 expected 短：流被截断，或者对方算错了长度。
    // 两种都不该被当成"差不多能用"——后面的内容校验会因为长度不符而失败，
    // 但那时已经写坏了一块，不如当场拒绝。
    if (rc != MZ_OK || destLen != expected) {
        out.clear();
        return false;
    }
    return true;
}

}   // namespace relayer::compression
