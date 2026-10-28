#include "relayer/core/transfer/WorkerPool.h"

#include <algorithm>

namespace relayer::core::transfer {

WorkerPool::WorkerPool(std::size_t threads, std::size_t queueCapacity) : capacity_(queueCapacity)
{
    if (threads == 0) {
        // 传输是 I/O 密集型：线程再多也只是排队等盘。
        // 上限 4 是因为典型场景是"一块在读、一块在写、一块在算哈希"，
        // 再加线程换来的是上下文切换而不是吞吐。
        const auto hw = std::thread::hardware_concurrency();
        threads = std::clamp<std::size_t>(hw == 0 ? 2 : hw, 2, 4);
    }

    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

// 一句话说清析构契约，因为这是最容易被误改的地方：**已在执行的任务会被
// 等完，还没开始的任务直接丢弃**。不是"排空队列"，也不是"立即返回"。
//
// 为什么必须等正在跑的那个：析构返回之后，成员 mutex_ / queue_ 就不存在了。
// 一条正在执行的写盘任务如果还在用它，就是 use-after-free。队列里的任务
// 相对安全（它们还躺在 queue_ 里，没碰任何东西），取消场景下也不该执行。
WorkerPool::~WorkerPool()
{
    stop();

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

bool WorkerPool::submit(Task task)
{
    if (!task) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return false;
        }
        if (capacity_ != 0 && queue_.size() >= capacity_) {
            // 队列满 = 背压信号。不阻塞等空位：阻塞会把调用方（strand）
            // 一起卡住，而 strand 卡住的后果比"这一块暂时不读"严重得多。
            return false;
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

std::size_t WorkerPool::pending() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t WorkerPool::running() const noexcept
{
    return running_.load(std::memory_order_relaxed);
}

std::size_t WorkerPool::threadCount() const noexcept
{
    return threads_.size();
}

void WorkerPool::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_.exchange(true)) {
            return;
        }
        queue_.clear();
    }
    cv_.notify_all();
}

bool WorkerPool::stopped() const noexcept
{
    return stop_.load(std::memory_order_relaxed);
}

void WorkerPool::workerLoop()
{
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_.load(std::memory_order_relaxed) || !queue_.empty(); });

            if (stop_.load(std::memory_order_relaxed)) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        running_.fetch_add(1, std::memory_order_relaxed);
        // 任务本身不该抛。真抛了（比如 std::bad_alloc）也不让整个线程死掉
        // ——死掉一个工作线程之后，剩下的线程会慢慢把队列啃完，而调用方
        // 完全看不出来"池子已经残了"。这里吞掉并继续，比静默降级更可控。
        try {
            task();
        } catch (...) {
        }
        running_.fetch_sub(1, std::memory_order_relaxed);
    }
}

}   // namespace relayer::core::transfer
