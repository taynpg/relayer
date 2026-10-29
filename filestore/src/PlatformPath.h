#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace relayer::filestore::detail {

// 把 UTF-8 字符串转成平台原生路径表示（Windows 上是 UTF-16 宽字符串，
// 其它平台保持 UTF-8 原样）。
//
// 单独抽出来是为了让转换逻辑只有一个实现、一处出错——v1 里各文件各自
// 调 QString::fromLocal8Bit / toUtf8，踩过编码不一致的坑。

#ifdef _WIN32
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

NativePath toNative(const std::string& utf8);
std::string fromNative(const NativePath& native);

// 当前用户的家目录（UTF-8）。取不到时返回空串。
// 用途：远端浏览的根列表里放一个"家目录"快捷入口（IFileStore::roots）。
std::string homeDirectory();

// 以 UTF-8 路径打开 C 流。Windows 上内部走 _wfopen。
// 约定：本模块内所有文件打开都必须经由此函数，不要直接调 std::fopen。
std::FILE* openNativeFile(const std::string& utf8Path, const char* mode);

// 稳定哈希：用于 quickFingerprint。
// 调用方保证 data 非空；实现是单次 SHA-256，输出 64 位十六进制。
std::string sha256Hex(const void* data, std::size_t len);
void sha256Raw(const void* data, std::size_t len, std::uint8_t out[32]);

// 把系统错误码翻译成 ErrorCode。
// 两个平台分别实现，映射表集中在这里，避免散落在各个调用点。
enum class SysError {
    kNone,
    kNotFound,
    kPermission,
    kBusy,
    kNoSpace,
    kIsDir,
    kNotDir,
    kExists,
    kInvalid,
    kOther,
};

SysError classifySysError(int code);

}   // namespace relayer::filestore::detail
