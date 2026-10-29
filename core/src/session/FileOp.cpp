#include "relayer/core/session/FileOp.h"

#include "ByteReader.h"

namespace relayer::core::session {

namespace {
using detail::putU16;
using detail::putU32;
using detail::putU8;
using detail::Reader;

// 一条路径能否放进 u16 长度前缀。空路径在协议里没有意义（"空"是根列表，
// 那是列举才用的语义），所以这里直接判非法。
bool acceptablePath(std::string_view p) noexcept
{
    return !p.empty() && p.size() <= kMaxOpPathBytes;
}

void putPath(std::vector<char>& out, std::string_view p)
{
    putU16(out, static_cast<std::uint16_t>(p.size()));
    out.insert(out.end(), p.begin(), p.end());
}

// 读一条路径。空长度视为结构错误：请求方不会（也不该）发空路径。
bool takePath(Reader& r, std::string& out)
{
    const auto len = r.u16();
    if (!r.ok() || len == 0) {
        return false;
    }
    out = r.str(len);
    return r.ok();
}
}   // namespace

std::vector<char> encodeAskPath(std::string_view path)
{
    if (!acceptablePath(path)) {
        return {};   // 空体 → 解码侧判失败，调用方看到的是"请求不合法"
    }
    std::vector<char> out;
    putPath(out, path);
    return out;
}

bool decodeAskPath(const std::vector<char>& body, std::string& path, std::string& err)
{
    path.clear();
    Reader r(body);
    if (!takePath(r, path)) {
        err = "ask path truncated or empty";
        return false;
    }
    return true;   // 尾部多余字节容忍（对端版本更新），与其它解码一致
}

std::vector<char> encodeAskRename(std::string_view from, std::string_view to)
{
    if (!acceptablePath(from) || !acceptablePath(to)) {
        return {};
    }
    std::vector<char> out;
    putPath(out, from);
    putPath(out, to);
    return out;
}

bool decodeAskRename(const std::vector<char>& body, std::string& from, std::string& to, std::string& err)
{
    from.clear();
    to.clear();
    Reader r(body);
    if (!takePath(r, from)) {
        err = "ask rename truncated: bad source path";
        return false;
    }
    if (!takePath(r, to)) {
        err = "ask rename truncated: bad target path";
        return false;
    }
    return true;
}

std::vector<char> encodeAskDelete(const std::vector<std::string>& paths)
{
    if (paths.empty() || paths.size() > kMaxOpPaths) {
        return {};
    }
    for (const auto& p : paths) {
        if (!acceptablePath(p)) {
            return {};
        }
    }
    std::vector<char> out;
    putU32(out, static_cast<std::uint32_t>(paths.size()));
    for (const auto& p : paths) {
        putPath(out, p);
    }
    return out;
}

bool decodeAskDelete(const std::vector<char>& body, std::vector<std::string>& paths, std::string& err)
{
    paths.clear();
    Reader r(body);
    const auto count = r.u32();
    if (!r.ok()) {
        err = "ask delete truncated: missing count";
        return false;
    }
    if (count == 0 || count > kMaxOpPaths) {
        err = "ask delete count out of range";
        return false;
    }
    paths.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string p;
        if (!takePath(r, p)) {
            err = "ask delete truncated inside path";
            return false;
        }
        paths.push_back(std::move(p));
    }
    return true;
}

Status encodeAnswerFileOp(FileOpStatus status, std::vector<char>& out)
{
    out.clear();
    putU8(out, static_cast<std::uint8_t>(status));
    return Status::Ok();
}

bool decodeAnswerFileOp(const std::vector<char>& body, FileOpStatus& status, std::string& err)
{
    Reader r(body);
    status = static_cast<FileOpStatus>(r.u8());
    if (!r.ok()) {
        err = "answer file op missing status";
        return false;
    }
    return true;
}

Status encodeAnswerDelete(FileOpStatus status, const std::vector<std::string>& failed, std::vector<char>& out)
{
    out.clear();
    if (failed.size() > kMaxOpPaths) {
        return Status::Fail(ErrorCode::kProtocol, "delete failed list too large");
    }
    for (const auto& p : failed) {
        if (!acceptablePath(p)) {
            return Status::Fail(ErrorCode::kProtocol, "delete failed path is empty or too long");
        }
    }
    putU8(out, static_cast<std::uint8_t>(status));
    putU32(out, static_cast<std::uint32_t>(failed.size()));
    for (const auto& p : failed) {
        putPath(out, p);
    }
    return Status::Ok();
}

bool decodeAnswerDelete(const std::vector<char>& body, FileOpStatus& status, std::vector<std::string>& failed, std::string& err)
{
    failed.clear();
    Reader r(body);
    status = static_cast<FileOpStatus>(r.u8());
    const auto count = r.u32();
    if (!r.ok()) {
        err = "answer delete truncated";
        return false;
    }
    if (count > kMaxOpPaths) {
        err = "answer delete failed count out of range";
        return false;
    }
    failed.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string p;
        if (!takePath(r, p)) {
            err = "answer delete truncated inside failed path";
            return false;
        }
        failed.push_back(std::move(p));
    }
    return true;
}

Status encodeAnswerSha256(FileOpStatus status, const std::vector<char>& digest, std::vector<char>& out)
{
    out.clear();
    // -- 成功必须带完整摘要：半截摘要在界面上会显示成一个"看起来正常"的
    // 短串，比明确失败更糟。
    if (status == FileOpStatus::kOk && digest.size() != 32) {
        return Status::Fail(ErrorCode::kProtocol, "sha256 digest must be 32 bytes");
    }
    putU8(out, static_cast<std::uint8_t>(status));
    if (status == FileOpStatus::kOk) {
        putU8(out, static_cast<std::uint8_t>(digest.size()));
        out.insert(out.end(), digest.begin(), digest.end());
    }
    return Status::Ok();
}

bool decodeAnswerSha256(const std::vector<char>& body, FileOpStatus& status, std::vector<char>& digest, std::string& err)
{
    digest.clear();
    Reader r(body);
    status = static_cast<FileOpStatus>(r.u8());
    if (!r.ok()) {
        err = "answer sha256 missing status";
        return false;
    }
    if (status != FileOpStatus::kOk) {
        return true;   // 失败时不带摘要，正常
    }
    const auto len = r.u8();
    if (!r.ok() || len != 32) {
        err = "answer sha256 digest length is not 32";
        return false;
    }
    const std::string raw = r.str(len);
    if (!r.ok()) {
        err = "answer sha256 truncated inside digest";
        return false;
    }
    digest.assign(raw.begin(), raw.end());
    return true;
}

Status encodeAnswerUnarchive(FileOpStatus status, std::string_view dir, std::vector<char>& out)
{
    out.clear();
    const std::string_view d = (status == FileOpStatus::kOk) ? dir : std::string_view{};
    if (d.size() > kMaxOpPathBytes) {
        return Status::Fail(ErrorCode::kProtocol, "unarchive dir path too long");
    }
    putU8(out, static_cast<std::uint8_t>(status));
    putU16(out, static_cast<std::uint16_t>(d.size()));
    out.insert(out.end(), d.begin(), d.end());
    return Status::Ok();
}

bool decodeAnswerUnarchive(const std::vector<char>& body, FileOpStatus& status, std::string& dir, std::string& err)
{
    dir.clear();
    Reader r(body);
    status = static_cast<FileOpStatus>(r.u8());
    const auto len = r.u16();
    if (!r.ok()) {
        err = "answer unarchive truncated";
        return false;
    }
    if (len > 0) {
        dir = r.str(len);
        if (!r.ok()) {
            err = "answer unarchive truncated inside dir";
            return false;
        }
    }
    return true;
}

const char* fileOpStatusText(FileOpStatus status) noexcept
{
    switch (status) {
    case FileOpStatus::kOk:
        return "成功";
    case FileOpStatus::kNotFound:
        return "路径不存在";
    case FileOpStatus::kExists:
        return "目标已存在";
    case FileOpStatus::kDenied:
        return "权限不足或被占用";
    case FileOpStatus::kRejected:
        return "请求不合法（路径为空或超长）";
    case FileOpStatus::kFailed:
        return "操作失败";
    }
    return "未知状态";
}

}   // namespace relayer::core::session
