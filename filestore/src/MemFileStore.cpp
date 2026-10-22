#include "relayer/filestore/MemFileStore.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "relayer/common/crypto/Sha256.h"

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

// 写流直接操作 Impl 里的节点。
//
// **为什么直接操作共享节点而不是各自持一份副本**：早期实现是构造时把
// node.content 拷进 buffer_、commit() 时整份写回。顺序写没问题，但并发
// 随机写会错得很隐蔽——4 个写流各持一份完整副本，最后 commit 的那个把
// 别人的写入全部覆盖掉，结果"只留下一块数据、长度却正确"。真实文件系统
// 上多个 FILE* 各自 fseek+fwrite 是能正确随机写的（由内核协调偏移），
// 内存实现必须复刻这一点，否则"用 MemFileStore 跑通的并发测试"什么也
// 证明不了。
//
// 代价是浮出了 Impl 的节点结构（本来就在同一个 .cpp 里，外部看不见）。
class MemWriteStream final : public IWriteStream
{
public:
    MemWriteStream(MemFileStore::Impl *impl, std::string path)
        : impl_(impl)
        , path_(std::move(path))
    {
        auto &node = impl_->nodes[path_];
        node.isDir = false;

        // buffer_ 是"这次会话看到的文件内容"，必须以节点现有内容为底。
        // 不复制的话写第一块时 buffer_ 是空的，replace(pos=kChunk,...) 越界。
        buffer_ = node.content;

        // 初值放在末尾：不 seek 直接连续 write 等价于 append，
        // 这是既有调用方（TarXzArchiver 等）依赖的行为。
        pos_ = buffer_.size();
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

        // 写入缓冲，标记脏区间。**不直接改共享节点**——那会让
        // setFlushOnClose(false) 的"未 flush 就崩溃"场景失去意义。
        //
        // 覆盖式写入，从 pos_ 开始。早先这里是 append()，即"只能追加"：
        // 那对顺序写没问题，但乱序落盘（收到第 7 块时第 3 块还没到）需要
        // "定位后覆盖"，追加语义会把第 7 块写到文件尾部——内容对、位置错，
        // 只有最终哈希能发现。追加语义靠 pos_ 的初值保住（见构造函数）。
        const auto end = static_cast<std::size_t>(pos_) + len;
        if (end > buffer_.size())
        {
            // 中间可能有空洞（定位到末尾之后写）。空洞补零，与文件系统
            // 对 lseek 越界后再写的处理一致。
            buffer_.resize(end, '\0');
        }
        buffer_.replace(static_cast<std::size_t>(pos_), len, static_cast<const char *>(buf), len);

        // 记录脏区间，flush 时只把这一段应用到共享节点。
        //
        // "只应用脏区间"是并发正确性的关键：整份写回会让后 flush 的流
        // 覆盖掉别的流的写入，结果"只留下一块数据、长度却正确"。
        if (dirtyBegin_ < 0)
        {
            dirtyBegin_ = static_cast<std::int64_t>(pos_);
        }
        dirtyEnd_ = std::max<std::int64_t>(dirtyEnd_, static_cast<std::int64_t>(end));

        pos_ = end;
        return Status::Ok();
    }

    Status seek(std::uint64_t offset) override
    {
        // 与 fseek 一致：允许定位到文件末尾之后，后续写入时补零。
        // 只挡"size_t 表示不下"的偏移，否则后续 resize 会静默截断。
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        {
            return Status::Fail(ErrorCode::kInvalidPath, "seek offset out of range writing " + path_);
        }
        pos_ = offset;
        return Status::Ok();
    }

    [[nodiscard]] std::uint64_t tell() const override
    {
        return pos_;
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

        // resize 影响整个文件长度，所以它天然让整个文件变脏。
        buffer_.resize(target, '\0');
        resizedTo_ = size;

        // 截短之后 pos_ 可能落在末尾之外。夹回末尾：停在越界位置继续写
        // 会补零，那对调用方来说更难理解。
        if (pos_ > size)
        {
            pos_ = size;
        }

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

        // "别的文件占了多少" + "本文件写完之后多大"。
        //
        // 本文件用 max(current, pos_+len) 而不是 current+len：覆盖写不会
        // 让文件变大，简单相加会在反复重传同一块时误报 kNoSpace。
        const std::uint64_t used = impl_->usedBytesInternal();
        const std::uint64_t current = impl_->nodes.count(path_) != 0 ? impl_->nodes.at(path_).content.size() : 0;
        const std::uint64_t others = used >= current ? used - current : 0;

        const auto after = std::max<std::uint64_t>(current, pos_ + len);
        if (others + after > impl_->capacity)
        {
            return Status::Fail(ErrorCode::kNoSpace, "capacity exceeded writing " + path_);
        }
        return Status::Ok();
    }

    // 把缓冲里脏的那一段应用到共享节点。
    void commit()
    {
        auto &node = impl_->nodes[path_];
        node.isDir = false;
        node.mtime = 0;  // 内存实现不模拟时间，固定 0 让断言稳定
        auto &content = node.content;

        // resize 只影响长度，不该顺手覆盖别的流写的字节。
        //
        // 早先这里是"resize 时把整个 buffer_ 写回"，那会让一个只做了
        // resize 的流把别的流的并发写入全部回退——症状是"文件长度对了
        // 但内容缺一块"，比长度错误更难查。
        if (resizedTo_ >= 0)
        {
            content.resize(static_cast<std::size_t>(resizedTo_), '\0');
        }

        if (dirtyBegin_ >= 0)
        {
            const auto begin = static_cast<std::size_t>(dirtyBegin_);
            const auto end = static_cast<std::size_t>(dirtyEnd_);
            if (end > content.size())
            {
                content.resize(end, '\0');
            }
            // 只覆盖 [begin, end) 这一段，别的流的写入原样保留。
            content.replace(begin, end - begin, buffer_.data() + begin, end - begin);
        }

        dirtyBegin_ = -1;
        dirtyEnd_ = -1;
        resizedTo_ = -1;
    }

    MemFileStore::Impl *impl_;
    std::string path_;

    // 完整内容镜像。写入进这里，flush 时把脏区间应用到共享节点。
    std::string buffer_;

    // 下一次 write 的落点。初值是 buffer_.size()（追加语义）。
    std::uint64_t pos_ = 0;

    // 脏区间 [dirtyBegin_, dirtyEnd_)，-1 表示没有未提交的写入。
    std::int64_t dirtyBegin_ = -1;
    std::int64_t dirtyEnd_ = -1;

    // resize 的目标长度，-1 表示没 resize 过。resize 会让整个文件变脏。
    std::int64_t resizedTo_ = -1;
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
