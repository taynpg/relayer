#include "relayer/common/crypto/Sha256.h"

#include <cstring>
#include <vector>

namespace relayer::crypto {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr char kHexDigits[] = "0123456789abcdef";

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

inline std::uint32_t bigEndian32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

}   // namespace

Sha256::Sha256() : bitlen_(0), bufferLen_(0)
{
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::transform(const std::uint8_t block[kBlockSize])
{
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = bigEndian32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t len)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    if (p == nullptr || len == 0) {
        return;
    }

    // 先填满上一轮剩下的缓冲
    if (bufferLen_ > 0) {
        const std::size_t need = kBlockSize - bufferLen_;
        const std::size_t take = (len < need) ? len : need;
        std::memcpy(buffer_ + bufferLen_, p, take);
        bufferLen_ += static_cast<std::uint32_t>(take);
        p += take;
        len -= take;
        if (bufferLen_ == kBlockSize) {
            transform(buffer_);
            bitlen_ += kBlockSize * 8;
            bufferLen_ = 0;
        }
    }

    // 整块直通，不经过缓冲
    while (len >= kBlockSize) {
        transform(p);
        bitlen_ += kBlockSize * 8;
        p += kBlockSize;
        len -= kBlockSize;
    }

    if (len > 0) {
        std::memcpy(buffer_, p, len);
        bufferLen_ = static_cast<std::uint32_t>(len);
    }
}

Sha256::Digest Sha256::digest()
{
    // 追加 0x80 + 零填充 + 64 位大端比特长度。
    // 注意：digest() 会修改内部状态，调用后不应继续 update。
    const std::uint64_t totalBits = bitlen_ + static_cast<std::uint64_t>(bufferLen_) * 8;

    buffer_[bufferLen_++] = 0x80;
    if (bufferLen_ > 56) {
        std::memset(buffer_ + bufferLen_, 0, kBlockSize - bufferLen_);
        transform(buffer_);
        bufferLen_ = 0;
    }
    std::memset(buffer_ + bufferLen_, 0, 56 - bufferLen_);
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::uint8_t>(totalBits >> (56 - i * 8));
    }
    transform(buffer_);

    Digest out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return out;
}

Sha256::Digest Sha256::hash(const void* data, std::size_t len)
{
    Sha256 h;
    h.update(data, len);
    return h.digest();
}

std::string Sha256::hex(const Digest& d)
{
    std::string out;
    out.resize(d.size() * 2);
    for (std::size_t i = 0; i < d.size(); ++i) {
        out[i * 2 + 0] = kHexDigits[d[i] >> 4];
        out[i * 2 + 1] = kHexDigits[d[i] & 0x0F];
    }
    return out;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

Sha256::Digest hmacSha256(const void* key, std::size_t keyLen, const void* data, std::size_t dataLen)
{
    // RFC 2104：
    //   K' = key 长度 > 64 ? SHA256(key) : key 右补零到 64
    //   HMAC = SHA256((K' xor opad) || SHA256((K' xor ipad) || data))
    constexpr std::size_t blockSize = Sha256::kBlockSize;

    std::uint8_t paddedKey[blockSize] = {};

    if (keyLen > blockSize) {
        const Sha256::Digest hashed = Sha256::hash(key, keyLen);
        std::memcpy(paddedKey, hashed.data(), hashed.size());
    } else if (keyLen > 0) {
        std::memcpy(paddedKey, key, keyLen);
    }

    std::uint8_t ipad[blockSize];
    std::uint8_t opad[blockSize];
    for (std::size_t i = 0; i < blockSize; ++i) {
        ipad[i] = static_cast<std::uint8_t>(paddedKey[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(paddedKey[i] ^ 0x5C);
    }

    // 内层：(K' xor ipad) || data。分两次 update，避免为拼接待 MAC 的消息
    // 再分配一块连续内存（data 可能很长，而这里只想拷两次头）。
    std::uint8_t inner[Sha256::kDigestSize];
    {
        Sha256 h;
        h.update(ipad, blockSize);
        if (dataLen > 0) {
            h.update(data, dataLen);
        }
        const Sha256::Digest d = h.digest();
        std::memcpy(inner, d.data(), d.size());
    }

    Sha256 outer;
    outer.update(opad, blockSize);
    outer.update(inner, sizeof(inner));
    return outer.digest();
}

Sha256::Digest hmacSha256(std::string_view key, std::string_view data)
{
    return hmacSha256(key.data(), key.size(), data.data(), data.size());
}

bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t len) noexcept
{
    if (a == nullptr || b == nullptr) {
        return a == b;
    }

    // volatile 让编译器不能把这个循环优化成"遇到不同就提前跳出的 memcmp"。
    // 差异累积到一个累加器里，最后一次性判断——分支只在末尾出现一次。
    volatile std::uint8_t diff = 0;
    for (std::size_t i = 0; i < len; ++i) {
        diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256
// ---------------------------------------------------------------------------

HmacKey pbkdf2HmacSha256(std::string_view password, const std::uint8_t* salt, std::size_t saltLen, std::uint32_t iterations)
{
    HmacKey out{};
    if (iterations == 0) {
        return out;   // 调用方传 0 是 bug，但不要因此崩掉或返回未初始化内存
    }

    // 只需要 32 字节输出，恰好是 HMAC-SHA256 的一个块，所以只算 U1（块索引 1）。
    // RFC 8018：U1 = HMAC(P, S || INT_32_BE(1))，U2 = HMAC(P, U1)，...
    //            T  = U1 xor U2 xor ... xor Uc
    std::uint8_t u[Sha256::kDigestSize];

    {
        // 拼 S || INT(1)。salt 通常 16 字节，这里用固定上限的栈缓冲，
        // 超出则退回堆分配——不假设 salt 长度，但常见路径不分配。
        std::uint8_t stackBuf[128];
        const std::size_t total = saltLen + 4;
        std::uint8_t* msg = stackBuf;
        std::vector<std::uint8_t> heap;
        if (total > sizeof(stackBuf)) {
            heap.resize(total);
            msg = heap.data();
        }
        if (saltLen > 0) {
            std::memcpy(msg, salt, saltLen);
        }
        msg[saltLen + 0] = 0;
        msg[saltLen + 1] = 0;
        msg[saltLen + 2] = 0;
        msg[saltLen + 3] = 1;

        const Sha256::Digest d = hmacSha256(password.data(), password.size(), msg, total);
        std::memcpy(u, d.data(), d.size());
    }

    std::memcpy(out.data(), u, out.size());

    for (std::uint32_t i = 1; i < iterations; ++i) {
        const Sha256::Digest d = hmacSha256(password.data(), password.size(), u, sizeof(u));
        std::memcpy(u, d.data(), d.size());
        for (std::size_t j = 0; j < out.size(); ++j) {
            out[j] = static_cast<std::uint8_t>(out[j] ^ u[j]);
        }
    }

    return out;
}

}   // namespace relayer::crypto
