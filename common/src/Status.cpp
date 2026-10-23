#include "relayer/common/Status.h"

namespace relayer
{

std::string_view toString(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::kOk:
        return "ok";

    case ErrorCode::kCancelled:
        return "cancelled";
    case ErrorCode::kTimeout:
        return "timeout";
    case ErrorCode::kUnknown:
        return "unknown";

    case ErrorCode::kNotFound:
        return "not_found";
    case ErrorCode::kPermissionDenied:
        return "permission_denied";
    case ErrorCode::kAlreadyExists:
        return "already_exists";
    case ErrorCode::kNotADirectory:
        return "not_a_directory";
    case ErrorCode::kIsADirectory:
        return "is_a_directory";
    case ErrorCode::kNoSpace:
        return "no_space";
    case ErrorCode::kBusy:
        return "busy";
    case ErrorCode::kTooManyHandles:
        return "too_many_handles";
    case ErrorCode::kInvalidPath:
        return "invalid_path";
    case ErrorCode::kIoError:
        return "io_error";
    case ErrorCode::kUnsupported:
        return "unsupported";

    case ErrorCode::kProtocol:
        return "protocol";
    case ErrorCode::kAuthFailed:
        return "auth_failed";
    case ErrorCode::kIdTaken:
        return "id_taken";
    case ErrorCode::kIdInvalid:
        return "id_invalid";
    }

    // 走到这里说明新增了枚举值却没更新本函数。
    return "unrecognized";
}

}  // namespace relayer
