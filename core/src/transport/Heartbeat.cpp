#include "relayer/core/transport/Heartbeat.h"

namespace relayer::core::transport
{

std::string_view toString(HeartbeatAction a) noexcept
{
    switch (a)
    {
    case HeartbeatAction::kNone:
        return "none";
    case HeartbeatAction::kSendPing:
        return "send_ping";
    case HeartbeatAction::kTimedOut:
        return "timed_out";
    }
    return "unknown";
}

void HeartbeatPolicy::onBytesReceived(heartbeat::TimePoint now) noexcept
{
    lastReceived_ = now;

    // 收到任何字节都清掉"未应答"标记。
    //
    // 是不是太宽松了？假设对端只发业务帧、从不回应心跳——那我们的 ping
    // 就永远不会被"应答"，但对方显然还活着。这时判超时是错的。
    // 心跳的目的是检测"对方是否还有任何反应"，不是"对方是否遵循心跳协议"。
    // 所以任何入站字节都算数。
    pingOutstanding_ = false;
}

void HeartbeatPolicy::reset(heartbeat::TimePoint now) noexcept
{
    lastReceived_ = now;
    lastPingSent_ = now;
    pingOutstanding_ = false;
}

HeartbeatAction HeartbeatPolicy::check(heartbeat::TimePoint now) noexcept
{
    // --- 第一步：未应答的 ping 是否已经超时 ---
    //
    // 必须先于"该不该发新 ping"判断。写反的话，已经超时的连接上会继续
    // 发新的 ping（因为"空闲超过 20 秒"的条件一直成立），把问题掩盖成
    // "一直在重试"，而永远不会断开。
    if (pingOutstanding_)
    {
        if (now - lastPingSent_ >= heartbeat::kPingTimeout)
        {
            ++timeoutsDetected_;
            // 不在这里清 pingOutstanding_：超时是终态，调用方会断开连接。
            // 清掉它会让后续的 check() 又走回"该发 ping"分支。
            return HeartbeatAction::kTimedOut;
        }
        // 还没超时，安静等待回应。注意这里**不**重复发 ping——
        // 一个未应答的 ping 还在路上时再发一个，只会让对端收到一串
        // 重复心跳，而我们要的信息完全一样。
        return HeartbeatAction::kNone;
    }

    // --- 第二步：空闲够久了，该探一下 ---
    if (now - lastReceived_ >= heartbeat::kIdleBeforePing)
    {
        lastPingSent_ = now;
        pingOutstanding_ = true;
        ++pingsSent_;
        return HeartbeatAction::kSendPing;
    }

    return HeartbeatAction::kNone;
}

}  // namespace relayer::core::transport
