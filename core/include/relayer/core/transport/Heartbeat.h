#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "relayer/common/Status.h"

// 心跳与空闲超时策略（PROTOCOL.md §8.5）。
//
// 这里**只做判断，不做 I/O**：输入是"现在几点"和"最近一次收到/发出字节是几点"，
// 输出是"该发心跳了 / 该判超时了 / 什么都不用做"。
//
// 为什么要把时间决策抽成纯函数：
//
//   超时逻辑本质上是对时间轴的判断，而测试它需要"快进时间"。如果把
//   steady_clock::now() 散在定时器回调里，测试就只能真的 sleep 20 秒——
//   那会让测试变得又慢又不稳定（CI 上偶发失败的头号来源）。
//
//   抽出来之后，测试可以传任意虚构的时间点，把 30 分钟的行为在微秒内跑完。
//
// 为什么要双向心跳而不是只靠 TCP keepalive：
//
//   TCP keepalive 默认 2 小时才报死，而且只检测"连接是否被中间设备丢弃"，
//   检测不到"对端进程卡死但 socket 还开着"。文件传输里对端卡住十秒就可能
//   意味着一整个 4MB 块白传，需要更早发现。
//
//   但也不能太激进：传输大块数据时对端可能长时间只写不读，此时"我们收不到
//   数据"是正常的。所以超时判定看的是**对端是否还有任何生命迹象**，
//   而不是"有没有业务数据"——心跳帧本身就是生命迹象。

namespace relayer::core::transport
{

namespace heartbeat
{
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// 空闲多久之后开始发心跳。
//
// 20 秒的依据：正常空闲连接（用户开着客户端不操作）上，20 秒发一次
// 空帧的开销可以忽略（帧头 36 字节）；而用户感知的"连接断了"通常
// 需要 30 秒以上才注意到，所以要在那之前发现。
inline constexpr std::chrono::seconds kIdleBeforePing{20};

// 发心跳之后等多久算超时。
//
// 这个值必须明显大于"一次往返 + 对端处理"的时间，否则网络抖一下就误杀。
// 10 秒足够覆盖跨洲 RTT（约 300ms）加对端调度延迟，同时对"对端已死"
// 的检测也不算慢：最坏情况 20 + 10 = 30 秒发现。
inline constexpr std::chrono::seconds kPingTimeout{10};

// 定时器检查间隔。
//
// 不是"20 秒就设个 20 秒定时器"：那样在收到数据后需要重新设定时器，
// 频繁重置定时器本身有成本，而且容易漏。改成固定间隔轮询 + 时间戳比较，
// 逻辑更简单，也不受"重置定时器"这类操作影响。
//
// 1 秒的粒度相对于 20 秒的超时是充分的。
inline constexpr std::chrono::milliseconds kTickInterval{1000};
}  // namespace heartbeat

// 一次检查的结论。
enum class HeartbeatAction
{
    kNone,      // 什么都不用做
    kSendPing,  // 该发心跳了（发完 pingSentAt 会被记录）
    kTimedOut,  // 对端失去响应，应当断开
};

[[nodiscard]] std::string_view toString(HeartbeatAction a) noexcept;

// 心跳状态机。
//
// 用法：每隔 kTickInterval 调一次 check(now)，按返回的动作行事。
// 收到任何字节时调 onBytesReceived(now)——**任何帧都算生命迹象**，
// 包括对端发来的心跳应答，也包括业务帧。
class HeartbeatPolicy
{
public:
    // 记录"收到字节"的时刻。任何入站数据都要调它。
    //
    // 为什么要单独区分"收到"和"发出"：
    //   发出的心跳没有任何意义——只有对方的回应才能证明对方还活着。
    //   如果只跟踪"最后活动"，我们会因为自己一直在发而永远不超时，
    //   哪怕对端早就死了。
    void onBytesReceived(heartbeat::TimePoint now) noexcept;

    // 检查当前该做什么。
    //
    // 判定顺序很重要，写反了会出错：
    //   1. 先看有没有未应答的 ping——如果有且超过 kPingTimeout，判超时。
    //      这一步必须在"该不该发新 ping"之前：否则已经超时了还会继续
    //      发新的 ping，掩盖问题。
    //   2. 再看空闲是否超过 kIdleBeforePing，是则要求发 ping。
    [[nodiscard]] HeartbeatAction check(heartbeat::TimePoint now) noexcept;

    // 上一次成功发出 ping 的时刻（测试与诊断用）
    [[nodiscard]] heartbeat::TimePoint lastPingSent() const noexcept
    {
        return lastPingSent_;
    }
    [[nodiscard]] bool pingOutstanding() const noexcept
    {
        return pingOutstanding_;
    }
    [[nodiscard]] std::uint64_t pingsSent() const noexcept
    {
        return pingsSent_;
    }
    [[nodiscard]] std::uint64_t timeoutsDetected() const noexcept
    {
        return timeoutsDetected_;
    }

    // 手动指定初始的"最后收到时间"。构造后立即设置，用于避免"刚连上就
    // 因为 lastReceived_ 是 epoch 而立刻判空闲"。
    void reset(heartbeat::TimePoint now) noexcept;

private:
    heartbeat::TimePoint lastReceived_{};
    heartbeat::TimePoint lastPingSent_{};
    bool pingOutstanding_ = false;
    std::uint64_t pingsSent_ = 0;
    std::uint64_t timeoutsDetected_ = 0;
};

}  // namespace relayer::core::transport
