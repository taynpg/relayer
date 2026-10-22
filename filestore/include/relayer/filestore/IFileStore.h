#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// 文件子系统抽象接口。
//
// 设计约束（修改前请先读 docs/REWRITE-PLAN.md 第 3.1 节）：
//
// 1. 路径一律 UTF-8 编码的 std::string，Windows 宽字符转换由实现负责。
//    不要在接口上暴露 std::wstring / QString / std::filesystem::path，
//    否则换实现时调用方要跟着改。
//
// 2. 失败是常态，不用异常穿越接口边界。所有可能失败的操作用 Status 返回，
//    或通过出参带回。异常只允许出现在实现内部（例如 std::filesystem 抛出的
//    filesystem_error），且必须就地捕获转成 Status。
//
// 3. 接口方法全部 const。实现内部如果需要缓存（例如缓存的根目录规范形式），
//    用 mutable 成员并在文档里说明。这样调用方可以持有 const 引用传递。
//
// 4. 没有全局状态、没有单例。实例由调用方创建并注入，
//    传输引擎只认接口，不认具体实现。

namespace relayer::filestore
{

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------

// Status 与 ErrorCode 定义在 relayer/common/Status.h（core 与 filestore 共用）。
// 这里引入别名，是为了让本模块内的代码继续写 Status / ErrorCode 而不必
// 到处加 relayer:: 前缀；对外签名也没有变化，调用方不受影响。
//
// ErrorCode 的可读名称由 relayer::toString(ErrorCode) 提供（也在 Status.h 里）。
// 这里**不**再导出一个 filestore::toString 转发：两个同名函数在 relayer
// 命名空间里同时可见时，非限定调用会产生歧义。需要名字的地方写
// relayer::toString(code)。
using Status = ::relayer::Status;
using ErrorCode = ::relayer::ErrorCode;

// ---------------------------------------------------------------------------
// 元信息
// ---------------------------------------------------------------------------

struct FileStat
{
    bool exists{};
    bool isDir{};
    bool isSymlink{};
    std::uint64_t size{};  // 目录为 0
    std::int64_t mtime{};  // Unix epoch 秒；取不到时为 0
    bool readable{};
    bool writable{};
    bool executable{};
    // POSIX 权限位（低 12 位：rwxrwxrwx + setuid/setgid/sticky）。
    //
    // 为什么存原始位而不是三个 bool：打包时要把权限原样写进 tar 头，
    // 用 readable/writable/executable 反推重建会丢掉 setgid 之类的位
    // （解出来是 0755 而不是 2755）。Windows 上取不到时填 0644/0755 的
    // 合理默认：普通文件 0644、目录 0755、.exe/.bat/.cmd/.dll 0755。
    std::uint16_t mode{};
};

struct DirEntry
{
    std::string name;  // 仅文件名，不含目录部分
    bool isDir{};
    std::uint64_t size{};
    std::int64_t mtime{};
};

// ---------------------------------------------------------------------------
// 流
// ---------------------------------------------------------------------------

// 顺序读。read 返回实际读到的字节数；0 且 Status 为 Ok 表示正常到达文件尾。
class IReadStream
{
public:
    virtual ~IReadStream() = default;

    // 读取至多 len 字节到 buf。返回实际字节数，失败时置 st 并返回 0。
    virtual std::size_t read(void *buf, std::size_t len, Status &st) = 0;

    // 绝对定位。用于断点续传与乱序落盘。
    virtual Status seek(std::uint64_t offset) = 0;

    [[nodiscard]] virtual std::uint64_t tell() const = 0;
};

// 顺序写。实现负责缓冲，flush 与 sync 的语义见各自注释。
class IWriteStream
{
public:
    virtual ~IWriteStream() = default;

    // 写入 len 字节。必须全部写入或返回失败，不允许"部分写入但返回成功"
    // ——传输引擎依赖这一点来判断是否需要重传。
    virtual Status write(const void *buf, std::size_t len) = 0;

    // 把用户态缓冲交给操作系统。不保证落盘。
    virtual Status flush() = 0;

    // 请求操作系统把数据刷到存储介质。代价高，只在关键点调用
    // （传输完成、原子替换之前）。
    virtual Status sync() = 0;

    // 改变文件长度。截断或补零。
    virtual Status resize(std::uint64_t size) = 0;
};

// ---------------------------------------------------------------------------
// 接口
// ---------------------------------------------------------------------------

class IFileStore
{
public:
    virtual ~IFileStore() = default;

    // 实现名，用于日志与诊断（例如 "std" / "qt" / "mem"）
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // ---- 路径 ----
    // 路径折叠、拼接、绝对性判定都是纯字符串逻辑，与磁盘无关，
    // 实现应当退化为 `return policy_.normalize(path);`。
    //
    // 之所以还留在接口上（而不是让调用方直接用 PathPolicy）：
    // 调用方手上只有 IFileStore&，不该再额外传一个 policy；
    // 而且允许实现覆盖（例如某个实现自带"沙箱根"，拼接时要额外处理）。
    //
    // 注意 join 不折叠 `..`，需要折叠请再调 normalize。
    [[nodiscard]] virtual std::string normalize(std::string_view path) const = 0;
    [[nodiscard]] virtual std::string join(std::string_view a, std::string_view b) const = 0;

    // 归档条目落点校验的唯一入口（zip-slip / tar 路径穿越）。
    // 返回空串表示"该条目必须被拒绝"：绝对路径、逃出 root、指向 root 自身。
    // 具体规则见 PathPolicy::resolveWithin 的注释，实现直接转调即可。
    [[nodiscard]] virtual std::string resolveWithin(std::string_view root, std::string_view child) const = 0;

    // ---- 元信息 ----
    // 路径不存在时返回 exists=false 的成功结果，而不是失败。
    [[nodiscard]] virtual FileStat stat(std::string_view path) const = 0;

    // 可写探测：路径存在则测试能否写入；不存在则测试父目录能否创建文件。
    // Windows 上需要识别"被占用"（kBusy），这是 v1 无法统一处理的地方。
    [[nodiscard]] virtual Status probeWritable(std::string_view path) const = 0;

    // ---- 读写 ----
    virtual std::unique_ptr<IReadStream> openRead(std::string_view path, Status &st) const = 0;
    virtual std::unique_ptr<IWriteStream> openWrite(std::string_view path, bool truncate, Status &st) const = 0;

    // ---- 目录 ----
    // 不递归时只列直接子项；递归时深度优先，结果顺序不保证。
    virtual Status list(std::string_view dir, bool recursive, std::vector<DirEntry> &out) const = 0;
    virtual Status mkdirs(std::string_view dir) const = 0;

    // ---- 变更 ----
    virtual Status remove(std::string_view path) const = 0;
    virtual Status resize(std::string_view path, std::uint64_t size) const = 0;

    // 原子替换：把 from 移动到 to，目标已存在时**必须覆盖**。
    // 语义要求：要么完整生效，要么完全不生效，不允许出现半截状态。
    // Windows 下的裸 std::filesystem::rename 在目标存在时会失败，
    // 实现必须用 MoveFileEx(MOVEFILE_REPLACE_EXISTING) 或等价手段。
    virtual Status atomicReplace(std::string_view from, std::string_view to) const = 0;

    // ---- 便捷 ----
    // 快速指纹：用来判断"这份文件/残片是不是那个源文件的一部分"。
    //
    // 输入 = SHA256( 总长(8B, LE) || 前 64KB 内容 || 有效长度(8B, LE) )
    //   - 总长：sizeOverride >= 0 时用它，否则用文件自身长度；
    //   - 内容：固定取前 min(64KB, 总长) 字节，短读部分补零，
    //     保证源文件与残片哈希的是等长前缀，可以相互比较；
    //   - 有效长度：实际从文件里读到的字节数。
    //
    // 语义：
    //   - 两个文件**完全一致**（总长、内容、有效长度都相同）时指纹相等；
    //   - 残片（有效长度 < 总长）与完整文件的指纹**不同**，不会被误判为完整；
    //   - sizeOverride 的用处是让残片按"源文件总长"参与计算，从而与源文件
    //     落在同一比较空间里。v1 没做这件事，残片指纹与源文件永远对不上。
    //
    // 路径不存在或读失败时返回空串（调用方按"无法判断"处理）。
    [[nodiscard]] virtual std::string quickFingerprint(std::string_view path, std::int64_t sizeOverride = -1) const = 0;
};

}  // namespace relayer::filestore
