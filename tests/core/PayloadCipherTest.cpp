#include "relayer/core/protocol/PayloadCipher.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "relayer/core/protocol/Frame.h"

using namespace relayer;
using namespace relayer::protocol;

namespace {

Frame makeFrame(std::string body = "hello payload")
{
    Frame f;
    f.type = FrameType::kFileRequestSend;
    f.sessionId = 42;
    f.index = 7;
    f.from = "tx";
    f.to = "rx";
    f.fuuid = "0123456789abcdef";
    f.body.assign(body.begin(), body.end());
    return f;
}

std::array<std::uint8_t, PayloadCipher::kKeySize> keyOf(std::uint8_t seed)
{
    std::array<std::uint8_t, PayloadCipher::kKeySize> k{};
    for (std::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::uint8_t>(seed + i);
    }
    return k;
}

std::string bodyOf(const Frame& f)
{
    return std::string(f.body.begin(), f.body.end());
}

}   // namespace

TEST_CASE("payload cipher round-trips a frame", "[protocol][cipher]")
{
    const PayloadCipher cipher(keyOf(1));
    const auto in = makeFrame();

    Frame sealed;
    REQUIRE(cipher.seal(in, sealed));
    REQUIRE(sealed.isEncrypted());
    // nonce + ciphertext + tag，密文与明文等长
    REQUIRE(sealed.body.size() == PayloadCipher::kNonceSize + in.body.size() + PayloadCipher::kTagSize);
    // 密文里不该出现明文——seal 至少要真的加密了。
    REQUIRE(bodyOf(sealed).find("hello payload") == std::string::npos);

    Frame opened;
    REQUIRE(cipher.open(sealed, opened));
    REQUIRE(opened.type == in.type);
    REQUIRE(opened.sessionId == in.sessionId);
    REQUIRE(opened.index == in.index);
    REQUIRE(opened.from == in.from);
    REQUIRE(opened.to == in.to);
    REQUIRE(opened.fuuid == in.fuuid);
    REQUIRE(bodyOf(opened) == "hello payload");
    // 交给上层的是明文帧：加密位必须已经清掉
    REQUIRE_FALSE(opened.isEncrypted());
}

TEST_CASE("payload cipher handles an empty body", "[protocol][cipher]")
{
    // 空 body 也要走一遍：kAnswerId 之类的帧可能真的是空的，而 GCM 对
    // 空明文是合法的（密文 0 字节 + tag 16 字节），不能当边界情况跳过。
    const PayloadCipher cipher(keyOf(3));
    const auto in = makeFrame("");

    Frame sealed;
    REQUIRE(cipher.seal(in, sealed));
    REQUIRE(sealed.body.size() == PayloadCipher::kNonceSize + PayloadCipher::kTagSize);

    Frame opened;
    REQUIRE(cipher.open(sealed, opened));
    REQUIRE(opened.body.empty());
}

TEST_CASE("payload cipher never reuses a nonce", "[protocol][cipher]")
{
    // 同一帧连着加密两次，nonce 必须不同。相同就意味着同一密钥下 nonce
    // 复用——GCM 在那一刻起就不再保密了，而这一点从外部完全看不出来。
    const PayloadCipher cipher(keyOf(5));
    const auto in = makeFrame();

    Frame a;
    Frame b;
    REQUIRE(cipher.seal(in, a));
    REQUIRE(cipher.seal(in, b));

    const auto nonceA = bodyOf(a).substr(0, PayloadCipher::kNonceSize);
    const auto nonceB = bodyOf(b).substr(0, PayloadCipher::kNonceSize);
    REQUIRE(nonceA != nonceB);
    REQUIRE(bodyOf(a) != bodyOf(b));
}

TEST_CASE("payload cipher rejects a wrong key", "[protocol][cipher]")
{
    const PayloadCipher alice(keyOf(1));
    const PayloadCipher bob(keyOf(2));   // 口令不一致

    Frame sealed;
    REQUIRE(alice.seal(makeFrame(), sealed));

    Frame opened;
    REQUIRE_FALSE(bob.open(sealed, opened));
}

TEST_CASE("payload cipher rejects a tampered body", "[protocol][cipher]")
{
    const PayloadCipher cipher(keyOf(9));
    Frame sealed;
    REQUIRE(cipher.seal(makeFrame(), sealed));

    // 动密文的最后一个字节（tag 区）。GCM 的 tag 校验必须挡下来。
    REQUIRE_FALSE(sealed.body.empty());
    sealed.body.back() = static_cast<char>(sealed.body.back() ^ 0x01);

    Frame opened;
    REQUIRE_FALSE(cipher.open(sealed, opened));
}

TEST_CASE("payload cipher authenticates the fixed header", "[protocol][cipher]")
{
    const PayloadCipher cipher(keyOf(11));
    Frame sealed;
    REQUIRE(cipher.seal(makeFrame(), sealed));

    // AAD 覆盖 type：把 kAskDelete 改成 kAskFileList 必须验不过。
    // 这是 §6.1 举的那个例子的可执行版本。
    Frame retyped = sealed;
    retyped.type = FrameType::kAskFileList;

    Frame opened;
    REQUIRE_FALSE(cipher.open(retyped, opened));
}

TEST_CASE("payload aad is canonical over the crc flag", "[protocol][cipher]")
{
    // AAD 里的 flags 必须是**规范形式**：加密位置上、CRC 位清掉。
    // 否则发送方（还不知道最终 body 多长）与接收方（CRC 位已定）算出来的
    // AAD 不一致，所有帧都会解不开。
    Frame a = makeFrame();
    a.flags = kFlagNone;
    Frame b = makeFrame();
    b.flags = kFlagBodyCrc;   // 只有 CRC 位不同

    std::uint8_t aadA[PayloadCipher::kAadSize]{};
    std::uint8_t aadB[PayloadCipher::kAadSize]{};
    PayloadCipher::aadOf(a, aadA);
    PayloadCipher::aadOf(b, aadB);
    REQUIRE(std::memcmp(aadA, aadB, sizeof(aadA)) == 0);

    // 加密位也不参与区分：发送方算 AAD 时可能已经置位，接收方拿到的一定置位。
    Frame c = makeFrame();
    c.flags = kFlagEncrypted;
    std::uint8_t aadC[PayloadCipher::kAadSize]{};
    PayloadCipher::aadOf(c, aadC);
    REQUIRE(std::memcmp(aadA, aadC, sizeof(aadA)) == 0);

    // 但 type 必须参与：改类型是应该被发现的篡改。
    Frame d = makeFrame();
    d.type = FrameType::kAskFileList;
    std::uint8_t aadD[PayloadCipher::kAadSize]{};
    PayloadCipher::aadOf(d, aadD);
    REQUIRE(std::memcmp(aadA, aadD, sizeof(aadA)) != 0);
}

TEST_CASE("payload cipher survives pack and unpack", "[protocol][cipher]")
{
    // 真实路径：加密 → 打包 → 解包 → 解密。打包会把 flags 里的 CRC 位
    // 按 body 长度重算，这一层不能把 §6 的约定弄坏。
    const PayloadCipher cipher(keyOf(13));
    Frame sealed;
    REQUIRE(cipher.seal(makeFrame("0123456789"), sealed));

    std::vector<char> wire;
    REQUIRE(packFrameChecked(sealed, wire).ok());

    Frame parsed;
    std::size_t consumed = 0;
    REQUIRE(unpackFrame(wire.data(), wire.size(), parsed, consumed) == UnPackResult::kOk);
    REQUIRE(consumed == wire.size());
    REQUIRE(parsed.isEncrypted());

    Frame opened;
    REQUIRE(cipher.open(parsed, opened));
    REQUIRE(bodyOf(opened) == "0123456789");
}

TEST_CASE("payload cipher derives a stable key from the same passphrase", "[protocol][cipher]")
{
    const auto a = PayloadCipher::fromPassphrase("correct horse battery staple");
    const auto b = PayloadCipher::fromPassphrase("correct horse battery staple");
    const auto c = PayloadCipher::fromPassphrase("correct horse battery staplf");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    REQUIRE(a->key() == b->key());
    REQUIRE(a->key() != c->key());

    // 空口令 = 关闭加密，不是"用空密钥"
    REQUIRE(PayloadCipher::fromPassphrase("") == nullptr);

    // 派生出来的两端能互通，换个口令就不行
    Frame sealed;
    REQUIRE(a->seal(makeFrame(), sealed));
    Frame opened;
    REQUIRE(b->open(sealed, opened));
    REQUIRE_FALSE(c->open(sealed, opened));
}
