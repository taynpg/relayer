#include "relayer/core/session/PeerTable.h"

#include <cstring>
#include <string>

namespace relayer::core::session
{

namespace
{

// 载荷读写的小工具。
//
// 单独写而不是用 protocol 里的实现：protocol 的那套面向完整帧（含魔法、
// CRC、路由字段），而这里只是 body 内部的裸字节，两者的错误处理方式也
// 不同（一个返回 UnPackResult，一个只需 bool + err）。
void putU8(std::vector<char> &out, std::uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

void putU16(std::vector<char> &out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void putU32(std::vector<char> &out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

// 顺序读取器。越界时 ok() 变 false，之后所有读操作都返回 0。
//
// 用"读失败也不抛、但标记失败"而不是每步都判：解码函数里连续读四五个
// 字段时，逐步判会让主要逻辑淹没在错误处理里。最后统一判一次就够。
class Reader
{
public:
    explicit Reader(const std::vector<char> &b) noexcept
        : buf_(b)
    {
    }

    std::uint8_t u8()
    {
        if (!ok_ || pos_ + 1 > buf_.size())
        {
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
        for (int i = 0; i < 4; ++i)
        {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::string str(std::size_t len)
    {
        if (!ok_ || pos_ + len > buf_.size())
        {
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
    const std::vector<char> &buf_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace

std::string_view toString(RegisterResult r) noexcept
{
    switch (r)
    {
    case RegisterResult::kOk:
        return "ok";
    case RegisterResult::kDuplicateId:
        return "duplicate_id";
    case RegisterResult::kFull:
        return "full";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// PeerTable
// ---------------------------------------------------------------------------

PeerTable::PeerTable(std::size_t capacity) noexcept
    : capacity_(capacity)
{
}

RegisterResult PeerTable::add(const PeerInfo &info)
{
    // 空 id 必须在查重之前拦掉：`find("")` 也会返回 "不存在"，于是空 id
    // 会以"成功"的假象插进表里，之后所有按空 id 查找的调用都会命中它。
    if (!isValidPeerId(info.id))
    {
        return RegisterResult::kDuplicateId;
    }

    if (peers_.find(info.id) != peers_.end())
    {
        return RegisterResult::kDuplicateId;
    }

    if (peers_.size() >= capacity_)
    {
        return RegisterResult::kFull;
    }

    peers_.emplace(info.id, info);
    return RegisterResult::kOk;
}

bool PeerTable::remove(std::string_view id)
{
    const auto it = peers_.find(id);
    if (it == peers_.end())
    {
        return false;
    }
    peers_.erase(it);
    return true;
}

const PeerInfo *PeerTable::find(std::string_view id) const
{
    const auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : &it->second;
}

bool PeerTable::contains(std::string_view id) const
{
    return peers_.find(id) != peers_.end();
}

std::vector<PeerInfo> PeerTable::list() const
{
    std::vector<PeerInfo> out;
    out.reserve(peers_.size());
    for (const auto &kv : peers_)
    {
        out.push_back(kv.second);
    }
    return out;
}

void PeerTable::clear() noexcept
{
    peers_.clear();
}

// ---------------------------------------------------------------------------
// id 分配
// ---------------------------------------------------------------------------

std::string allocateId(const PeerTable &table, std::uint64_t &nextHint)
{
    // 循环直到找到空位。正常情况下第一次就命中——nextHint 单调递增，
    // 只有"表里恰好有人拿到了更大的号"（不可能，因为号都从这里发）或者
    // 调用方自己重置了 hint 时才会连续命中。
    //
    // 循环有一个隐含前提：表不可能被填满到让 while 永不终止。容量上限
    // 1024，而来这里的前提是"确实要新增一条"（调用方已经确认未满），
    // 所以最多试到表满就会退出循环。下面再兜一个断言式返回防止死循环。
    for (std::size_t guard = 0; guard <= table.capacity(); ++guard)
    {
        std::string candidate = "client-";
        candidate.append(std::to_string(nextHint));
        ++nextHint;

        if (!table.contains(candidate))
        {
            return candidate;
        }
    }

    // 理论上到不了这里。返回空串让调用方按"没有 id 可用"处理，
    // 而不是返回一个可能重复的 id 静默制造冲突。
    return {};
}

bool isValidPeerId(std::string_view id) noexcept
{
    // 空 id 不合法：路由字段允许为空（表示"发给服务端自己"），
    // 所以一个空的客户端 id 会与"未指定目标"混淆。
    if (id.empty())
    {
        return false;
    }
    // 上限 255：路由字段用 u8 长度前缀（PROTOCOL.md §9.1）。
    if (id.size() > 255)
    {
        return false;
    }
    // 拒绝 NUL：id 会被塞进 string 并比较、打印，内嵌 NUL 会让日志截断，
    // 也让"看起来相同的两个 id"不等。
    for (const char c : id)
    {
        if (c == '\0')
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 载荷编解码
// ---------------------------------------------------------------------------

std::vector<char> encodeAskId()
{
    return {};
}

Status encodeAnswerId(std::string_view assignedId, std::vector<char> &out)
{
    out.clear();

    if (assignedId.empty())
    {
        // 空 id 表示"服务端没能给你分配"——用 status=1 表达，
        // 而不是发一个长度为 0 的 id（那会被对端当成"成功但身份空白"）。
        putU8(out, 1);
        return Status::Ok();
    }

    if (!isValidPeerId(assignedId))
    {
        return Status::Fail(ErrorCode::kProtocol, "assigned id is not a valid routing field");
    }

    putU8(out, 0);
    putU8(out, static_cast<std::uint8_t>(assignedId.size()));
    out.insert(out.end(), assignedId.begin(), assignedId.end());
    return Status::Ok();
}

bool decodeAnswerId(const std::vector<char> &body, std::uint8_t &status, std::string &assignedId, std::string &err)
{
    status = 0;
    assignedId.clear();

    Reader r(body);
    status = r.u8();
    if (!r.ok())
    {
        err = "answer_id truncated: missing status byte";
        return false;
    }

    if (status != 0)
    {
        // 失败时不该带 id。带了说明对端实现有问题——但这不是安全风险，
        // 只是信息多余，所以容忍并把 id 清空（而不是判错断开）。
        return true;
    }

    const auto len = r.u8();
    if (!r.ok())
    {
        err = "answer_id truncated: missing id length";
        return false;
    }
    if (len == 0)
    {
        // status=0 却说 id 为空：矛盾。这是协议错误——对端声称成功
        // 却给不出身份，我们不能把它登记进 peer 表。
        err = "answer_id has status=ok but empty id";
        return false;
    }

    assignedId = r.str(len);
    if (!r.ok())
    {
        err = "answer_id truncated: id shorter than declared length";
        return false;
    }

    if (r.remaining() != 0)
    {
        // 尾部多余字节：说明对端版本比我们新，或者实现错乱。
        // 不当成错误——多余信息可以忽略，而断开会让版本升级变得更痛苦。
        return true;
    }

    return true;
}

Status encodeClientList(const std::vector<PeerInfo> &peers, std::vector<char> &out)
{
    out.clear();

    if (peers.size() > 0xFFFFFFFFu)
    {
        return Status::Fail(ErrorCode::kProtocol, "client list too large");
    }

    putU32(out, static_cast<std::uint32_t>(peers.size()));
    for (const auto &p : peers)
    {
        if (!isValidPeerId(p.id))
        {
            return Status::Fail(ErrorCode::kProtocol, "client list contains an invalid id");
        }
        if (p.address.size() > 0xFFFFu)
        {
            return Status::Fail(ErrorCode::kProtocol, "client list address too long");
        }

        putU8(out, static_cast<std::uint8_t>(p.id.size()));
        out.insert(out.end(), p.id.begin(), p.id.end());
        putU16(out, static_cast<std::uint16_t>(p.address.size()));
        out.insert(out.end(), p.address.begin(), p.address.end());
    }
    return Status::Ok();
}

bool decodeClientList(const std::vector<char> &body, std::vector<PeerInfo> &peers, std::string &err)
{
    peers.clear();

    Reader r(body);
    const auto count = r.u32();
    if (!r.ok())
    {
        err = "client_list truncated: missing count";
        return false;
    }

    // 先按"每条至少 3 字节"检查 count 与剩余长度的关系，避免一个恶意的
    // 大 count 让我们 reserve 出一块巨大内存然后失败。
    //
    // 每条最短是 1（idLen）+ 1（id，至少 1 字节）+ 2（addrLen）= 4 字节。
    if (count > r.remaining() / 4)
    {
        err = "client_list count exceeds remaining bytes";
        return false;
    }

    peers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        PeerInfo p;
        const auto idLen = r.u8();
        if (!r.ok())
        {
            err = "client_list truncated in id length";
            return false;
        }
        p.id = r.str(idLen);
        if (!r.ok())
        {
            err = "client_list truncated in id";
            return false;
        }

        const auto addrLen = r.u16();
        if (!r.ok())
        {
            err = "client_list truncated in address length";
            return false;
        }
        p.address = r.str(addrLen);
        if (!r.ok())
        {
            err = "client_list truncated in address";
            return false;
        }

        peers.push_back(std::move(p));
    }

    if (r.remaining() != 0)
    {
        err = "client_list has trailing bytes";
        return false;
    }

    return true;
}

}  // namespace relayer::core::session
