#include "relayer/core/transfer/StateMachine.h"

namespace relayer::core::transfer {

std::string_view toString(State s) noexcept
{
    switch (s) {
    case State::kIdle:
        return "Idle";
    case State::kHandshaking:
        return "Handshaking";
    case State::kTransferring:
        return "Transferring";
    case State::kFinalizing:
        return "Finalizing";
    case State::kInterrupted:
        return "Interrupted";
    case State::kDone:
        return "Done";
    }
    return "unknown";
}

std::string_view toString(Event e) noexcept
{
    switch (e) {
    case Event::kStartHandshake:
        return "StartHandshake";
    case Event::kAuthOk:
        return "AuthOk";
    case Event::kAuthFail:
        return "AuthFail";
    case Event::kStartTransfer:
        return "StartTransfer";
    case Event::kAllChunksAcked:
        return "AllChunksAcked";
    case Event::kInterrupt:
        return "Interrupt";
    case Event::kVerifyOk:
        return "VerifyOk";
    case Event::kVerifyFail:
        return "VerifyFail";
    case Event::kResume:
        return "Resume";
    case Event::kAbort:
        return "Abort";
    }
    return "unknown";
}

const Transition* findTransition(State from, Event event) noexcept
{
    // 线性扫描。表只有 10 项，二分或哈希表在这个规模下只会让代码更难读。
    for (const auto& t : kTransitions) {
        if (t.from == from && t.event == event) {
            return &t;
        }
    }
    return nullptr;
}

bool isLegalTransition(State from, Event event) noexcept
{
    return findTransition(from, event) != nullptr;
}

StateMachine::StateMachine(State initial) noexcept : state_(initial)
{
}

bool StateMachine::canFire(Event event) const noexcept
{
    return isLegalTransition(state_, event);
}

Status StateMachine::fire(Event event) noexcept
{
    const Transition* t = findTransition(state_, event);
    if (t == nullptr) {
        // 消息里同时给出当前状态和事件，是为了让日志能直接回答
        // "在什么状态下收到了什么"——这是排查状态机问题唯一有用的信息。
        std::string msg;
        msg.reserve(48);
        msg.append("illegal transition: ");
        msg.append(toString(state_));
        msg.append(" --");
        msg.append(toString(event));
        msg.append("--> (no such transition)");
        return Status::Fail(ErrorCode::kProtocol, std::move(msg));
    }

    state_ = t->to;
    return Status::Ok();
}

void StateMachine::reset(State s) noexcept
{
    state_ = s;
}

}   // namespace relayer::core::transfer
