#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "relayer/common/crypto/Aes256Gcm.h"
#include "relayer/core/protocol/Frame.h"

// 载荷加密（PROTOCOL.md §6）。
//
// 解决的是"中继能看到明文字节"。本项目的定位是"中继只转发、不落盘"，
// 但**不落盘不等于看不到**——它转发的每一个字节它都能读。加密之后，中继
// 仍然是唯一的路径，只是内容对它不可读。
//
// ## 密钥从哪来：带外口令，没有协商
//
// 两台机器上各填同一个口令，密钥 = PBKDF2-HMAC-SHA256(口令, 固定盐,
// 20000 次, 32 字节)。刻意**不做**密钥协商（ECDH 之类）：
//
//   - 两端之间唯一可用的信道就是中继本身。在中继上协商密钥等于让中继
//     参与协商，它随时可以 MITM；没有可信第三方时，未认证的 ECDH 只是
//     把明文换成"看起来加密过的明文"。
//   - 带外口令（当面/电话/IM 里说一次）是唯一不依赖中继的认证方式。
//
// 代价必须说清楚：**口令强度就是全部安全性**。固定盐意味着可以预计算，
// 所以 PBKDF2 的 20000 次迭代是唯一的减速带。这不是"银行级加密"，
// 它挡的是"中继的运维顺手看一眼"，挡不住"有人拿着密文离线爆破弱口令"。
//
// ## nonce 怎么保证唯一
//
// 每帧 12 字节**随机**（系统 CSPRNG），不用计数器：密钥是长期口令派生的，
// 进程重启/多条连接都会从 0 重新计数，计数器方案必然撞车；GCM 一旦
// nonce 复用，明文异或直接泄露。
//
// 随机 nonce 的唯一性有概率上界：同一密钥下 2^32 帧之后碰撞概率约 2^-33，
// 对"一个人给另一台机器传文件"这个场景远远够用（要传 4 亿个帧才会撞）。

namespace relayer::protocol {

class PayloadCipher
{
public:
    static constexpr std::size_t kKeySize = crypto::kAesKeySize;
    static constexpr std::size_t kNonceSize = crypto::kGcmNonceSize;
    static constexpr std::size_t kTagSize = crypto::kGcmTagSize;

    // AAD 长度：固定头的前 28 字节（magic..index，不含 frameLen 与 headerCrc）。
    static constexpr std::size_t kAadSize = 28;

    // 从口令派生密钥。**空口令返回 nullptr**——那表示"不加密"，而不是
    // "用一个空密钥加密"。两者混起来会让"忘了填口令"变成静默的弱加密。
    [[nodiscard]] static std::shared_ptr<const PayloadCipher> fromPassphrase(std::string_view passphrase);

    // 用现成密钥构造（测试与将来的密钥协商路径用）。
    explicit PayloadCipher(const std::array<std::uint8_t, kKeySize>& key);

    // 加密：`out` 是**新帧**，flags 置 kFlagEncrypted，body 换成
    // `nonce || ciphertext || tag`。返回 false 只可能是拿不到随机数。
    [[nodiscard]] bool seal(const Frame& in, Frame& out) const;

    // 解密：`out` 是明文帧（加密位已清）。失败不区分"口令错"与"被篡改"
    // ——GCM 的设计就是不给区分依据，调用方只该说一句"解不开"。
    [[nodiscard]] bool open(const Frame& in, Frame& out) const;

    // AAD 的字节（PROTOCOL.md §6.1）。
    //
    // flags 取**规范形式**：置上加密位、清掉 body CRC 位。为什么必须清 CRC：
    // 那个位是打包时按 body 长度决定的，而加密发生在打包之前——发送方算
    // AAD 时还不知道最终 body 有多长，接收方拿到的帧里却已经定了。两者的
    // 唯一交集就是"把这一位抹掉"。CRC 位被改也无所谓：body 长度变了，
    // frameLen 就对不上，在解包阶段就挂了。
    static void aadOf(const Frame& f, std::uint8_t out[kAadSize]);

    [[nodiscard]] const std::array<std::uint8_t, kKeySize>& key() const noexcept
    {
        return key_;
    }

private:
    std::array<std::uint8_t, kKeySize> key_{};
};

}   // namespace relayer::protocol
