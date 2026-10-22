#include "relayer/core/transfer/SendWindow.h"

#include <algorithm>

namespace relayer::core::transfer
{

SendWindow::SendWindow(std::uint32_t totalChunks, std::uint32_t windowChunks)
    : total_(totalChunks)
    , window_(windowChunks == 0 ? 1 : windowChunks)
    , state_(totalChunks, ChunkState::kPending)
{
}

bool SendWindow::inUse(std::uint32_t index) const noexcept
{
    if (index >= total_)
    {
        return false;
    }
    const auto s = state_[index];
    // 只有"真的占着窗口位置"的两种状态才算占用：
    //   kInflight        —— 已经发出去，在对端手里，等 ACK
    //   kNeedRetransmit  —— 发过但判丢，还要再发一次
    //
    // kPending **不算**。它就是"还没轮到我发"，本质上是被窗口排队挡住
    // 的结果，而不是占用者。把它算进来的后果是：窗口等于总块数时，
    // 所有块都"已占用"，freeSlots() 恒为 0，takeNextBatch() 一个都取不出
    // ——传输从第一步就卡死，且不报错。
    return s == ChunkState::kInflight || s == ChunkState::kNeedRetransmit;
}

std::uint32_t SendWindow::inflight() const noexcept
{
    return static_cast<std::uint32_t>(std::count(state_.begin(), state_.end(), ChunkState::kInflight));
}

std::uint32_t SendWindow::pending() const noexcept
{
    return static_cast<std::uint32_t>(std::count(state_.begin(), state_.end(), ChunkState::kPending));
}

std::uint32_t SendWindow::needRetransmit() const noexcept
{
    return static_cast<std::uint32_t>(std::count(state_.begin(), state_.end(), ChunkState::kNeedRetransmit));
}

ChunkState SendWindow::stateOf(std::uint32_t index) const noexcept
{
    return index < total_ ? state_[index] : ChunkState::kAcked;
}

std::uint32_t SendWindow::freeSlots() const noexcept
{
    // 窗口容量：受"在飞 + 待重传"占用约束。
    //
    // 这里**不含** kPending——它本来就是被窗口挡住的那部分，不是占用者。
    std::uint32_t used = 0;
    for (std::uint32_t i = 0; i < total_; ++i)
    {
        if (inUse(i))
        {
            ++used;
        }
    }
    const auto byWindow = used >= window_ ? 0 : window_ - used;

    // 再夹一个"真的还有块可发"的上界：只数状态仍是 kPending 的块。
    //
    // 少了这个上界会怎样：一块 1MB 的文件配 4MB 窗口，全部发完之后
    // freeSlots() 会一直报 1/2/3 —— 窗口没满，但没块可发。调用方据此
    // 认为"还有活要干"，于是空转。
    //
    // 注意不能用 total_ - acked_ 来算：块一旦进入在飞状态，它既没被确认
    // 也不能再被取走，用它做上界会让"全部在飞、一块都取不出来"的情况
    // 仍然报出空位。只有 kPending 数才是真正的"可发数量"。
    const auto available = pending();

    return std::min(byWindow, available);
}

bool SendWindow::done() const noexcept
{
    return acked_ >= total_;
}

std::vector<std::uint32_t> SendWindow::takeNextBatch(std::uint32_t maxCount)
{
    std::vector<std::uint32_t> out;
    if (maxCount == 0 || done())
    {
        return out;
    }

    auto budget = std::min(maxCount, freeSlots());
    if (budget == 0)
    {
        return out;
    }

    // 下面两段循环各自也可能因为"没有可取的块"而提前停，所以实际发出的
    // 数量不一定等于 budget。这是对的：freeSlots() 是容量上界，不是承诺。

    // 先补空洞。
    //
    // 顺序很重要：新块不会让 ackThrough 前进，只有空洞被填上才会。
    // 若先发新块，窗口会被"永远得不到确认推进"的块占满，空洞永远补不上
    // ——这是重传饥饿，表现为"卡在 99% 不动"。
    std::size_t consumed = 0;
    while (consumed < retransmitQueue_.size() && out.size() < budget)
    {
        const auto idx = retransmitQueue_[consumed];
        // 队列可能与状态脱节（比如重复标记），以状态为准。
        if (idx < total_ && state_[idx] == ChunkState::kNeedRetransmit)
        {
            state_[idx] = ChunkState::kInflight;
            out.push_back(idx);
        }
        ++consumed;
    }
    retransmitQueue_.erase(retransmitQueue_.begin(), retransmitQueue_.begin() + static_cast<std::ptrdiff_t>(consumed));

    // 再发新块。
    //
    // 从 ackThrough 开始扫而不是从 0 开始：前缀之前的块一定已确认。
    //
    // 但**不能停在第一个非 kPending 的块**——乱序 ACK 会在前缀之后留下
    // 已确认的块，跳过它们继续往后才是对的，否则每遇到一个空洞就少发一批。
    for (std::uint32_t i = ackThrough_; i < total_ && out.size() < budget; ++i)
    {
        if (state_[i] == ChunkState::kPending)
        {
            state_[i] = ChunkState::kInflight;
            out.push_back(i);
        }
    }

    return out;
}

std::uint32_t SendWindow::applyAck(const ChunkAck &ack)
{
    std::uint32_t newly = 0;

    const auto through = std::min(ack.ackThrough, total_);
    for (std::uint32_t i = ackThrough_; i < through; ++i)
    {
        if (state_[i] != ChunkState::kAcked)
        {
            state_[i] = ChunkState::kAcked;
            ++newly;
        }
    }
    if (through > ackThrough_)
    {
        ackThrough_ = through;
    }

    // 位图描述的是 ackThrough 之后的块。
    //
    // 注意基准点是**本次 ACK 里的 ackThrough**，不是更新后的 ackThrough_：
    // 位图第 i 位对应块 (ack.ackThrough + i)。用错基准会让整段块错位，
    // 而错位在"内容全对但位置错"时只有最终哈希能发现。
    for (std::uint32_t i = 0; i < ack.bitmap.size() * 32; ++i)
    {
        if (!bitmapTest(ack.bitmap, i))
        {
            continue;
        }
        const auto idx = static_cast<std::uint64_t>(ack.ackThrough) + i;
        if (idx >= total_)
        {
            break;
        }
        const auto k = static_cast<std::uint32_t>(idx);
        if (state_[k] != ChunkState::kAcked)
        {
            state_[k] = ChunkState::kAcked;
            ++newly;
        }
    }

    // 重算已确认总数。不用 += 累加：同一位被 ACK 两次（重复 ACK 在丢包
    // 场景里很常见）会让计数虚高，最终 done() 提前为真——表现为"提前收尾、
    // 文件短一截"。
    acked_ = static_cast<std::uint32_t>(std::count(state_.begin(), state_.end(), ChunkState::kAcked));

    // 连续前缀要跟着推进：位图可能已经把 ackThrough 之后的块都补齐了。
    while (ackThrough_ < total_ && state_[ackThrough_] == ChunkState::kAcked)
    {
        ++ackThrough_;
    }

    return newly;
}

void SendWindow::release(const std::vector<std::uint32_t> &chunks)
{
    for (auto idx : chunks)
    {
        if (idx < total_ && state_[idx] == ChunkState::kInflight)
        {
            state_[idx] = ChunkState::kPending;
        }
    }
}

std::uint32_t SendWindow::markRetransmit(const std::vector<std::uint32_t> &chunks)
{
    std::uint32_t marked = 0;
    for (auto idx : chunks)
    {
        // 只有"在飞"的块能被判丢。已确认的不该回退（那说明 ACK 有矛盾，
        // 而回退会让已经落盘的数据重传一遍，还可能覆盖接收方正在写的
        // 其他块）；从没发过的块本来就该发，不需要走到重传队列。
        if (idx < total_ && state_[idx] == ChunkState::kInflight)
        {
            state_[idx] = ChunkState::kNeedRetransmit;
            retransmitQueue_.push_back(idx);
            ++marked;
        }
    }
    return marked;
}

std::vector<std::uint32_t> SendWindow::outstanding() const
{
    // 只返回**在飞**的块，不是"所有未确认的块"。
    //
    // 理由：这个函数的唯一用途是"拿去过一遍重传定时器"，而只有发出去
    // 的东西才谈得上超时。把 kPending / kNeedRetransmit 也算进来会让
    // 调用方误以为它们已在路上，进而重复发送。
    std::vector<std::uint32_t> out;
    out.reserve(total_ - acked_);
    for (std::uint32_t i = 0; i < total_; ++i)
    {
        if (state_[i] == ChunkState::kInflight)
        {
            out.push_back(i);
        }
    }
    return out;
}

}  // namespace relayer::core::transfer
