#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// deflate（zlib 格式）的薄封装。
//
// 为什么在这里：core 的传输引擎需要"把一块数据压小一点再上线"，但它不该
// 认识 miniz——miniz 是 C 库，头文件里塞满 tdefl/tinfl 的细节，而 core 的
// 头文件要能被任何 C++ 项目直接 include。包一层之后，核心库里只出现
// `compression::deflateBlock / inflateBlock` 两个名字。
//
// 为什么用 zlib 格式而不是裸 deflate：只差 2 字节头 + 4 字节 adler32，而
// 它能自校验、能被任何工具解开（调试时可以直接 zlib.decompress 看内容）。
//
// 线程安全：这两个函数都是纯函数（没有全局状态、没有静态缓冲），可以在
// 任意线程上并发调用。压缩**必须**跑在工作线程上：deflate 一块 256 KiB
// 是毫秒级的 CPU 活，压在 io 线程上会让心跳和 ACK 一起停摆。
namespace relayer::compression {

// 压缩一块数据，返回 zlib 流。
//
// 返回空 vector 表示"不值得压缩"——调用方应当按原样发送：
//   - 输入为空；
//   - 压缩后没有比原数据小 `kWorthItRatio` 以上。
//
// 最后那条不是优化而是必要：对已经压过的数据（zip/jpeg/xz）deflate 只会
// 变大，白花了 CPU 还多传几字节。
inline constexpr double kWorthItRatio = 0.95;   // 至少省 5% 才用压缩结果

[[nodiscard]] std::vector<char> deflateBlock(const char* data, std::size_t len, int level = 6);

// 解压一块数据，**输出长度必须恰好等于 expected**。
//
// expected 由调用方给（块号 → 块长度是确定的），这一点是安全边界：
// 压缩炸弹的全部危害在于"小输入解出大输出"，而这里容量是写死的，
// 超出直接判失败，不会分配出意外的内存。
//
// 失败（流损坏、长度不符、输入被截断）返回 false，out 内容未定义。
[[nodiscard]] bool inflateBlock(const char* data, std::size_t len, std::size_t expected, std::vector<char>& out);

}   // namespace relayer::compression
