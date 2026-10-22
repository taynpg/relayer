#pragma once

#include <functional>

// 传输引擎两侧共用的流水线原语。
//
// 单独一个头而不是让接收侧去 include 发送侧：TransferSender 与
// TransferReceiver 是对等的两个引擎，任何一个都不该成为另一个的依赖。
// 早先 PostFn 定义在 TransferSender.h 里，接收侧要它就得把发送侧的整个
// 头（含 SendWindow、WorkerPool）拖进来——依赖关系与直觉相反，将来想
// 单独复用接收侧时立刻绊住。

namespace relayer::core::transfer
{

// 读盘/写盘完成后的投递函数。
//
// 签名是 `void(std::function<void()>)`：接一个回调，负责把它送到正确的
// 执行上下文（通常是某个 strand）上。调用方一般这样给：
//
//   [&conn](std::function<void()> fn) { asio::post(conn.strand(), std::move(fn)); }
//
// 之所以做成回调参数而不是让引擎持有 executor：
//
//   - 引擎不必依赖 asio，单测不需要起 io_context；
//   - "回到哪个 strand"只有调用方知道，引擎猜不出来；
//   - 忘绑 executor 的后果（回调并发跑，所有"不必加锁"的假设失效）只在
//     多线程跑 io_context 时才暴露，把绑定变成显式参数能让人看见它。
using PostFn = std::function<void(std::function<void()>)>;

}  // namespace relayer::core::transfer
