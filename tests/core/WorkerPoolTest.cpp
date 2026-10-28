#include "relayer/core/transfer/WorkerPool.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace relayer::core::transfer;

// 线程池的测试与别的模块不同：它的失败模式是**偶发**的。
//
// 一个漏掉的锁、一个没被唤醒的 cv，在单核机器上可能一万次都不显形。
// 所以这里不用 sleep 凑时序（那既慢又不稳），而是用"条件变量等结果 +
// 硬超时兜底"的写法：等不到就是失败，而不是挂死。
//
// 唯一需要 sleep 的地方是"确认某件事**没有**发生"——那种断言没有
// 完成信号可等，只能靠一个足够长的时间窗。

namespace {

using namespace std::chrono_literals;

// 等一个原子标志变成期望值，最多等 timeout。返回 false 表示超时。
//
// 不用 catch 自带的 REQUIRE 里塞轮询，是因为这里的等待本身就是要
// 断言的语义（"任务必须被执行"），而不是测试的准备工作。
bool waitFor(const std::atomic<int>& flag, int want, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire) == want) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return flag.load(std::memory_order_acquire) == want;
}

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}   // namespace

// ===========================================================================
// 基本执行
// ===========================================================================

TEST_CASE("runs a submitted task", "[transfer][pool]")
{
    WorkerPool pool(2, 16);

    std::atomic<int> ran{0};
    REQUIRE(pool.submit([&ran] { ran.store(1, std::memory_order_release); }));

    REQUIRE(waitFor(ran, 1));
    REQUIRE(pool.threadCount() == 2);
}

TEST_CASE("runs many tasks across all threads", "[transfer][pool]")
{
    constexpr int kTasks = 200;
    WorkerPool pool(4, 256);

    std::atomic<int> count{0};
    for (int i = 0; i < kTasks; ++i) {
        // 队列容量 256 > 200，所以这里不该有 false。真有 false 说明
        // submit 的边界判断错了（比如把 >= 写成了 >）。
        REQUIRE(pool.submit([&count] { count.fetch_add(1, std::memory_order_relaxed); }));
    }

    REQUIRE(waitUntil([&count] { return count.load(std::memory_order_acquire) == kTasks; }));
}

TEST_CASE("default thread count is bounded", "[transfer][pool]")
{
    // 传输是 I/O 密集型，线程默认上限 4。这里只断言"不为 0 且不超过 4"，
    // 不硬编码具体数值——那取决于跑测试的机器，写死会让测试在别的机器上红。
    WorkerPool pool;
    REQUIRE(pool.threadCount() >= 1);
    REQUIRE(pool.threadCount() <= 4);
}

TEST_CASE("explicit thread count is honoured", "[transfer][pool]")
{
    // 队列容量要够放 20 个任务：容量不够时 submit 会返回 false（那是
    // 背压，不是 bug），会把这条用例变成"在测队列容量"。容量由专门
    // 的用例负责。
    WorkerPool pool(1, 32);
    REQUIRE(pool.threadCount() == 1);

    // 单线程池：任务严格串行。用一个"进来就检查前一个已经离开"的
    // 检测器验证——并行的话这个断言会随机失败。
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::atomic<int> done{0};

    // 队列容量要够放 20 个任务。容量不够时 submit 返回 false（那是背压，
    // 不是 bug），会把这条用例变成"在测队列容量"。
    for (int i = 0; i < 20; ++i) {
        REQUIRE(pool.submit([&] {
            if (inside.fetch_add(1, std::memory_order_acq_rel) != 0) {
                overlap.store(true, std::memory_order_release);
            }
            inside.fetch_sub(1, std::memory_order_acq_rel);
            done.fetch_add(1, std::memory_order_acq_rel);
        }));
    }

    REQUIRE(waitUntil([&done] { return done.load(std::memory_order_acquire) == 20; }));
    REQUIRE_FALSE(overlap.load(std::memory_order_acquire));
}

// ===========================================================================
// 背压
// ===========================================================================

TEST_CASE("submit returns false when the queue is full", "[transfer][pool]")
{
    // 这是背压的物理层落点。测试的关键是"卡住消费者"：
    // 单线程池 + 第一个任务阻塞，就能把队列稳定地填满。
    WorkerPool pool(1, 2);

    std::atomic<bool> hold{true};
    std::atomic<bool> started{false};

    REQUIRE(pool.submit([&] {
        started.store(true, std::memory_order_release);
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }));

    // 等消费者真正开始跑，而不是靠 sleep 猜。没等到就意味着第一个任务
    // 还在队列里占位置，后面的容量断言全是乱的。
    REQUIRE(waitUntil([&started] { return started.load(std::memory_order_acquire); }));

    // 消费者已经占住了唯一的线程，后面两个能进队列。
    const auto noop = [] {};
    REQUIRE(pool.submit(noop));         // 队列 1
    REQUIRE(pool.submit(noop));         // 队列 2（满）
    REQUIRE_FALSE(pool.submit(noop));   // 背压
    REQUIRE(pool.pending() == 2);

    hold.store(false, std::memory_order_release);

    // 放开之后队列应当被排空，且是**排空**而不是残着。
    REQUIRE(waitUntil([&pool] { return pool.pending() == 0; }));
}

TEST_CASE("queue capacity is enforced exactly", "[transfer][pool]")
{
    WorkerPool pool(1, 2);

    std::atomic<bool> hold{true};
    std::atomic<bool> started{false};

    REQUIRE(pool.submit([&] {
        started.store(true, std::memory_order_release);
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }));

    // 等消费者真正开始跑，而不是靠 sleep 猜。没等到就意味着第一个任务
    // 还在队列里占位置，后面的容量断言全是乱的。
    REQUIRE(waitUntil([&started] { return started.load(std::memory_order_acquire); }));

    const auto noop = [] {};
    REQUIRE(pool.submit(noop));         // 队列 1
    REQUIRE(pool.submit(noop));         // 队列 2（满）
    REQUIRE_FALSE(pool.submit(noop));   // 背压
    REQUIRE(pool.pending() == 2);

    hold.store(false, std::memory_order_release);
}

TEST_CASE("empty task is rejected", "[transfer][pool]")
{
    WorkerPool pool(1, 8);
    REQUIRE_FALSE(pool.submit(WorkerPool::Task{}));
}

TEST_CASE("unbounded queue accepts everything", "[transfer][pool]")
{
    // capacity = 0 表示不限，只用于测试与"确定内存充足"的场合。
    WorkerPool pool(1, 0);

    std::atomic<bool> hold{true};
    REQUIRE(pool.submit([&] {
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }));

    for (int i = 0; i < 100; ++i) {
        REQUIRE(pool.submit([] {}));
    }
    hold.store(false, std::memory_order_release);
}

// ===========================================================================
// 停止语义
// ===========================================================================

TEST_CASE("stop discards queued tasks", "[transfer][pool]")
{
    // "取消传输"时，一个已经排队但还没开始的写盘任务本来就不该执行。
    WorkerPool pool(1, 16);

    std::atomic<bool> hold{true};
    std::atomic<bool> started{false};
    std::atomic<int> queuedRan{0};

    REQUIRE(pool.submit([&] {
        started.store(true, std::memory_order_release);
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }));
    REQUIRE(waitUntil([&started] { return started.load(std::memory_order_acquire); }));

    for (int i = 0; i < 8; ++i) {
        (void)pool.submit([&queuedRan] { queuedRan.fetch_add(1, std::memory_order_relaxed); });
    }
    REQUIRE(pool.pending() == 8);

    pool.stop();
    REQUIRE(pool.stopped());
    REQUIRE(pool.pending() == 0);

    hold.store(false, std::memory_order_release);
    REQUIRE(waitFor(queuedRan, 0, 200ms));
    REQUIRE(queuedRan.load() == 0);
}

TEST_CASE("submit after stop is rejected", "[transfer][pool]")
{
    WorkerPool pool(1, 8);
    pool.stop();

    REQUIRE_FALSE(pool.submit([] {}));
    REQUIRE(pool.stopped());
}

TEST_CASE("stop is idempotent", "[transfer][pool]")
{
    WorkerPool pool(1, 8);
    pool.stop();
    pool.stop();
    REQUIRE(pool.stopped());
}

TEST_CASE("destructor waits for the running task", "[transfer][pool]")
{
    // 这条守的是析构契约的**容易漏掉的那一半**：已在执行的任务必须被等完。
    //
    // 为什么不等会出事：析构返回之后 mutex_ / queue_ 已经不存在，而一条
    // 正在执行的任务如果还在用它们，就是 use-after-free——而且是那种只在
    // 机器慢的时候才炸的。
    //
    // 另一半（队列里的任务被丢弃）在 "stop discards queued tasks" 里。
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};

    {
        // 单线程池：唯一的线程跑那条长任务，于是"它确实已经开始"和
        // "没有别的线程来抢"两件事都能确定。
        WorkerPool pool(1, 16);

        REQUIRE(pool.submit([&] {
            entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(40ms);
            finished.store(true, std::memory_order_release);
        }));

        // 必须等信号，不能 sleep 猜：submit 只是把任务放进队列，
        // 什么时候开始跑由操作系统决定。
        REQUIRE(waitUntil([&entered] { return entered.load(std::memory_order_acquire); }));

        // 不显式 stop，直接让析构接手。
    }

    REQUIRE(finished.load(std::memory_order_acquire));
}

// ===========================================================================
// 异常
// ===========================================================================

TEST_CASE("throwing task does not kill the worker", "[transfer][pool]")
{
    // 任务里真抛了（比如 bad_alloc），线程要活着把后面的任务跑完。
    // 让线程死掉的话，池子会静默变小，而调用方完全看不出来。
    WorkerPool pool(1, 8);

    REQUIRE(pool.submit([] { throw std::runtime_error("boom"); }));

    std::atomic<int> after{0};
    REQUIRE(pool.submit([&after] { after.store(1, std::memory_order_release); }));

    REQUIRE(waitFor(after, 1));
    REQUIRE(pool.running() == 0);
}

// ===========================================================================
// 计数
// ===========================================================================

TEST_CASE("running count reflects in-flight tasks", "[transfer][pool]")
{
    WorkerPool pool(1, 8);

    std::atomic<bool> hold{true};
    std::atomic<bool> started{false};

    REQUIRE(pool.submit([&] {
        started.store(true, std::memory_order_release);
        while (hold.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    }));
    REQUIRE(waitUntil([&started] { return started.load(std::memory_order_acquire); }));
    REQUIRE(waitUntil([&pool] { return pool.running() == 1; }));

    hold.store(false, std::memory_order_release);
    REQUIRE(waitUntil([&pool] { return pool.running() == 0; }));
}

TEST_CASE("pending drains to zero", "[transfer][pool]")
{
    WorkerPool pool(2, 64);

    std::atomic<int> done{0};
    for (int i = 0; i < 40; ++i) {
        (void)pool.submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
    }

    REQUIRE(waitUntil([&done] { return done.load(std::memory_order_acquire) == 40; }));
    REQUIRE(waitUntil([&pool] { return pool.pending() == 0; }));
}
