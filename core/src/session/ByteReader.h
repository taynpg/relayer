#pragma once

// 载荷内部字节读写（仅 core/src/session 内部使用，不进公开头文件）。
//
// 为什么不用 protocol 那一套：`core/protocol` 的帧编解码面向完整帧（magic、
// CRC、路由字段），失败语义是 UnPackResult；而这里的调用方处理的是帧体
// 内部的裸字节，失败只需要"结构坏了"一个信息。两套混用会让错误处理变形。
//
// 字节序：全部小端，与 PROTOCOL.md §2 的约定一致。
#include <cstdint>
#include <string>
#include <vector>

namespace relayer::core::session::detail {

inline void putU8(std::vector<char>& out, std::uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

inline void putU16(std::vector<char>& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

inline void putU32(std::vector<char>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

inline void putU64(std::vector<char>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

inline void putI64(std::vector<char>& out, std::int64_t v)
{
    putU64(out, static_cast<std::uint64_t>(v));
}

// 顺序读取器。越界时 ok() 变 false，之后所有读操作都返回 0。
//
// 用"读失败也不抛、但标记失败"而不是每步都判：解码函数里连续读四五个
// 字段时，逐步判会让主要逻辑淹没在错误处理里。最后统一判一次就够。
class Reader
{
public:
    explicit Reader(const std::vector<char>& b) noexcept : buf_(b)
    {
    }

    std::uint8_t u8()
    {
        if (!ok_ || pos_ + 1 > buf_.size()) {
            ok_ = false;
            return 0;
        }
        return static_cast<std::uint8_t>(buf_[pos_++]);
    }

    std::uint16_t u16()
    {
        const auto lo = u8();
        const auto hi = u8();
        return static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
    }

    std::uint32_t u32()
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::uint64_t u64()
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::int64_t i64()
    {
        return static_cast<std::int64_t>(u64());
    }

    std::string str(std::size_t len)
    {
        if (!ok_ || pos_ + len > buf_.size()) {
            ok_ = false;
            return {};
        }
        std::string s(buf_.data() + pos_, len);
        pos_ += len;
        return s;
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return ok_;
    }
    [[nodiscard]] std::size_t consumed() const noexcept
    {
        return pos_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return buf_.size() - pos_;
    }

private:
    const std::vector<char>& buf_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}   // namespace relayer::core::session::detail
