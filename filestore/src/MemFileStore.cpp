#include "relayer/filestore/MemFileStore.h"

#include <algorithm>
#include <cstring>

#include "relayer/filestore/detail/Sha256.h"

namespace relayer::filestore
{
namespace
{

constexpr std::size_t kFingerprintProbe = 64 * 1024;

// 指纹算法必须与 StdFileStore 完全一致，否则测试里用 MemFileStore 得到的
// 结论无法迁移到真实实现上。两处都调用这里，避免各写一遍。
std::string computeFingerprint(const std::string &content, std::int64_t sizeOverride, std::size_t &validOut)
{
    const std::uint64_t total =
        sizeOverride >= 0 ? static_cast<std::uint64_t>(sizeOverride) : static_cast<std::uint64_t>(content.size());
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(total, kFingerprintProbe));

    std::vector<char> head(want, '\0');
    const std::size_t got = std::min(want, content.size());
    if (got > 0)
    {
        std::memcpy(head.data(), content.data(), got);
    }
    validOut = got;

    relayer::crypto::Sha256 h;
    std::uint8_t totalLe[8];
    std::uint8_t validLe[8];
    for (int k = 0; k < 8; ++k)
    {
        totalLe[k] = static_cast<std::uint8_t>((total >> (k * 8)) & 0xFF);
        validLe[k] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(got) >> (k * 8)) & 0xFF);
    }
    h.update(totalLe, sizeof(totalLe));
    h.update(head.data(), head.size());
    h.update(validLe, sizeof(validLe));
    return relayer::crypto::Sha256::hex(h.digest());
}

}  // namespace

struct MemFileStore::Impl
{
    std::map<std::string, Node> nodes;
    std::vector<FaultInjection> faults;
    std::uint64_t capacity = 0;  // 0 = 不限
    std::uint64_t writeCounter = 0;
    bool flushOnClose = true;

    // 检查是否应当因注入的故障而失败。
    //
    // 计数语义：**只有写操作（write）计数**，openWrite/remove 这类
    // "打开"不算。否则 `afterOps = 2` 的含义会随调用方是否先 open 而变化，
    // 测试里根本说不清"第几次写失败"。
    //
    // 因此调用方要区分两种模式：
    //   - 打开时判定（openWrite / remove）：只按路径匹配，不看计数；
    //   - 写入时判定（write）：先自增计数，再判断是否越过 afterOps。
    Status checkFaultOnOpen(const std::string &path)
    {
        return matchFault(path, false);
    }

    Status checkFaultOnWrite(const std::string &path)
    {
        ++writeCounter;
        return matchFault(path, true);
    }

    Status matchFault(const std::string &path, bool countAware)
    {
        for (auto it = faults.begin(); it != faults.end(); ++it)
        {
            auto &f = *it;
            if (!f.pathPrefix.empty() && path.compare(0, f.pathPrefix.size(), f.pathPrefix) != 0)
            {
                continue;
            }
            // 打开阶段的故障只对 afterOps == 0 的规则生效：
            // "第 3 次写失败"不该在 openWrite 时就把文件打开失败掉。
            if (f.afterOps != 0)
            {
                if (!countAware || writeCounter <= f.afterOps)
                {
                    continue;
                }
            }
            Status st = Status::Fail(f.code, "injected fault for " + path);
            if (f.once)
            {
                faults.erase(it);
            }
            return st;
        }
        return Status::Ok();
    }

    // 容量是否还够写 len 字节
    Status checkCapacity(const std::string &path, std::uint64_t alreadyHave, std::uint64_t len) const
    {
        if (capacity == 0)
        {
            return Status::Ok();
        }
        const std::uint64_t used = usedBytesInternal();
        const std::uint64_t remain = used >= alreadyHave ? used - alreadyHave : 0;
        if (remain + len > capacity)
        {
            return Status::Fail(ErrorCode::kNoSpace, "capacity exceeded writing " + path);
        }
        return Status::Ok();
    }

    [[nodiscard]] std::uint64_t usedBytesInternal() const
    {
        std::uint64_t total = 0;
        for (const auto &kv : nodes)
        {
            if (!kv.second.isDir)
            {
                total += kv.second.content.size();
            }
        }
        return total;
    }
};

// ---------------------------------------------------------------------------
// 流实现
// ---------------------------------------------------------------------------

namespace
{

// 读流持有内容的一份拷贝。
// 为什么不持有指针：测试里经常"读完就改文件"，持有指针会让读到的内容
// 随文件变化而变化，掩盖掉真实实现（会读到缓存内容或短读）的行为差异。
class MemReadStream final : public IReadStream
{
public:
    explicit MemReadStream(std::string content)
        : content_(std::move(content))
    {
    }

    std::size_t read(void *buf, std::size_t len, Status &st) override
    {
        st = Status::Ok();
        if (pos_ >= content_.size() || len == 0)
        {
            return 0;
        }
        const std::size_t n = std::min(len, content_.size() - pos_);
        std::memcpy(buf, content_.data() + pos_, n);
        pos_ += n;
        return n;
    }

    Status seek(std::uint64_t offset) override
    {
        // 允许 seek 到文件尾（续传时常见），但要拒绝越过尾部太多，
        // 否则后续 read 返回 0 会让调用方以为"正常 EOF"。
        if (offset > content_.size())
        {
            return Status::Fail(ErrorCode::kInvalidPath, "seek beyond end");
        }
        pos_ = static_cast<std::size_t>(offset);
        return Status::Ok();
    }

    [[nodiscard]] std::uint64_t tell() const override
    {
        return pos_;
    }

private:
    std::string content_;
    std::size_t pos_ = 0;
};

}  // namespace

// 写流直接操作 Impl 里的节点，保证多次 openWrite 同一路径时行为一致
// （真实文件系统上第二个写句柄会覆盖第一个的写入，这里也一样）。
class MemWriteStream final : public IWriteStream
{
public:
    MemWriteStream(MemFileStore::Impl *impl, std::string path)
        : impl_(impl)
        , path_(std::move(path))
    {
        auto &node = impl_->nodes[path_];
        node.isDir = false;
        buffer_ = node.content;  // 追加模式的基础内容
    }

    ~MemWriteStream() override
    {
        if (impl_->flushOnClose)
        {
            commit();
        }
    }

    Status write(const void *buf, std::size_t len) override
    {
        if (len == 0)
        {
            return Status::Ok();
        }
        if (const auto st = impl_->checkFaultOnWrite(path_); !st)
        {
            return st;
        }
        if (const auto st = checkCapacity(len); !st)
        {
            return st;
        }
        const auto *p = static_cast<const char *>(buf);
        buffer_.append(p, len);
        return Status::Ok();
    }

    Status flush() override
    {
        commit();
        return Status::Ok();
    }

    Status sync() override
    {
        return flush();
    }

    Status resize(std::uint64_t size) override
    {
        if (const auto st = impl_->checkFaultOnWrite(path_); !st)
        {
            return st;
        }
        const auto target = static_cast<std::size_t>(size);
        if (const auto st = checkCapacity(target > buffer_.size() ? target - buffer_.size() : 0); !st)
        {
            return st;
        }
        buffer_.resize(target, '\0');
        commit();
        return Status::Ok();
    }

private:
    Status checkCapacity(std::uint64_t len) const
    {
        if (impl_->capacity == 0)
        {
            return Status::Ok();
        }
        const std::uint64_t used = impl_->usedBytesInternal();
        const std::uint64_t mine = impl_->nodes.count(path_) != 0 ? impl_->nodes.at(path_).content.size() : 0;
        const std::uint64_t base = used >= mine ? used - mine : 0;
        if (base + buffer_.size() + len > impl_->capacity)
        {
            return Status::Fail(ErrorCode::kNoSpace, "capacity exceeded writing " + path_);
        }
        return Status::Ok();
    }

    void commit()
    {
        auto &node = impl_->nodes[path_];
        node.isDir = false;
        node.content = buffer_;
        node.mtime = 0;  // 内存实现不模拟时间，固定 0 让断言稳定
    }

    MemFileStore::Impl *impl_;
    std::string path_;
    std::string buffer_;
};

// ---------------------------------------------------------------------------
// MemFileStore
// ---------------------------------------------------------------------------

MemFileStore::MemFileStore()
    : impl_(std::make_unique<Impl>())
{
}

MemFileStore::~MemFileStore() = default;

// ---- 路径 ----

std::string MemFileStore::normalize(std::string_view path) const
{
    static const PathPolicy policy;
    return policy.normalize(path);
}

std::string MemFileStore::join(std::string_view a, std::string_view b) const
{
    static const PathPolicy policy;
    return policy.join(a, b);
}

std::string MemFileStore::resolveWithin(std::string_view root, std::string_view child) const
{
    static const PathPolicy policy;
    return policy.resolveWithin(root, child);
}

// ---- 元信息 ----

FileStat MemFileStore::stat(std::string_view path) const
{
    FileStat out;
    const auto key = normalize(path);
    const auto it = impl_->nodes.find(key);
    if (it == impl_->nodes.end())
    {
        return out;
    }
    out.exists = true;
    out.isDir = it->second.isDir;
    out.size = it->second.isDir ? 0 : it->second.content.size();
    out.mtime = it->second.mtime;
    out.readable = true;
    out.writable = true;
    out.executable = false;
    // 内存实现不建模权限，给一组固定值——测试里归档往返时
    // 读侧能看到 0644/0755，与真实实现的 Windows 分支一致。
    out.mode = static_cast<std::uint16_t>(out.isDir ? 0755 : 0644);
    return out;
}

Status MemFileStore::probeWritable(std::string_view path) const
{
    const auto key = normalize(path);
    const auto it = impl_->nodes.find(key);
    if (it != impl_->nodes.end())
    {
        if (it->second.isDir)
        {
            return Status::Fail(ErrorCode::kIsADirectory, "is a directory: " + key);
        }
        return Status::Ok();
    }
    // 不存在：父目录必须存在且是目录
    const std::string dir = PathPolicy{}.parent(key);
    const auto dit = impl_->nodes.find(dir);
    if (dit == impl_->nodes.end())
    {
        return Status::Fail(ErrorCode::kNotFound, "parent does not exist: " + dir);
    }
    if (!dit->second.isDir)
    {
        return Status::Fail(ErrorCode::kNotADirectory, "parent is not a directory: " + dir);
    }
    return Status::Ok();
}

// ---- 读写 ----

std::unique_ptr<IReadStream> MemFileStore::openRead(std::string_view path, Status &st) const
{
    st = Status::Ok();
    const auto key = normalize(path);
    const auto it = impl_->nodes.find(key);
    if (it == impl_->nodes.end() || it->second.isDir)
    {
        st = Status::Fail(ErrorCode::kNotFound, "no such file: " + key);
        return nullptr;
    }
    return std::make_unique<MemReadStream>(it->second.content);
}

std::unique_ptr<IWriteStream> MemFileStore::openWrite(std::string_view path, bool truncate, Status &st) const
{
    st = Status::Ok();
    const auto key = normalize(path);
    if (const auto f = impl_->checkFaultOnOpen(key); !f)
    {
        st = f;
        return nullptr;
    }
    const auto it = impl_->nodes.find(key);
    if (it != impl_->nodes.end() && it->second.isDir)
    {
        st = Status::Fail(ErrorCode::kIsADirectory, "is a directory: " + key);
        return nullptr;
    }
    if (it != impl_->nodes.end() && truncate)
    {
        impl_->nodes[key].content.clear();
    }
    // 追加模式（truncate=false）要求文件已存在，与 "r+b" 一致
    if (!truncate && it == impl_->nodes.end())
    {
        st = Status::Fail(ErrorCode::kNotFound, "open for append on missing file: " + key);
        return nullptr;
    }
    return std::make_unique<MemWriteStream>(impl_.get(), key);
}

// ---- 目录 ----

Status MemFileStore::list(std::string_view dir, bool recursive, std::vector<DirEntry> &out) const
{
    out.clear();
    const auto base = normalize(dir);
    const auto it = impl_->nodes.find(base);
    if (it == impl_->nodes.end())
    {
        return Status::Fail(ErrorCode::kNotFound, "no such directory: " + base);
    }
    if (!it->second.isDir)
    {
        return Status::Fail(ErrorCode::kNotADirectory, "not a directory: " + base);
    }
    const std::string prefix = base == "/" ? std::string("/") : base + "/";

    for (const auto &kv : impl_->nodes)
    {
        if (kv.first == base || kv.first.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const std::string rest = kv.first.substr(prefix.size());
        if (!recursive && rest.find('/') != std::string::npos)
        {
            continue;
        }
        DirEntry e;
        // 递归时 name 仍只给"最后一段"，与文档中"仅文件名"一致
        const auto slash = rest.rfind('/');
        e.name = slash == std::string::npos ? rest : rest.substr(slash + 1);
        e.isDir = kv.second.isDir;
        e.size = kv.second.isDir ? 0 : kv.second.content.size();
        e.mtime = kv.second.mtime;
        out.push_back(std::move(e));
    }
    return Status::Ok();
}

Status MemFileStore::mkdirs(std::string_view dir) const
{
    const auto key = normalize(dir);
    if (key.empty() || key == ".")
    {
        return Status::Ok();
    }
    if (const auto f = impl_->checkFaultOnOpen(key); !f)
    {
        return f;
    }
    // 逐级建，但如果某一级已是文件则失败（与真实文件系统一致）
    PathPolicy policy;
    std::string cur;
    std::size_t pos = 0;
    if (!key.empty() && key[0] == '/')
    {
        cur = "/";
        pos = 1;
    }
    while (pos <= key.size())
    {
        const auto slash = key.find('/', pos);
        const auto seg = key.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!seg.empty())
        {
            if (!cur.empty() && cur.back() != '/')
            {
                cur.push_back('/');
            }
            cur.append(seg);
            auto it = impl_->nodes.find(cur);
            if (it != impl_->nodes.end() && !it->second.isDir)
            {
                return Status::Fail(ErrorCode::kNotADirectory, "not a directory: " + cur);
            }
            impl_->nodes[cur].isDir = true;
        }
        if (slash == std::string::npos)
        {
            break;
        }
        pos = slash + 1;
    }
    return Status::Ok();
}

// ---- 变更 ----

Status MemFileStore::remove(std::string_view path) const
{
    const auto key = normalize(path);
    if (const auto f = impl_->checkFaultOnOpen(key); !f)
    {
        return f;
    }
    const auto it = impl_->nodes.find(key);
    if (it == impl_->nodes.end())
    {
        return Status::Ok();  // 不存在视为成功，与 StdFileStore 一致
    }
    if (it->second.isDir)
    {
        // 递归删子项，与 std::filesystem::remove 不同（那个只删空目录）。
        // 这里选了"尽力删除"，因为测试里更常见的是想清干净。
        const std::string prefix = key + "/";
        for (auto i = impl_->nodes.begin(); i != impl_->nodes.end();)
        {
            if (i->first == key || i->first.compare(0, prefix.size(), prefix) == 0)
            {
                i = impl_->nodes.erase(i);
            }
            else
            {
                ++i;
            }
        }
        return Status::Ok();
    }
    impl_->nodes.erase(it);
    return Status::Ok();
}

Status MemFileStore::resize(std::string_view path, std::uint64_t size) const
{
    Status st;
    auto stream = openWrite(path, false, st);
    if (!stream)
    {
        return st;
    }
    return stream->resize(size);
}

Status MemFileStore::atomicReplace(std::string_view from, std::string_view to) const
{
    const auto src = normalize(from);
    const auto dst = normalize(to);
    const auto it = impl_->nodes.find(src);
    if (it == impl_->nodes.end())
    {
        return Status::Fail(ErrorCode::kNotFound, "no such file: " + src);
    }
    if (const auto f = impl_->checkFaultOnOpen(dst); !f)
    {
        return f;
    }
    impl_->nodes[dst] = it->second;
    impl_->nodes.erase(it);
    return Status::Ok();
}

// ---- 指纹 ----

std::string MemFileStore::quickFingerprint(std::string_view path, std::int64_t sizeOverride) const
{
    const auto key = normalize(path);
    const auto it = impl_->nodes.find(key);
    if (it == impl_->nodes.end() || it->second.isDir)
    {
        return {};
    }
    std::size_t valid = 0;
    return computeFingerprint(it->second.content, sizeOverride, valid);
}

// ---- 测试辅助 ----

void MemFileStore::putFile(const std::string &path, const std::string &content)
{
    const auto key = normalize(path);
    // 自动建父目录，省掉测试里大量 mkdirs 样板
    const std::string dir = PathPolicy{}.parent(key);
    if (!dir.empty() && dir != "." && dir != "/")
    {
        mkdirs(dir);
    }
    impl_->nodes[key] = Node{false, content, 0};
}

void MemFileStore::putDir(const std::string &path)
{
    mkdirs(path);
}

std::string MemFileStore::getFile(const std::string &path) const
{
    const auto key = normalize(path);
    const auto it = impl_->nodes.find(key);
    if (it == impl_->nodes.end() || it->second.isDir)
    {
        return {};
    }
    return it->second.content;
}

bool MemFileStore::has(const std::string &path) const
{
    return impl_->nodes.count(normalize(path)) != 0;
}

void MemFileStore::setCapacity(std::uint64_t bytes)
{
    impl_->capacity = bytes;
}

std::uint64_t MemFileStore::usedBytes() const
{
    return impl_->usedBytesInternal();
}

void MemFileStore::injectFault(FaultInjection fault)
{
    impl_->faults.push_back(std::move(fault));
}

void MemFileStore::clearFaults()
{
    impl_->faults.clear();
    impl_->writeCounter = 0;
}

std::vector<std::string> MemFileStore::allFiles() const
{
    std::vector<std::string> out;
    for (const auto &kv : impl_->nodes)
    {
        if (!kv.second.isDir)
        {
            out.push_back(kv.first);
        }
    }
    return out;
}

void MemFileStore::setFlushOnClose(bool value)
{
    impl_->flushOnClose = value;
}

}  // namespace relayer::filestore
