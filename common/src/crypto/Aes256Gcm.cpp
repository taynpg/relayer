#include "relayer/common/crypto/Aes256Gcm.h"

#include <algorithm>
#include <cstring>

#include "relayer/common/crypto/Sha256.h"

namespace relayer::crypto {
namespace {

// ===========================================================================
// AES-256 基本变换
// ===========================================================================

constexpr std::uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
    0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
    0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
    0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

// 下标 0 是占位，使 rcon[i/8] 的写法成立（轮常数从 1 开始，AES-256 最多用到 i/8 == 7）
constexpr std::uint8_t kRcon[11] = {0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

constexpr std::uint32_t rotWord(std::uint32_t x)
{
    return (x << 8) | (x >> 24);
}

std::uint32_t subWord(std::uint32_t x)
{
    return (static_cast<std::uint32_t>(kSbox[x >> 24]) << 24) | (static_cast<std::uint32_t>(kSbox[(x >> 16) & 0xff]) << 16) |
           (static_cast<std::uint32_t>(kSbox[(x >> 8) & 0xff]) << 8) | static_cast<std::uint32_t>(kSbox[x & 0xff]);
}

inline std::uint8_t xtime(std::uint8_t x)
{
    // (x << 1) ^ (x 最高位 ? 0x1b : 0)
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(x << 1) ^ ((x >> 7) * 0x1b));
}
inline std::uint8_t mul2(std::uint8_t x)
{
    return xtime(x);
}
inline std::uint8_t mul3(std::uint8_t x)
{
    return static_cast<std::uint8_t>(xtime(x) ^ x);
}

// T-table：把 SubBytes + ShiftRows + MixColumns 合并成 4 张 256 项的 uint32 表。
// 每轮每列只需 4 次查表 + 4 次异或。
struct TTables {
    std::uint32_t te0[256]{};
    std::uint32_t te1[256]{};
    std::uint32_t te2[256]{};
    std::uint32_t te3[256]{};

    TTables()
    {
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint32_t s = kSbox[i];
            const std::uint32_t s2 = mul2(kSbox[i]);
            const std::uint32_t s3 = mul3(kSbox[i]);
            te0[i] = (s2 << 24) | (s << 16) | (s << 8) | s3;
            te1[i] = (s3 << 24) | (s2 << 16) | (s << 8) | s;
            te2[i] = (s << 24) | (s3 << 16) | (s2 << 8) | s;
            te3[i] = (s << 24) | (s << 16) | (s3 << 8) | s2;
        }
    }
};

const TTables& tables()
{
    // C++11 起函数内 static 的初始化是线程安全的（magic static），
    // 所以这里不需要额外加锁，也允许并发首次调用。
    static const TTables t;
    return t;
}

// AES-256 密钥扩展：32 字节密钥 -> 60 个 32 位轮密钥
void aes256KeyExpansion(const std::uint8_t key[kAesKeySize], std::uint32_t rk[60])
{
    for (std::size_t i = 0; i < 8; ++i) {
        rk[i] = (static_cast<std::uint32_t>(key[4 * i]) << 24) | (static_cast<std::uint32_t>(key[4 * i + 1]) << 16) |
                (static_cast<std::uint32_t>(key[4 * i + 2]) << 8) | static_cast<std::uint32_t>(key[4 * i + 3]);
    }

    for (std::size_t i = 8; i < 60; ++i) {
        std::uint32_t temp = rk[i - 1];
        if (i % 8 == 0) {
            temp = subWord(rotWord(temp)) ^ (static_cast<std::uint32_t>(kRcon[i / 8]) << 24);
        } else if (i % 8 == 4) {
            temp = subWord(temp);
        }
        rk[i] = rk[i - 8] ^ temp;
    }
}

inline std::uint32_t loadBE(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void storeBE(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void aes256EncryptBlock(const std::uint8_t in[16], std::uint8_t out[16], const std::uint32_t rk[60])
{
    const TTables& t = tables();

    std::uint32_t s0 = loadBE(in) ^ rk[0];
    std::uint32_t s1 = loadBE(in + 4) ^ rk[1];
    std::uint32_t s2 = loadBE(in + 8) ^ rk[2];
    std::uint32_t s3 = loadBE(in + 12) ^ rk[3];

    for (std::size_t r = 1; r < 14; ++r) {
        const std::uint32_t* k = rk + 4 * r;
        const std::uint32_t t0 = t.te0[s0 >> 24] ^ t.te1[(s1 >> 16) & 0xff] ^ t.te2[(s2 >> 8) & 0xff] ^ t.te3[s3 & 0xff] ^ k[0];
        const std::uint32_t t1 = t.te0[s1 >> 24] ^ t.te1[(s2 >> 16) & 0xff] ^ t.te2[(s3 >> 8) & 0xff] ^ t.te3[s0 & 0xff] ^ k[1];
        const std::uint32_t t2 = t.te0[s2 >> 24] ^ t.te1[(s3 >> 16) & 0xff] ^ t.te2[(s0 >> 8) & 0xff] ^ t.te3[s1 & 0xff] ^ k[2];
        const std::uint32_t t3 = t.te0[s3 >> 24] ^ t.te1[(s0 >> 16) & 0xff] ^ t.te2[(s1 >> 8) & 0xff] ^ t.te3[s2 & 0xff] ^ k[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }

    // 最后一轮没有 MixColumns，直接查 sbox 拼字。
    // 注意：^ 的优先级高于 |，轮密钥异或必须对整个字加括号。
    const std::uint32_t* k = rk + 56;
    const std::uint32_t o0 =
        ((static_cast<std::uint32_t>(kSbox[s0 >> 24]) << 24) | (static_cast<std::uint32_t>(kSbox[(s1 >> 16) & 0xff]) << 16) |
         (static_cast<std::uint32_t>(kSbox[(s2 >> 8) & 0xff]) << 8) | static_cast<std::uint32_t>(kSbox[s3 & 0xff])) ^
        k[0];
    const std::uint32_t o1 =
        ((static_cast<std::uint32_t>(kSbox[s1 >> 24]) << 24) | (static_cast<std::uint32_t>(kSbox[(s2 >> 16) & 0xff]) << 16) |
         (static_cast<std::uint32_t>(kSbox[(s3 >> 8) & 0xff]) << 8) | static_cast<std::uint32_t>(kSbox[s0 & 0xff])) ^
        k[1];
    const std::uint32_t o2 =
        ((static_cast<std::uint32_t>(kSbox[s2 >> 24]) << 24) | (static_cast<std::uint32_t>(kSbox[(s3 >> 16) & 0xff]) << 16) |
         (static_cast<std::uint32_t>(kSbox[(s0 >> 8) & 0xff]) << 8) | static_cast<std::uint32_t>(kSbox[s1 & 0xff])) ^
        k[2];
    const std::uint32_t o3 =
        ((static_cast<std::uint32_t>(kSbox[s3 >> 24]) << 24) | (static_cast<std::uint32_t>(kSbox[(s0 >> 16) & 0xff]) << 16) |
         (static_cast<std::uint32_t>(kSbox[(s1 >> 8) & 0xff]) << 8) | static_cast<std::uint32_t>(kSbox[s2 & 0xff])) ^
        k[3];

    storeBE(out, o0);
    storeBE(out + 4, o1);
    storeBE(out + 8, o2);
    storeBE(out + 12, o3);
}

// ===========================================================================
// GF(2^128) 乘法与 GHASH
// ===========================================================================

// 128 位块用 hi/lo 两个 uint64 表示，语义是大端（块字节 0..7 -> hi，8..15 -> lo）。
struct GfBlock {
    std::uint64_t hi;
    std::uint64_t lo;
};

GfBlock gfFromBytes(const std::uint8_t b[16])
{
    GfBlock r{0, 0};
    for (std::size_t i = 0; i < 8; ++i) {
        r.hi = (r.hi << 8) | b[i];
    }
    for (std::size_t i = 8; i < 16; ++i) {
        r.lo = (r.lo << 8) | b[i];
    }
    return r;
}

void gfToBytes(GfBlock v, std::uint8_t out[16])
{
    for (std::size_t i = 8; i-- > 0;) {
        out[i] = static_cast<std::uint8_t>(v.hi & 0xff);
        v.hi >>= 8;
    }
    for (std::size_t i = 16; i-- > 8;) {
        out[i] = static_cast<std::uint8_t>(v.lo & 0xff);
        v.lo >>= 8;
    }
}

// 逐位乘法（NIST SP 800-38D Algorithm 1）。只在建表时用，不参与热路径。
GfBlock gfMul(GfBlock x, GfBlock y)
{
    GfBlock z{0, 0};
    GfBlock v = y;
    for (std::size_t i = 0; i < 128; ++i) {
        const std::uint64_t xi = (i < 64) ? ((x.hi >> (63 - i)) & 1) : ((x.lo >> (127 - i)) & 1);
        if (xi != 0) {
            z.hi ^= v.hi;
            z.lo ^= v.lo;
        }
        // V >>= 1，若移出位为 1 则异或约减多项式 R（0xE1 在最高字节）
        const std::uint64_t carry = v.lo & 1;
        v.lo = (v.lo >> 1) | (v.hi << 63);
        v.hi >>= 1;
        if (carry != 0) {
            v.hi ^= 0xE100000000000000ULL;
        }
    }
    return z;
}

// 4 位查表乘法（Shoup 变体）：m[n][v] = H · Q(v) · x^(4n)。每块只需 32 次查表 + 异或。
struct GhashTable {
    GfBlock m[32][16]{};

    // Z·x^4。rtab[c] 是低 4 位为 c 的块乘 x^4 后因 x^128 约减产生的修正项。
    static GfBlock x4mul(GfBlock z)
    {
        static const GfBlock rtab[16] = {
            {0x0000000000000000ULL, 0}, {0x1C20000000000000ULL, 0}, {0x3840000000000000ULL, 0}, {0x2460000000000000ULL, 0},
            {0x7080000000000000ULL, 0}, {0x6CA0000000000000ULL, 0}, {0x48C0000000000000ULL, 0}, {0x54E0000000000000ULL, 0},
            {0xE100000000000000ULL, 0}, {0xFD20000000000000ULL, 0}, {0xD940000000000000ULL, 0}, {0xC560000000000000ULL, 0},
            {0x9180000000000000ULL, 0}, {0x8DA0000000000000ULL, 0}, {0xA9C0000000000000ULL, 0}, {0xB5E0000000000000ULL, 0},
        };
        GfBlock r = rtab[z.lo & 0xF];
        z.lo = (z.lo >> 4) | (z.hi << 60);
        z.hi >>= 4;
        z.hi ^= r.hi;
        z.lo ^= r.lo;
        return z;
    }

    void init(GfBlock h)
    {
        for (std::size_t v = 0; v < 16; ++v) {
            std::uint8_t b[16] = {0};
            b[0] = static_cast<std::uint8_t>(v << 4);
            m[0][v] = gfMul(gfFromBytes(b), h);
        }
        for (std::size_t n = 1; n < 32; ++n) {
            for (std::size_t v = 0; v < 16; ++v) {
                m[n][v] = x4mul(m[n - 1][v]);
            }
        }
    }

    GfBlock mul(GfBlock x) const
    {
        GfBlock z{0, 0};
        for (std::size_t n = 0; n < 16; ++n) {
            const GfBlock t = m[n][(x.hi >> (60 - 4 * n)) & 0xF];
            z.hi ^= t.hi;
            z.lo ^= t.lo;
        }
        for (std::size_t n = 0; n < 16; ++n) {
            const GfBlock t = m[16 + n][(x.lo >> (60 - 4 * n)) & 0xF];
            z.hi ^= t.hi;
            z.lo ^= t.lo;
        }
        return z;
    }
};

// GHASH(H, AAD, C) = out
void ghash(const std::uint8_t h[16], const std::uint8_t* aad, std::size_t aadLen, const std::uint8_t* cipher,
           std::size_t cipherLen, std::uint8_t out[16])
{
    GhashTable gt;
    gt.init(gfFromBytes(h));

    GfBlock y{0, 0};
    std::uint8_t block[16];

    const auto absorb = [&](const std::uint8_t* data, std::size_t len) {
        std::size_t i = 0;
        while (i < len) {
            const std::size_t n = std::min(len - i, static_cast<std::size_t>(16));
            std::memset(block, 0, 16);
            std::memcpy(block, data + i, n);
            const GfBlock b = gfFromBytes(block);
            y.hi ^= b.hi;
            y.lo ^= b.lo;
            y = gt.mul(y);
            i += 16;
        }
    };

    if (aad != nullptr && aadLen > 0) {
        absorb(aad, aadLen);
    }
    if (cipher != nullptr && cipherLen > 0) {
        absorb(cipher, cipherLen);
    }

    // 末块：两个 64 位**比特**长度，大端
    std::memset(block, 0, 16);
    std::uint64_t aadBits = static_cast<std::uint64_t>(aadLen) * 8;
    std::uint64_t cipherBits = static_cast<std::uint64_t>(cipherLen) * 8;
    for (std::size_t i = 8; i-- > 0;) {
        block[i] = static_cast<std::uint8_t>(aadBits & 0xff);
        aadBits >>= 8;
    }
    for (std::size_t i = 16; i-- > 8;) {
        block[i] = static_cast<std::uint8_t>(cipherBits & 0xff);
        cipherBits >>= 8;
    }
    const GfBlock lb = gfFromBytes(block);
    y.hi ^= lb.hi;
    y.lo ^= lb.lo;
    y = gt.mul(y);

    gfToBytes(y, out);
}

// 由 nonce 构造 J0（初始计数器块）。本项目只用 96 位 nonce，
// 这条路径取 J0 = nonce || 0x00000001，不需要走 GHASH。
void makeJ0(const std::uint8_t nonce[kGcmNonceSize], std::uint8_t j0[16])
{
    std::memcpy(j0, nonce, kGcmNonceSize);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;
}

// 计数器递增：只作用于最后 32 位（GCM 规定的 inc32）。
// 溢出到 0 时说明同一 nonce 下加密了 2^32 个块（64 GiB），
// 那是必须换密钥的量级，这里不做特殊处理（协议层单帧上限远小于它）。
void inc32(std::uint8_t counter[16])
{
    for (std::size_t j = 16; j-- > 12;) {
        ++counter[j];
        if (counter[j] != 0) {
            break;
        }
    }
}

void ctrXor(const std::uint32_t rk[60], const std::uint8_t j0[16], const std::uint8_t* in, std::size_t len, std::uint8_t* out)
{
    std::uint8_t counter[16];
    std::memcpy(counter, j0, 16);

    for (std::size_t i = 0; i < len; i += 16) {
        inc32(counter);
        std::uint8_t ks[16];
        aes256EncryptBlock(counter, ks, rk);
        const std::size_t n = std::min(len - i, static_cast<std::size_t>(16));
        for (std::size_t j = 0; j < n; ++j) {
            out[i + j] = static_cast<std::uint8_t>(in[i + j] ^ ks[j]);
        }
    }
}

void computeTag(const std::uint32_t rk[60], const std::uint8_t h[16], const std::uint8_t j0[16], const std::uint8_t* aad,
                std::size_t aadLen, const std::uint8_t* cipher, std::size_t len, std::uint8_t tag[kGcmTagSize])
{
    std::uint8_t ghashOut[16];
    ghash(h, aad, aadLen, cipher, len, ghashOut);

    std::uint8_t ekJ0[16];
    aes256EncryptBlock(j0, ekJ0, rk);
    for (std::size_t i = 0; i < kGcmTagSize; ++i) {
        tag[i] = static_cast<std::uint8_t>(ghashOut[i] ^ ekJ0[i]);
    }
}

// 加密/解密共用的准备：算 H 与 J0
void prepare(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], std::uint32_t rk[60],
             std::uint8_t h[16], std::uint8_t j0[16])
{
    aes256KeyExpansion(key, rk);

    // H = AES_K(0^128)
    std::uint8_t zero[16] = {0};
    aes256EncryptBlock(zero, h, rk);

    makeJ0(nonce, j0);
}

}   // namespace

// ===========================================================================
// 公开接口
// ===========================================================================

void aes256GcmEncrypt(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], const std::uint8_t* aad,
                      std::size_t aadLen, const std::uint8_t* plain, std::size_t len, std::uint8_t* cipher,
                      std::uint8_t tag[kGcmTagSize])
{
    std::uint32_t rk[60];
    std::uint8_t h[16];
    std::uint8_t j0[16];
    prepare(key, nonce, rk, h, j0);

    if (len > 0) {
        ctrXor(rk, j0, plain, len, cipher);
    }

    computeTag(rk, h, j0, aad, aadLen, cipher, len, tag);
}

bool aes256GcmDecrypt(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], const std::uint8_t* aad,
                      std::size_t aadLen, const std::uint8_t* cipher, std::size_t len, const std::uint8_t tag[kGcmTagSize],
                      std::uint8_t* out)
{
    std::uint32_t rk[60];
    std::uint8_t h[16];
    std::uint8_t j0[16];
    prepare(key, nonce, rk, h, j0);

    // 先校验 tag 再解密：GCM 的正确用法是"不认证的数据一个字节都不吐出去"，
    // 否则调用方可能在检查返回值之前就用上了被篡改的明文。
    std::uint8_t computed[kGcmTagSize];
    computeTag(rk, h, j0, aad, aadLen, cipher, len, computed);

    if (!constantTimeEquals(computed, tag, kGcmTagSize)) {
        return false;
    }

    if (len > 0) {
        ctrXor(rk, j0, cipher, len, out);
    }
    return true;
}

std::vector<std::uint8_t> aes256GcmSeal(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize],
                                        std::string_view aad, std::string_view plain)
{
    std::vector<std::uint8_t> out(plain.size() + kGcmTagSize);

    const auto* plainPtr = reinterpret_cast<const std::uint8_t*>(plain.data());
    const auto* aadPtr = reinterpret_cast<const std::uint8_t*>(aad.data());
    std::uint8_t tag[kGcmTagSize];

    aes256GcmEncrypt(key, nonce, aadPtr, aad.size(), plainPtr, plain.size(), out.data(), tag);
    std::memcpy(out.data() + plain.size(), tag, kGcmTagSize);
    return out;
}

bool aes256GcmOpen(const std::uint8_t key[kAesKeySize], const std::uint8_t nonce[kGcmNonceSize], std::string_view aad,
                   const std::uint8_t* sealed, std::size_t sealedLen, std::vector<std::uint8_t>& plain)
{
    plain.clear();

    if (sealed == nullptr || sealedLen < kGcmTagSize) {
        return false;   // 连 tag 都不够，直接拒绝
    }

    const std::size_t bodyLen = sealedLen - kGcmTagSize;
    plain.resize(bodyLen);

    const auto* aadPtr = reinterpret_cast<const std::uint8_t*>(aad.data());
    const bool ok = aes256GcmDecrypt(key, nonce, aadPtr, aad.size(), sealed, bodyLen, sealed + bodyLen, plain.data());

    if (!ok) {
        // 认证失败：不把可能已被部分写入的明文留在调用方手里。
        plain.clear();
    }
    return ok;
}

}   // namespace relayer::crypto
