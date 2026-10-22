#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// 载荷内的字节读写（PROTOCOL.md §9.1）。
//
// 这套工具只处理 **frame.body 内部的裸字节**，不碰帧头、不碰 CRC、不碰路由
// 字段——那些属于 protocol 层（Frame.h）。两者刻意分开：帧层的读失败要区分
// "半包 / 垃圾 / 协议错误"三种（UnPackResult），而 body 内部读越界只有一种
// 含义——对端给了个不符合规格的载荷，直接判协议错误即可。
//
// 放在 common/ 而不是各模块各写一份：这份代码已经在 session 与 transfer 两处
// 需要，第三处（控制类帧的应答解析）也已经在排队。复制三份之后"小端还是
// 大端""长度前缀是 u8 还是 u16"这类约定就会开始分叉，而分叉的后果是
// 两端对不上却都能各自自测通过。
//
// 约定：
//   - 整数一律小端（§9.1），宽度显式
//   - 字符串用 u16 长度前缀，**不带 NUL**；路由字段例外，用 u8（见 Reader::shortStr）
//   - 列表用 u32 元素个数前缀
//
// 错误处理：写侧超限返回 false（调用方转成 Status）；读侧越界后 ok() 变 false，
// 之后所有读都返回 0/空。这样解码函数可以连续读五六个字段，最后统一判一次。

namespace relayer::common
{

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------

class PayloadWriter
{
public:
    explicit PayloadWriter(std::vector<char> &out) noexcept
        : out_(out)
    {
    }

    void u8(std::uint8_t v)
    {
        out_.push_back(static_cast<char>(v));
    }

    void u16(std::uint16_t v)
    {
        out_.push_back(static_cast<char>(v & 0xFF));
        out_.push_back(static_cast<char>((v >> 8) & 0xFF));
    }

    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }

    void u64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
        {
            out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }

    // i64 按补码原样写出（mtime 等字段可能为负）。
    void i64(std::int64_t v)
    {
        u64(static_cast<std::uint64_t>(v));
    }

    // 原始字节，无长度前缀。调用方自己保证长度约定。
    void raw(const void *data, std::size_t len)
    {
        if (len == 0)
        {
            return;
        }
        const auto *p = static_cast<const char *>(data);
        out_.insert(out_.end(), p, p + len);
    }

    // u16 长度前缀 + 内容。超长返回 false 且不写入任何字节。
    //
    // 不写入是关键：半截写入会让调用方在失败后仍然拿到一个"看起来完整"
    // 但实际缺了尾巴的载荷，而那个样子很像成功。
    [[nodiscard]] bool str(std::string_view s)
    {
        if (s.size() > 0xFFFF)
        {
            return false;
        }
        u16(static_cast<std::uint16_t>(s.size()));
        raw(s.data(), s.size());
        return true;
    }

    // u8 长度前缀 + 内容（路由字段与 id 用这个，§9.1）。
    [[nodiscard]] bool shortStr(std::string_view s)
    {
        if (s.size() > 0xFF)
        {
            return false;
        }
        u8(static_cast<std::uint8_t>(s.size()));
        raw(s.data(), s.size());
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return out_.size();
    }

private:
    std::vector<char> &out_;
};

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

class PayloadReader
{
public:
    // 借用外部缓冲，不复制。
    explicit PayloadReader(const void *data, std::size_t len) noexcept
        : ptr_(static_cast<const char *>(data))
        , len_(len)
    {
    }

    explicit PayloadReader(const std::vector<char> &b) noexcept
        : ptr_(b.data())
        , len_(b.size())
    {
    }

    std::uint8_t u8()
    {
        if (!ok_ || pos_ + 1 > len_)
        {
            ok_ = false;
            return 0;
        }
        return static_cast<std::uint8_t>(ptr_[pos_++]);
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
        for (int i = 0; i < 4; ++i)
        {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::uint64_t u64()
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::int64_t i64()
    {
        return static_cast<std::int64_t>(u64());
    }

    // 取一段裸字节的指针 + 长度。不复制。
    //
    // 返回的指针指向原缓冲，调用方不得跨缓冲生命周期保存。
    const char *raw(std::size_t n)
    {
        if (!ok_ || pos_ + n > len_)
        {
            ok_ = false;
            return nullptr;
        }
        const char *p = ptr_ + pos_;
        pos_ += n;
        return p;
    }

    // u16 长度前缀的字符串。
    std::string str()
    {
        const auto n = u16();
        if (!ok_)
        {
            return {};
        }
        const char *p = raw(n);
        return p == nullptr ? std::string{} : std::string(p, n);
    }

    // u8 长度前缀的字符串。
    std::string shortStr()
    {
        const auto n = u8();
        if (!ok_)
        {
            return {};
        }
        const char *p = raw(n);
        return p == nullptr ? std::string{} : std::string(p, n);
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return ok_;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return pos_ <= len_ ? len_ - pos_ : 0;
    }

    [[nodiscard]] std::size_t pos() const noexcept
    {
        return pos_;
    }

    // 是否已经读到末尾。
    [[nodiscard]] bool atEnd() const noexcept
    {
        return pos_ == len_;
    }

    // 主动标记失败。用于"字段读出来了但值不合法"这类语义校验。
    void fail() noexcept
    {
        ok_ = false;
    }

private:
    const char *ptr_ = nullptr;
    std::size_t len_ = 0;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace relayer::common
