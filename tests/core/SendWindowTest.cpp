#include "relayer/core/transfer/SendWindow.h"

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::transfer;

// 窗口逻辑的 bug 有一个共同特征：**不会立刻报错**。
//
//   窗口算大  → 内存涨、对端不舒服，但传输成功
//   窗口算小  → 慢，但成功
//   ACK 算错  → 传完之后哈希不匹配
//   重传饥饿  → 卡在 99% 不动
//
// 前两种靠人眼能发现，后两种只能靠这里。所以本文件的重点全在
// "乱序 / 丢包 / 重复 ACK" 这三类输入上。

namespace
{

// 构造一个 ACK：ackThrough = through，其后 overrides 里为 true 的块视为已收到。
ChunkAck makeAck(std::uint32_t through, const std::vector<std::uint32_t> &receivedAfter)
{
    ChunkAck a;
    a.ackThrough = through;
    for (auto idx : receivedAfter)
    {
        if (idx >= through)
        {
            bitmapSet(a.bitmap, idx - through);
        }
    }
    return a;
}

}  // namespace

// ===========================================================================
// 基本推进
// ===========================================================================

TEST_CASE("empty transfer is immediately done", "[transfer][window]")
{
    SendWindow w(0, 4);
    REQUIRE(w.done());
    REQUIRE(w.takeNextBatch(10).empty());
    REQUIRE(w.freeSlots() == 0);  // 没有块要发，也就没有"空位"
}

TEST_CASE("takes chunks in order up to the window", "[transfer][window]")
{
    SendWindow w(10, 4);
    REQUIRE(w.pending() == 10);

    const auto batch = w.takeNextBatch(10);
    REQUIRE(batch == std::vector<std::uint32_t>{0, 1, 2, 3});
    REQUIRE(w.inflight() == 4);
    REQUIRE(w.pending() == 6);
    REQUIRE(w.freeSlots() == 0);

    // 窗口满了，再取拿不到东西——这就是背压的信号源。
    REQUIRE(w.takeNextBatch(10).empty());
}

TEST_CASE("ack advances window and frees slots", "[transfer][window]")
{
    SendWindow w(10, 4);
    auto batch = w.takeNextBatch(4);
    REQUIRE(batch.size() == 4);

    // 全部确认。
    const auto newly = w.applyAck(makeAck(4, {}));
    REQUIRE(newly == 4);
    REQUIRE(w.contiguousAcked() == 4);
    REQUIRE(w.acked() == 4);
    REQUIRE(w.freeSlots() == 4);

    // 可以继续取。
    const auto next = w.takeNextBatch(4);
    REQUIRE(next == std::vector<std::uint32_t>{4, 5, 6, 7});
}

TEST_CASE("takeNextBatch respects maxCount", "[transfer][window]")
{
    SendWindow w(100, 64);
    const auto b = w.takeNextBatch(3);
    REQUIRE(b == std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(w.inflight() == 3);
}

// ===========================================================================
// 乱序与空洞
// ===========================================================================

TEST_CASE("ack through stops at the first gap", "[transfer][window]")
{
    SendWindow w(10, 8);
    (void)w.takeNextBatch(8);  // 0..7 在飞

    // 收到 0,1，跳过 2，收到 3,4。
    const auto newly = w.applyAck(makeAck(2, {3, 4}));
    REQUIRE(newly == 4);
    REQUIRE(w.contiguousAcked() == 2);  // 2 还没到
    REQUIRE(w.acked() == 4);

    // 块 2 仍是 in-flight（还没被判丢）。
    REQUIRE(w.stateOf(2) == ChunkState::kInflight);
    REQUIRE(w.stateOf(3) == ChunkState::kAcked);
}

TEST_CASE("gap is filled and prefix jumps forward", "[transfer][window]")
{
    SendWindow w(10, 8);
    (void)w.takeNextBatch(8);

    (void)w.applyAck(makeAck(2, {3, 4}));
    REQUIRE(w.contiguousAcked() == 2);

    // 块 2 终于到了。连续前缀应当一路跳到 5，因为 3、4 早就在位图里确认过。
    (void)w.applyAck(makeAck(3, {4}));
    REQUIRE(w.contiguousAcked() == 5);
}

TEST_CASE("ack position uses the ack's own through as base", "[transfer][window]")
{
    // 这条测试专治一个隐蔽错误：位图的基准点。
    //
    // 位图第 i 位对应块 (ack.ackThrough + i)，不是"更新后的 ackThrough + i"。
    // 用错基准会让整段块错位——而"内容全对但位置错"只有最终哈希能发现。
    SendWindow w(20, 20);
    (void)w.takeNextBatch(20);

    // ackThrough = 2，位图位 2 => 块 4。
    ChunkAck a = makeAck(2, {});
    bitmapSet(a.bitmap, 2);
    (void)w.applyAck(a);

    REQUIRE(w.stateOf(4) == ChunkState::kAcked);
    REQUIRE(w.stateOf(2) != ChunkState::kAcked);  // 2 本身没收到
    REQUIRE(w.stateOf(6) != ChunkState::kAcked);
}

// ===========================================================================
// 重复 ACK
// ===========================================================================

TEST_CASE("duplicate ack does not double count", "[transfer][window]")
{
    // 重复 ACK 在丢包场景里很常见（接收方每个 ACK 都可能被重传两份。
    // 若 acked_ 用 += 累加，计数会虚高，done() 提前为真——表现为
    // "提前收尾、文件短一截"。
    SendWindow w(10, 4);
    (void)w.takeNextBatch(4);

    REQUIRE(w.applyAck(makeAck(4, {})) == 4);
    REQUIRE(w.acked() == 4);

    // 同一个 ACK 再来两次。
    REQUIRE(w.applyAck(makeAck(4, {})) == 0);
    REQUIRE(w.applyAck(makeAck(4, {})) == 0);
    REQUIRE(w.acked() == 4);
    REQUIRE_FALSE(w.done());
}

TEST_CASE("older ack does not roll back", "[transfer][window]")
{
    // 网络乱序时会收到"旧"ACK。它不该让已确认的块退回未确认——
    // 那会导致已落盘的数据被重传，还可能覆盖接收方正在写的其他块。
    SendWindow w(10, 8);
    (void)w.takeNextBatch(8);

    (void)w.applyAck(makeAck(5, {}));
    REQUIRE(w.contiguousAcked() == 5);

    // 迟到的旧 ACK：ackThrough = 2。
    REQUIRE(w.applyAck(makeAck(2, {})) == 0);
    REQUIRE(w.contiguousAcked() == 5);
    REQUIRE(w.acked() == 5);
}

TEST_CASE("ack through beyond total is clamped", "[transfer][window]")
{
    // 对方多报不该让我们越界。
    SendWindow w(3, 8);
    (void)w.takeNextBatch(3);
    (void)w.applyAck(makeAck(100, {}));

    REQUIRE(w.contiguousAcked() == 3);
    REQUIRE(w.done());
}

TEST_CASE("bitmap bits beyond total are ignored", "[transfer][window]")
{
    SendWindow w(4, 8);
    (void)w.takeNextBatch(4);

    ChunkAck a;
    a.ackThrough = 0;
    // 位 0 到 40 全置上，远超 4 块。
    a.bitmap.assign(2, 0xFFFFFFFFu);
    REQUIRE_NOTHROW(w.applyAck(a));
    REQUIRE(w.done());
}

// ===========================================================================
// 重传
// ===========================================================================

TEST_CASE("retransmit is preferred over new chunks", "[transfer][window]")
{
    // 这是重传饥饿的防线：新块不会让 ackThrough 前进，空洞会。
    // 顺序反过来会让窗口被"永远推不动的块"占满，表现为卡在 99%。
    SendWindow w(10, 4);
    (void)w.takeNextBatch(4);  // 0..3 在飞

    // 块 1 超时。
    REQUIRE(w.markRetransmit({1}) == 1);
    REQUIRE(w.stateOf(1) == ChunkState::kNeedRetransmit);

    // 空出一个位置：确认 0。
    (void)w.applyAck(makeAck(1, {}));
    REQUIRE(w.freeSlots() >= 1);

    // 取下一批：必须先是块 1（重传），然后才是新块。
    const auto batch = w.takeNextBatch(1);
    REQUIRE(batch == std::vector<std::uint32_t>{1});
}

TEST_CASE("retransmit does not apply to acked or pending chunks", "[transfer][window]")
{
    SendWindow w(10, 8);
    (void)w.takeNextBatch(4);          // 0..3 在飞
    (void)w.applyAck(makeAck(2, {}));  // 0,1 已确认

    // 已确认的块不该被判丢：那说明 ACK 自相矛盾，回退会让已经落盘的
    // 数据重传一遍。
    REQUIRE(w.markRetransmit({0}) == 0);
    REQUIRE(w.stateOf(0) == ChunkState::kAcked);

    // 从没发过的块也不该走重传队列（它本来就该发）。
    REQUIRE(w.markRetransmit({5}) == 0);
    REQUIRE(w.stateOf(5) == ChunkState::kPending);

    // 在飞的块可以。
    REQUIRE(w.markRetransmit({2}) == 1);
}

TEST_CASE("released chunks become pending again", "[transfer][window]")
{
    // 发送失败（比如网络刚断）时归还块，让它们还能被重新取走。
    // 不归还的话窗口会永久占用，传输就此停摆。
    SendWindow w(10, 4);
    auto batch = w.takeNextBatch(4);
    REQUIRE(batch.size() == 4);

    w.release(batch);
    REQUIRE(w.inflight() == 0);
    REQUIRE(w.pending() == 10);
    REQUIRE(w.freeSlots() == 4);

    const auto again = w.takeNextBatch(4);
    REQUIRE(again == std::vector<std::uint32_t>{0, 1, 2, 3});
}

TEST_CASE("release ignores chunks that are not in flight", "[transfer][window]")
{
    SendWindow w(4, 4);
    (void)w.takeNextBatch(2);  // 0,1
    (void)w.applyAck(makeAck(1, {}));

    // 块 0 已确认，释放它不该让它回到 pending。
    w.release({0, 99});
    REQUIRE(w.stateOf(0) == ChunkState::kAcked);
    REQUIRE(w.pending() == 2);  // 2,3
}

TEST_CASE("outstanding reports only in-flight chunks", "[transfer][window]")
{
    SendWindow w(10, 8);
    (void)w.takeNextBatch(3);          // 0,1,2 在飞
    (void)w.applyAck(makeAck(1, {}));  // 0 确认

    const auto o = w.outstanding();
    REQUIRE(o == std::vector<std::uint32_t>{1, 2});
}

// ===========================================================================
// 窗口容量
// ===========================================================================

TEST_CASE("zero window is clamped to one", "[transfer][window]")
{
    // 窗口为 0 会让传输永远无法推进，而表现是"卡住不报错"。
    // 夹到 1 至少能跑完（慢，但不会挂死）。
    SendWindow w(5, 0);
    REQUIRE(w.windowChunks() == 1);
    REQUIRE(w.takeNextBatch(10) == std::vector<std::uint32_t>{0});
}

TEST_CASE("window of one forces strict lockstep", "[transfer][window]")
{
    SendWindow w(3, 1);
    REQUIRE(w.takeNextBatch(1) == std::vector<std::uint32_t>{0});
    REQUIRE(w.takeNextBatch(1).empty());  // 必须等 ACK

    (void)w.applyAck(makeAck(1, {}));
    REQUIRE(w.takeNextBatch(1) == std::vector<std::uint32_t>{1});

    (void)w.applyAck(makeAck(2, {}));
    REQUIRE(w.takeNextBatch(1) == std::vector<std::uint32_t>{2});

    (void)w.applyAck(makeAck(3, {}));
    REQUIRE(w.done());
    REQUIRE(w.takeNextBatch(1).empty());
}

TEST_CASE("window larger than total is fine", "[transfer][window]")
{
    SendWindow w(3, 100);
    const auto b = w.takeNextBatch(100);
    REQUIRE(b.size() == 3);

    // 3 块全部在飞，没块可发了。窗口还有 97 个空位，但 freeSlots() 报 0
    // ——它同时受"剩余未确认块数"约束。报 97 的话调用方会以为还有活要干。
    REQUIRE(w.freeSlots() == 0);

    (void)w.applyAck(makeAck(3, {}));
    REQUIRE(w.done());
    REQUIRE(w.freeSlots() == 0);
}

TEST_CASE("freeSlots is bounded by remaining chunks", "[transfer][window]")
{
    // 这条单独固化上面的语义：窗口空着，但没块可发时 freeSlots 必须是 0。
    //
    // 少了这个上界，一块 1MB 的文件配 4MB 窗口就会一直报 1/2/3，
    // 调用方据此空转——表现为"传输完成了但调度器还在跑"。
    SendWindow w(2, 16);
    REQUIRE(w.freeSlots() == 2);  // 只剩 2 块，窗口再大也只有 2

    (void)w.takeNextBatch(2);
    REQUIRE(w.freeSlots() == 0);
}

// ===========================================================================
// 完整流程
// ===========================================================================

TEST_CASE("full transfer with a lost chunk completes", "[transfer][window]")
{
    // 模拟：20 块、窗口 5、块 7 的**第一次**发送丢失（后续重传能到）。
    //
    // 这个模拟刻意把"发送"和"到达"分开：takeNextBatch 只是把块交给网络，
    // 到不到是另一回事。混在一起（取到就算到达）会让丢包永远修不好——
    // 因为块一旦被算作到达，就轮不到重传了，循环空转到上限。
    constexpr std::uint32_t kTotal = 20;
    constexpr std::uint32_t kWindow = 5;

    SendWindow w(kTotal, kWindow);
    std::vector<bool> received(kTotal, false);
    std::vector<bool> firstAttempt(kTotal, false);

    bool droppedOnce = false;
    int rounds = 0;
    const int kMaxRounds = 1000;  // 防死循环：真挂住时给个明确失败而不是卡住

    while (!w.done() && rounds < kMaxRounds)
    {
        ++rounds;

        // 1. 发送方尽量填满窗口，把块交给网络。
        const auto batch = w.takeNextBatch(kWindow);

        // 2. 网络：块 7 的第一次传输丢失，之后的重传正常到达。
        std::vector<std::uint32_t> lost;
        for (auto idx : batch)
        {
            if (idx == 7 && !firstAttempt[7] && !droppedOnce)
            {
                firstAttempt[7] = true;
                droppedOnce = true;
                lost.push_back(idx);
                continue;
            }
            received[idx] = true;
        }

        // 3. 丢掉的块立刻判丢（真实实现里由重传定时器驱动，这里直接触发
        //    以保持模拟可复现）。判丢后它会回到重传队列，优先被再次取走。
        if (!lost.empty())
        {
            REQUIRE(w.markRetransmit(lost) == lost.size());
        }

        // 4. 接收方回累积 ACK + SACK 位图。
        (void)w.applyAck(buildAck(received, kTotal));
    }

    REQUIRE(rounds < kMaxRounds);
    REQUIRE(droppedOnce);  // 模拟确实制造了一次丢包，否则这条测试没验证到东西
    REQUIRE(w.done());
    REQUIRE(w.acked() == kTotal);

    // 最终内容必须齐全。
    for (std::uint32_t i = 0; i < kTotal; ++i)
    {
        INFO("chunk " << i);
        REQUIRE(received[i]);
    }
}

TEST_CASE("pending chunks after a gap are still taken", "[transfer][window]")
{
    // 乱序 ACK 会在 ackThrough 之后留下已确认的块。扫新块时若在第一个
    // 非 kPending 的块上停住，那个空洞就会挡住后面所有块——窗口明明还有
    // 空位，却发不出东西。
    SendWindow w(8, 8);
    (void)w.takeNextBatch(8);  // 0..7 全部在飞

    // 只有块 0 和 2 到了，块 1 还没。于是 3..7 仍在飞。
    (void)w.applyAck(makeAck(1, {2}));
    REQUIRE(w.contiguousAcked() == 1);
    REQUIRE(w.stateOf(2) == ChunkState::kAcked);
    REQUIRE(w.stateOf(3) == ChunkState::kInflight);

    // 归还块 1 和 3：让它们回到 kPending，其中块 1 在 ackThrough 之后
    // 第一个位置——正是"空洞"。
    w.release({1, 3});

    // 扫描必须越过已确认的块 2，才能取到块 3。若停在第一个非 kPending
    // 的块（块 2），这里只会拿到 {1}。
    const auto batch = w.takeNextBatch(8);
    REQUIRE(batch == std::vector<std::uint32_t>{1, 3});
}

TEST_CASE("out of order arrival converges without retransmit", "[transfer][window]")
{
    // 全都不丢，只是乱序：不该触发任何重传。
    constexpr std::uint32_t kTotal = 16;
    SendWindow w(kTotal, 8);
    std::vector<bool> received(kTotal, false);

    // 第 1 轮发 0..7，但只让偶数块"到达"。
    auto b1 = w.takeNextBatch(8);
    REQUIRE(b1.size() == 8);
    for (auto idx : b1)
    {
        if (idx % 2 == 0)
        {
            received[idx] = true;
        }
    }
    (void)w.applyAck(buildAck(received, kTotal));
    REQUIRE(w.contiguousAcked() == 1);  // 只有块 0

    // 奇数块迟到。
    for (std::uint32_t i = 1; i < 8; i += 2)
    {
        received[i] = true;
    }
    (void)w.applyAck(buildAck(received, kTotal));
    REQUIRE(w.contiguousAcked() == 8);

    // 后 8 块正常走完。
    auto b2 = w.takeNextBatch(8);
    REQUIRE(b2 == std::vector<std::uint32_t>{8, 9, 10, 11, 12, 13, 14, 15});
    for (auto idx : b2)
    {
        received[idx] = true;
    }
    (void)w.applyAck(buildAck(received, kTotal));

    REQUIRE(w.done());
    REQUIRE(w.needRetransmit() == 0);
}
