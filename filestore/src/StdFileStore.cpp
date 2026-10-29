#include "relayer/filestore/StdFileStore.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "PlatformPath.h"
#include "StdStream.h"
#include "relayer/common/crypto/Sha256.h"

namespace fs = std::filesystem;

#ifdef _WIN32
#include <windows.h>
#endif

namespace relayer::filestore {
namespace {

// errno 到 ErrorCode 的统一映射。
// 注意 Windows 上 fopen 失败时 errno 是 CRT 的映射值（EBUSY 等），
// 不是 GetLastError；共享冲突在 CRT 里通常表现为 EACCES，所以这里把
// EACCES 直接归为 permission，真正的"被占用"由 probeWritable 用
// 原生 API 单独判断。
ErrorCode errnoToCode(int err)
{
    switch (detail::classifySysError(err)) {
    case detail::SysError::kNone:
        return ErrorCode::kOk;
    case detail::SysError::kNotFound:
        return ErrorCode::kNotFound;
    case detail::SysError::kPermission:
        return ErrorCode::kPermissionDenied;
    case detail::SysError::kBusy:
        return ErrorCode::kBusy;
    case detail::SysError::kNoSpace:
        return ErrorCode::kNoSpace;
    case detail::SysError::kIsDir:
        return ErrorCode::kIsADirectory;
    case detail::SysError::kNotDir:
        return ErrorCode::kNotADirectory;
    case detail::SysError::kExists:
        return ErrorCode::kAlreadyExists;
    case detail::SysError::kInvalid:
        return ErrorCode::kInvalidPath;
    case detail::SysError::kOther:
        break;
    }
    return ErrorCode::kIoError;
}

// file_time_type 到 Unix epoch 秒的换算。
//
// C++17 的 file_time_type 纪元未标准化（MSVC 是 1601，libstdc++ 是 2174），
// 没有直接换算公式。用"同一时刻在两种时钟下的差"绕过去：delta 是文件时间
// 相对当前时刻的偏移，套回 system_clock 就得到 Unix 时间。精度受两次 now()
// 的调用间隔影响（微秒级），对秒级 mtime 绰绰有余。
std::int64_t toUnixTime(std::filesystem::file_time_type ftime)
{
    const auto sysNow = std::chrono::system_clock::now();
    const auto fileNow = std::filesystem::file_time_type::clock::now();
    const auto delta = ftime - fileNow;
    const auto sysTime = sysNow + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(sysTime));
}

// 统一的失败构造：附上路径与系统提示，日志里能直接定位
Status fromSystem(const std::error_code& ec, const char* op, const std::string& path)
{
    ErrorCode code = ErrorCode::kIoError;
    switch (detail::classifySysError(ec.value())) {
    case detail::SysError::kNotFound:
        code = ErrorCode::kNotFound;
        break;
    case detail::SysError::kPermission:
        code = ErrorCode::kPermissionDenied;
        break;
    case detail::SysError::kBusy:
        code = ErrorCode::kBusy;
        break;
    case detail::SysError::kNoSpace:
        code = ErrorCode::kNoSpace;
        break;
    case detail::SysError::kIsDir:
        code = ErrorCode::kIsADirectory;
        break;
    case detail::SysError::kNotDir:
        code = ErrorCode::kNotADirectory;
        break;
    case detail::SysError::kExists:
        code = ErrorCode::kAlreadyExists;
        break;
    case detail::SysError::kInvalid:
        code = ErrorCode::kInvalidPath;
        break;
    case detail::SysError::kNone:
        return Status::Ok();
    case detail::SysError::kOther:
        break;
    }
    return Status::Fail(code, std::string(op) + " failed: " + path + " (" + ec.message() + ")");
}

fs::path toPath(std::string_view p)
{
    return fs::path(detail::toNative(std::string(p)));
}

}   // namespace

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------
// 全部委托给 PathPolicy。不在这里用 std::filesystem::path 的 lexically_normal，
// 是因为它要先把路径转成宽字符再转回来，多两次分配，而且 C++17/C++20 对
// 相对路径里 `..` 的处理有细微差异。纯字符串实现的规则可控、可测。

std::string StdFileStore::normalize(std::string_view path) const
{
    return policy_.normalize(path);
}

std::string StdFileStore::join(std::string_view a, std::string_view b) const
{
    return policy_.join(a, b);
}

std::string StdFileStore::resolveWithin(std::string_view root, std::string_view child) const
{
    return policy_.resolveWithin(root, child);
}

// ---------------------------------------------------------------------------
// 元信息
// ---------------------------------------------------------------------------

FileStat StdFileStore::stat(std::string_view path) const
{
    FileStat out{};
    std::error_code ec;
    const fs::path p = toPath(path);
    const auto st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return out;
    }
    out.exists = true;
    out.isSymlink = fs::is_symlink(st);

    // 符号链接跟随后再取真实属性：断链时 exists 仍为 true 但大小取不到
    const auto target = fs::status(p, ec);
    if (!ec) {
        out.isDir = fs::is_directory(target);
        out.readable = (target.permissions() & fs::perms::owner_read) != fs::perms::none;
        out.writable = (target.permissions() & fs::perms::owner_write) != fs::perms::none;
        out.executable = (target.permissions() & fs::perms::owner_exec) != fs::perms::none;
    }
    if (!out.isDir) {
        const auto size = fs::file_size(p, ec);
        if (!ec) {
            out.size = size;
        }
        const auto ftime = fs::last_write_time(p, ec);
        if (!ec) {
            out.mtime = toUnixTime(ftime);
        }
    }

    // 权限位：POSIX 上按位取，Windows 上给一组合理默认。
    // 默认值不是随便拍的——打包后解到 Linux 上，0644/0755 是最不会
    // 出问题的组合（可执行位只给确实需要执行的扩展名）。
    out.mode = static_cast<std::uint16_t>(out.isDir ? 0755 : 0644);
#if defined(_WIN32)
    if (!out.isDir) {
        // 扩展名判断，大小写不敏感
        const std::string ext = [&] {
            const std::size_t dot = std::string_view(path).find_last_of('.');
            if (dot == std::string_view::npos) {
                return std::string{};
            }
            std::string e(path.substr(dot));
            for (char& c : e) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return e;
        }();
        if (ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".dll" || ext == ".com") {
            out.mode = 0755;
        }
    }
#else
    if (!ec) {
        const auto raw = static_cast<unsigned>(target.permissions());
        out.mode = static_cast<std::uint16_t>(raw & 07777U);
    }
#endif
    return out;
}

Status StdFileStore::probeWritable(std::string_view path) const
{
    const fs::path p = toPath(path);
    std::error_code ec;

    if (fs::exists(p, ec)) {
        // 存在：以追加模式打开但不写任何内容，Windows 上能区分"被独占占用"与"没权限"
        std::FILE* fp = detail::openNativeFile(std::string(path), "ab");
        if (fp == nullptr) {
            const int err = errno;
            switch (detail::classifySysError(err)) {
            case detail::SysError::kBusy:
                return Status::Fail(ErrorCode::kBusy, "file is locked by another process: " + std::string(path));
            case detail::SysError::kPermission:
                return Status::Fail(ErrorCode::kPermissionDenied, "no write permission: " + std::string(path));
            default:
                return Status::Fail(ErrorCode::kIoError,
                                    "cannot open for write: " + std::string(path) + " (" + std::strerror(err) + ")");
            }
        }
        std::fclose(fp);
        return Status::Ok();
    }

    // 不存在：探测父目录
    const fs::path dir = p.parent_path();
    if (!fs::exists(dir, ec)) {
        return Status::Fail(ErrorCode::kNotFound, "parent directory does not exist: " + dir.generic_string());
    }
    if (!fs::is_directory(dir, ec)) {
        return Status::Fail(ErrorCode::kNotADirectory, "parent is not a directory: " + dir.generic_string());
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

std::unique_ptr<IReadStream> StdFileStore::openRead(std::string_view path, Status& st) const
{
    st = Status::Ok();
    std::FILE* fp = detail::openNativeFile(std::string(path), "rb");
    if (fp == nullptr) {
        const int err = errno;
        st = Status::Fail(errnoToCode(err), "open for read failed: " + std::string(path) + " (" + std::strerror(err) + ")");
        return nullptr;
    }
    return std::make_unique<detail::StdReadStream>(fp, std::string(path));
}

std::unique_ptr<IWriteStream> StdFileStore::openWrite(std::string_view path, bool truncate, Status& st) const
{
    st = Status::Ok();
    std::FILE* fp = detail::openNativeFile(std::string(path), truncate ? "wb" : "r+b");
    if (fp == nullptr) {
        const int err = errno;
        st = Status::Fail(errnoToCode(err), "open for write failed: " + std::string(path) + " (" + std::strerror(err) + ")");
        return nullptr;
    }
    return std::make_unique<detail::StdWriteStream>(fp, std::string(path));
}

// ---------------------------------------------------------------------------
// 目录
// ---------------------------------------------------------------------------

Status StdFileStore::list(std::string_view dir, bool recursive, std::vector<DirEntry>& out) const
{
    out.clear();
    std::error_code ec;
    const fs::path base = toPath(dir);
    if (!fs::exists(base, ec)) {
        return Status::Fail(ErrorCode::kNotFound, "directory not found: " + std::string(dir));
    }
    if (!fs::is_directory(base, ec)) {
        return Status::Fail(ErrorCode::kNotADirectory, "not a directory: " + std::string(dir));
    }

    auto emit = [&out](const fs::directory_entry& entry) {
        std::error_code inner;
        DirEntry e;
        e.name = detail::fromNative(entry.path().filename().wstring());
        e.isDir = entry.is_directory(inner);
        if (!e.isDir) {
            const auto size = entry.file_size(inner);
            e.size = inner ? 0 : size;
        }
        // mtime 对目录也有意义（内容变化即更新）。取不到（权限/竞态删除）
        // 就留 0，让上层显示"未知"而不是报错——列举是读操作，别因单条
        // 元信息失败丢掉整个列表。
        const auto ftime = entry.last_write_time(inner);
        if (!inner) {
            e.mtime = toUnixTime(ftime);
        }
        out.push_back(std::move(e));
    };

    if (!recursive) {
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (ec) {
                return fromSystem(ec, "list", std::string(dir));
            }
            emit(entry);
        }
        if (ec) {
            return fromSystem(ec, "list", std::string(dir));
        }
        return Status::Ok();
    }

    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return fromSystem(ec, "recursive list", std::string(dir));
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            // 递归中某个子目录无权限：跳过继续，不要整体失败
            ec.clear();
            continue;
        }
        emit(*it);
    }
    return Status::Ok();
}

Status StdFileStore::mkdirs(std::string_view dir) const
{
    std::error_code ec;
    const fs::path p = toPath(dir);
    if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
        return Status::Ok();
    }
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p, ec)) {
        return fromSystem(ec, "mkdirs", std::string(dir));
    }
    return Status::Ok();
}

std::vector<std::string> StdFileStore::roots() const
{
    std::vector<std::string> out;
#if defined(_WIN32)
    // 用 GetLogicalDrives 而不是遍历 'A'..'Z' 判存在：后者会把映射过但
    // 已断开的网络驱动器也列出来（exists 对断开的映射返回 false，但
    // 每次探测都要等一次网络超时，盘符一多界面就卡住）。
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1UL << i)) == 0) {
            continue;
        }
        std::string root;
        root.push_back(static_cast<char>('A' + i));
        root += ":\\";
        out.push_back(std::move(root));
    }
#else
    out.emplace_back("/");
#endif
    // 家目录不是"根"，但把它放进根列表是给用户的快捷键：绝大多数远端
    // 浏览都是从家目录开始，而不是从 / 或 C:\ 一层层点下去。
    const auto home = detail::homeDirectory();
    if (!home.empty()) {
        out.push_back(home);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 变更
// ---------------------------------------------------------------------------

Status StdFileStore::remove(std::string_view path) const
{
    std::error_code ec;
    const fs::path p = toPath(path);
    const bool removed = fs::remove(p, ec);
    if (ec) {
        return fromSystem(ec, "remove", std::string(path));
    }
    (void)removed;   // 不存在时返回 false 且 ec 为空，视为成功
    return Status::Ok();
}

Status StdFileStore::resize(std::string_view path, std::uint64_t size) const
{
    Status st;
    auto stream = openWrite(path, false, st);
    if (!stream) {
        return st;
    }
    return stream->resize(size);
}

Status StdFileStore::atomicReplace(std::string_view from, std::string_view to) const
{
#ifdef _WIN32
    // 裸 std::filesystem::rename 在目标存在时会失败；
    // MOVEFILE_REPLACE_EXISTING 才是"覆盖式原子替换"的正确语义。
    const std::wstring wfrom = detail::toNative(std::string(from));
    const std::wstring wto = detail::toNative(std::string(to));
    if (::MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return Status::Ok();
    }
    return Status::Fail(ErrorCode::kIoError, "atomicReplace failed: " + std::string(from) + " -> " + std::string(to));
#else
    std::error_code ec;
    fs::rename(toPath(from), toPath(to), ec);
    if (ec) {
        return fromSystem(ec, "atomicReplace", std::string(from) + " -> " + std::string(to));
    }
    return Status::Ok();
#endif
}

// ---------------------------------------------------------------------------
// 快速指纹
// ---------------------------------------------------------------------------

std::string StdFileStore::quickFingerprint(std::string_view path, std::int64_t sizeOverride) const
{
    Status st;
    auto stream = openRead(path, st);
    if (!stream) {
        return {};
    }

    std::uint64_t total = 0;
    if (sizeOverride >= 0) {
        total = static_cast<std::uint64_t>(sizeOverride);
    } else {
        const FileStat fs = stat(path);
        total = fs.size;
    }

    // 指纹 = SHA256(size 的小端 8 字节 || 前 64KB 内容 || 有效内容长度的小端 8 字节)
    //
    // 为什么这么设计（v1 的坑在这里必须彻底解决）：
    //
    //   续传场景下，.relaypart 残片只有前 N 字节，N 通常远小于文件总长。指纹要能
    //   回答"这份残片是不是那个文件的"，就必须让残片与源文件的指纹可比较。
    //
    //   做法：内容长度固定取 min(64KB, 源文件总长)，缺的部分按零字节补齐；
    //   同时把"实际有效长度"也混进哈希。于是：
    //     - 源文件（16 字节）   → 哈希(size=16, "0123456789ABCDEF", valid=16)
    //     - 残片（前 6 字节）   → 哈希(size=16, "012345" + 10 个零, valid=6)
    //   两者不同，但**前缀部分一致**，调用方比对时可以看到 valid 差异并决定
    //   "这是同一份但还没下完"。
    //
    //   因此真正用于续传判断的接口应当同时返回 size 与 valid，见 Fingerprint。
    constexpr std::size_t kProbe = 64 * 1024;
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(total, kProbe));

    std::vector<char> head(want, '\0');
    std::size_t got = 0;
    while (got < want) {
        Status rs;
        const std::size_t n = stream->read(head.data() + got, want - got, rs);
        if (!rs) {
            return {};
        }
        if (n == 0) {
            break;
        }
        got += n;
    }

    relayer::crypto::Sha256 h;
    std::uint8_t totalLe[8];
    std::uint8_t validLe[8];
    for (int i = 0; i < 8; ++i) {
        totalLe[i] = static_cast<std::uint8_t>((total >> (i * 8)) & 0xFF);
        validLe[i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(got) >> (i * 8)) & 0xFF);
    }
    h.update(totalLe, sizeof(totalLe));
    h.update(head.data(), head.size());
    h.update(validLe, sizeof(validLe));
    return relayer::crypto::Sha256::hex(h.digest());
}

}   // namespace relayer::filestore
