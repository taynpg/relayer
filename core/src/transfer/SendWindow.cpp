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
    // 这个函数回答的是"takeNextBatch 现在最多能取出多少块"，必须与
    // takeNextBatch 的实际规则逐条对齐——否则调用方（hasWork）会据它做出
    // "没活干"的判断，而实际上还有块可取，传输就无声停住了。
    //
    // 取出规则分两段，配额也分两段：
    //
    //   重传段：不受窗口约束。kNeedRetransmit 本来就占着槽，取出来只是
    //           沿用它，不消耗新配额。这一段能取的量 = needRetransmit()。
    //   新块段：受窗口约束。它们要**新占**槽位，能取的量 = 窗口剩余空位。
    //
    // 历史上这里只算后者（旧实现的 byWindow 夹法），于是
    // "inflight=0 且 needRetransmit=5 而 window=4" 时返回 0 —— 而那时
    // 恰恰有 5 块可取。直接后果是 32MB/1MB 块的传输必定停在第 27MB。
    // 详见 takeNextBatch 里的长注释。
    //
    // 窗口剩余空位按 inUse 计数（inflight + needRetransmit）。used 可能
    // 大于 window（重传判定能让它越界），此时夹到 0，新块不发——这是对的：
    // 先把已经在窗口里的那些重传完。

    const auto retransmittable = needRetransmit();

    std::uint32_t used = 0;
    for (std::uint32_t i = 0; i < total_; ++i)
    {
        if (inUse(i))
        {
            ++used;
        }
    }
    const auto byWindow = used >= window_ ? 0 : window_ - used;

    // 新块还能取多少：窗口空位与"真的还有未发的块"取小。
    //
    // 少了后者会怎样：一块 1MB 的文件配 4MB 窗口，全部发完之后 freeSlots()
    // 会一直报 1/2/3 —— 窗口没满，但没块可发。调用方据此认为"还有活要干"，
    // 于是空转。
    const auto newAvailable = std::min(byWindow, pending());

    return retransmittable + newAvailable;
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

    // 先补空洞，**这一段的配额与窗口无关**。
    //
    // 这里踩过一个致命的坑，值得写清楚：
    //
    //   原先的实现是 budget = min(maxCount, freeSlots())，然后重传段也用
    //   这个 budget 卡。而窗口约束是 inflight + needRetransmit <= window。
    //   于是当一批块同时超时（3 秒内 5 块，而窗口只有 4），状态变成
    //
    //       inflight = 0, needRetransmit = 5  →  used(5) > window(4)
    //       byWindow = max(0, 4-5) = 0        →  freeSlots() = 0
    //
    //   取重传块要 freeSlots > 0，而 freeSlots 要 used <= window，而 used
    //   恰好就是这些取不出来的重传块——**自锁**。另一头 checkRetransmit
    //   只扫 outstanding()（kInflight），也看不见它们。两头都以为没活干。
    //
    //   实测症状：32MB 配 1MB 块，必定停在第 27MB（acked=26/31, out=0），
    //   一直挂到 hard timeout。而 8MB 只发 8 块、窗口 4 块，凑不出
    //   "同时超时数 > 窗口"这个条件，所以小文件全过——这个缺陷只在
    //   "文件足够大 + 块足够大 + 一批块集中超时"时出现。
    //
    // 正确的模型：重传块**本来就在窗口里占着槽**，把 kNeedRetransmit 取出来
    // 变回 kInflight 是"沿用原来的槽"，不消耗新配额。所以配额只该管新块。
    //
    // 顺序仍然重要：新块不会让 ackThrough 前进，只有空洞被填上才会。若先
    // 发新块，窗口会被"永远得不到确认推进"的块占满——经典的重传饥饿。
    std::size_t consumed = 0;
    while (consumed < retransmitQueue_.size() && out.size() < maxCount)
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

    // 再发新块。新块才受窗口约束——它们要**新占**槽位。
    //
    // 重传段已经用掉了一些配额，剩下的才是新块能用的。freeSlots() 在重传段
    // 跑完之后重新算：那一段把 kNeedRetransmit 变成了 kInflight，inUse 的
    // 判定两边都算，所以 used 不变——这正说明重传没有额外占用窗口。
    if (out.size() >= maxCount)
    {
        return out;
    }

    const auto newBudget = std::min(maxCount - static_cast<std::uint32_t>(out.size()), freeSlots());
    if (newBudget == 0)
    {
        return out;
    }

    // 从 ackThrough 开始扫而不是从 0 开始：前缀之前的块一定已确认。
    //
    // 但**不能停在第一个非 kPending 的块**——乱序 ACK 会在前缀之后留下
    // 已确认的块，跳过它们继续往后才是对的，否则每遇到一个空洞就少发一批。
    std::uint32_t taken = 0;
    for (std::uint32_t i = ackThrough_; i < total_ && taken < newBudget; ++i)
    {
        if (state_[i] == ChunkState::kPending)
        {
            state_[i] = ChunkState::kInflight;
            out.push_back(i);
            ++taken;
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
