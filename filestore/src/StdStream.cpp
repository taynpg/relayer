#include "StdStream.h"

#include <cerrno>
#include <cstring>

#include "PlatformPath.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace relayer::filestore::detail
{
namespace
{

ErrorCode mapErrno(int err)
{
    switch (classifySysError(err))
    {
    case SysError::kNotFound:
        return ErrorCode::kNotFound;
    case SysError::kPermission:
        return ErrorCode::kPermissionDenied;
    case SysError::kBusy:
        return ErrorCode::kBusy;
    case SysError::kNoSpace:
        return ErrorCode::kNoSpace;
    case SysError::kIsDir:
        return ErrorCode::kIsADirectory;
    case SysError::kNotDir:
        return ErrorCode::kNotADirectory;
    case SysError::kExists:
        return ErrorCode::kAlreadyExists;
    case SysError::kInvalid:
        return ErrorCode::kInvalidPath;
    case SysError::kNone:
        return ErrorCode::kOk;
    case SysError::kOther:
        break;
    }
    return ErrorCode::kIoError;
}

Status ioFailure(int err, const char *op, const std::string &path)
{
    return Status::Fail(mapErrno(err), std::string(op) + " failed: " + path + " (" + std::strerror(err) + ")");
}

int seekFile(std::FILE *fp, std::uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(fp, static_cast<__int64>(offset), SEEK_SET);
#else
    return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::int64_t tellFile(std::FILE *fp)
{
#ifdef _WIN32
    return _ftelli64(fp);
#else
    return static_cast<std::int64_t>(ftello(fp));
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// StdReadStream
// ---------------------------------------------------------------------------

StdReadStream::StdReadStream(std::FILE *fp, std::string path)
    : fp_(fp)
    , path_(std::move(path))
{
}

StdReadStream::~StdReadStream()
{
    if (fp_ != nullptr)
    {
        std::fclose(fp_);
    }
}

std::size_t StdReadStream::read(void *buf, std::size_t len, Status &st)
{
    st = Status::Ok();
    if (len == 0)
    {
        return 0;
    }
    // 单次 fread 的上限用 size_t 就够；调用方按块（通常 64KB~1MB）读
    const std::size_t got = std::fread(buf, 1, len, fp_);
    if (got < len)
    {
        if (std::ferror(fp_) != 0)
        {
            const int err = errno;
            st = ioFailure(err == 0 ? EIO : err, "read", path_);
            return got;
        }
        // 没有错误但不是满读：文件尾
    }
    return got;
}

Status StdReadStream::seek(std::uint64_t offset)
{
    if (seekFile(fp_, offset) != 0)
    {
        return ioFailure(errno, "seek", path_);
    }
    return Status::Ok();
}

std::uint64_t StdReadStream::tell() const
{
    const std::int64_t pos = tellFile(fp_);
    return pos < 0 ? 0 : static_cast<std::uint64_t>(pos);
}

// ---------------------------------------------------------------------------
// StdWriteStream
// ---------------------------------------------------------------------------

StdWriteStream::StdWriteStream(std::FILE *fp, std::string path)
    : fp_(fp)
    , path_(std::move(path))
{
}

StdWriteStream::~StdWriteStream()
{
    if (fp_ != nullptr)
    {
        std::fclose(fp_);
    }
}

Status StdWriteStream::write(const void *buf, std::size_t len)
{
    if (len == 0)
    {
        return Status::Ok();
    }
    const std::size_t wrote = std::fwrite(buf, 1, len, fp_);
    if (wrote != len)
    {
        const int err = errno == 0 ? EIO : errno;
        return ioFailure(err, "write", path_);
    }
    return Status::Ok();
}

Status StdWriteStream::flush()
{
    if (std::fflush(fp_) != 0)
    {
        return ioFailure(errno, "fflush", path_);
    }
    return Status::Ok();
}

Status StdWriteStream::sync()
{
    // 先 flush 用户态缓冲，再要求操作系统落盘。
    // Windows 上 _commit 等价于 fsync；POSIX 上 fsync。
    if (const auto st = flush(); !st)
    {
        return st;
    }
#ifdef _WIN32
    if (_commit(_fileno(fp_)) != 0)
    {
        return ioFailure(errno, "fsync", path_);
    }
#else
    if (::fsync(fileno(fp_)) != 0)
    {
        return ioFailure(errno, "fsync", path_);
    }
#endif
    return Status::Ok();
}

Status StdWriteStream::resize(std::uint64_t size)
{
    if (const auto st = flush(); !st)
    {
        return st;
    }
    const std::int64_t rawCurrent = tellFile(fp_);
    if (rawCurrent < 0)
    {
        return ioFailure(errno, "tell", path_);
    }
    const auto current = static_cast<std::uint64_t>(rawCurrent);
    if (current == size)
    {
        return Status::Ok();
    }
#ifdef _WIN32
    // _chsize_s 同时负责截断与补零扩展，一条 API 覆盖两种情况。
    if (_chsize_s(_fileno(fp_), static_cast<__int64>(size)) != 0)
    {
        return ioFailure(errno, "resize", path_);
    }
#else
    if (::ftruncate(::fileno(fp_), static_cast<off_t>(size)) != 0)
    {
        return ioFailure(errno, "resize", path_);
    }
#endif
    if (const auto st = flush(); !st)
    {
        return st;
    }
    // ftruncate/_chsize_s 不改变 C 流的当前位置，但 resize 之后调用方通常
    // 仍会按自己的偏移写入，这里恢复到原位置以免隐式跳变造成困惑。
    if (seekFile(fp_, current > size ? size : current) != 0)
    {
        return ioFailure(errno, "resize-seek", path_);
    }
    return Status::Ok();
}

}  // namespace relayer::filestore::detail
