#include "relayer/core/protocol/PayloadCipher.h"

#include <cstring>
#include <string>
#include <vector>

#include "relayer/common/crypto/Random.h"
#include "relayer/common/crypto/Sha256.h"

namespace relayer::protocol {

namespace {

// 口令派生用的盐。
//
// 固定值，不是"忘了加随机盐"：两端没有机会在加密**之前**交换盐（交换本身
// 就得先有一条可信信道，而中继不是），所以只能固定。代价是攻击者可以
// 预计算口令字典，这也是为什么说"口令强度就是全部安全性"。
//
// 换个字符串等于换协议：改了它，老版本判定的密钥全变，两端会互相解不开。
constexpr std::string_view kPayloadSalt = "relayer/payload/v1";

// 与握手一致（auth::kDefaultIterations）。分开写而不是共用一个常量：
// 这两处将来可能各自调整，共用一个会让"改握手强度"意外地改掉加密强度。
constexpr std::uint32_t kPayloadIterations = 20000;

void appendU16(std::vector<char>& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void appendU64(std::vector<char>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

}   // namespace

PayloadCipher::PayloadCipher(const std::array<std::uint8_t, kKeySize>& key) : key_(key)
{
}

std::shared_ptr<const PayloadCipher> PayloadCipher::fromPassphrase(std::string_view passphrase)
{
    if (passphrase.empty()) {
        return nullptr;
    }
    const auto key = crypto::pbkdf2HmacSha256(passphrase, reinterpret_cast<const std::uint8_t*>(kPayloadSalt.data()),
                                              kPayloadSalt.size(), kPayloadIterations);
    return std::make_shared<const PayloadCipher>(key);
}

void PayloadCipher::aadOf(const Frame& f, std::uint8_t out[kAadSize])
{
    // 逐字节按线格式写，而不是"把 Frame 结构体内存拷一段"：结构体布局随
    // 编译器和字段增删变化，那样写出来的 AAD 在另一台机器上就对不上了。
    const auto flags = static_cast<std::uint16_t>((f.flags | kFlagEncrypted) & ~static_cast<std::uint16_t>(kFlagBodyCrc));

    std::vector<char> buf;
    buf.reserve(kAadSize);
    buf.push_back(static_cast<char>(frame::kMagic & 0xFF));
    buf.push_back(static_cast<char>((frame::kMagic >> 8) & 0xFF));
    buf.push_back(static_cast<char>((frame::kMagic >> 16) & 0xFF));
    buf.push_back(static_cast<char>((frame::kMagic >> 24) & 0xFF));
    appendU16(buf, frame::kVersion);
    appendU16(buf, flags);
    appendU16(buf, static_cast<std::uint16_t>(f.type));
    appendU16(buf, 0);   // reserved，与打包时一致，恒为 0
    appendU64(buf, static_cast<std::uint64_t>(f.sessionId));
    appendU64(buf, static_cast<std::uint64_t>(f.index));

    std::memcpy(out, buf.data(), kAadSize);
}

bool PayloadCipher::seal(const Frame& in, Frame& out) const
{
    std::uint8_t nonce[kNonceSize]{};
    if (!crypto::fillRandom(nonce, sizeof(nonce))) {
        // 拿不到系统随机数就不加密。**不要**退化成计数器或时间戳：
        // 可预测的 nonce 会让 GCM 的保密性直接失效，而调用方看不出来。
        return false;
    }

    // AAD 用"规范 flags"算，先造一个标志位正确的临时帧。
    out.type = in.type;
    out.flags = static_cast<std::uint16_t>(in.flags | kFlagEncrypted);
    out.sessionId = in.sessionId;
    out.index = in.index;
    out.from = in.from;
    out.to = in.to;
    out.fuuid = in.fuuid;

    std::uint8_t aad[kAadSize]{};
    aadOf(out, aad);

    const std::string_view aadView(reinterpret_cast<const char*>(aad), sizeof(aad));
    const std::string_view plain(in.body.data(), in.body.size());
    const auto sealed = crypto::aes256GcmSeal(key_.data(), nonce, aadView, plain);

    out.body.clear();
    out.body.reserve(kNonceSize + sealed.size());
    for (const auto b : nonce) {
        out.body.push_back(static_cast<char>(b));
    }
    out.body.insert(out.body.end(), reinterpret_cast<const char*>(sealed.data()),
                    reinterpret_cast<const char*>(sealed.data()) + sealed.size());
    return true;
}

bool PayloadCipher::open(const Frame& in, Frame& out) const
{
    // 至少要有 nonce + tag。少于此长度说明对端根本没按 §6 打包——当作
    // 解不开处理，不要试图"猜一下没加密"然后放行。
    if (in.body.size() < kNonceSize + kTagSize) {
        return false;
    }

    std::uint8_t aad[kAadSize]{};
    aadOf(in, aad);

    const auto* raw = reinterpret_cast<const std::uint8_t*>(in.body.data());
    const std::string_view aadView(reinterpret_cast<const char*>(aad), sizeof(aad));

    std::vector<std::uint8_t> plain;
    const bool ok = crypto::aes256GcmOpen(key_.data(), raw, aadView, raw + kNonceSize, in.body.size() - kNonceSize, plain);
    if (!ok) {
        return false;
    }

    out.type = in.type;
    // 交给上层的是一帧**明文**：加密位已经完成使命，留着会让上层以为
    // body 还是密文（比如再解一次、或者把它原样转发出去）。
    out.flags = static_cast<std::uint16_t>(in.flags & ~static_cast<std::uint16_t>(kFlagEncrypted) &
                                           ~static_cast<std::uint16_t>(kFlagBodyCrc));
    out.sessionId = in.sessionId;
    out.index = in.index;
    out.from = in.from;
    out.to = in.to;
    out.fuuid = in.fuuid;
    out.body.assign(reinterpret_cast<const char*>(plain.data()), reinterpret_cast<const char*>(plain.data()) + plain.size());
    return true;
}

}   // namespace relayer::protocol
