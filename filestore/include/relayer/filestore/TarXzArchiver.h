#pragma once

#include <memory>

#include "relayer/filestore/IArchive.h"
#include "relayer/filestore/IFileStore.h"

// tar.xz 归档。
//
// 为什么默认格式选它而不是 zip：
//   - 单文件流式读写，不需要"先写中央目录、再回头改偏移"的随机访问，
//     损坏时能救回前面完整的条目；zip 的中央目录一旦坏掉整包报废；
//   - LZMA2 对源码、日志、文本的压缩率明显优于 deflate；
//   - 格式简单到能自己实现，编译不引入新依赖。
//
// 代价：不支持随机访问单个条目，列目录要完整解压一遍（list 会做这件事，
// 但对大包会慢）。真正需要随机访问的场景再用 zip。
//
// 实现说明：
//   写：IFileStore 读源文件 → TarWriter → XzEncoder → IFileStore 写归档
//   读：IFileStore 读归档 → XzDecoder → TarReader → IFileStore 写目标
//   两侧都是"分块 + 显式缓冲区"，没有回调套回调。

namespace relayer::filestore
{

class TarXzArchiver final : public IArchive
{
public:
    // store 由调用方持有，生命周期必须覆盖本对象。
    explicit TarXzArchiver(const IFileStore &store);
    ~TarXzArchiver() override;

    TarXzArchiver(const TarXzArchiver &) = delete;
    TarXzArchiver &operator=(const TarXzArchiver &) = delete;

    [[nodiscard]] ArchiveFormat format() const noexcept override;

    Status create(std::string_view archivePath, const std::vector<ArchiveItem> &items, const ArchiveOptions &options,
                  std::vector<std::string> &skipped) const override;

    Status list(std::string_view archivePath, std::vector<ArchiveEntry> &out) const override;

    Status extract(std::string_view archivePath, std::string_view destRoot, const ArchiveOptions &options,
                   std::vector<std::string> &extracted, std::vector<std::string> &skipped) const override;

private:
    const IFileStore &store_;
};

}  // namespace relayer::filestore
