#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// 远端文件操作（PROTOCOL.md §11.12）：删除、重命名、新建目录、算 SHA-256、
// 解压归档。都是"让**对端**在它自己的盘上做一件事"，与 §11.8 的列举、
// §11.9 的取回并列。
//
// 为什么现在才补：这些帧号从 v1 沿革下来一直在 `Frame.h` 里，编解码却从来
// 没实现过——v1 用的是另一套 cereal 序列化（`deserializeStruct`）。v2 换了
// 手写 body 编码（见 PathMeta.cpp / FileList.cpp），所以每条帧都要重新写
// 编码器，这才是真正的缺口，不是"协议不支持"。
//
// 安全边界与 §11.8 完全一致：路径是**对端视角的绝对路径**，没有共享根。
// 能连上中继并知道本机 id 的人可以让本机删文件——这是 DESIGN.md §3.8 已经
// 写明的取舍（单用户自用工具），不要在别处把它当沙箱。
namespace relayer::core::session {

// 应答状态。用 u8 而不是复用它处的枚举：这些操作的失败原因很有限，
// 且要能在界面上分门别类地提示（"已存在"和"没权限"给用户的下一步动作
// 完全不同）。kOk 必须是 0，方便解码侧把"缺状态字节"当成失败。
enum class FileOpStatus : std::uint8_t {
    kOk = 0,
    kNotFound = 1,   // 源路径不存在
    kExists = 2,     // 目标已存在（重命名时拒绝覆盖）
    kDenied = 3,     // 权限不足 / 被占用 / 非空目录删不掉
    kRejected = 4,   // 请求本身不合法（空路径、超长、条数超限）
    kFailed = 5,     // 其它失败（磁盘错误等）
};

// 单次删除请求的路径数上限。与 kMaxMetaPaths 同量级：一次删几千项已经
// 不合理，而服务端是逐个删，条数越多阻塞越久。
inline constexpr std::size_t kMaxOpPaths = 1000;

// 路径长度上限（u16 长度前缀）。与 PathMeta 保持一致。
inline constexpr std::size_t kMaxOpPathBytes = 32767;

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

// 单路径请求：`u16 len || path`。用于 kAskSha256 / kAskCreateDir / kAskUnarchive。
[[nodiscard]] std::vector<char> encodeAskPath(std::string_view path);
[[nodiscard]] bool decodeAskPath(const std::vector<char>& body, std::string& path, std::string& err);

// 重命名：`u16 len || from || u16 len || to`。
[[nodiscard]] std::vector<char> encodeAskRename(std::string_view from, std::string_view to);
[[nodiscard]] bool decodeAskRename(const std::vector<char>& body, std::string& from, std::string& to, std::string& err);

// 批量删除：`u32 count || count × (u16 len || path)`（与 kAskPathMeta 同构）。
[[nodiscard]] std::vector<char> encodeAskDelete(const std::vector<std::string>& paths);
[[nodiscard]] bool decodeAskDelete(const std::vector<char>& body, std::vector<std::string>& paths, std::string& err);

// ---------------------------------------------------------------------------
// 应答
// ---------------------------------------------------------------------------

// 只带状态的应答：kAnswerRename / kAnswerCreateDir。`u8 status`。
[[nodiscard]] Status encodeAnswerFileOp(FileOpStatus status, std::vector<char>& out);
[[nodiscard]] bool decodeAnswerFileOp(const std::vector<char>& body, FileOpStatus& status, std::string& err);

// 删除应答：`u8 status || u32 failCount || failCount × (u16 len || path)`。
//
// 部分失败是常态（一个目录里有个文件被别人占着），所以结论不是单一状态：
// status 表达"整体是否全部成功"，failed 逐条列出没删掉的，界面照 v1 的
// 做法把它们留在表格里。
[[nodiscard]] Status encodeAnswerDelete(FileOpStatus status, const std::vector<std::string>& failed, std::vector<char>& out);
[[nodiscard]] bool decodeAnswerDelete(const std::vector<char>& body, FileOpStatus& status, std::vector<std::string>& failed,
                                      std::string& err);

// SHA-256 应答：`u8 status || u8 32 || 32 字节摘要`。status 非 kOk 时不带摘要。
[[nodiscard]] Status encodeAnswerSha256(FileOpStatus status, const std::vector<char>& digest, std::vector<char>& out);
[[nodiscard]] bool decodeAnswerSha256(const std::vector<char>& body, FileOpStatus& status, std::vector<char>& digest,
                                      std::string& err);

// 解压应答：`u8 status || u16 len || 解出的目录`。失败时目录为空。
[[nodiscard]] Status encodeAnswerUnarchive(FileOpStatus status, std::string_view dir, std::vector<char>& out);
[[nodiscard]] bool decodeAnswerUnarchive(const std::vector<char>& body, FileOpStatus& status, std::string& dir, std::string& err);

// 状态的中文说明（界面与日志共用，避免两处各写一套措辞）。
[[nodiscard]] const char* fileOpStatusText(FileOpStatus status) noexcept;

}   // namespace relayer::core::session
