#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// 远端目录列举（PROTOCOL.md §11.8）。
//
// 谁回答：**对端客户端**，不是中继。服务端只做转发（docs/DESIGN.md §3.1），
// 它没有文件能力，也不该有——否则"服务端不碰用户数据"这条边界就破了。
//
// 安全模型：被列举的一方只暴露一个**共享根目录**（配置项），请求里的路径
// 一律按"相对该根"解释，且必须通过 resolveWithin 的越界检查。绝对路径、
// `..` 逃逸、指向根目录之外的一切一律拒绝。默认不共享（shareRoot 为空时
// 直接回拒绝），要显式打开。
namespace relayer::core::session
{

struct RemoteEntry
{
    std::string name;  // 仅文件名
    bool isDir = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;  // Unix epoch 秒
};

// 应答状态（`kAnswerFileList` 的第一个字节）。
//
// kTruncated 仍然带条目：客户端目录可能有几十万项，一次全塞进一帧会撞上
// 帧长上限，而"什么都没返回"对用户毫无用处。截断时明确告知，UI 才能显示
// "只列出前 N 项"而不是假装目录就这么多。
enum class FileListStatus : std::uint8_t
{
    kOk = 0,
    kNotFound = 1,   // 路径不存在 / 不是目录
    kRejected = 2,   // 未开启共享 / 绝对路径 / 越出共享根
    kTruncated = 3,  // 条目过多，只返回前 kMaxListEntries 条
};

// 单次应答的条目上限。取 5000：按每条约 20~60 字节算，帧体在 300KB 内，
// 远小于 kMaxFrameBody；而真实目录（照片、文档）超过 5000 项时本来也该
// 用筛选而不是一次列全。
inline constexpr std::size_t kMaxListEntries = 5000;

// `kAskFileList`：`u8 pathLen || path`（相对共享根；空 = 根目录本身）。
[[nodiscard]] std::vector<char> encodeAskFileList(std::string_view relPath);

// 解码请求。只校验结构；"能不能访问"是服务侧的策略。
[[nodiscard]] bool decodeAskFileList(const std::vector<char> &body, std::string &relPath, std::string &err);

// `kAnswerFileList`：
//   `u8 status || u32 count || count × (u8 nameLen || name || u8 type || u64 size || i64 mtime)`
// type：0 = 文件，1 = 目录。全部小端。
[[nodiscard]] Status encodeAnswerFileList(FileListStatus status, const std::vector<RemoteEntry> &entries,
                                          std::vector<char> &out);

[[nodiscard]] bool decodeAnswerFileList(const std::vector<char> &body, FileListStatus &status,
                                        std::vector<RemoteEntry> &entries, std::string &err);

// ---------------------------------------------------------------------------
// 远端取回（PROTOCOL.md §11.9）
// ---------------------------------------------------------------------------

// `kAnswerFilePull` 的唯一字节。
//
// 请求体**复用** encodeAskFileList / decodeAskFileList：两者的 body 都是
// "相对共享根的路径"，为同一件事定义两套编解码只会让"长度前缀用 u8 还是
// u16"这种问题出现两个答案。
enum class FilePullStatus : std::uint8_t
{
    kAccepted = 0,  // 已接受，随后对端会用 kFileRequestSend 发起一次正常发送
    kNotFound = 1,  // 路径不存在，或指向的是目录
    kRejected = 2,  // 未开启共享 / 路径越界 / 当前正忙
};

[[nodiscard]] std::vector<char> encodeAnswerFilePull(FilePullStatus status);

// 解码。多余字节容忍（对端版本更新），与 answer_id / answer_file_list 一致。
[[nodiscard]] bool decodeAnswerFilePull(const std::vector<char> &body, FilePullStatus &status, std::string &err);

}  // namespace relayer::core::session
