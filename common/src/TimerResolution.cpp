#include "relayer/common/TimerResolution.h"

#ifdef _WIN32
// clang-format off
// windows.h 必须先于 mmsystem.h（后者依赖前者的类型定义）。clang-format 会
// 按字母序把它们排反，排反之后是一屏 "LONG does not name a type"。
#include <windows.h>
#include <mmsystem.h>
// clang-format on
#endif

namespace relayer::common {

#ifdef _WIN32

TimerResolutionGuard::TimerResolutionGuard()
{
    // 1ms 是 timeBeginPeriod 的常用值，也是这里需要的量级：tick 配的是 2ms，
    // 精度必须比它细。返回码不检查——失败（极少见）只是退回默认精度，
    // 不影响正确性，只是慢些。
    timeBeginPeriod(1);
}

TimerResolutionGuard::~TimerResolutionGuard()
{
    timeEndPeriod(1);
}

#else

// 非 Windows 上不需要做任何事：Linux/macOS 的定时器精度本来就在微秒级，
// 且没有"进程级全局分辨率"这个概念。
TimerResolutionGuard::TimerResolutionGuard() = default;

TimerResolutionGuard::~TimerResolutionGuard() = default;

#endif

}   // namespace relayer::common
