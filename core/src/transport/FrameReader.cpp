#include "relayer/core/transport/FrameReader.h"

#include <algorithm>

namespace relayer::core::transport
{

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
        // unpackFrame 要求连续内存，而 deque 不保证连续。所以每次尝试都要
        // 拷一份到临时 vector。
        //
        // 这个拷贝看起来低效，但换来的是协议层完全不用为容器形态操心——
        // unpackFrame 只需要 (const char*, len) 这个最朴素的接口，它可以被
        // 单元测试用栈上数组直接调用。如果要避免拷贝，就得让 unpackFrame
        // 接受两个迭代器区间，那会把一个本来很简单的函数变复杂（每次越过
        // 边界都要判断），得不偿失。
        //
        // 帧最大 32MB，拷贝一次的代价相对于后面的 CRC 计算可以忽略。
        std::vector<char> contiguous(buffer_.begin(), buffer_.end());

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
                // 扫描跳过的前导垃圾：丢掉，然后继续尝试解帧
                garbageBytes_ += consumed;
                for (std::size_t i = 0; i < consumed; ++i)
                {
                    buffer_.pop_front();
                }
                continue;
            }
            // consumed == 0 表示真的缺字节：保留，等下一次 append
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
