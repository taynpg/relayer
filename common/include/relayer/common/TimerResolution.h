#pragma once

// 系统定时器精度（Windows）。
//
// 为什么这个在 common 里：它不承载任何业务语义，只是"把操作系统的默认值
// 改成一个能用值"的平台原语，而 cli / server / app 三个可执行文件都需要它
// ——放在任何一个上层都会逼另外两个反向依赖。
//
// ---------------------------------------------------------------------------
// 为什么需要它
// ---------------------------------------------------------------------------
//
// Windows 默认的定时器分辨率是 15.625ms。`asio::steady_timer` 的到期时间会
// 被它**向上取整**，于是：
//
//     tickInterval = 10ms  → 实际 15.6ms
//     tickInterval = 2ms   → 实际还是 15.6ms
//
// 而传输引擎的推进完全按 tick 走（提交读盘、取块、发块、检查 ACK）。两端
// 各挂 15.6ms，一个窗口周转就是 30ms 起，吞吐被钉死。
//
// 这个现象在排查时极具误导性：改 tick 参数完全不生效，看起来像"配置没传
// 下去"。实测线索是"ACK 间隔恒为 15.6ms，与任何配置都无关"。
//
// 提到 1ms 之后 tick 才真的是配置里的那个值。
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//
//     int main()
//     {
//         const relayer::common::TimerResolutionGuard timerGuard;
//         ...
//     }
//
// 作用域是**进程级**的（影响所有线程的定时器），所以每个可执行文件的入口
// 各放一个。非 Windows 平台上它是空操作。

namespace relayer::common {

// RAII：构造时把精度提到 1ms，析构时恢复。
//
// 必须在 main 的作用域里存活到退出——提前析构等于提前把精度还回去。
class TimerResolutionGuard
{
public:
    TimerResolutionGuard();
    ~TimerResolutionGuard();

    TimerResolutionGuard(const TimerResolutionGuard&) = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;
};

}   // namespace relayer::common
