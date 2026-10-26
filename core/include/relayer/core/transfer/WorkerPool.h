#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "relayer/common/Status.h"

// 工作线程池（传输引擎的 I/O 侧）。
//
// 为什么必须有它：会话层的所有回调都跑在 Connection 的 strand 上，而
// strand 是串行的——在里面做一次 4MB 的 read() 会把这条连接上的心跳、
// 收帧、超时判定全部卡住。心跳 20 秒空闲 / 10 秒超时，一次慢盘读完全
// 可能超过它，于是"传输大文件时无缘无故被判失联"。
//
// 所以读盘、写盘、算哈希都丢到这里；strand 上只做"派发任务 + 处理结果"。
//
// 几条刻意的取舍：
//
// 1. **有界队列**。无界队列在接收侧等于把"磁盘跟不上"转成"内存无限增长"，
//    而那正是背压要避免的事（docs/DESIGN.md §3.4）。队列满时 submit()
//    返回 false，调用方据此暂停从网络取数据——这就是背压的着力点。
//
// 2. **不拥有 io_context**，只持有它的引用用于把完成回调投回去。
//    post 到 io_context 而不是直接调用：完成回调要回到 strand 上执行，
//    而 strand 的归属只有调用方知道。
//
// 3. **析构时排空在跑的任务，丢弃未开始的**。不做"优雅等待全部完成"：
//    一个已经排队但还没开始的写盘任务，在"用户取消传输"的语境下本来就
//    不该执行。这一点与 std::thread 的默认语义不同，写清楚了才不会误用。

namespace relayer::core::transfer
{

class WorkerPool
{
public:
    // 任务。
    //
    // 用 std::function<void()> 而不是 std::packaged_task：这里不需要
    // future（完成通知统一走 asio::post），而 packaged_task 会引入一个
    // 每任务必付的异常捕获与共享状态分配。
    using Task = std::function<void()>;

    // threads：工作线程数。0 表示"按硬件并发数，上限 4"——传输是 I/O
    // 密集型，线程再多也只是排队等盘，反而增加上下文切换。
    //
    // queueCapacity：未开始任务的队列上限。0 表示不限（只用于测试）。
    explicit WorkerPool(std::size_t threads = 0, std::size_t queueCapacity = 64);
    ~WorkerPool();

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    // 提交一个任务。
    //
    // 返回 false 表示队列已满，任务**没有被接受**（也不会被执行）。
    // 调用方必须处理这个 false——那是背压信号，正确反应是暂停生产。
    [[nodiscard]] bool submit(Task task);

    // 队列里还有多少没开始的任务。
    [[nodiscard]] std::size_t pending() const noexcept;

    // 正在执行的任务数。
    [[nodiscard]] std::size_t running() const noexcept;

    [[nodiscard]] std::size_t threadCount() const noexcept;

    // 请求停止：不再接受新任务，已排队的丢弃。
    //
    // 与析构的区别：析构会隐式调用它。单独暴露是为了让调用方能"先停、
    // 再等"，而不是只能靠析构——那会让关闭时机不可控。
    void stop();

    [[nodiscard]] bool stopped() const noexcept;

private:
    void workerLoop();

    std::vector<std::thread> threads_;
    std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;

    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> running_{0};
};

// ---------------------------------------------------------------------------
// 完成投递
// ---------------------------------------------------------------------------

// 把一个完成回调投回 io_context。
//
// 单独包一层而不是让各处直接 asio::post：完成回调几乎总要在"这条传输
// 所属的 strand"上执行，而调用方很容易忘记绑 executor——忘记的后果是
// 回调可能并发跑，于是所有"不需要加锁"的假设全部失效，而且只在多线程
// 跑 io_context 时才暴露。
//
// handler 必须是可拷贝的（asio::post 的既有要求）。
template <typename Handler> void postTo(asio::io_context &ctx, Handler handler)
{
    asio::post(ctx, std::move(handler));
}

}  // namespace relayer::core::transfer
