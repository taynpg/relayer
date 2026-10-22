#include "relayer/core/transfer/MerkleTree.h"

#include <cstring>

namespace relayer::core::transfer
{

namespace
{

// 补齐缓冲。补零不分配 chunkSize 字节，而是复用一块固定大小的零，
// 分多次 update。1 MiB 的选择让 4 MiB 块最多 4 次 update，开销可忽略。
constexpr std::size_t kZeroPadChunk = 1024 * 1024;

// 一块静态零。放在函数里而不是文件级的 std::array：
// 4 MiB 的常量数组会进 .bss，而这里 1 MiB 也够用了。
const std::uint8_t *zeroBlock() noexcept
{
    static const std::vector<std::uint8_t> zeros(kZeroPadChunk, 0);
    return zeros.data();
}

void updateZeros(crypto::Sha256 &h, std::uint64_t bytes)
{
    const auto *z = zeroBlock();
    while (bytes > 0)
    {
        const auto n = static_cast<std::size_t>(bytes < kZeroPadChunk ? bytes : kZeroPadChunk);
        h.update(z, n);
        bytes -= n;
    }
}

int hexVal(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// 分块几何
// ---------------------------------------------------------------------------

std::uint32_t chunkCountFor(std::uint64_t totalSize, std::uint32_t chunkSize) noexcept
{
    if (totalSize == 0 || chunkSize == 0)
    {
        return 0;
    }
    // 向上取整。用 (a + b - 1) / b 在 totalSize 接近 UINT64_MAX 时会溢出，
    // 改成先除后补。
    const std::uint64_t full = totalSize / chunkSize;
    const std::uint64_t rem = totalSize % chunkSize;
    const std::uint64_t n = full + (rem != 0 ? 1u : 0u);

    // 块数在线上是 u32。超过就意味着参数错了（或文件大到不该走这条路），
    // 截断会让发送方少传数据而不报错，所以这里明确封顶到最大值，
    // 由调用方在描述里校验 totalSize/chunkSize 的组合是否自洽。
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(n);
}

std::uint64_t chunkLengthAt(std::uint32_t index, std::uint64_t totalSize, std::uint32_t chunkSize) noexcept
{
    if (chunkSize == 0)
    {
        return 0;
    }
    const std::uint64_t start = static_cast<std::uint64_t>(index) * chunkSize;
    if (start >= totalSize)
    {
        return 0;
    }
    const std::uint64_t remain = totalSize - start;
    return remain < chunkSize ? remain : chunkSize;
}

// ---------------------------------------------------------------------------
// 哈希
// ---------------------------------------------------------------------------

crypto::Sha256::Digest leafHash(const void *chunk, std::size_t len, std::uint32_t chunkSize)
{
    crypto::Sha256 h;
    const std::uint8_t prefix = kMerkleLeafPrefix;
    h.update(&prefix, 1);
    if (len > 0)
    {
        h.update(chunk, len);
    }
    // 补齐到 chunkSize。len 超过 chunkSize 说明调用方搞错了块边界，
    // 这里不截断也不报错——那是 addChunk 的职责，本函数只做纯计算。
    if (len < chunkSize)
    {
        updateZeros(h, static_cast<std::uint64_t>(chunkSize) - len);
    }
    return h.digest();
}

crypto::Sha256::Digest nodeHash(const crypto::Sha256::Digest &left, const crypto::Sha256::Digest &right)
{
    crypto::Sha256 h;
    const std::uint8_t prefix = kMerkleNodePrefix;
    h.update(&prefix, 1);
    h.update(left.data(), left.size());
    h.update(right.data(), right.size());
    return h.digest();
}

const crypto::Sha256::Digest &emptyRoot() noexcept
{
    // SHA256("") = e3b0c442...
    static const crypto::Sha256::Digest kEmpty = crypto::Sha256::hash("", 0);
    return kEmpty;
}

crypto::Sha256::Digest computeRoot(const std::vector<crypto::Sha256::Digest> &leaves)
{
    if (leaves.empty())
    {
        return emptyRoot();
    }
    if (leaves.size() == 1)
    {
        return leaves[0];
    }

    std::vector<crypto::Sha256::Digest> level = leaves;
    while (level.size() > 1)
    {
        std::vector<crypto::Sha256::Digest> next;
        next.reserve((level.size() + 1) / 2);

        std::size_t i = 0;
        for (; i + 1 < level.size(); i += 2)
        {
            next.push_back(nodeHash(level[i], level[i + 1]));
        }
        // 奇数个：最后一个**单独上提**，不与自己合并。
        // 复制最后一个会让 [a,b,c] 与 [a,b,c,c] 同根（PROTOCOL.md §8.3.1）。
        if (i < level.size())
        {
            next.push_back(level[i]);
        }
        level.swap(next);
    }
    return level[0];
}

// ---------------------------------------------------------------------------
// MerkleBuilder
// ---------------------------------------------------------------------------

MerkleBuilder::MerkleBuilder(std::uint32_t chunkSize) noexcept
    : chunkSize_(chunkSize)
{
    if (chunkSize_ < kMinChunkSize || chunkSize_ > kMaxChunkSize || chunkSize_ % kChunkSizeAlignment != 0)
    {
        valid_ = false;
    }
}

Status MerkleBuilder::addChunk(const void *data, std::size_t len)
{
    if (!valid_)
    {
        return Status::Fail(ErrorCode::kProtocol, "merkle: chunkSize out of range or misaligned");
    }
    if (len > chunkSize_)
    {
        return Status::Fail(ErrorCode::kProtocol, "merkle: chunk longer than chunkSize");
    }

    ++count_;
    leaves_.push_back(leafHash(data, len, chunkSize_));
    return Status::Ok();
}

crypto::Sha256::Digest MerkleBuilder::root() const
{
    if (count_ == 0)
    {
        return emptyRoot();
    }

    // 直接复用 computeRoot。
    //
    // 这里曾经写过一版"边收边合并上提"的增量算法，省掉了存全部叶子，
    // 但它与"逐层两两合并、奇数上提"的规范形状**不等价**：
    //
    //   3 个叶子时落单的 c 需要作为 node(a,b) 的右操作数，而边收边合并
    //   的写法会先把 c 上提到与 node(a,b) 同层，谁在左谁在右取决于是
    //   哪一层先落单；
    //   7 个叶子时两层各有一个落单节点，拼接顺序更没有唯一答案。
    //
    // 这类错误的表现是"两端实现都对、就是根不一致"，在现场无法定位。
    // 换成直接调 computeRoot 之后只有一份形状实现，不可能与它分歧。
    //
    // 代价是存下全部叶子：每个 32 字节。1GB 文件配 4MB 块 = 256 个叶子
    // = 8KB；配 1KB 块 = 100 万个叶子 = 32MB。后者是可观的开销，但
    // 1KB 块本来就不会用于大文件（§9.4.1 说块大小由发送方按读盘速度选），
    // 真要那样用也应当先担心"100 万次 HTTP 式往返"而不是 32MB 内存。
    return computeRoot(leaves_);
}

// ---------------------------------------------------------------------------
// 便捷
// ---------------------------------------------------------------------------

crypto::Sha256::Digest rootOfBuffer(const void *data, std::size_t len, std::uint32_t chunkSize)
{
    if (len == 0)
    {
        return emptyRoot();
    }
    if (chunkSize == 0)
    {
        return emptyRoot();
    }

    MerkleBuilder b(chunkSize);
    if (!b.valid())
    {
        return emptyRoot();
    }

    const auto *p = static_cast<const std::uint8_t *>(data);
    std::size_t off = 0;
    while (off < len)
    {
        const auto n = static_cast<std::size_t>(std::min<std::size_t>(chunkSize, len - off));
        const auto st = b.addChunk(p + off, n);
        if (!st.ok())
        {
            return emptyRoot();
        }
        off += n;
    }
    return b.root();
}

std::string toHex(const crypto::Sha256::Digest &d)
{
    return crypto::Sha256::hex(d);
}

bool fromHex(std::string_view hex, crypto::Sha256::Digest &out) noexcept
{
    if (hex.size() != out.size() * 2)
    {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const int hi = hexVal(hex[i * 2]);
        const int lo = hexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace relayer::core::transfer
