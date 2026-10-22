#pragma once

#include <cstdint>
#include <vector>

#include "relayer/core/transfer/TransferPayload.h"

// 发送侧的窗口与重传调度（PROTOCOL.md §8.3 / §9.7）。
//
// 这是流水线里**唯一必须严格可测**的部分：窗口算错的表现是"传完哈希不
// 匹配"，而那要等整份文件传完才暴露，且有随机性（取决于丢包模式）。
// 所以这里做成一坨纯数据 + 纯函数，没有任何 I/O、定时器、线程。
//
// 状态模型：
//
//   已确认 ─┬─ 在飞（已发出，等 ACK） ─┬─ 待发（未发出）
//           │                          └─ 待重传（发出后判丢）
//           └─ ackThrough 之前的连续前缀
//
// 窗口约束的是 **inflight + needRetransmit**，不含 pending：
//
//   inflight + needRetransmit <= windowChunks
//
// pending 是"还没轮到发"，本来就是被窗口挡住的结果，不算占用者。
// 把它算进来会让"窗口 >= 总块数"的情况直接死锁（所有块都算占用，
// freeSlots() 恒为 0，一块也取不出来）。
//
// 这是背压在**逻辑层**的落点。物理层的落点是 WorkerPool 的队列上限
// （磁盘跟不上时 submit 返回 false）。

namespace relayer::core::transfer
{

// 一块的当前状态。
enum class ChunkState : std::uint8_t
{
    kPending = 0,         // 还没发过
    kInflight = 1,        // 已发出，等 ACK
    kAcked = 2,           // 已确认落盘
    kNeedRetransmit = 3,  // 发过但被判丢（来自 SACK 或重传定时器）
};

class SendWindow
{
public:
    // totalChunks：总块数。0 表示空文件（窗口立即"全部完成"）。
    // windowChunks：同时在飞 + 待重传的块数上限。0 会被夹到 1，
    //               否则窗口永远无法推进，表现为"传输卡住不报错"。
    SendWindow(std::uint32_t totalChunks, std::uint32_t windowChunks);

    [[nodiscard]] std::uint32_t totalChunks() const noexcept
    {
        return total_;
    }
    [[nodiscard]] std::uint32_t windowChunks() const noexcept
    {
        return window_;
    }

    // 取下一批要发的块号，最多 maxCount 个，且不超过窗口容量。
    //
    // 优先返回**待重传**的块，其次才是从没发过的。理由：重传的块通常对应
    // 接收方的空洞，先补空洞能让 ackThrough 向前推进，从而腾出窗口——
    // 而发送新块不会让 ackThrough 前进。顺序反过来会让窗口被"新块"
    // 占满，空洞永远补不上（经典的重传饥饿）。
    //
    // 返回的块已经标记为 kInflight。调用方必须真的把它们发出去，
    // 否则窗口会永久占用（用 release() 归还）。
    [[nodiscard]] std::vector<std::uint32_t> takeNextBatch(std::uint32_t maxCount);

    // 收到 ACK 后更新。返回"新确认了多少块"。
    //
    // ack.ackThrough 可能超过 total_（对方多报），按 total_ 截断；
    // 位图越界位一律忽略。
    std::uint32_t applyAck(const ChunkAck &ack);

    // 归还一批块（发送失败 / 会话取消）。状态退回 kPending，
    // 让它们还能被重新取走。
    void release(const std::vector<std::uint32_t> &chunks);

    // 把在飞超过 timeout 的块标记为待重传。返回被标记的块数。
    //
    // 由调用方的定时器驱动（本类不认识时间，只认识"这些块超时了"）。
    // 传入的是"待重传的块号列表"，由调用方自己按发送时刻算出来——
    // 把时间塞进来会让本类无法用纯逻辑测试。
    std::uint32_t markRetransmit(const std::vector<std::uint32_t> &chunks);

    // 是否全部确认。
    [[nodiscard]] bool done() const noexcept;

    // 各类计数，用于诊断与测试。
    [[nodiscard]] std::uint32_t acked() const noexcept
    {
        return acked_;
    }
    [[nodiscard]] std::uint32_t inflight() const noexcept;
    [[nodiscard]] std::uint32_t pending() const noexcept;
    [[nodiscard]] std::uint32_t needRetransmit() const noexcept;

    // 累计"被取出用于重传"的块次（同一块重传两次算两次）。
    //
    // 用途是诊断与验收：丢包场景下它应当约等于**丢失的块数**，而不是
    // 总块数。没有这个数字时，"每次丢一块就整份重传"这种退化只能靠
    // 传完之后的总耗时去猜，而那在慢机器上完全不可分辨。
    [[nodiscard]] std::uint32_t retransmitted() const noexcept
    {
        return retransmitted_;
    }

    [[nodiscard]] ChunkState stateOf(std::uint32_t index) const noexcept;

    // 连续已确认前缀的长度。等于全部块数时说明收齐了。
    [[nodiscard]] std::uint32_t contiguousAcked() const noexcept
    {
        return ackThrough_;
    }

    // 窗口还能再塞多少块。已夹到"剩余未确认块数"以内——窗口再大，
    // 没有块可发时也不该报到正数，否则调用方会以为自己还有活要干。
    [[nodiscard]] std::uint32_t freeSlots() const noexcept;

    // 所有**在飞**的块号（升序）。用于"过一遍重传定时器"。
    //
    // 刻意不包含 kPending / kNeedRetransmit：只有发出去的东西才谈得上
    // 超时，把它们算进来会让调用方误以为已在路上而重复发送。
    [[nodiscard]] std::vector<std::uint32_t> outstanding() const;

private:
    [[nodiscard]] bool inUse(std::uint32_t index) const noexcept;

    std::uint32_t total_;
    std::uint32_t window_;

    std::vector<ChunkState> state_;

    std::uint32_t ackThrough_ = 0;  // 连续已确认前缀长度
    std::uint32_t acked_ = 0;       // 已确认总数（含空洞之外的）

    // 累计重传块次。只增不减：release() 把块退回 kPending 也不回退它，
    // 因为"曾经重传过"这个事实不会因为后来的归还而消失。
    std::uint32_t retransmitted_ = 0;

    // 待重传的块号，按 FIFO 排。用 deque 的替代：vector + 头指针，
    // 因为窗口上限不大（4MB 窗口 / 4MB 块 = 1 块起，1MB 块 = 4 块），
    // 简单线性结构足够，且便于断言顺序。
    std::vector<std::uint32_t> retransmitQueue_;
};

}  // namespace relayer::core::transfer
