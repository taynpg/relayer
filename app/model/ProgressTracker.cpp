#include "ProgressTracker.h"

namespace relayer::app
{

namespace
{
// 传输开始后多久内的"平均速度"不可信。200ms 内通常只过了一两个块，
// 读盘抖动足以让均值虚高一个数量级；显示 "-" 比显示假速度好。
constexpr std::chrono::milliseconds kMinSpanForAverage{200};
}  // namespace

void ProgressTracker::reset()
{
    started_ = std::chrono::steady_clock::now();
}

QString ProgressTracker::update(std::uint64_t bytesDone, std::uint64_t bytesTotal, double bytesPerSecond)
{
    // 总量为 0：空文件（一瞬就完）或"还没等到文件请求"的占位期。
    // 百分比会除零、速度没有意义，给固定占位。
    if (bytesTotal == 0)
    {
        return QStringLiteral("--");
    }

    const double pct = 100.0 * static_cast<double>(bytesDone) / static_cast<double>(bytesTotal);

    QString speed;
    if (bytesPerSecond > 0.0)
    {
        speed = humanBytes(static_cast<std::uint64_t>(bytesPerSecond)) + QStringLiteral("/s");
    }
    else
    {
        // 底层没给瞬时值：自算平均速度，但开头一段不算——分母太小时
        // 均值完全由抖动决定。
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        if (elapsed >= kMinSpanForAverage)
        {
            const double secs = std::chrono::duration<double>(elapsed).count();
            speed =
                humanBytes(static_cast<std::uint64_t>(static_cast<double>(bytesDone) / secs)) + QStringLiteral("/s");
        }
        else
        {
            speed = QStringLiteral("-");
        }
    }

    return QStringLiteral("%1%2 · %3").arg(pct, 0, 'f', 1).arg(QLatin1String("%")).arg(speed);
}

QString humanBytes(std::uint64_t n)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        ++i;
    }
    return QString::number(v, 'f', 1) + QLatin1String(units[i]);
}

}  // namespace relayer::app
