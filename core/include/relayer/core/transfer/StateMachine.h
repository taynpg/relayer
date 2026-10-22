#pragma once

#include <cstddef>
#include <string_view>

#include "relayer/common/Status.h"

// 传输会话状态机（PROTOCOL.md §8）。
//
// 这个文件只做一件事：把"哪些 (状态, 事件) 组合是合法的"写成一张表，并
// 提供一个 fire() 去查它。没有 I/O、没有线程、没有回调——它甚至不知道
// "传输"是什么东西，只知道状态名。
//
// 为什么值得单独抽出来：
//
//   状态机的 bug 几乎全是"漏了某个组合"或"某个非法组合没拦住"。这类问题
//   用代码审查很难发现——人眼扫一行 switch 很容易漏掉一个 case。写成表之后，
//   测试可以遍历 6 种状态 × 9 种事件的完整矩阵，逐个断言"表里写的都成功、
//   表里没写的都失败"。漏一个组合会立刻变成红测试。
//
//   另一个好处是：这张表就是 PROTOCOL.md §8.2 那张表的可执行版本。文档改了
//   而代码没改，工具能查出来（tools/check_transition_doc.py）。

namespace relayer::core::transfer
{

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
//
// 与 PROTOCOL.md §8.1 一一对应。数值不稳定，**不要**序列化到线上或日志里
// 依赖数字：日志用 toString()。

enum class State
{
    kIdle,          // 连接已建立，未开始任何会话
    kHandshaking,   // 认证握手中
    kTransferring,  // 正在传数据块
    kFinalizing,    // 数据发完，等最终校验与落盘确认
    kInterrupted,   // 中断（网络/取消/对端消失），可续传
    kDone,          // 成功完成；也用于"作废"
};

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

enum class Event
{
    kStartHandshake,  // Idle -> Handshaking
    kAuthOk,          // 握手通过
    kAuthFail,        // 握手失败 -> 作废
    kStartTransfer,   // 开始传数据
    kAllChunksAcked,  // 所有块都已确认 -> 进入收尾
    kInterrupt,       // 中断 -> 可续传
    kVerifyOk,        // 收尾校验通过 -> 完成
    kVerifyFail,      // 收尾校验失败 -> 可续传
    kResume,          // 从 Interrupted 恢复
    kAbort,           // 放弃 -> 完成（作废）
};

// 遍历用的边界常量。加新状态/事件时必须同步更新这两个值和 kTransitionCount
// 的期望值，否则 tests/core/StateMachineTest.cpp 的完整性测试会失败——这是
// 刻意的：漏更新等于漏测试。
namespace detail
{
inline constexpr std::size_t kStateCount = 6;
inline constexpr std::size_t kEventCount = 10;
}  // namespace detail

// 稳定名称，用于日志与文档比对。未定义值返回 "unknown"。
[[nodiscard]] std::string_view toString(State s) noexcept;
[[nodiscard]] std::string_view toString(Event e) noexcept;

// 是否终态。终态之后任何事件都应被拒——包括看起来"合理"的 Resume：
// 一次已经作废的传输不该因为一条迟到的 Resume 又活过来。
//
// kDone 是唯一的终态。kInterrupted 不是终态（它可以 Resume 或 Abort）。
[[nodiscard]] constexpr bool isTerminal(State s) noexcept
{
    return s == State::kDone;
}

// ---------------------------------------------------------------------------
// 转移表
// ---------------------------------------------------------------------------

struct Transition
{
    State from;
    Event event;
    State to;
};

// 权威转移表。内容必须与 PROTOCOL.md §8.2 完全一致。
//
// 顺序按"从哪个状态出发"分组，便于人工核对。
inline constexpr Transition kTransitions[] = {
    // Idle
    {State::kIdle, Event::kStartHandshake, State::kHandshaking},
    {State::kIdle, Event::kStartTransfer, State::kTransferring},

    // Handshaking
    {State::kHandshaking, Event::kAuthOk, State::kIdle},
    {State::kHandshaking, Event::kAuthFail, State::kDone},

    // Transferring
    {State::kTransferring, Event::kAllChunksAcked, State::kFinalizing},
    {State::kTransferring, Event::kInterrupt, State::kInterrupted},

    // Finalizing
    {State::kFinalizing, Event::kVerifyOk, State::kDone},
    {State::kFinalizing, Event::kVerifyFail, State::kInterrupted},

    // Interrupted
    {State::kInterrupted, Event::kResume, State::kTransferring},
    {State::kInterrupted, Event::kAbort, State::kDone},

    // Done：无出边
};

inline constexpr std::size_t kTransitionCount = sizeof(kTransitions) / sizeof(kTransitions[0]);

// 查表。找到返回对应转移（指针指向 kTransitions 里的项），否则返回 nullptr。
//
// 返回指针而不是 optional<State>，是为了让调用方在日志里能同时拿到 from/to
// 和事件，而不必再拼一次。
[[nodiscard]] const Transition *findTransition(State from, Event event) noexcept;

// 等价于 findTransition(...) != nullptr。
[[nodiscard]] bool isLegalTransition(State from, Event event) noexcept;

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

class StateMachine
{
public:
    explicit StateMachine(State initial = State::kIdle) noexcept;

    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }

    // 施加一个事件。
    //
    // 合法转移：state_ 更新为新状态，返回成功。
    // 非法转移：state_ **不变**，返回 kProtocol 错误且 message 里带
    //           "from -> event 非法"这样的描述。
    //
    // 为什么非法时状态不变而不是进一个 Failed 状态：状态机的读者需要知道
    // "当前处在哪一步"才能做诊断。把状态改成 Failed 会丢掉这个信息，而
    // 上层的处理（断开连接）本来就不依赖状态机内部状态。
    [[nodiscard]] Status fire(Event event) noexcept;

    // 只判断不施加。用于"这个事件我现在能不能处理"这类预检查。
    [[nodiscard]] bool canFire(Event event) const noexcept;

    // 回到初始状态。用于同一个连接上开新会话（§8.2 里 AuthOk 回到 Idle 之后
    // 可以再开传输；一个连接跑完多次传输是正常的）。
    void reset(State s = State::kIdle) noexcept;

private:
    State state_;
};

}  // namespace relayer::core::transfer
