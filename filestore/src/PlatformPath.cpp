#include "PlatformPath.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "relayer/filestore/IFileStore.h"
#include "relayer/filestore/detail/Sha256.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

namespace relayer::filestore::detail
{

#ifdef _WIN32

std::wstring toNative(const std::string &utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int need =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0)
    {
        // 非法 UTF-8：退回按系统代码页解释，至少不让调用方拿到空串
        const int fallback = MultiByteToWideChar(CP_ACP, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (fallback <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<std::size_t>(fallback), L'\0');
        MultiByteToWideChar(CP_ACP, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), fallback);
        return out;
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), need);
    return out;
}

std::string fromNative(const std::wstring &wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int need =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), need, nullptr, nullptr);
    return out;
}

#else

NativePath toNative(const std::string &utf8)
{
    return utf8;
}

std::string fromNative(const std::string &native)
{
    return native;
}

#endif

// fopen 的可移植包装：Windows 走 _wfopen 以支持中文/emoji 路径，
// 其它平台直接 fopen。所有打开文件的地方都必须经过这里，
// 这样"编码处理只有一处"的约定才真正成立。
std::FILE *openNativeFile(const std::string &utf8Path, const char *mode)
{
    const NativePath native = toNative(utf8Path);
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return ::_wfopen(native.c_str(), wmode.c_str());
#else
    return std::fopen(native.c_str(), mode);
#endif
}

std::string sha256Hex(const void *data, std::size_t len)
{
    return relayer::crypto::Sha256::hex(relayer::crypto::Sha256::hash(data, len));
}

void sha256Raw(const void *data, std::size_t len, std::uint8_t out[32])
{
    const auto digest = relayer::crypto::Sha256::hash(data, len);
    std::memcpy(out, digest.data(), digest.size());
}

SysError classifySysError(int code)
{
#ifdef _WIN32
    switch (code)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return SysError::kNotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return SysError::kPermission;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return SysError::kBusy;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return SysError::kNoSpace;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return SysError::kExists;
    case ERROR_DIRECTORY:
        return SysError::kNotDir;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_BAD_PATHNAME:
        return SysError::kInvalid;
    default:
        return SysError::kOther;
    }
#else
    switch (code)
    {
    case ENOENT:
    case ENOTDIR:
        return SysError::kNotFound;
    case EACCES:
    case EPERM:
        return SysError::kPermission;
    case EBUSY:
    case ETXTBSY:
        return SysError::kBusy;
    case ENOSPC:
    case EDQUOT:
        return SysError::kNoSpace;
    case EEXIST:
        return SysError::kExists;
    case EISDIR:
        return SysError::kIsDir;
    case ENAMETOOLONG:
    case EINVAL:
        return SysError::kInvalid;
    default:
        return SysError::kOther;
    }
#endif
}

}  // namespace relayer::filestore::detail
