#pragma once

#include <asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

namespace relayer::test {

// 事件循环推进工具。
//
// 这两个函数几乎每个 socket / 会话测试都要用，早先它们在 ConnectionTest.cpp
// 和 SessionTest.cpp 里各抄了一份，所以一旦要改语义就得改两处——而"漏改一处"
// 的表现是某批测试突然变慢或变脆，很难联想到是复制品不一致。
//
// ---------------- 为什么不用 run() / run_one() ----------------
//
// asio 的 run_one() 在"没有就绪任务、但有挂起的异步操作"（比如一个还在等
// 的 async_read）时会**阻塞等待**。测试里经常出现"连接还开着、断言已经做
// 完"的情况，那时候 run_one() 会永久阻塞。
//
// run_for 有明确上限，跑满即返回。测试挂死不给任何信息，比失败更糟。
//
// ---------------- 为什么还要 pumpUntil ----------------
//
// 只用 run_for 的问题在**耗时**：它的时长是"上限"，不是"预期"。一条用例
// 需要 2ms，写 run_for(2s) 就真的跑 2s。20 条这样的用例串起来就是 40 秒，
// 而 CI 里的 timeout 一旦设得比它小，进程会被外部杀掉——看起来像"挂住"，
// 实际是测试本身太慢。这个坑真的踩过一次，排查了半天。
//
// pumpUntil 把"等一个条件"和"跑事件循环"合成一件事：条件满足立刻返回，
// 只有真出问题才耗到超时。

// 跑 duration 那么久（上限语义）。用于"确认某件事**没有**发生"。
inline void pump(asio::io_context& ctx, std::chrono::milliseconds duration)
{
    ctx.restart();
    ctx.run_for(duration);
}

// 反复推进事件循环，直到 pred() 为真或超时。返回 pred() 的最终值。
//
// 分段推进（每次 1ms）而不是一次 run_for(timeout)：一次跑满的话，条件在
// 1ms 时就满足也要等到超时。分段让"顺利时快、卡住时也不会永远等"两件事
// 同时成立。
//
// 每次 run_for 之间让出一下时间片：某些平台上的 IO 完成通知需要一点点
// 实际时间才会被投递，纯 busy loop 可能一直看不到。
inline bool pumpUntil(asio::io_context& ctx, const std::function<bool()>& pred,
                      std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        ctx.restart();
        ctx.run_for(std::chrono::milliseconds(1));

        if (pred()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // 最后再看一眼：超时前那一瞬间可能刚好满足。
            return pred();
        }
        std::this_thread::yield();
    }
}

// 推到条件满足，但**不**返回布尔值——用于"必须成功，失败由随后的断言报错"
// 的场景，让测试代码读起来是一条直线。
inline void pumpUntilDone(asio::io_context& ctx, const std::function<bool()>& pred,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    (void)pumpUntil(ctx, pred, timeout);
}

}   // namespace relayer::test
