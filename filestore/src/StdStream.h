#pragma once

#include <cstdio>
#include <string>

#include "relayer/filestore/IFileStore.h"

// C stdio 实现的读写流。
//
// 为什么不用 std::fstream：
//   1. Windows 上 fstream 打开非 ASCII 路径要走 std::filesystem::path 的
//      宽字符重载，跨标准版本行为不一致；_wfopen 直接明确。
//   2. fread/fwrite 的返回值语义比 ostream 的 failbit 更直接——传输引擎需要
//      "要么全写、要么失败"的确定性判断。
//   3. 错误码可以直接取自 errno，便于统一映射成 ErrorCode。

namespace relayer::filestore::detail {

class StdReadStream final : public IReadStream
{
public:
    StdReadStream(std::FILE* fp, std::string path);
    ~StdReadStream() override;

    StdReadStream(const StdReadStream&) = delete;
    StdReadStream& operator=(const StdReadStream&) = delete;

    std::size_t read(void* buf, std::size_t len, Status& st) override;
    Status seek(std::uint64_t offset) override;
    [[nodiscard]] std::uint64_t tell() const override;

private:
    std::FILE* fp_;
    std::string path_;
};

class StdWriteStream final : public IWriteStream
{
public:
    StdWriteStream(std::FILE* fp, std::string path);
    ~StdWriteStream() override;

    StdWriteStream(const StdWriteStream&) = delete;
    StdWriteStream& operator=(const StdWriteStream&) = delete;

    Status write(const void* buf, std::size_t len) override;
    Status seek(std::uint64_t offset) override;
    [[nodiscard]] std::uint64_t tell() const override;
    Status flush() override;
    Status sync() override;
    Status resize(std::uint64_t size) override;

private:
    std::FILE* fp_;
    std::string path_;
};

}   // namespace relayer::filestore::detail
