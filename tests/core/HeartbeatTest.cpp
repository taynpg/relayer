#include "relayer/core/transport/Heartbeat.h"

#include <chrono>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::transport;

namespace
{

// 虚构的时间起点。所有测试都基于它做偏移，不依赖真实时钟。
const heartbeat::TimePoint kT0 = heartbeat::TimePoint{} + std::chrono::hours(1);

heartbeat::TimePoint at(int seconds)
{
    return kT0 + std::chrono::seconds(seconds);
}

HeartbeatPolicy fresh()
{
    HeartbeatPolicy p;
    p.reset(kT0);
    return p;
}

}  // namespace

// ===========================================================================
// 常量
// ===========================================================================
//
// 这些数字会写进 PROTOCOL.md §8.5，钉死在这里防止"顺手调一下"。

TEST_CASE("heartbeat constants are pinned", "[heartbeat]")
{
    REQUIRE(heartbeat::kIdleBeforePing == std::chrono::seconds(20));
    REQUIRE(heartbeat::kPingTimeout == std::chrono::seconds(10));
    REQUIRE(heartbeat::kTickInterval == std::chrono::milliseconds(1000));

    // 超时必须明显大于一次往返，否则网络抖一下就误杀。
    // 反过来，最坏发现时间（空闲 + 超时）应当在一分钟以内。
    REQUIRE(heartbeat::kPingTimeout < heartbeat::kIdleBeforePing);
    REQUIRE(heartbeat::kIdleBeforePing + heartbeat::kPingTimeout < std::chrono::seconds(60));

    // 检查粒度必须远小于超时，否则超时误差会大到不可接受
    REQUIRE(heartbeat::kTickInterval * 10 <= heartbeat::kPingTimeout);
}

// ===========================================================================
// 基本时序
// ===========================================================================

TEST_CASE("no action while recently active", "[heartbeat]")
{
    auto p = fresh();
    for (int s = 0; s < 19; ++s)
    {
        p.onBytesReceived(at(s));
        REQUIRE(p.check(at(s)) == HeartbeatAction::kNone);
    }
}

TEST_CASE("ping is sent after idle threshold", "[heartbeat]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);

    // 19 秒仍不动作
    REQUIRE(p.check(at(19)) == HeartbeatAction::kNone);
    REQUIRE_FALSE(p.pingOutstanding());

    // 20 秒该发
    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);
    REQUIRE(p.pingOutstanding());
    REQUIRE(p.pingsSent() == 1);
    REQUIRE(p.lastPingSent() == at(20));
}

TEST_CASE("no repeated ping while one is outstanding", "[heartbeat]")
{
    // 一个 ping 还在路上时再发一个，只会让对端收到重复心跳，
    // 而我们需要的判断信息完全一样。
    auto p = fresh();
    p.onBytesReceived(kT0);

    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);
    REQUIRE(p.check(at(21)) == HeartbeatAction::kNone);
    REQUIRE(p.check(at(25)) == HeartbeatAction::kNone);
    REQUIRE(p.check(at(29)) == HeartbeatAction::kNone);
    REQUIRE(p.pingsSent() == 1);
}

TEST_CASE("timeout fires when ping is not answered", "[heartbeat]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);

    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);

    // 29 秒还没到 10 秒超时
    REQUIRE(p.check(at(29)) == HeartbeatAction::kNone);
    // 30 秒正好 10 秒
    REQUIRE(p.check(at(30)) == HeartbeatAction::kTimedOut);
    REQUIRE(p.timeoutsDetected() == 1);
}

TEST_CASE("receiving bytes clears the outstanding ping", "[heartbeat]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);

    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);
    REQUIRE(p.pingOutstanding());

    // 对端回了一个心跳应答（或任何帧）
    p.onBytesReceived(at(22));
    REQUIRE_FALSE(p.pingOutstanding());

    // 之后 10 秒内不会超时
    REQUIRE(p.check(at(30)) == HeartbeatAction::kNone);
    REQUIRE(p.check(at(31)) == HeartbeatAction::kNone);
    REQUIRE(p.timeoutsDetected() == 0);
}

TEST_CASE("a live peer never times out", "[heartbeat]")
{
    // 模拟 10 分钟的空闲连接：对端每 25 秒回一次心跳。
    // 永远不该判超时。
    auto p = fresh();
    int pings = 0;
    for (int s = 0; s <= 600; ++s)
    {
        const auto action = p.check(at(s));
        if (action == HeartbeatAction::kSendPing)
        {
            ++pings;
            // 模拟对端 2 秒后回应
            p.onBytesReceived(at(s + 2));
        }
        REQUIRE(action != HeartbeatAction::kTimedOut);
    }
    // 600 秒 / 25 秒一轮，大约 24 次
    REQUIRE(pings >= 20);
    REQUIRE(p.pingsSent() == static_cast<std::uint64_t>(pings));
    REQUIRE(p.timeoutsDetected() == 0);
}

// ===========================================================================
// 边界
// ===========================================================================

TEST_CASE("timeout is detected exactly at the boundary", "[heartbeat][boundary]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);
    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);

    // 差 1 毫秒不算超时
    const auto justBefore = at(20) + std::chrono::seconds(10) - std::chrono::milliseconds(1);
    REQUIRE(p.check(justBefore) == HeartbeatAction::kNone);

    // 正好 10 秒算超时（用 >= 而不是 >，避免"恰好卡在边界永远不触发"）
    REQUIRE(p.check(at(20) + std::chrono::seconds(10)) == HeartbeatAction::kTimedOut);
}

TEST_CASE("idle threshold is inclusive", "[heartbeat][boundary]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);

    REQUIRE(p.check(at(20) - std::chrono::milliseconds(1)) == HeartbeatAction::kNone);
    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);
}

TEST_CASE("check before reset does not fire immediately", "[heartbeat][boundary]")
{
    // 没有 reset 的话 lastReceived_ 是 epoch（0），任何 now 都会
    // 显得"空闲了很久"。这是个真实的易错点：构造后必须 reset。
    HeartbeatPolicy p;
    p.reset(kT0);
    REQUIRE(p.check(at(0)) == HeartbeatAction::kNone);
}

TEST_CASE("repeated checks at the same moment are stable", "[heartbeat]")
{
    auto p = fresh();
    p.onBytesReceived(kT0);

    // 第一次要求发 ping
    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);
    // 同一时刻再问，仍是"等回应"
    REQUIRE(p.check(at(20)) == HeartbeatAction::kNone);
    REQUIRE(p.pingsSent() == 1);
}

// ===========================================================================
// 业务帧也算生命迹象
// ===========================================================================

TEST_CASE("business traffic suppresses the ping", "[heartbeat]")
{
    // 正在传文件的连接上不该发心跳：数据本身就在证明对方活着。
    auto p = fresh();
    for (int s = 0; s < 100; ++s)
    {
        p.onBytesReceived(at(s));  // 每秒都有业务数据
        REQUIRE(p.check(at(s)) == HeartbeatAction::kNone);
    }
    REQUIRE(p.pingsSent() == 0);
}

TEST_CASE("ping does not reset the idle clock on its own", "[heartbeat]")
{
    // 发 ping 是出站动作，不能当作"连接还健康"的证据。
    // 如果发 ping 也重置 lastReceived_，就永远不会超时了。
    auto p = fresh();
    p.onBytesReceived(kT0);
    REQUIRE(p.check(at(20)) == HeartbeatAction::kSendPing);

    // 即使我们继续"发"了 ping，count 也不该因为重置而清零
    REQUIRE(p.pingsSent() == 1);
    REQUIRE(p.check(at(30)) == HeartbeatAction::kTimedOut);
}

// ===========================================================================
// 名称
// ===========================================================================

TEST_CASE("heartbeat action names are stable", "[heartbeat]")
{
    REQUIRE(toString(HeartbeatAction::kNone) == "none");
    REQUIRE(toString(HeartbeatAction::kSendPing) == "send_ping");
    REQUIRE(toString(HeartbeatAction::kTimedOut) == "timed_out");
}
