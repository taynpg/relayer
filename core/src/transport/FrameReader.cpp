#include "relayer/core/transport/FrameReader.h"

#include <algorithm>
#include <cstdint>

namespace relayer::core::transport
{

namespace
{

// 小端读 u32。Frame.cpp 里有同名实现，但它在匿名 namespace 里、不对外——
// 协议层刻意只暴露 unpackFrame 这一个入口，不让"直接摸帧头"变成公共 API。
// 这里只需要读两个字段（magic 和帧长），复制六行比扩大协议层的接口面划算。
//
// 偏移量与 Frame.cpp 必须一致：magic 在 0，帧长在 28。改协议布局时这两处
// 会一起被 header_crc 测试抓到（那组测试会真的 pack 一帧再 unpack）。
inline std::uint32_t readU32LE(const char *p) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kFrameLenOffset = 28;

}  // namespace

Status FrameReader::append(const char *data, std::size_t len)
{
    if (len == 0)
    {
        return Status::Ok();
    }
    if (data == nullptr)
    {
        // 长度非 0 却给了空指针，是调用方的编程错误，不是对端的错。
        // 用 kUnknown 而不是 kProtocol：不能把自己的 bug 记成"对方协议坏了"。
        return Status::Fail(ErrorCode::kUnknown, "FrameReader::append: null data with non-zero length");
    }

    // 先检查上限再插入：如果直接插进去再判断，超大输入已经占用了内存，
    // 检查就失去意义了。
    if (buffer_.size() + len > kMaxBufferedBytes)
    {
        return Status::Fail(ErrorCode::kProtocol, "frame reader buffer overflow: peer is not sending valid frames");
    }

    buffer_.insert(buffer_.end(), data, data + len);
    return Status::Ok();
}

Status FrameReader::append(const std::vector<char> &data)
{
    return append(data.data(), data.size());
}

bool FrameReader::next(protocol::Frame &frame, Status &status)
{
    status = Status::Ok();

    // 内循环，直到"解出一帧"或"确实需要更多数据"为止。
    //
    // 为什么必须循环：unpackFrame 在丢掉前导垃圾时返回 kNeedMoreData，
    // 此时 consumed 非零但帧其实就在后面——只是还没被解析。如果这里
    // 直接返回 false，调用方会以为"还差字节"，而实际上字节已经够了，
    // 要等到下一次 socket 读才可能解出来。对端若之后不再发数据（比如
    // 它正好发完最后一帧），这一帧就永远卡在缓冲里。
    //
    // 循环的终止条件很明确：consumed == 0 的 kNeedMoreData 表示真的缺字节。
    while (!buffer_.empty())
    {
        // ------------------------------------------------------------------
        // 第一步：只看固定头，判断整帧还要等多少字节。
        //
        // 为什么不能像早先那样"直接把整个缓冲拷成 vector 交给 unpackFrame"：
        // 那是 O(n²)，而且 n 是帧大小。实测在 1MB 块 + 4KB 读缓冲下会直接
        // 把两个进程的 CPU 各吃到 7% 而吞吐掉到 1MB/s：
        //
        //   1MB 的帧需要 256 次 4KB 读才凑齐。每读一次，next() 就把"已累积
        //   的全部字节"完整拷一遍——平均 512KB。单帧就是 256 × 512KB ≈ 130MB
        //   的纯拷贝，乘上窗口里并发的几块，CPU 全花在 memcpy 上。更糟的是
        //   接收端一旦落后（窗口一压），累积缓冲变大，拷贝量按平方增长，
        //   于是"越慢越慢"，最后表现为传输卡死 + CPU 空转。
        //
        // 现在只在两种情况下拷贝：找 magic 时最多 4 字节，以及整帧确实收齐
        //   之后拷一次。每次 read 的常态开销降到几十字节。
        // ------------------------------------------------------------------
        const std::size_t avail = buffer_.size();

        // 不足一个固定头：连帧长都读不出来，等下一次 append。
        if (avail < protocol::frame::kFixedHeaderSize)
        {
            return false;
        }

        // 拷出固定头。36 字节，代价与缓冲大小无关。
        char head[protocol::frame::kFixedHeaderSize];
        {
            std::size_t i = 0;
            for (auto it = buffer_.begin(); i < sizeof(head); ++it, ++i)
            {
                head[i] = *it;
            }
        }

        // 先找 magic。magic 不在开头说明前面是垃圾（连接中段混入的字节，
        // 或者上一条流没排干净）。逐字节丢掉，直到 magic 落在开头。
        //
        // 这里仍然只在 36 字节的栈缓冲上判断，不碰主缓冲。
        std::size_t skip = 0;
        while (skip + 4 <= sizeof(head) &&
               readU32LE(head + skip + kMagicOffset) != protocol::frame::kMagic)
        {
            ++skip;
        }

        if (skip + 4 > sizeof(head))
        {
            // 整个头里都没有 magic。留最后 3 字节（可能是半个 magic），
            // 其余全部当垃圾丢掉。
            skip = (avail > 3) ? (avail - 3) : 0;
            if (skip > 0)
            {
                garbageBytes_ += skip;
                for (std::size_t i = 0; i < skip; ++i)
                {
                    buffer_.pop_front();
                }
            }
            return false;
        }

        if (skip > 0)
        {
            garbageBytes_ += skip;
            for (std::size_t i = 0; i < skip; ++i)
            {
                buffer_.pop_front();
            }
            // 丢掉垃圾之后缓冲可能已经不足一个固定头了，重新判断。
            continue;
        }

        // 现在 magic 在开头。读帧长（偏移 28，小端 u32）。
        const std::uint32_t frameLen = readU32LE(head + kFrameLenOffset);

        // 帧长非法：不可能是真帧，跳过这 1 字节继续找下一个 magic。
        //
        // 不做这一步的后果是灾难性的：body 里出现 'RFM1' 相当常见（压缩数据、
        // 随机二进制都会有），把它当成帧头就会按一个荒谬的长度去等，白等到
        // 缓冲上限然后断连。跳过 1 字节是最便宜的正确做法。
        if (frameLen < protocol::frame::kMinFrameSize || frameLen > protocol::frame::kMaxFrameSize)
        {
            garbageBytes_ += 1;
            buffer_.pop_front();
            continue;
        }

        // 整帧还没收全：**不拷贝**，直接等下一批字节。
        //
        // 这是本次修的关键一行。早先的实现走到这里时已经把整个缓冲拷过一遍了，
        // 而大帧恰恰会在这里反复命中——每来 4KB 就白白拷一次几百 KB 到几 MB。
        if (avail < frameLen)
        {
            return false;
        }

        // ------------------------------------------------------------------
        // 第二步：整帧收齐了，拷这一次交给 unpackFrame 做完整校验与解析。
        // ------------------------------------------------------------------
        std::vector<char> contiguous;
        contiguous.reserve(frameLen);
        {
            std::size_t i = 0;
            for (auto it = buffer_.begin(); i < frameLen; ++it, ++i)
            {
                contiguous.push_back(*it);
            }
        }

        std::size_t consumed = 0;
        const auto result = protocol::unpackFrame(contiguous.data(), contiguous.size(), frame, consumed);

        switch (result)
        {
        case protocol::UnPackResult::kOk:
            for (std::size_t i = 0; i < consumed; ++i)
            {
                buffer_.pop_front();
            }
            ++framesRead_;
            return true;

        case protocol::UnPackResult::kNeedMoreData:
            if (consumed > 0)
            {
                // 头看起来对、但 unpackFrame 进一步校验（头部 CRC、路由字段）
                // 否掉了它，于是只跳过 consumed 字节继续找。这与上面的帧长
                // 检查是两层过滤：前者便宜但粗，后者完整但需要整帧。
                garbageBytes_ += consumed;
                for (std::size_t i = 0; i < consumed; ++i)
                {
                    buffer_.pop_front();
                }
                continue;
            }
            // consumed == 0：整帧都拷出来了却说缺字节，只能继续等。
            // 正常不会发生（上面已经用 avail >= frameLen 拦过），保留出口。
            return false;

        case protocol::UnPackResult::kProtocolError:
            status = Status::Fail(ErrorCode::kProtocol, "frame reader: unrecoverable stream error");
            // 出错后清空缓冲：这条连接已经作废，留着字节只会在下一次 next()
            // 时被重新解析一遍，产生一堆重复的错误日志。
            buffer_.clear();
            return false;
        }
    }

    return false;
}

void FrameReader::clear() noexcept
{
    buffer_.clear();
}

}  // namespace relayer::core::transport
