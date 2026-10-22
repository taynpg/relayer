#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "relayer/common/crypto/Aes256Gcm.h"
#include "relayer/common/crypto/Sha256.h"

using namespace relayer::crypto;

namespace
{

using Bytes = std::vector<std::uint8_t>;

// "hex string" -> bytes。测试向量都是十六进制写的，手工敲 uint8_t 数组
// 容易错位且没法读，所以统一在这里解。
Bytes fromHex(std::string_view hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);

    const auto nibble = [](char c) -> std::uint8_t
    {
        if (c >= '0' && c <= '9')
        {
            return static_cast<std::uint8_t>(c - '0');
        }
        if (c >= 'a' && c <= 'f')
        {
            return static_cast<std::uint8_t>(c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F')
        {
            return static_cast<std::uint8_t>(c - 'A' + 10);
        }
        return 0xFF;
    };

    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        const std::uint8_t hi = nibble(hex[i]);
        const std::uint8_t lo = nibble(hex[i + 1]);
        REQUIRE(hi != 0xFF);
        REQUIRE(lo != 0xFF);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string toHex(const std::uint8_t *data, std::size_t len)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i)
    {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 0x0F]);
    }
    return s;
}

std::string toHex(const Bytes &b)
{
    return toHex(b.data(), b.size());
}

std::string toHex(const Sha256::Digest &d)
{
    return toHex(d.data(), d.size());
}

}  // namespace

// ===========================================================================
// HMAC-SHA256 —— RFC 4231 附录里的官方测试向量
// ===========================================================================

TEST_CASE("HMAC-SHA256 RFC 4231 测试向量", "[crypto][hmac][vector]")
{
    SECTION("TC1: 20 字节密钥，数据 Hi There")
    {
        const Bytes key(20, 0x0b);
        const std::string data = "Hi There";
        REQUIRE(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
                "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }

    SECTION("TC2: 密钥 Jefe，数据 what do ya want for nothing?")
    {
        const std::string key = "Jefe";
        const std::string data = "what do ya want for nothing?";
        REQUIRE(toHex(hmacSha256(key, data)) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }

    SECTION("TC3: 20 字节 0xaa 密钥，50 字节 0xdd 数据")
    {
        const Bytes key(20, 0xaa);
        const Bytes data(50, 0xdd);
        REQUIRE(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
                "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    }

    SECTION("TC4: 25 字节序列密钥")
    {
        const Bytes key = fromHex("0102030405060708090a0b0c0d0e0f10111213141516171819");
        const Bytes data(50, 0xcd);
        REQUIRE(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
                "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    }

    SECTION("TC6: 131 字节超块长密钥（需要先哈希）")
    {
        const Bytes key(131, 0xaa);
        const std::string data = "Test Using Larger Than Block-Size Key - Hash Key First";
        REQUIRE(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
                "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }

    SECTION("TC7: 131 字节密钥 + 152 字节数据")
    {
        const Bytes key(131, 0xaa);
        const std::string data = "This is a test using a larger than block-size key and a larger "
                                 "than block-size data. The key needs to be hashed before being "
                                 "used by the HMAC algorithm.";
        REQUIRE(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
                "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    }

    SECTION("空密钥与空数据")
    {
        // 没有 RFC 向量，但与常见的空输入行为一致：
        // HMAC = SHA256(opad || SHA256(ipad))，密钥按 64 字节全零填充
        const Sha256::Digest got = hmacSha256(nullptr, 0, nullptr, 0);
        // 用 openssl dgst -sha256 -hmac "" 得到
        REQUIRE(toHex(got) == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    }
}

// ===========================================================================
// PBKDF2-HMAC-SHA256
// ===========================================================================

TEST_CASE("PBKDF2-HMAC-SHA256 与公开参考值一致", "[crypto][pbkdf2][vector]")
{
    // 参考值可以用 Python 一行复算：
    //   hashlib.pbkdf2_hmac('sha256', b'password', b'salt', iterations, 32).hex()
    // 这些向量在 Python 3、OpenSSL 与 RustCrypto 上都验证过。

    SECTION("iterations=1")
    {
        const Bytes salt = fromHex("73616c74");  // "salt"
        const auto got = pbkdf2HmacSha256("password", salt.data(), salt.size(), 1);
        REQUIRE(toHex(got) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    }

    SECTION("iterations=2")
    {
        const Bytes salt = fromHex("73616c74");
        const auto got = pbkdf2HmacSha256("password", salt.data(), salt.size(), 2);
        REQUIRE(toHex(got) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    }

    SECTION("iterations=4096")
    {
        const Bytes salt = fromHex("73616c74");
        const auto got = pbkdf2HmacSha256("password", salt.data(), salt.size(), 4096);
        REQUIRE(toHex(got) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    }

    SECTION("iterations=20000（协议默认值）")
    {
        const Bytes salt = fromHex("73616c74");
        const auto got = pbkdf2HmacSha256("password", salt.data(), salt.size(), 20000);
        REQUIRE(toHex(got) == "2a6a4f0832d046838f4a22cbadc94dff08a5bcc328467ee3cade8db8c5d93b87");
    }

    SECTION("空 salt")
    {
        const auto got = pbkdf2HmacSha256("password", nullptr, 0, 1);
        REQUIRE(toHex(got) == "c1232f10f62715fda06ae7c0a2037ca19b33cf103b727ba56d870c11f290a2ab");
    }
}

TEST_CASE("PBKDF2 迭代次数为 0 时返回全零而非崩溃", "[crypto][pbkdf2][edge]")
{
    // 约定见头文件：0 是调用方的 bug，但不能因此崩掉或返回未初始化内存。
    const Bytes salt = fromHex("73616c74");
    const auto got = pbkdf2HmacSha256("password", salt.data(), salt.size(), 0);
    REQUIRE(got == Sha256::Digest{});
}

TEST_CASE("PBKDF2 对 salt 与口令敏感", "[crypto][pbkdf2][property]")
{
    const Bytes salt1 = fromHex("00000000000000000000000000000001");
    const Bytes salt2 = fromHex("00000000000000000000000000000002");

    const auto k1 = pbkdf2HmacSha256("password", salt1.data(), salt1.size(), 100);
    const auto k2 = pbkdf2HmacSha256("password", salt2.data(), salt2.size(), 100);
    const auto k3 = pbkdf2HmacSha256("Password", salt1.data(), salt1.size(), 100);

    REQUIRE(k1 != k2);  // salt 变一位，密钥必须完全不同
    REQUIRE(k1 != k3);  // 口令大小写变一位，同上
}

// ===========================================================================
// 常量时间比较
// ===========================================================================

TEST_CASE("constantTimeEquals 语义", "[crypto][compare]")
{
    const std::uint8_t a[4] = {1, 2, 3, 4};
    const std::uint8_t same[4] = {1, 2, 3, 4};
    const std::uint8_t diffAtFirst[4] = {9, 2, 3, 4};
    const std::uint8_t diffAtLast[4] = {1, 2, 3, 9};

    REQUIRE(constantTimeEquals(a, same, 4));
    REQUIRE_FALSE(constantTimeEquals(a, diffAtFirst, 4));
    REQUIRE_FALSE(constantTimeEquals(a, diffAtLast, 4));

    // 长度为 0 视为相等（空序列没有可比的字节）
    REQUIRE(constantTimeEquals(a, diffAtFirst, 0));

    // 空指针：两个都空才算相等
    REQUIRE(constantTimeEquals(nullptr, nullptr, 4));
    REQUIRE_FALSE(constantTimeEquals(a, nullptr, 4));
}

// ===========================================================================
// AES-256-GCM —— McGrew & Viega 论文附录 B 与 NIST 向量
// ===========================================================================

TEST_CASE("AES-256-GCM 空明文", "[crypto][gcm][vector]")
{
    // 只有 tag 的用例，用来验证 H 与 J0 的推导路径
    const Bytes key(32, 0x00);
    const Bytes nonce(12, 0x00);

    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, nullptr, 0, nullptr, tag);

    REQUIRE(toHex(tag, 16) == "530f8afbc74536b9a963b4f1c4cb738b");
}

TEST_CASE("AES-256-GCM 单块明文", "[crypto][gcm][vector]")
{
    const Bytes key(32, 0x00);
    const Bytes nonce(12, 0x00);
    const Bytes plain(16, 0x00);

    std::uint8_t cipher[16] = {};
    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, plain.data(), plain.size(), cipher, tag);

    REQUIRE(toHex(cipher, 16) == "cea7403d4d606b6e074ec5d3baf39d18");
    REQUIRE(toHex(tag, 16) == "d0d1c8a799996bf0265b98b5d48ab919");
}

TEST_CASE("AES-256-GCM 多块明文", "[crypto][gcm][vector]")
{
    // McGrew & Viega 论文附录 B 的 AES-256 用例（60 字节明文，无 AAD）。
    // 明文是 0x00..0x3b 递增序列，覆盖 CTR 计数器进位与跨块 GHASH。
    const Bytes key = fromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const Bytes nonce = fromHex("cafebabefacedbaddecaf888");
    const Bytes plain = fromHex("d9313225f88406e5a55909c5aff5269a"
                                "86a7a9531534f7da2e4c303d8a318a72"
                                "1c3c0c95956809532fcf0e2449a6b525"
                                "b16aedf5aa0de657ba637b39");

    REQUIRE(plain.size() == 60);

    Bytes cipher(plain.size());
    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, plain.data(), plain.size(), cipher.data(), tag);

    REQUIRE(toHex(cipher) == "522dc1f099567d07f47f37a32a84427d"
                             "643a8cdcbfe5c0c97598a2bd2555d1aa"
                             "8cb08e48590dbb3da7b08b1056828838"
                             "c5f61e6393ba7a0abcc9f662");
    REQUIRE(toHex(tag, 16) == "eb9f796c8d356fc31a8433884b696f4f");

    // 解密回去
    Bytes out(plain.size());
    REQUIRE(aes256GcmDecrypt(key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag, out.data()));
    REQUIRE(out == plain);
}

TEST_CASE("AES-256-GCM 带 AAD", "[crypto][gcm][vector][aad]")
{
    // 同一组 McGrew 用例加上 20 字节 AAD。密文不变，只有 tag 变——
    // 这条断言正是" AAD 参与认证但不参与加密"的定义。
    const Bytes key = fromHex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const Bytes nonce = fromHex("cafebabefacedbaddecaf888");
    const Bytes aad = fromHex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const Bytes plain = fromHex("d9313225f88406e5a55909c5aff5269a"
                                "86a7a9531534f7da2e4c303d8a318a72"
                                "1c3c0c95956809532fcf0e2449a6b525"
                                "b16aedf5aa0de657ba637b39");

    Bytes cipher(plain.size());
    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), aad.data(), aad.size(), plain.data(), plain.size(), cipher.data(), tag);

    // 密文与不带 AAD 的用例完全相同（AAD 只影响 tag）
    REQUIRE(toHex(cipher) == "522dc1f099567d07f47f37a32a84427d"
                             "643a8cdcbfe5c0c97598a2bd2555d1aa"
                             "8cb08e48590dbb3da7b08b1056828838"
                             "c5f61e6393ba7a0abcc9f662");
    REQUIRE(toHex(tag, 16) == "76fc6ece0f4e1768cddf8853bb2d551b");

    // 解密必须同样带上 AAD 才能通过
    Bytes out(plain.size());
    REQUIRE(aes256GcmDecrypt(key.data(), nonce.data(), aad.data(), aad.size(), cipher.data(), cipher.size(), tag,
                             out.data()));
    REQUIRE(out == plain);

    // AAD 改一位 -> 认证失败
    const Bytes wrongAad = fromHex("feedfacedeadbeeffeedfacedeadbeefabaddad3");
    REQUIRE_FALSE(aes256GcmDecrypt(key.data(), nonce.data(), wrongAad.data(), wrongAad.size(), cipher.data(),
                                   cipher.size(), tag, out.data()));

    // 完全不给 AAD 也应当失败（不能"省略 AAD 就跳过认证"）
    REQUIRE_FALSE(
        aes256GcmDecrypt(key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag, out.data()));
}

TEST_CASE("AES-256-GCM 往返：各种长度", "[crypto][gcm][roundtrip]")
{
    const Bytes key = fromHex("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210");
    const Bytes nonce = fromHex("0f0e0d0c0b0a090807060504");

    // 覆盖 0、1、15、16、17、31、32 这些围绕块边界的长度
    for (const std::size_t len : {std::size_t(0), std::size_t(1), std::size_t(15), std::size_t(16), std::size_t(17),
                                  std::size_t(31), std::size_t(32), std::size_t(1000), std::size_t(65536)})
    {
        Bytes plain(len);
        for (std::size_t i = 0; i < len; ++i)
        {
            plain[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
        }

        Bytes cipher(len);
        std::uint8_t tag[16] = {};
        aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, plain.data(), plain.size(), cipher.data(), tag);

        Bytes out(len);
        REQUIRE(aes256GcmDecrypt(key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag, out.data()));
        REQUIRE(out == plain);
    }
}

TEST_CASE("AES-256-GCM 拒绝被篡改的数据", "[crypto][gcm][tamper]")
{
    const Bytes key = fromHex("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210");
    const Bytes nonce = fromHex("0f0e0d0c0b0a090807060504");
    const Bytes plain = fromHex("4142434445464748494a4b4c4d4e4f50");

    Bytes cipher(plain.size());
    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, plain.data(), plain.size(), cipher.data(), tag);

    // 1. 改密文任意一位
    {
        Bytes bad = cipher;
        bad[3] ^= 0x01;
        Bytes out(plain.size());
        REQUIRE_FALSE(aes256GcmDecrypt(key.data(), nonce.data(), nullptr, 0, bad.data(), bad.size(), tag, out.data()));
    }

    // 2. 改 tag 任意一位
    {
        std::uint8_t badTag[16];
        std::copy(std::begin(tag), std::end(tag), std::begin(badTag));
        badTag[15] ^= 0x80;
        Bytes out(plain.size());
        REQUIRE_FALSE(
            aes256GcmDecrypt(key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), badTag, out.data()));
    }

    // 3. 换 nonce
    {
        const Bytes badNonce = fromHex("0f0e0d0c0b0a090807060505");
        Bytes out(plain.size());
        REQUIRE_FALSE(
            aes256GcmDecrypt(key.data(), badNonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag, out.data()));
    }

    // 4. 换密钥
    {
        const Bytes badKey = fromHex("1123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210");
        Bytes out(plain.size());
        REQUIRE_FALSE(
            aes256GcmDecrypt(badKey.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag, out.data()));
    }
}

TEST_CASE("AES-256-GCM seal/open 便捷接口", "[crypto][gcm][roundtrip]")
{
    const Bytes key = fromHex("0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210");
    const Bytes nonce = fromHex("0f0e0d0c0b0a090807060504");

    const std::string aad = "relayer/auth/v1";
    const std::string plain = "the quick brown fox jumps over the lazy dog";

    const Bytes sealed = aes256GcmSeal(key.data(), nonce.data(), aad, plain);
    REQUIRE(sealed.size() == plain.size() + kGcmTagSize);

    std::vector<std::uint8_t> opened;
    REQUIRE(aes256GcmOpen(key.data(), nonce.data(), aad, sealed.data(), sealed.size(), opened));
    REQUIRE(std::string(opened.begin(), opened.end()) == plain);

    // AAD 不同 -> 失败，且 opened 被清空（不把半截明文交给调用方）
    std::vector<std::uint8_t> dummy = {1, 2, 3};
    REQUIRE_FALSE(aes256GcmOpen(key.data(), nonce.data(), "relayer/auth/v2", sealed.data(), sealed.size(), dummy));
    REQUIRE(dummy.empty());
}

TEST_CASE("AES-256-GCM open 拒绝过短的输入", "[crypto][gcm][edge]")
{
    const Bytes key(32, 0);
    const Bytes nonce(12, 0);

    std::vector<std::uint8_t> out;
    // 连 16 字节 tag 都不够
    const std::uint8_t shortInput[8] = {};
    REQUIRE_FALSE(aes256GcmOpen(key.data(), nonce.data(), "", shortInput, sizeof(shortInput), out));
    REQUIRE(out.empty());

    // 空指针
    REQUIRE_FALSE(aes256GcmOpen(key.data(), nonce.data(), "", nullptr, 0, out));

    // 恰好 16 字节 = 空明文 + tag，AAD 为空时应当成功
    std::uint8_t tag[16] = {};
    aes256GcmEncrypt(key.data(), nonce.data(), nullptr, 0, nullptr, 0, nullptr, tag);
    REQUIRE(aes256GcmOpen(key.data(), nonce.data(), "", tag, sizeof(tag), out));
    REQUIRE(out.empty());
}
