#pragma once

#include <memory>
#include <string>

#include "relayer/filestore/IFileStore.h"
#include "relayer/filestore/PathPolicy.h"

namespace relayer::filestore {

// 基于 C++17 标准库的默认实现。
//
// 平台细节都关在这里：
//   - Windows：路径先转成 std::wstring 再交给标准库，避免非 ASCII 路径出现
//     "找不到文件"。不用 std::filesystem::u8path —— 它在 C++20 换了返回类型，
//     跨标准版本编译会炸。
//   - Windows：打开文件统一走 _wfopen（见 PlatformPath.h 的 openNativeFile），
//     std::fopen 只认 ANSI 代码页，中文路径会失败。
//   - Windows：atomicReplace 用 MoveFileEx(MOVEFILE_REPLACE_EXISTING)，
//     裸 std::filesystem::rename 在目标存在时会失败。
//   - 打开失败时把 errno / GetLastError 翻译成 ErrorCode，其中
//     "被占用" 映射为 kBusy（v1 靠 QFile 的 PermissionError 猜，这里显式判定）。
class StdFileStore final : public IFileStore
{
public:
    StdFileStore() = default;

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "std";
    }

    [[nodiscard]] std::string normalize(std::string_view path) const override;
    [[nodiscard]] std::string join(std::string_view a, std::string_view b) const override;
    [[nodiscard]] std::string resolveWithin(std::string_view root, std::string_view child) const override;

    [[nodiscard]] FileStat stat(std::string_view path) const override;
    [[nodiscard]] Status probeWritable(std::string_view path) const override;

    std::unique_ptr<IReadStream> openRead(std::string_view path, Status& st) const override;
    std::unique_ptr<IWriteStream> openWrite(std::string_view path, bool truncate, Status& st) const override;

    Status list(std::string_view dir, bool recursive, std::vector<DirEntry>& out) const override;
    [[nodiscard]] std::vector<std::string> roots() const override;
    Status mkdirs(std::string_view dir) const override;

    Status remove(std::string_view path) const override;
    Status resize(std::string_view path, std::uint64_t size) const override;
    Status atomicReplace(std::string_view from, std::string_view to) const override;

    [[nodiscard]] std::string quickFingerprint(std::string_view path, std::int64_t sizeOverride = -1) const override;

private:
    PathPolicy policy_;
};

}   // namespace relayer::filestore
