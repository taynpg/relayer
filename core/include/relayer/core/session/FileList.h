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
// 路径模型：**对端视角的绝对路径**（DESIGN.md §3.8）。不再有共享根这层
// 沙箱——本工具的定位是单用户自用，v1 就是全路径访问，多一层"相对根"
// 只会让"我要看 D:\照片"变成先配置再浏览两步。
//
// 根层：请求路径为空时返回"根列表"（Windows 各盘符 + 家目录，Unix 是
// `/` 与家目录）。根列表里的条目名是完整路径（`C:\`、`/home/x`），
// 不是单段名——这是唯一允许条目名带分隔符的地方，见 RemoteEntry 注释。
namespace relayer::core::session {

struct RemoteEntry {
    std::string name;   // 普通条目：仅文件名；根列表：完整路径
    bool isDir = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;   // Unix epoch 秒
};

// 应答状态（`kAnswerFileList` 的第一个字节）。
//
// kTruncated 仍然带条目：客户端目录可能有几十万项，一次全塞进一帧会撞上
// 帧长上限，而"什么都没返回"对用户毫无用处。截断时明确告知，UI 才能显示
// "只列出前 N 项"而不是假装目录就这么多。
enum class FileListStatus : std::uint8_t {
    kOk = 0,
    kNotFound = 1,    // 路径不存在 / 不是目录
    kRejected = 2,    // 未开启共享 / 绝对路径 / 越出共享根
    kTruncated = 3,   // 条目过多，只返回前 kMaxListEntries 条
};

// 单次应答的条目上限。取 5000：按每条约 20~60 字节算，帧体在 300KB 内，
// 远小于 kMaxFrameBody；而真实目录（照片、文档）超过 5000 项时本来也该
// 用筛选而不是一次列全。
inline constexpr std::size_t kMaxListEntries = 5000;

// `kAskFileList`：`u16 pathLen || path`（绝对路径；空 = 根列表）。
//
// 路径长度用 u16 而不是 u8：绝对路径在 Windows 上轻易超过 255 字节。
[[nodiscard]] std::vector<char> encodeAskFileList(std::string_view absPath);

// 解码请求。只校验结构；"这个路径能不能读"是服务侧的事。
[[nodiscard]] bool decodeAskFileList(const std::vector<char>& body, std::string& absPath, std::string& err);

// `kAnswerFileList`：
//   `u8 status || u8 flags || u32 count || count × (u8 nameLen || name || u8 type || u64 size || i64 mtime)`
// type：0 = 文件，1 = 目录。flags bit0 = 这是根列表（条目名是完整路径）。
// 全部小端。
//
// 为什么根列表要单独标记而不是靠"名字里有没有分隔符"猜：普通列表的条目名
// 必须单段（防对端塞 `../..`），而根列表天生就是完整路径。让解码侧按
// 一个显式标志切换校验规则，比让校验器去猜"这条看起来像不像绝对路径"
// 可靠——后者对端只要发一个叫 `../../etc` 的条目就能绕过。
[[nodiscard]] Status encodeAnswerFileList(FileListStatus status, bool rootListing, const std::vector<RemoteEntry>& entries,
                                          std::vector<char>& out);

[[nodiscard]] bool decodeAnswerFileList(const std::vector<char>& body, FileListStatus& status, bool& rootListing,
                                        std::vector<RemoteEntry>& entries, std::string& err);

// ---------------------------------------------------------------------------
// 远端取回（PROTOCOL.md §11.9）
// ---------------------------------------------------------------------------

// `kAnswerFilePull` 的唯一字节。
//
// 请求体**复用** encodeAskFileList / decodeAskFileList：两者的 body 都是
// "相对共享根的路径"，为同一件事定义两套编解码只会让"长度前缀用 u8 还是
// u16"这种问题出现两个答案。
enum class FilePullStatus : std::uint8_t {
    kAccepted = 0,   // 已接受，随后对端会用 kFileRequestSend 发起一次正常发送
    kNotFound = 1,   // 路径不存在，或指向的是目录
    kRejected = 2,   // 未开启共享 / 路径越界 / 当前正忙
};

[[nodiscard]] std::vector<char> encodeAnswerFilePull(FilePullStatus status);

// 解码。多余字节容忍（对端版本更新），与 answer_id / answer_file_list 一致。
[[nodiscard]] bool decodeAnswerFilePull(const std::vector<char>& body, FilePullStatus& status, std::string& err);

}   // namespace relayer::core::session
