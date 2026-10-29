#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "relayer/common/Status.h"

// 批量路径元信息（PROTOCOL.md §11.11）。
//
// 用途：发送前把"对方已经有这个文件了"挑出来问用户（v1 的基本检查 +
// 覆盖提示）。一次往返查一批绝对路径，而不是逐条发请求。
//
// 与 kAskFileList 的分工：
//   - 列举是"给我看看这个目录里有什么"，面向人；
//   - 这里是"按我知道的路径查状态"，面向判断，不枚举、不看兄弟项。
//
// 路径是**对端视角的绝对路径**（DESIGN.md §3.8）：单用户工具，不再有
// 共享根这一层沙箱，v1 就是这么干的。
namespace relayer::core::session {

struct RemoteMeta {
    bool exists = false;
    bool isDir = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;        // Unix epoch 秒
    std::string fingerprint;       // IFileStore::quickFingerprint；空 = 没算/读失败
};

// 单次请求的路径数上限。
//
// 取 2000：对照表一次点上千行已经不合理，而每条约 64 字节 + 对端要逐个
// stat/读 64KB 算指纹，再多就变成一次"对方被问懵"的长阻塞。
inline constexpr std::size_t kMaxMetaPaths = 2000;

// `kAskPathMeta`：`u32 count || count × (u16 pathLen || path)`
[[nodiscard]] std::vector<char> encodeAskPathMeta(const std::vector<std::string>& paths);

[[nodiscard]] bool decodeAskPathMeta(const std::vector<char>& body, std::vector<std::string>& paths, std::string& err);

// `kAnswerPathMeta`：`u32 count || count × (u8 flags || u64 size || i64 mtime || u16 fpLen || fp)`
// flags：bit0 = 存在，bit1 = 是目录。条数与请求**一一对应、顺序相同**——
// 请求方按下标对回自己的路径，不在应答里重复路径（省一半带宽，也避免
// 两边对"路径规范化后是否相等"产生分歧）。
[[nodiscard]] Status encodeAnswerPathMeta(const std::vector<RemoteMeta>& metas, std::vector<char>& out);

[[nodiscard]] bool decodeAnswerPathMeta(const std::vector<char>& body, std::vector<RemoteMeta>& metas, std::string& err);

}   // namespace relayer::core::session
