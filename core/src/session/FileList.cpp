#include "relayer/core/session/FileList.h"

#include "ByteReader.h"

namespace relayer::core::session
{

namespace
{
using detail::putI64;
using detail::putU32;
using detail::putU64;
using detail::putU8;
using detail::Reader;

// 条目名必须是**单段**：不是路径、不是 `.`/`..`、不含分隔符。
//
// 为什么要在解码层拦，而不是等 UI 双击时再检查：应答来自对端，那里的
// 文件名不在我们控制之内（对端可能被改过、也可能有 bug）。一条名叫
// `../..` 的条目一旦进了列表，UI 双击就会把它拼进当前路径——虽然服务端
// 会再拒一次，但客户端不该把这种请求发出去。放在这里，所有消费者
// （GUI / CLI / 将来别的）一次受保护。
//
// **只拦 `/`，不拦 `\\`**：Unix 允许文件名含反斜杠，那是合法文件；
// Windows 上它才是分隔符，那层判断属于 UI 的平台语义，不在这里做
// （见 ExplorerControl 双击处的检查）。
bool isSingleSegmentName(std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }
    if (name.find('/') != std::string_view::npos)
    {
        return false;
    }
    if (name.find('\0') != std::string_view::npos)
    {
        return false;
    }
    return true;
}
}  // namespace

std::vector<char> encodeAskFileList(std::string_view relPath)
{
    std::vector<char> out;
    // 与路由字段同用 u8 长度（§9.1）：同一个"路径"概念不该有两套长度编码。
    // 超过 255 字节的相对路径直接截断是危险的，所以由调用方保证长度；
    // 这里超长时给空体（解码侧会判成"空路径 = 根"，不会误解成别的目录）。
    if (relPath.size() > 255)
    {
        return out;
    }
    putU8(out, static_cast<std::uint8_t>(relPath.size()));
    out.insert(out.end(), relPath.begin(), relPath.end());
    return out;
}

bool decodeAskFileList(const std::vector<char> &body, std::string &relPath, std::string &err)
{
    relPath.clear();
    Reader r(body);
    const auto len = r.u8();
    if (!r.ok())
    {
        err = "ask_file_list truncated: missing length byte";
        return false;
    }
    if (r.remaining() != len)
    {
        // 严格等长：多出来的字节说明发送方不是按这张表编码的。容忍会让
        // "长度字段与实际内容不一致"这类 bug 一直藏下去。
        err = "ask_file_list length mismatch";
        return false;
    }
    relPath = r.str(len);
    if (!r.ok())
    {
        err = "ask_file_list truncated: missing path bytes";
        return false;
    }
    return true;
}

Status encodeAnswerFileList(FileListStatus status, const std::vector<RemoteEntry> &entries, std::vector<char> &out)
{
    out.clear();
    putU8(out, static_cast<std::uint8_t>(status));

    // 拒绝/不存在时也发 count=0，而不是省掉这个字段：解码侧不必为
    // "有没有 count"分两种结构。
    const std::size_t count =
        (status == FileListStatus::kRejected || status == FileListStatus::kNotFound) ? 0 : entries.size();
    if (count > 0xFFFFFFFFULL)
    {
        return Status::Fail(ErrorCode::kProtocol, "file list too large to encode");
    }
    putU32(out, static_cast<std::uint32_t>(count));

    if (count == 0)
    {
        return Status::Ok();
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &e = entries[i];
        if (e.name.size() > 255)
        {
            // 名字长度用 u8（与路径一致）。超长名字在 Windows 上几乎不可能
            // （上限 255 UTF-16 单元），但真出现时宁可在编码处失败，也不
            // 静默截断——截断后的名字指向的是另一个文件。
            return Status::Fail(ErrorCode::kProtocol, "file list entry name is too long");
        }
        if (!isSingleSegmentName(e.name))
        {
            // 与解码侧同一条规则：名字必须是单段。编码处出现这种值说明
            // 本地列举的实现有问题（DirEntry.name 不该含分隔符），早失败
            // 比发出去让对方拒收更靠近肇事点。
            return Status::Fail(ErrorCode::kProtocol, "file list entry name is not a single path segment");
        }
        putU8(out, static_cast<std::uint8_t>(e.name.size()));
        out.insert(out.end(), e.name.begin(), e.name.end());
        putU8(out, e.isDir ? 1 : 0);
        putU64(out, e.size);
        putI64(out, e.mtime);
    }
    return Status::Ok();
}

bool decodeAnswerFileList(const std::vector<char> &body, FileListStatus &status, std::vector<RemoteEntry> &entries,
                          std::string &err)
{
    status = FileListStatus::kNotFound;
    entries.clear();

    Reader r(body);
    const auto rawStatus = r.u8();
    if (!r.ok())
    {
        err = "answer_file_list truncated: missing status byte";
        return false;
    }
    if (rawStatus > static_cast<std::uint8_t>(FileListStatus::kTruncated))
    {
        // 未知状态说明对端版本更新。不当成连接错误——上层会按"失败"显示，
        // 而断开会把一次版本差异变成一次不可恢复的会话失败。
        err = "answer_file_list has unknown status";
        return false;
    }
    status = static_cast<FileListStatus>(rawStatus);

    const auto count = r.u32();
    if (!r.ok())
    {
        err = "answer_file_list truncated: missing count";
        return false;
    }
    if (count > kMaxListEntries)
    {
        // 超过上限直接判结构错误：否则一个坏长度会让内存按 4GB 规模分配。
        err = "answer_file_list count exceeds limit";
        return false;
    }

    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto nameLen = r.u8();
        if (!r.ok())
        {
            err = "answer_file_list truncated: missing name length";
            return false;
        }
        RemoteEntry e;
        e.name = r.str(nameLen);
        const auto type = r.u8();
        e.size = r.u64();
        e.mtime = r.i64();
        if (!r.ok())
        {
            err = "answer_file_list truncated inside entry";
            return false;
        }
        e.isDir = (type == 1);

        // 整份拒绝而不是跳过这一条：合法对端不可能发出这种名字，出现即
        // 说明对端有 bug 或被动过，此时整份列表都不值得信。放过坏条目、
        // 只丢一条会变成"目录里莫名少了个文件"，比明确报错更难查。
        if (!isSingleSegmentName(e.name))
        {
            err = "answer_file_list entry name is not a single path segment";
            entries.clear();
            return false;
        }
        entries.push_back(std::move(e));
    }

    // 尾部多余字节容忍（对端版本更新），与 PeerTable 的 answer_id 一致。
    return true;
}

}  // namespace relayer::core::session
