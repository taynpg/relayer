#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "relayer/filestore/IFileStore.h"
#include "relayer/filestore/PathPolicy.h"

// 内存文件系统。
//
// 存在的唯一目的：让"写盘失败要中断传输"这类逻辑可以被单测。
// v1 里这条逻辑完全无法验证——它只能靠真把磁盘写满来触发，
// 于是从来没被验证过，线上才暴露出"磁盘满后传输挂住不报错"。
//
// 用法：注入故障，然后断言传输引擎的行为。
//
//   MemFileStore fs;
//   fs.setFailAfterBytes(1024 * 1024);   // 写满 1MB 后开始失败
//   fs.injectError("save/out.bin", ErrorCode::kNoSpace);
//   auto engine = TransferEngine{fs, ...};
//
// 设计取舍：
//
//   - **不是线程安全的**。它只用于单线程测试。需要并发测试时请自行加锁，
//     不要在本类内部加锁——那会掩盖被测代码里真正的竞态。
//   - **容量可设上限**，用来模拟磁盘满。超限时返回 kNoSpace 而不是崩溃。
//   - **不模拟权限位**（Unix 的那套），只按路径前缀注入错误，
//     因为 Windows 上权限模型完全不同，模拟"权限拒绝"没有通用意义。
//   - 路径规则复用 PathPolicy，因此它和 StdFileStore 的路径语义一致——
//     测试里换实现不会得到不同的拼接结果。

namespace relayer::filestore
{

// 一次故障注入。所有字段都是可选的。
struct FaultInjection
{
    // 命中该前缀的路径触发故障。空串表示"所有路径"。
    std::string pathPrefix;

    // 允许前 afterOps 次**写操作**成功，之后开始失败。
    //
    // 语义细节（容易踩）：
    //   - 只有 `write()` / `resize()` 计数，`openWrite()` / `remove()` 不计数。
    //     否则"第 3 次写失败"的含义会随调用方是否先 open 而变化，测试里说不清。
    //   - `afterOps == 0` 表示不按次数触发，从 openWrite 阶段就失败
    //     ——用于模拟"根本打不开文件"（被占用、无权限）。
    //   - `afterOps > 0` 只在写入阶段生效，openWrite 一定成功
    //     ——用于模拟"写到一半磁盘满"。
    std::uint64_t afterOps = 0;

    // 触发时返回的错误码
    ErrorCode code = ErrorCode::kIoError;

    // 是否只触发一次（true）还是一直触发（false）
    bool once = false;
};

class MemFileStore final : public IFileStore
{
public:
    MemFileStore();
    ~MemFileStore() override;

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "mem";
    }

    // ---- 路径（与 StdFileStore 语义一致）----
    [[nodiscard]] std::string normalize(std::string_view path) const override;
    [[nodiscard]] std::string join(std::string_view a, std::string_view b) const override;
    [[nodiscard]] std::string resolveWithin(std::string_view root, std::string_view child) const override;

    [[nodiscard]] FileStat stat(std::string_view path) const override;
    [[nodiscard]] Status probeWritable(std::string_view path) const override;

    std::unique_ptr<IReadStream> openRead(std::string_view path, Status &st) const override;
    std::unique_ptr<IWriteStream> openWrite(std::string_view path, bool truncate, Status &st) const override;

    Status list(std::string_view dir, bool recursive, std::vector<DirEntry> &out) const override;
    Status mkdirs(std::string_view dir) const override;

    Status remove(std::string_view path) const override;
    Status resize(std::string_view path, std::uint64_t size) const override;
    Status atomicReplace(std::string_view from, std::string_view to) const override;

    [[nodiscard]] std::string quickFingerprint(std::string_view path, std::int64_t sizeOverride = -1) const override;

    // ---- 测试辅助 ----
    // 这些方法不属于 IFileStore，只在测试里用。

    // 直接放入一个文件（不经过流，不触发故障注入）
    void putFile(const std::string &path, const std::string &content);
    void putDir(const std::string &path);

    // 取回文件内容。不存在时返回空并可查 exists
    [[nodiscard]] std::string getFile(const std::string &path) const;
    [[nodiscard]] bool has(const std::string &path) const;

    // 磁盘容量上限（字节），0 表示不限。
    // 超限时写入返回 kNoSpace，模拟真实磁盘满。
    void setCapacity(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t usedBytes() const;

    // 注入故障。可叠加多条，按添加顺序判断。
    void injectFault(FaultInjection fault);
    void clearFaults();

    // 当前所有文件的路径（排序后），便于断言目录结构
    [[nodiscard]] std::vector<std::string> allFiles() const;

    // 写流被销毁时是否自动 flush。默认 true。
    // 设为 false 可以模拟"进程崩溃导致缓冲丢失"。
    void setFlushOnClose(bool value);

private:
    struct Node
    {
        bool isDir = false;
        std::string content;  // 仅文件
        std::int64_t mtime = 0;
    };

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // 写流需要直接读写 Impl 里的节点。用 friend 而不是把 Impl 暴露到头上，
    // 是为了让"内存实现的结构"成为真正的私有细节。
    friend class MemWriteStream;
    friend class MemReadStream;
};

}  // namespace relayer::filestore
