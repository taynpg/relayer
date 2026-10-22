#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/common/crypto/Sha256.h"

// 分块 Merkle 树（PROTOCOL.md §8.3.1）。
//
// 存在的理由：v1 只比对文件长度，而"长度一致、内容错位"的场景它发现不了
// ——某一块写到了错误的偏移上，总长不变，文件却是坏的。加上块级哈希之后，
// 内容正确性变成可证明的。
//
// 本文件是**纯计算**：没有 I/O，不认识 IFileStore，也不知道块从哪来。
// 调用方负责把每个块的内容喂进来（或先算好叶子哈希再喂），这里只负责合并。
//
// 为什么不让调用方自己拼树：树的形状（奇数上提、不复制末尾、叶子加 0x00
// 前缀）必须两端完全一致，否则"根相同"什么也证明不了。把形状写在一个地方
// 比写两份注释可靠。

namespace relayer::core::transfer
{

// 叶子/内部节点的域分隔前缀（RFC 6962 的做法）。
//
// 没有这两个字节，内部节点和叶子就落在同一个哈希空间里，攻击者可以构造
// 一个内部节点冒充叶子内容（第二原像）。加了之后两类输入的哈希不可互换。
inline constexpr std::uint8_t kMerkleLeafPrefix = 0x00;
inline constexpr std::uint8_t kMerkleNodePrefix = 0x01;

// 块大小的合法范围（PROTOCOL.md §9.4.1）。
inline constexpr std::uint32_t kMinChunkSize = 1024;
inline constexpr std::uint32_t kMaxChunkSize = 4u * 1024 * 1024;
inline constexpr std::uint32_t kChunkSizeAlignment = 1024;

// 默认块大小。4 MiB 是"一次读盘 + 一次帧发送"的折中：更小则帧头与
// 往返开销占比升高，更大则单块失败的重传代价与内存峰值同时上升。
inline constexpr std::uint32_t kDefaultChunkSize = kMaxChunkSize;

// 给定文件总长与块大小，算出块数。
//
// 空文件算 **0 块**，但根不是"空树"——见 root() 的说明。
[[nodiscard]] std::uint32_t chunkCountFor(std::uint64_t totalSize, std::uint32_t chunkSize) noexcept;

// 第 index 块的字节长度（最后一块可能短）。
// index 越界返回 0。
[[nodiscard]] std::uint64_t chunkLengthAt(std::uint32_t index, std::uint64_t totalSize,
                                          std::uint32_t chunkSize) noexcept;

// 计算单个叶子的哈希：SHA256(0x00 || 补齐到 chunkSize 的块内容)。
//
// **补零是规格的一部分**：最后一块不足时补到 chunkSize 再哈希。这样根只由
// "文件内容 + 块大小"决定，不依赖 payloadLen 这类可变的传输细节。
//
// 补零通过流式 update 完成，不会为了补齐而真的分配 chunkSize 字节：
// 4 MiB 的块乘上并发任务数很容易把内存吃光。
[[nodiscard]] crypto::Sha256::Digest leafHash(const void *chunk, std::size_t len, std::uint32_t chunkSize);

// 计算内部节点：SHA256(0x01 || left || right)。
[[nodiscard]] crypto::Sha256::Digest nodeHash(const crypto::Sha256::Digest &left, const crypto::Sha256::Digest &right);

// 空文件的根：SHA256("")。写死一个常量比让各处自己处理"零叶子返回什么"
// 安全——那边界几乎没人测。
[[nodiscard]] const crypto::Sha256::Digest &emptyRoot() noexcept;

// 从叶子哈希列表算根。
//
// 规则（PROTOCOL.md §8.3.1）：
//   - 空列表 → emptyRoot()
//   - 只有一个叶子 → 该叶子本身（不再套一层节点）
//   - 奇数个节点 → 最后一个**单独上提**到上一层，不复制
//
// 第 3 条与比特币的写法不同，这是刻意的：复制最后一个会让 [a,b,c] 与
// [a,b,c,c] 得到同一个根，两种不同的文件内容通过同一个校验。
[[nodiscard]] crypto::Sha256::Digest computeRoot(const std::vector<crypto::Sha256::Digest> &leaves);

// 增量收集器。调用方按顺序喂块，最后取根。
//
// 为什么还要一个增量版本：一边读盘一边算叶子是自然的流式过程，先攒齐
// 全部叶子再算根意味着要额外持有 N × 32 字节。对 1GB / 4MB 来说只有
// 256 个叶子（8KB），影响不大；但对小块场景（1KB 块 × 1GB = 100 万个叶子，
// 32MB）就不一样了。增量版内部只保留"当前层"的节点，峰值同样只有 N×32，
// 但它让调用方不必等全部读完才开始。
class MerkleBuilder
{
public:
    // chunkSize 必须在 [kMinChunkSize, kMaxChunkSize] 内且按 1024 对齐。
    // 越界时构造出的对象处于"非法"状态，addChunk() 会失败——构造不抛
    // 异常是跨模块的既定约定（Status + ErrorCode，见 MEMORY）。
    explicit MerkleBuilder(std::uint32_t chunkSize) noexcept;

    // 追加一个块。必须按块号顺序调用，且除最后一块外长度必须等于 chunkSize。
    //
    // 顺序强制：乱序喂块会让"第几个叶子"变得不确定，而叶子顺序直接决定根。
    // 宁可在这里失败，也不要让两端各自推测顺序。
    [[nodiscard]] Status addChunk(const void *data, std::size_t len);

    // 已喂入的块数。
    [[nodiscard]] std::uint32_t count() const noexcept
    {
        return count_;
    }

    [[nodiscard]] std::uint32_t chunkSize() const noexcept
    {
        return chunkSize_;
    }

    // 算出根。可在任意时刻调用（不修改内部状态）。
    [[nodiscard]] crypto::Sha256::Digest root() const;

    // 是否还能继续喂块。false 表示构造参数非法，或上一次 addChunk 失败。
    [[nodiscard]] bool valid() const noexcept
    {
        return valid_;
    }

private:
    std::uint32_t chunkSize_;
    std::uint32_t count_ = 0;
    bool valid_ = true;

    // 已经算好的叶子哈希。root() 时才合并成树。
    //
    // 为什么不边收边合并：那个增量版本与"逐层两两合并、奇数上提"的规范
    // 形状不等价（3 个、7 个叶子时都会分叉，见 .cpp 里的说明），而形状
    // 不一致的表现是"两端都对、就是根不同"，几乎无法定位。这里选择用
    // 一点内存换"只有一份形状实现"。
    std::vector<crypto::Sha256::Digest> leaves_;
};

// ---------------------------------------------------------------------------
// 便捷：一次性算整份数据的根
// ---------------------------------------------------------------------------

// 从一段完整内容算根。内部按 chunkSize 切块，最后一块补零。
// 数据量大于 chunkSize * 若干 时不建议用（会多一次切分开销），
// 但测试与小文件用起来很方便。
[[nodiscard]] crypto::Sha256::Digest rootOfBuffer(const void *data, std::size_t len, std::uint32_t chunkSize);

// 十六进制小写，与 §9.4 的 sha256 字段格式一致。
[[nodiscard]] std::string toHex(const crypto::Sha256::Digest &d);

// 解析十六进制（大小写均可）。长度不是 64 或含非十六进制字符时返回 false。
[[nodiscard]] bool fromHex(std::string_view hex, crypto::Sha256::Digest &out) noexcept;

}  // namespace relayer::core::transfer
