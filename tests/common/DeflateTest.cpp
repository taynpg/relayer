#include "relayer/common/compress/Deflate.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

using namespace relayer;

namespace {

// 可压缩的内容：重复文本。
std::string compressible(std::size_t len)
{
    std::string s;
    s.reserve(len);
    const std::string line = "2026-11-04 20:00:00 INFO  relayer: chunk sent ok\n";
    while (s.size() < len) {
        s += line;
    }
    s.resize(len);
    return s;
}

// 不可压缩的内容：伪随机字节（确定性，方便复现）。
std::string incompressible(std::size_t len)
{
    std::string s(len, '\0');
    std::uint32_t x = 0x12345678u;
    for (std::size_t i = 0; i < len; ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = static_cast<char>((x >> 24) & 0xFF);
    }
    return s;
}

}   // namespace

TEST_CASE("deflate round-trips compressible data", "[compress][deflate]")
{
    const auto plain = compressible(256 * 1024);

    const auto packed = compression::deflateBlock(plain.data(), plain.size());
    REQUIRE_FALSE(packed.empty());
    // 重复文本应当压得很小，这里只要求"明显更小"——具体比例是编排细节，
    // 钉死它会变成在测 miniz 的版本。
    REQUIRE(packed.size() < plain.size() / 4);

    std::vector<char> back;
    REQUIRE(compression::inflateBlock(packed.data(), packed.size(), plain.size(), back));
    REQUIRE(std::string(back.begin(), back.end()) == plain);
}

TEST_CASE("deflate declines incompressible data", "[compress][deflate]")
{
    // 压不动就返回空，让调用方按原样发。这条不是优化而是正确性的一部分：
    // 对已压缩的数据（zip/jpeg/xz）硬压只会变大。
    const auto plain = incompressible(64 * 1024);
    REQUIRE(compression::deflateBlock(plain.data(), plain.size()).empty());
}

TEST_CASE("deflate handles an empty input", "[compress][deflate]")
{
    REQUIRE(compression::deflateBlock(nullptr, 0).empty());
    REQUIRE(compression::deflateBlock("", 0).empty());

    std::vector<char> out;
    REQUIRE_FALSE(compression::inflateBlock(nullptr, 0, 16, out));
    REQUIRE_FALSE(compression::inflateBlock("x", 1, 0, out));
}

TEST_CASE("inflate refuses to expand beyond the expected length", "[compress][deflate]")
{
    // 压缩炸弹的全部危害在"小输入解出大输出"。接收方知道每块**应该**多长
    // （块号 → 长度是确定的），容量写死之后这里就只是一条长度校验。
    const std::string zeros(1 << 20, '\0');   // 1 MiB 全零，压完几百字节
    const auto packed = compression::deflateBlock(zeros.data(), zeros.size());
    REQUIRE_FALSE(packed.empty());
    REQUIRE(packed.size() < 4096);

    std::vector<char> out;
    // 上游说这是 16 字节的块：解出来是 1 MiB，必须拒绝而不是"尽力而为"。
    REQUIRE_FALSE(compression::inflateBlock(packed.data(), packed.size(), 16, out));
    REQUIRE(out.empty());

    // 反向也一样：上游说这是 2 MiB 的块，实际只有 1 MiB，同样拒绝。
    REQUIRE_FALSE(compression::inflateBlock(packed.data(), packed.size(), 2u << 20, out));

    // 长度给对了才通过
    REQUIRE(compression::inflateBlock(packed.data(), packed.size(), zeros.size(), out));
    REQUIRE(out.size() == zeros.size());
}

TEST_CASE("inflate rejects corrupted or truncated streams", "[compress][deflate]")
{
    const auto plain = compressible(4096);
    auto packed = compression::deflateBlock(plain.data(), plain.size());
    REQUIRE_FALSE(packed.empty());

    std::vector<char> out;

    // 截断：zlib 尾部带 adler32，砍掉一半必然失败。
    REQUIRE_FALSE(compression::inflateBlock(packed.data(), packed.size() / 2, plain.size(), out));

    // 翻一位：要么 adler 不对，要么解出来的长度不对，两种都要失败。
    packed[packed.size() / 2] = static_cast<char>(packed[packed.size() / 2] ^ 0x01);
    REQUIRE_FALSE(compression::inflateBlock(packed.data(), packed.size(), plain.size(), out));
}
