#include "relayer/common/crypto/Sha256.h"

#include <algorithm>
#include <string>

#include <catch2/catch_test_macros.hpp>

using relayer::crypto::Sha256;

namespace
{

std::string hexOf(const std::string &s)
{
    return Sha256::hex(Sha256::hash(s.data(), s.size()));
}

}  // namespace

// 参考值取自 FIPS 180-4 / NIST 测试向量。
// 这些用例的意义在于：一旦有人"优化"实现把某处常量改错，
// 单靠自洽性测试（两次算同一份数据相等）是发现不了的。
TEST_CASE("SHA-256 标准测试向量", "[sha256][vector]")
{
    REQUIRE(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 分块 update 与一次性结果一致", "[sha256][streaming]")
{
    std::string data;
    for (int i = 0; i < 100000; ++i)
    {
        data.push_back(static_cast<char>(i & 0xFF));
    }
    const std::string oneShot = Sha256::hex(Sha256::hash(data.data(), data.size()));

    // 以不规则块长喂入，确保跨块边界的状态转移正确
    for (const std::size_t chunk :
         {std::size_t(1), std::size_t(7), std::size_t(63), std::size_t(64), std::size_t(65), std::size_t(4096)})
    {
        Sha256 h;
        std::size_t off = 0;
        while (off < data.size())
        {
            const std::size_t n = std::min(chunk, data.size() - off);
            h.update(data.data() + off, n);
            off += n;
        }
        REQUIRE(Sha256::hex(h.digest()) == oneShot);
    }
}

TEST_CASE("SHA-256 一百万个 a", "[sha256][vector][slow]")
{
    std::string million(1000000, 'a');
    REQUIRE(hexOf(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
