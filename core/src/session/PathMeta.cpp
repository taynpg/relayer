#include "relayer/core/session/PathMeta.h"

#include "ByteReader.h"

namespace relayer::core::session {

namespace {
using detail::putI64;
using detail::putU16;
using detail::putU32;
using detail::putU64;
using detail::putU8;
using detail::Reader;

constexpr std::size_t kMaxPathBytes = 32767;   // u16 长度前缀的上限
constexpr std::uint8_t kFlagExists = 1U << 0;
constexpr std::uint8_t kFlagDir = 1U << 1;
}   // namespace

std::vector<char> encodeAskPathMeta(const std::vector<std::string>& paths)
{
    // 超限直接给空体：解码侧会因为"缺 count"判失败，调用方看到的是
    // "对方拒绝了/请求不合法"，而不是一条被静默截断的路径列表——截断后
    // 按下标对结果会全部错位，比失败更难查。
    if (paths.size() > kMaxMetaPaths) {
        return {};
    }
    for (const auto& p : paths) {
        if (p.empty() || p.size() > kMaxPathBytes) {
            return {};
        }
    }

    std::vector<char> out;
    putU32(out, static_cast<std::uint32_t>(paths.size()));
    for (const auto& p : paths) {
        putU16(out, static_cast<std::uint16_t>(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

bool decodeAskPathMeta(const std::vector<char>& body, std::vector<std::string>& paths, std::string& err)
{
    paths.clear();
    Reader r(body);
    const auto count = r.u32();
    if (!r.ok()) {
        err = "ask_path_meta truncated: missing count";
        return false;
    }
    if (count > kMaxMetaPaths) {
        err = "ask_path_meta count exceeds limit";
        return false;
    }
    paths.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto len = r.u16();
        if (!r.ok() || len == 0) {
            err = "ask_path_meta truncated: bad path length";
            return false;
        }
        auto p = r.str(len);
        if (!r.ok()) {
            err = "ask_path_meta truncated inside path";
            return false;
        }
        paths.push_back(std::move(p));
    }
    // 尾部多余字节容忍（对端版本更新），与其它应答解码一致。
    return true;
}

Status encodeAnswerPathMeta(const std::vector<RemoteMeta>& metas, std::vector<char>& out)
{
    out.clear();
    if (metas.size() > 0xFFFFFFFFULL) {
        return Status::Fail(ErrorCode::kProtocol, "path meta list too large");
    }
    putU32(out, static_cast<std::uint32_t>(metas.size()));
    for (const auto& m : metas) {
        if (m.fingerprint.size() > 0xFFFF) {
            return Status::Fail(ErrorCode::kProtocol, "path meta fingerprint is too long");
        }
        std::uint8_t flags = 0;
        if (m.exists) {
            flags |= kFlagExists;
        }
        if (m.isDir) {
            flags |= kFlagDir;
        }
        putU8(out, flags);
        putU64(out, m.size);
        putI64(out, m.mtime);
        putU16(out, static_cast<std::uint16_t>(m.fingerprint.size()));
        out.insert(out.end(), m.fingerprint.begin(), m.fingerprint.end());
    }
    return Status::Ok();
}

bool decodeAnswerPathMeta(const std::vector<char>& body, std::vector<RemoteMeta>& metas, std::string& err)
{
    metas.clear();
    Reader r(body);
    const auto count = r.u32();
    if (!r.ok()) {
        err = "answer_path_meta truncated: missing count";
        return false;
    }
    if (count > kMaxMetaPaths) {
        err = "answer_path_meta count exceeds limit";
        return false;
    }
    metas.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto flags = r.u8();
        RemoteMeta m;
        m.size = r.u64();
        m.mtime = r.i64();
        const auto fpLen = r.u16();
        if (!r.ok()) {
            err = "answer_path_meta truncated inside entry";
            return false;
        }
        m.fingerprint = r.str(fpLen);
        if (!r.ok()) {
            err = "answer_path_meta truncated in fingerprint";
            return false;
        }
        m.exists = (flags & kFlagExists) != 0;
        m.isDir = (flags & kFlagDir) != 0;
        metas.push_back(std::move(m));
    }
    return true;
}

}   // namespace relayer::core::session
