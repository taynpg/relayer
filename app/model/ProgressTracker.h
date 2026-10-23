#pragma once

#include <QString>
#include <chrono>
#include <cstdint>

namespace relayer::app
{

// 字节数的人读格式（B/KiB/MiB/GiB/TiB）。原先在 MainWindow 与
// RelayTaskControl 各有一份相同实现，收拢到这里。
[[nodiscard]] QString humanBytes(std::uint64_t n);

// 单文件进度/速度跟踪（REWRITE-PLAN §3.7.2 清单 #11）。
//
// v1 的进度计算散落在 RelayTask.cpp 的循环里，空文件除零靠事后补丁。
// v2 把除零与时间边界收进本类闭环：
//   - bytesTotal == 0（空文件、或常驻接收还没等到请求）：百分比与速度
//     都没有信息量，给占位 "--"，调用方不特判；
//   - bytesPerSecond <= 0（底层偶尔给不出瞬时值）：退回自算平均速度
//     （已完成 / 自开始经过的时间）；
//   - 传输刚开始（< 200ms）的平均速度不可信（分母接近零、首块读盘
//     抖动大），速度段给 "-" 而不是显示一个虚高的数。
//
// 调用约定：每个传输项开始时 reset()，此后每次进度回调喂 update()，
// 返回值直接渲染进表格单元格。
class ProgressTracker
{
public:
    void reset();

    // 返回该展示的进度文本，如 "35.2% · 1.2MiB/s"。
    [[nodiscard]] QString update(std::uint64_t bytesDone, std::uint64_t bytesTotal, double bytesPerSecond);

private:
    std::chrono::steady_clock::time_point started_{};
};

}  // namespace relayer::app
