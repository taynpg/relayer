#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "relayer/filestore/IFileStore.h"

// 归档格式的读写。
//
// 设计约束（改之前先读 docs/REWRITE-PLAN.md 3.1.4）：
//
// 1. **不碰具体文件系统实现**，只依赖 IFileStore。归档层因此可以在内存里
//    跑完整测试（MemFileStore），不必为了测 zip-slip 真去落盘。
//
// 2. **条目的落点必须经过 resolveWithin**。归档里记录的名字是攻击者可控的
//    数据——构造一个含 `../../` 或绝对路径的 zip 就能往任意位置写文件。
//    v1 的防线是 `startsWith(rootPrefix)`，但没有分隔符边界，
//    `/data/foo2` 会被判成在 `/data/foo` 之内。v2 一律走 PathPolicy。
//
// 3. **不信任归档自报的任何绝对路径**。即使归档里写了 `C:/Windows/x.dll`，
//    也只会被解释成 root 下的相对路径——这比"拒绝"更安全也更省事：
//    用户拿到一个从别人机器打包的归档（内含 originalPath 之类的字段），
//    解包时仍然落在自己选的目录里。
//
// 4. 失败返回 Status，不抛异常（与 IFileStore 一致）。

namespace relayer::filestore
{

// 归档格式。
enum class ArchiveFormat
{
    kZip,    // .zip，miniz 实现，可随机访问
    kTarXz,  // .tar.xz，ustar + PAX + LZMA2，流式压缩率更高
};

[[nodiscard]] std::string_view toString(ArchiveFormat format);

// 根据文件名后缀猜格式。不认识返回 false。
[[nodiscard]] bool guessFormat(std::string_view fileName, ArchiveFormat &out);

// 清洗一个待打包的条目名，返回空串表示"这个条目不能进归档"。
//
// 规则：
//   - 反斜杠统一成 `/`（Windows 调用方可能传 `dir\file`）；
//   - 丢掉空段（`a//b`）和 `.` 段（`./a`）；
//   - 含 `..` 段 → 拒绝；
//   - 以盘符开头（`C:...`）→ 拒绝；
//   - 清洗后为空（例如只传了 `/`）→ 拒绝。
//
// 为什么"含 `..` 就拒绝"而不是尝试折叠：
//   调用方给的路径本身有问题时，静默改写成别的名字会让"打出来的包"
//   和"用户以为打进去的东西"不一致。宁可让调用方在 skipped 里看到它。
//
// 注意这里只管"名字能不能进归档"，不管源路径是否真的存在。
[[nodiscard]] std::string sanitizeEntryName(std::string_view entryName);

// 打包时的一个条目。
struct ArchiveItem
{
    // 源路径（在 IFileStore 里）
    std::string sourcePath;

    // 归档里的名字。必须是相对路径，用 `/` 分隔，不带前导 `/`。
    // 由调用方决定（通常取相对于某个公共父目录的路径），
    // 归档层不再做二次推断——"相对谁的路径"这种决定一旦藏进库里，
    // 调用方就永远说不清自己打出来的包长什么样。
    std::string entryName;
};

// 解包时的一个条目（供列表/预览用）。
struct ArchiveEntry
{
    std::string name;  // 归档里的原始名字（未经清洗，可能是恶意路径）
    bool isDir{};
    std::uint64_t size{};
    std::uint32_t crc32{};  // zip 有；tar 恒为 0
};

struct ArchiveOptions
{
    // 压缩级别。zip: 0-10（0 = 只存储）；tar.xz: 0-9。
    // 默认 6：对传输场景来说，压缩时间不敏感（相比网络），
    // 但过高的级别收益递减明显。
    int compressionLevel = 6;

    // 解包时是否保留归档里的权限位（仅 Unix 有效）。
    // Windows 上忽略。
    bool preservePermissions = true;

    // 解包时遇到已存在的文件：true 覆盖，false 跳过。
    bool overwrite = true;
};

class IArchive
{
public:
    virtual ~IArchive() = default;

    [[nodiscard]] virtual ArchiveFormat format() const noexcept = 0;

    // 打包：把 items 写进 archivePath。
    //
    // 每项的 entryName 会先过一遍路径检查：必须是相对路径、不含 `..`、
    // 不以分隔符开头。不合规的条目不进归档（并在 skipped 里记下），
    // 因为"打出一个自己解不开的包"比"少打一个文件"更糟。
    virtual Status create(std::string_view archivePath, const std::vector<ArchiveItem> &items,
                          const ArchiveOptions &options, std::vector<std::string> &skipped) const = 0;

    // 列出归档内容。不写出任何文件。
    // 条目名字保留原样（不做清洗），调用方自己决定如何展示。
    virtual Status list(std::string_view archivePath, std::vector<ArchiveEntry> &out) const = 0;

    // 解包到 destRoot 之下。每个条目落点 = resolveWithin(destRoot, entryName)，
    // 返回空串的条目被跳过并在 skipped 里记下（不是失败——
    // 一个恶意条目不该让整个解包失败，用户还得拿到其余文件）。
    virtual Status extract(std::string_view archivePath, std::string_view destRoot, const ArchiveOptions &options,
                           std::vector<std::string> &extracted, std::vector<std::string> &skipped) const = 0;
};

}  // namespace relayer::filestore
