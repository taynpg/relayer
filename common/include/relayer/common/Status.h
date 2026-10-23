#pragma once

#include <string>
#include <string_view>
#include <utility>

// 跨模块共用的错误类型。
//
// 为什么单独抽出来（而不是留在 IFileStore.h 里）：
//
//   filestore 和 core 都要用它报错，而架构规则禁止 filestore 依赖 core。
//   把它放进任一侧都会造成反向依赖或者重复定义。放在 common/ 里，两边都
//   向下依赖同一个定义，错误在模块边界上不需要任何转换。
//
// 命名空间仍然是 relayer（不是 relayer::common）：调用方写
// relayer::Status 就够了，不必关心它物理上住在哪个目录。

namespace relayer
{

// 错误码。
//
// 分成三段是为了让上层能按段做粗粒度判断（比如"这个错误能不能重试"），
// 不用枚举每一个值：
//   kOk .. kUnknown            通用（0-99）
//   kNotFound .. kUnknown      文件系统（100-199）
//   kProtocol .. kAuthFailed   协议与认证（200-299）
//
// 新增错误码时**只能往后加**，不能改动既有值：错误码会进日志、进遥测，
// 改动既有值会让历史日志的解读发生偏移。
enum class ErrorCode
{
    kOk = 0,

    // --- 通用 ---
    kCancelled = 1,  // 调用方主动取消
    kTimeout = 2,    // 等待超时
    kUnknown = 3,

    // --- 文件系统 ---
    kNotFound = 100,
    kPermissionDenied,
    kAlreadyExists,
    kNotADirectory,
    kIsADirectory,
    kNoSpace,
    kBusy,  // 文件被占用（Windows 上常见：另一个进程独占打开）
    kTooManyHandles,
    kInvalidPath,
    kIoError,
    kUnsupported,

    // --- 协议与认证 ---
    kProtocol = 200,  // 对端发来的字节不符合协议（magic/版本/长度/CRC 不对）
    kAuthFailed,      // 口令错、proof 不匹配、重放
    kIdTaken,         // 想用的标识已被占用（kClaimId 被拒，PROTOCOL.md §11.7）
    kIdInvalid,       // 想用的标识不合法（空 / 超长 / 含 NUL）
};

// 错误码的稳定名称，用于日志。**不要**用于本地化 UI 文案：
// 这些字符串是给开发者和运维看的，改它们可能会破坏日志检索规则。
[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

struct Status
{
    ErrorCode code = ErrorCode::kOk;
    std::string message;  // 面向开发者，附带上出错的具体路径与系统错误

    Status() = default;
    Status(ErrorCode c, std::string msg)
        : code(c)
        , message(std::move(msg))
    {
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ErrorCode::kOk;
    }
    explicit operator bool() const noexcept
    {
        return ok();
    }

    static Status Ok()
    {
        return {};
    }
    static Status Fail(ErrorCode c, std::string msg)
    {
        return Status{c, std::move(msg)};
    }
};

}  // namespace relayer
