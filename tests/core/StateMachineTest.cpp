#include "relayer/core/transfer/StateMachine.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::transfer;

namespace
{

// 全部状态的显式列表。用列表而不是"从 0 遍历到 kStateCount"，
// 是因为枚举没有保证连续，而漏掉一个状态会让"全覆盖"测试静默失去意义。
const State kAllStates[] = {
    State::kIdle, State::kHandshaking, State::kTransferring, State::kFinalizing, State::kInterrupted, State::kDone,
};

const Event kAllEvents[] = {
    Event::kStartHandshake, Event::kAuthOk,   Event::kAuthFail,   Event::kStartTransfer, Event::kAllChunksAcked,
    Event::kInterrupt,      Event::kVerifyOk, Event::kVerifyFail, Event::kResume,        Event::kAbort,
};

// PROTOCOL.md §8.2 的表格，逐行抄写。
//
// 刻意**不**从 kTransitions 生成：如果从同一份数据生成，测试就退化成
// "表和自己相等"，永远不可能失败。手抄一遍，让文档与代码的差异变成红测试。
struct DocRow
{
    State from;
    Event event;
    State to;
};

const DocRow kDocumented[] = {
    {State::kIdle, Event::kStartHandshake, State::kHandshaking},
    {State::kHandshaking, Event::kAuthOk, State::kIdle},
    {State::kHandshaking, Event::kAuthFail, State::kDone},
    {State::kIdle, Event::kStartTransfer, State::kTransferring},
    {State::kTransferring, Event::kAllChunksAcked, State::kFinalizing},
    {State::kTransferring, Event::kInterrupt, State::kInterrupted},
    {State::kFinalizing, Event::kVerifyOk, State::kDone},
    {State::kFinalizing, Event::kVerifyFail, State::kInterrupted},
    {State::kInterrupted, Event::kResume, State::kTransferring},
    {State::kInterrupted, Event::kAbort, State::kDone},
};

}  // namespace

// ===========================================================================
// 表的结构
// ===========================================================================

TEST_CASE("transition table has no duplicate (from, event) pairs", "[sm][table]")
{
    // 重复项意味着"同一个组合有两个目标状态"，findTransition 返回哪个取决于
    // 表的顺序——这种不确定性在状态机里是不可接受的。
    std::set<std::pair<int, int>> seen;
    for (const auto &t : kTransitions)
    {
        const auto key = std::make_pair(static_cast<int>(t.from), static_cast<int>(t.event));
        REQUIRE(seen.insert(key).second);
    }
    REQUIRE(seen.size() == kTransitionCount);
}

TEST_CASE("every transition points at a known state", "[sm][table]")
{
    for (const auto &t : kTransitions)
    {
        bool fromOk = false;
        bool toOk = false;
        for (const auto s : kAllStates)
        {
            if (s == t.from)
            {
                fromOk = true;
            }
            if (s == t.to)
            {
                toOk = true;
            }
        }
        REQUIRE(fromOk);
        REQUIRE(toOk);
    }
    // 表里恰好 10 条（与 §8.2 的行数一致）
    REQUIRE(kTransitionCount == 10);
}

TEST_CASE("count constants match the enumerators", "[sm][table]")
{
    // kStateCount / kEventCount 只被测试用来做完整性断言。它们和实际枚举数量
    // 对不上，说明加了新状态却没同步——那就是漏测的开始。
    REQUIRE(sizeof(kAllStates) / sizeof(kAllStates[0]) == detail::kStateCount);
    REQUIRE(sizeof(kAllEvents) / sizeof(kAllEvents[0]) == detail::kEventCount);
}

TEST_CASE("done has no outgoing transitions", "[sm][table]")
{
    for (const auto e : kAllEvents)
    {
        REQUIRE_FALSE(isLegalTransition(State::kDone, e));
    }
}

// ===========================================================================
// 与 PROTOCOL.md §8.2 的一致性
// ===========================================================================

TEST_CASE("table matches PROTOCOL.md table exactly", "[sm][doc]")
{
    SECTION("文档里每一行都在表里，且目标状态一致")
    {
        for (const auto &row : kDocumented)
        {
            const Transition *t = findTransition(row.from, row.event);
            INFO(toString(row.from) << " --" << toString(row.event) << "-->");
            REQUIRE(t != nullptr);
            REQUIRE(t->to == row.to);
        }
    }

    SECTION("表里没有文档之外的转移")
    {
        REQUIRE(kTransitionCount == sizeof(kDocumented) / sizeof(kDocumented[0]));

        for (const auto &t : kTransitions)
        {
            bool documented = false;
            for (const auto &row : kDocumented)
            {
                if (row.from == t.from && row.event == t.event && row.to == t.to)
                {
                    documented = true;
                    break;
                }
            }
            INFO(toString(t.from) << " --" << toString(t.event) << "--> " << toString(t.to));
            REQUIRE(documented);
        }
    }
}

// ===========================================================================
// 全矩阵覆盖：60 个组合逐个断言
// ===========================================================================
//
// 这是本文件最重要的一条测试。PROTOCOL.md §8.2 明确要求：
// "其余所有 (状态, 事件) 组合都非法，必须显式失败而不是静默忽略。
//   这条规则需要有测试遍历全部组合来保证，不能靠'实现的时候记得'。"

TEST_CASE("every (state, event) combination behaves per the table", "[sm][exhaustive]")
{
    // 先确认矩阵规模：6 状态 × 10 事件
    constexpr std::size_t expectedCombos = 6 * 10;

    std::size_t checked = 0;
    std::size_t legal = 0;

    for (const auto from : kAllStates)
    {
        for (const auto event : kAllEvents)
        {
            ++checked;

            const bool expectedLegal = isLegalTransition(from, event);

            // 每次都用全新的状态机，避免前一次转移污染后一次
            StateMachine sm(from);
            const auto st = sm.fire(event);

            INFO("from=" << toString(from) << " event=" << toString(event));

            if (expectedLegal)
            {
                ++legal;
                REQUIRE(st.ok());
                // 状态必须真的变了
                REQUIRE(sm.state() != from);
                const Transition *t = findTransition(from, event);
                REQUIRE(t != nullptr);
                REQUIRE(sm.state() == t->to);
            }
            else
            {
                REQUIRE_FALSE(st.ok());
                REQUIRE(st.code == relayer::ErrorCode::kProtocol);
                // 非法转移后状态必须保持不变
                REQUIRE(sm.state() == from);
                // 错误消息要能定位问题
                REQUIRE(st.message.find(toString(from)) != std::string::npos);
                REQUIRE(st.message.find(toString(event)) != std::string::npos);
            }
        }
    }

    REQUIRE(checked == expectedCombos);
    // 表里恰好 10 条合法转移
    REQUIRE(legal == kTransitionCount);
    REQUIRE(legal == 10);
}

TEST_CASE("illegal combinations are exactly the complement", "[sm][exhaustive]")
{
    // 独立再算一遍：合法组合数 + 非法组合数 = 全部组合数。
    std::size_t illegal = 0;
    for (const auto from : kAllStates)
    {
        for (const auto event : kAllEvents)
        {
            if (!isLegalTransition(from, event))
            {
                ++illegal;
            }
        }
    }
    REQUIRE(illegal == 60 - 10);
    REQUIRE(illegal == 50);
}

// ===========================================================================
// 正常流程
// ===========================================================================

TEST_CASE("full transfer flow reaches Done", "[sm][flow]")
{
    StateMachine sm;
    REQUIRE(sm.state() == State::kIdle);

    REQUIRE(sm.fire(Event::kStartTransfer).ok());
    REQUIRE(sm.state() == State::kTransferring);

    REQUIRE(sm.fire(Event::kAllChunksAcked).ok());
    REQUIRE(sm.state() == State::kFinalizing);

    REQUIRE(sm.fire(Event::kVerifyOk).ok());
    REQUIRE(sm.state() == State::kDone);
    REQUIRE(isTerminal(sm.state()));
}

TEST_CASE("handshake flow returns to Idle", "[sm][flow]")
{
    StateMachine sm;
    REQUIRE(sm.fire(Event::kStartHandshake).ok());
    REQUIRE(sm.state() == State::kHandshaking);

    // 握手通过后回到 Idle，之后可以再开传输
    REQUIRE(sm.fire(Event::kAuthOk).ok());
    REQUIRE(sm.state() == State::kIdle);

    REQUIRE(sm.fire(Event::kStartTransfer).ok());
    REQUIRE(sm.state() == State::kTransferring);
}

TEST_CASE("interrupt and resume", "[sm][flow]")
{
    StateMachine sm(State::kTransferring);

    REQUIRE(sm.fire(Event::kInterrupt).ok());
    REQUIRE(sm.state() == State::kInterrupted);

    REQUIRE(sm.fire(Event::kResume).ok());
    REQUIRE(sm.state() == State::kTransferring);
}

TEST_CASE("verify failure can be resumed", "[sm][flow]")
{
    StateMachine sm(State::kFinalizing);

    REQUIRE(sm.fire(Event::kVerifyFail).ok());
    REQUIRE(sm.state() == State::kInterrupted);

    // 校验失败不等于作废——文件可能只差几块
    REQUIRE(sm.fire(Event::kResume).ok());
    REQUIRE(sm.state() == State::kTransferring);
}

TEST_CASE("auth failure is terminal", "[sm][flow]")
{
    StateMachine sm(State::kHandshaking);
    REQUIRE(sm.fire(Event::kAuthFail).ok());
    REQUIRE(sm.state() == State::kDone);

    // 握手失败之后必须重新建连接，不能在同一个状态机上继续
    REQUIRE_FALSE(sm.fire(Event::kStartTransfer).ok());
    REQUIRE_FALSE(sm.fire(Event::kStartHandshake).ok());
}

TEST_CASE("abort from interrupted ends the transfer", "[sm][flow]")
{
    StateMachine sm(State::kInterrupted);
    REQUIRE(sm.fire(Event::kAbort).ok());
    REQUIRE(sm.state() == State::kDone);
}

// ===========================================================================
// 终态与非法输入的防御
// ===========================================================================

TEST_CASE("terminal state rejects every event", "[sm][terminal]")
{
    for (const auto e : kAllEvents)
    {
        StateMachine sm(State::kDone);
        const auto st = sm.fire(e);
        INFO("event=" << toString(e));
        REQUIRE_FALSE(st.ok());
        REQUIRE(sm.state() == State::kDone);
    }
}

TEST_CASE("a late Resume cannot revive a Done transfer", "[sm][terminal]")
{
    // 针对一类真实故障：连接断开后重连，对端补发一条迟到的 Resume。
    // 如果允许它生效，已经作废的传输会"复活"并覆盖掉正确的结果。
    StateMachine sm(State::kDone);
    REQUIRE_FALSE(sm.fire(Event::kResume).ok());
    REQUIRE(sm.state() == State::kDone);
}

TEST_CASE("illegal fire leaves state untouched", "[sm][terminal]")
{
    StateMachine sm(State::kIdle);
    REQUIRE_FALSE(sm.fire(Event::kVerifyOk).ok());
    REQUIRE(sm.state() == State::kIdle);

    // 之后合法的事件仍然能正常工作
    REQUIRE(sm.fire(Event::kStartTransfer).ok());
    REQUIRE(sm.state() == State::kTransferring);
}

TEST_CASE("repeated illegal events are all rejected", "[sm][terminal]")
{
    StateMachine sm(State::kIdle);
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE_FALSE(sm.fire(Event::kAbort).ok());
    }
    REQUIRE(sm.state() == State::kIdle);
}

TEST_CASE("fire is noexcept and does not throw on illegal input", "[sm][terminal]")
{
    StateMachine sm(State::kIdle);
    REQUIRE_NOTHROW(sm.fire(Event::kAuthOk));
}

// ===========================================================================
// canFire 与 fire 的一致性
// ===========================================================================

TEST_CASE("canFire agrees with fire for the whole matrix", "[sm][exhaustive]")
{
    for (const auto from : kAllStates)
    {
        for (const auto event : kAllEvents)
        {
            StateMachine sm(from);
            const bool predicted = sm.canFire(event);
            const bool actual = sm.fire(event).ok();
            INFO("from=" << toString(from) << " event=" << toString(event));
            REQUIRE(predicted == actual);
        }
    }
}

TEST_CASE("canFire does not change state", "[sm][table]")
{
    StateMachine sm(State::kTransferring);
    for (const auto e : kAllEvents)
    {
        // 故意丢弃返回值：这里要验证的正是"调用它不会改变状态"。
        (void)sm.canFire(e);
        REQUIRE(sm.state() == State::kTransferring);
    }
}

// ===========================================================================
// reset
// ===========================================================================

TEST_CASE("reset returns to Idle by default", "[sm][reset]")
{
    StateMachine sm(State::kDone);
    sm.reset();
    REQUIRE(sm.state() == State::kIdle);
    REQUIRE(sm.fire(Event::kStartTransfer).ok());
}

TEST_CASE("reset can jump to any state", "[sm][reset]")
{
    for (const auto s : kAllStates)
    {
        StateMachine sm;
        sm.reset(s);
        REQUIRE(sm.state() == s);
    }
}

// ===========================================================================
// 名称
// ===========================================================================

TEST_CASE("state and event names are stable", "[sm][names]")
{
    // 这些名字会进日志，也是文档比对脚本的匹配键。改了会让历史日志失去可读性。
    REQUIRE(toString(State::kIdle) == "Idle");
    REQUIRE(toString(State::kHandshaking) == "Handshaking");
    REQUIRE(toString(State::kTransferring) == "Transferring");
    REQUIRE(toString(State::kFinalizing) == "Finalizing");
    REQUIRE(toString(State::kInterrupted) == "Interrupted");
    REQUIRE(toString(State::kDone) == "Done");

    REQUIRE(toString(Event::kStartHandshake) == "StartHandshake");
    REQUIRE(toString(Event::kAuthOk) == "AuthOk");
    REQUIRE(toString(Event::kAuthFail) == "AuthFail");
    REQUIRE(toString(Event::kStartTransfer) == "StartTransfer");
    REQUIRE(toString(Event::kAllChunksAcked) == "AllChunksAcked");
    REQUIRE(toString(Event::kInterrupt) == "Interrupt");
    REQUIRE(toString(Event::kVerifyOk) == "VerifyOk");
    REQUIRE(toString(Event::kVerifyFail) == "VerifyFail");
    REQUIRE(toString(Event::kResume) == "Resume");
    REQUIRE(toString(Event::kAbort) == "Abort");
}

TEST_CASE("names are unique", "[sm][names]")
{
    // 重名会让文档比对脚本误判，也让日志无法区分
    std::set<std::string> stateNames;
    for (const auto s : kAllStates)
    {
        REQUIRE(stateNames.insert(std::string(toString(s))).second);
    }

    std::set<std::string> eventNames;
    for (const auto e : kAllEvents)
    {
        REQUIRE(eventNames.insert(std::string(toString(e))).second);
    }
}

TEST_CASE("only Done is terminal", "[sm][names]")
{
    for (const auto s : kAllStates)
    {
        if (s == State::kDone)
        {
            REQUIRE(isTerminal(s));
        }
        else
        {
            REQUIRE_FALSE(isTerminal(s));
        }
    }
}
