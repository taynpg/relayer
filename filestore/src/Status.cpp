#include "relayer/filestore/IFileStore.h"

namespace relayer::filestore
{

std::string_view toString(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::kOk:
        return "ok";
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
    case ErrorCode::kUnknown:
        return "unknown";
    }
    return "unknown";
}

}  // namespace relayer::filestore
