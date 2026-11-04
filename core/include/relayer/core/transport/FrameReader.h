#pragma once

#include <cstddef>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/core/protocol/Frame.h"

// 字节流到帧的切分器（framing）。
//
// 这是 transport 层里最容易写错的一块，所以单独抽出来并且**完全不碰 socket**。
// 它只做一件事：不断喂入字节，吐出完整的帧。
//
// 要处理的情况（每一种都有对应的测试）：
//
//   半包    一次 read 只收到半个帧 —— 收下，等下次
//   粘包    一次 read 收到 3 个帧 —— 逐个吐出
//   垃圾    连接中段出现非帧字节 —— 靠 unpackFrame 的 magic 盲扫跳过
//   超长    对端声称 32MB 但我们不想为它分配内存 —— 设上限拦截
//
// 关于内存：buffered_ 里放的是"还没凑成完整帧"的字节。正常情况下它的峰值
// 就是一个帧的大小。但恶意对端可以持续发送垃圾字节、永远不构成完整帧，
// 让缓冲无限增长。所以这里有一个硬上限，超了就判协议错误——宁可断开，
// 也不能让一条连接吃光内存。

namespace relayer::core::transport {

// 缓冲上限。
//
// 为什么不直接用 kMaxFrameSize：帧本身最大 32MB，但"收到一半的帧"加上
// "扫描垃圾时保留的尾巴"可能略多。留一倍余量。超过这个值说明对端不是在
// 发帧，而是在灌垃圾。
inline constexpr std::size_t kMaxBufferedBytes = 2u * protocol::frame::kMaxFrameSize;

class FrameReader
{
public:
    FrameReader() = default;

    // 追加收到的字节。
    //
    // 返回失败表示"这条流已经不可信"，调用方应当断开连接。失败原因可能是
    // 协议错误（版本不符 / CRC 错）或缓冲溢出。
    //
    // 注意：追加成功不代表能解出帧——帧可能还没收全。解帧用 next()。
    [[nodiscard]] Status append(const char* data, std::size_t len);
    [[nodiscard]] Status append(const std::vector<char>& data);

    // 尝试取出一帧。
    //
    // 返回 true：frame 填入完整帧，内部缓冲已消费掉对应字节。
    // 返回 false：还没有完整帧（可能部分到达，也可能全是垃圾已清掉）。
    //           status 若失败表示流不可信。
    //
    // 用法是 while (reader.next(f, st)) { handle(f); } —— 一次 read 之后
    // 要把能解的帧全部解完，不能只解一个就回去等。
    [[nodiscard]] bool next(protocol::Frame& frame, Status& status);

    // 当前**尚未消费**的字节数（诊断用）。
    //
    // 注意语义：不是"容器里有多少字节"。容器尾部可能还留着已经消费过的
    // 前缀等压缩（见 consume 的说明），那些不再算作缓冲。
    [[nodiscard]] std::size_t buffered() const noexcept
    {
        return available();
    }

    // 已成功解出的帧数（诊断用）
    [[nodiscard]] std::uint64_t framesRead() const noexcept
    {
        return framesRead_;
    }

    // 已丢弃的垃圾字节数（诊断用）。
    //
    // 这个数字值得监控：正常连接上它应当恒为 0。非零说明对端实现有问题，
    // 或者有人在往连接里注入数据。
    [[nodiscard]] std::uint64_t garbageBytes() const noexcept
    {
        return garbageBytes_;
    }

    void clear() noexcept;

private:
    // **vector + 读指针**，不是 deque<char>。
    //
    // 原先用 deque 的理由是"要从头部消费字节，deque 的 pop_front 是 O(1)"。
    // 那个理由只在"每次消费几个字节"时成立，而实际消费模式是**每帧
    // 256KiB~1MiB**，且原实现是逐字节做的：
    //
    //     for (i < frameLen) contiguous.push_back(*it++);   // 逐字节跨 deque 块
    //     for (i < consumed) buffer_.pop_front();           // 逐字节弹出
    //
    // 一帧 256 KiB 就是 26 万次迭代器操作 + 26 万次 pop_front，实测单帧
    // 4-5ms。256 KiB 分块下整体稳定在 40 MB/s，而且**块越大越慢**——
    // 这正是"调块大小、调窗口、调并发全都没用"的原因：瓶颈根本不在那些
    // 参数上，而在这里的逐字节搬运。
    //
    // 换成 vector + readPos_ 之后：整帧只需一次 memcpy，消费只需一次加法。
    // 头部垃圾的压缩是摊销 O(1)——压一次搬动的字节数不少于搬动前累积的
    // 已消费量，而它们每字节只会被搬一次。
    std::vector<char> buffer_;
    std::size_t readPos_ = 0;

    // 复用给 unpackFrame 的连续缓冲。每帧一次 resize（只增不减）比每帧
    // 新建一个 vector 省一次分配——高频路径上这仍然值。
    std::vector<char> scratch_;

    std::uint64_t framesRead_ = 0;
    std::uint64_t garbageBytes_ = 0;

    [[nodiscard]] std::size_t available() const noexcept
    {
        return buffer_.size() - readPos_;
    }

    [[nodiscard]] const char* readPtr() const noexcept
    {
        return buffer_.data() + readPos_;
    }

    // 消费 n 字节。读指针过半时压缩一次，避免容器无限增长。
    void consume(std::size_t n) noexcept;
};

}   // namespace relayer::core::transport
