#include "relayer/core/transfer/TransferReceiver.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "relayer/core/transfer/MerkleTree.h"

namespace relayer::core::transfer
{

namespace
{

// 残片后缀。单独一个常量而不是各处拼字符串：上层要判断"这个文件是不是
// 未完成的残片"时得用同一个值。
constexpr const char *kPartSuffix = ".relaypart";

}  // namespace

TransferReceiver::TransferReceiver(filestore::IFileStore &store, WorkerPool &pool)
    : TransferReceiver(store, pool, Config{})
{
}

TransferReceiver::TransferReceiver(filestore::IFileStore &store, WorkerPool &pool, Config config)
    : store_(store)
    , pool_(pool)
    , config_(config)
{
    // 上限为 0 会让 acceptChunk 恒返回 kBusy——传输一动不动，且不报错。
    // 这里夹到至少 1，与 TransferSender 对 windowChunks 的处理一致。
    if (config_.maxBufferedChunks == 0)
    {
        config_.maxBufferedChunks = 1;
    }
    if (config_.maxWriteInFlight == 0)
    {
        config_.maxWriteInFlight = 1;
    }
}

TransferReceiver::~TransferReceiver()
{
    // 与 TransferSender 同一条理由：写盘任务体捕获了 this，越过本对象
    // 生命周期就是 use-after-free。而且这里更严重——写盘任务还在往文件
    // 里写，对象没了之后连"写到哪了"都没人知道。
    //
    // 只等已开始的。排队中的由 WorkerPool::stop() 丢弃，符合"取消时不该
    // 再写盘"的语义。
    for (;;)
    {
        const auto n = runningTasks_.load(std::memory_order_acquire);
        if (n == 0)
        {
            break;
        }
        std::this_thread::yield();
    }
}

Status TransferReceiver::open(const TransferDesc &desc, std::string_view targetPath, std::uint32_t &resumeFrom)
{
    if (opened_)
    {
        return Status::Fail(ErrorCode::kUnknown, "receiver already open");
    }

    if (auto st = validateDesc(desc); !st.ok())
    {
        return st;
    }

    if (targetPath.empty())
    {
        return Status::Fail(ErrorCode::kInvalidPath, "empty target path");
    }

    desc_ = desc;
    targetPath_ = store_.normalize(targetPath);
    partPath_ = targetPath_ + kPartSuffix;
    chunkSize_ = desc.chunkSize;
    totalSize_ = desc.totalSize;
    totalChunks_ = chunkCountFor(totalSize_, chunkSize_);

    {
        std::lock_guard<std::mutex> lock(mu_);
        written_.assign(totalChunks_, false);
        received_.assign(totalChunks_, false);
    }

    // 目标已存在且长度一致：当作"对方已经有这个文件了"。
    //
    // 不看内容——比内容要读两遍文件，而这一步的用途只是"别重复传一遍"。
    // 上层若在意内容是否相同，应当先比 quickFingerprint（那正是描述里
    // 带 fingerprint 的用途）。
    const auto targetStat = store_.stat(targetPath_);
    if (targetStat.exists && !targetStat.isDir && targetStat.size == totalSize_)
    {
        return Status::Fail(ErrorCode::kAlreadyExists, "target already exists with the same size");
    }

    // 残片：存在就尽量续传，不存在就建一个空的。
    //
    // 注意这里用 stat 而不是"直接 openWrite 试试"：openWrite(truncate=false)
    // 在文件不存在时会失败，而那个失败和"权限不足"混在一起，分不清。
    const auto partStat = store_.stat(partPath_);
    if (!partStat.exists)
    {
        Status st;
        // truncate=true 建新文件；写完立刻关掉，这一步只是占位。
        auto out = store_.openWrite(partPath_, true, st);
        if (!out)
        {
            return st.ok() ? Status::Fail(ErrorCode::kIoError, "create part file failed: " + partPath_) : st;
        }
        if (auto fs = out->flush(); !fs.ok())
        {
            return fs;
        }
    }
    else if (partStat.isDir)
    {
        // 同名的目录挡在残片路径上。不处理的话后面每次 openWrite 都失败，
        // 报的还是 kIsADirectory——但错误位置离根因很远。
        return Status::Fail(ErrorCode::kIsADirectory, "part path is a directory: " + partPath_);
    }

    opened_ = true;
    resumeFrom = scanResumePoint();
    return Status::Ok();
}

std::uint32_t TransferReceiver::scanResumePoint() const
{
    const auto st = store_.stat(partPath_);
    if (!st.exists || st.size == 0)
    {
        return 0;
    }

    // 只按长度算续传点，把残片截到整块边界。
    //
    // 为什么要截：残片可能停在半块上（比如写到 1.5 块时断电）。从 1.5 块
    // 之后继续的话，第 1 块永远缺后半截，而长度看起来"接近完整"。
    // 截到 1 块之后，第 1 块会被重新传一遍——代价是一块，换来的是
    // "残片长度始终是块大小的整数倍"这条不变量。
    const auto aligned = (st.size / chunkSize_) * chunkSize_;

    // 已经够整个文件长了：说明残片比目标还大，那它不可能是这次的产物
    // （块大小或文件改过）。直接从头来，别在垃圾上续传。
    if (aligned >= totalSize_)
    {
        return 0;
    }

    if (aligned != st.size)
    {
        // 截断失败不致命：多传一块总比传错强。上层会在最终校验时发现
        // 长度不符。这里静默继续，让"截断不了"变成"多传一块"。
        (void)store_.resize(partPath_, aligned);
    }

    // 块数必须封顶到 totalChunks_：aligned < totalSize_ 保证了这一点，
    // 但取整方式写错就会越界一格，这里显式夹一次。
    const auto chunks = static_cast<std::uint32_t>(aligned / chunkSize_);
    return std::min(chunks, totalChunks_);
}

std::uint32_t TransferReceiver::totalChunks() const noexcept
{
    return totalChunks_;
}

std::uint32_t TransferReceiver::chunkSize() const noexcept
{
    return chunkSize_;
}

std::uint64_t TransferReceiver::totalSize() const noexcept
{
    return totalSize_;
}

const std::string &TransferReceiver::partPath() const noexcept
{
    return partPath_;
}

const std::string &TransferReceiver::targetPath() const noexcept
{
    return targetPath_;
}

std::uint64_t TransferReceiver::offsetOf(std::uint32_t chunkIndex) const noexcept
{
    return static_cast<std::uint64_t>(chunkIndex) * chunkSize_;
}

std::uint32_t TransferReceiver::lengthOf(std::uint32_t chunkIndex) const noexcept
{
    return static_cast<std::uint32_t>(chunkLengthAt(chunkIndex, totalSize_, chunkSize_));
}

bool TransferReceiver::hasChunk(std::uint32_t chunkIndex) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (chunkIndex >= written_.size())
    {
        return false;
    }
    return written_[chunkIndex];
}

bool TransferReceiver::hasCapacity() const
{
    std::lock_guard<std::mutex> lock(mu_);

    // 判"在飞"而不是"received_ 里有多少 true"：received_ 是判重用的，
    // 一块写完之后它仍然留着（下次重传要能被判重），所以它的 true 数
    // 只增不减，拿来做容量判断会让上限永久占满。
    return inFlight_ < config_.maxBufferedChunks && inFlight_ < config_.maxWriteInFlight;
}

Status TransferReceiver::acceptChunk(std::uint32_t chunkIndex, const void *data, std::size_t len, const PostFn &post)
{
    if (!opened_)
    {
        return Status::Fail(ErrorCode::kUnknown, "acceptChunk before open");
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (aborted_)
        {
            return Status::Fail(ErrorCode::kCancelled, "receiver aborted");
        }
        if (!writeError_.ok())
        {
            return writeError_;
        }
    }

    if (chunkIndex >= totalChunks_)
    {
        return Status::Fail(ErrorCode::kProtocol, "chunk index out of range: " + std::to_string(chunkIndex));
    }

    // 长度必须与描述算出来的完全一致。多给的字节表示对端对块大小的理解
    // 与描述不符，少给的表示它对最后一块的处理不对——两者都会让最终根
    // 对不上，但"对不上"远不如"当场指出哪一块不对"好查。
    const auto want = lengthOf(chunkIndex);
    if (len != want)
    {
        return Status::Fail(ErrorCode::kProtocol, "chunk " + std::to_string(chunkIndex) + " length " +
                                                      std::to_string(len) + " != expected " + std::to_string(want));
    }

    {
        std::lock_guard<std::mutex> lock(mu_);

        // 已落盘的重传块直接丢弃。这是重传场景下的**正常路径**：
        // 对端没收到我们的 ACK 就会重发，我们的回应就是"再 ACK 一次"。
        if (written_[chunkIndex])
        {
            return Status::Fail(ErrorCode::kAlreadyExists, "chunk " + std::to_string(chunkIndex) + " already written");
        }

        // 已在写：同样丢弃。同时收到同一块两次说明对端把"重传"和"新发"
        // 搞混了，或者我们自己的 ACK 丢了。两种情况都不该写两遍。
        if (received_[chunkIndex])
        {
            return Status::Fail(ErrorCode::kAlreadyExists,
                                "chunk " + std::to_string(chunkIndex) + " already in flight");
        }

        if (inFlight_ >= config_.maxBufferedChunks || inFlight_ >= config_.maxWriteInFlight)
        {
            return Status::Fail(ErrorCode::kBusy, "receiver buffer full, pause reading");
        }

        received_[chunkIndex] = true;
        ++inFlight_;
    }

    ReceivedChunk chunk;
    chunk.chunkIndex = chunkIndex;
    // 拷一份。上层那块网络缓冲在返回后立刻可能被复用于下一次读，
    // 而写盘是异步的——不拷就会出现"写进去的是下一块的数据"。
    //
    // vector<char> 没有 assign(ptr, count) 的重载（只有迭代器版），
    // resize + memcpy 是这里最直接的写法：一次分配 + 一次拷贝，
    // 中间不经过临时 string。
    chunk.data.resize(want);
    if (want != 0)
    {
        std::memcpy(chunk.data.data(), data, want);
    }

    runningTasks_.fetch_add(1, std::memory_order_acq_rel);

    const auto accepted = pool_.submit(
        [this, c = std::move(chunk), post]() mutable
        {
            struct RunningGuard
            {
                std::atomic<std::uint32_t> &n;
                ~RunningGuard()
                {
                    n.fetch_sub(1, std::memory_order_acq_rel);
                }
            } guard{runningTasks_};

            const auto idx = c.chunkIndex;

            auto fail = [this, idx, &post](Status st)
            {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (writeError_.ok())
                    {
                        writeError_ = st;  // 留一份，writeError() 给上层查
                    }
                }
                if (post)
                {
                    // 传的是这一次的具体错误，不是 writeError_ ——后者可能是
                    // 更早一次失败留下来的旧值，用来描述"这一块怎么了"会误导。
                    post([this, idx, e = std::move(st)]() mutable { onChunkWriteFailed(idx, std::move(e)); });
                }
            };

            Status openStatus;

            // truncate=false：这里要的是"打开已存在的残片并保留内容"。
            // 用 true 会把前面所有块的成果清掉——症状是"永远只传最后一块"。
            auto out = store_.openWrite(partPath_, false, openStatus);
            if (!out)
            {
                fail(openStatus.ok() ? Status::Fail(ErrorCode::kIoError, "open part file failed: " + partPath_)
                                     : openStatus);
                return;
            }

            // 每块自己 seek。共用流位置在并发下必然踩乱（与发送侧同一个
            // 坑，见 TransferSender.cpp）。
            if (auto st = out->seek(offsetOf(idx)); !st.ok())
            {
                fail(std::move(st));
                return;
            }

            if (auto st = out->write(c.data.data(), c.data.size()); !st.ok())
            {
                fail(std::move(st));
                return;
            }

            // flush 是**必须**的，sync 是可选的。
            //
            // 必须 flush 的理由不在"数据安全"，而在 flush 会把用户态缓冲
            // 交给操作系统——不做这一步，write 成功只意味着"进了本进程的
            // 缓冲区"。而我们在下一行就要告诉对端"这块我收好了"。ACK 的
            // 语义是"已经到文件里了"，不是"已经到我的内存里了"。
            if (auto st = out->flush(); !st.ok())
            {
                fail(std::move(st));
                return;
            }

            if (config_.syncOnWrite)
            {
                if (auto st = out->sync(); !st.ok())
                {
                    fail(std::move(st));
                    return;
                }
            }

            // 流在这里析构关闭。关闭失败在多数实现里表现为 flush 失败，
            // 已经覆盖；有些实现把关闭时的错误留到下一次 open 才报，
            // 那属于实现细节，本层不额外处理。

            if (post)
            {
                post([this, cc = std::move(c)]() mutable { onChunkWritten(std::move(cc)); });
            }
        });

    if (!accepted)
    {
        // 队列满：任务不会执行，计数与位图都要还回去。
        runningTasks_.fetch_sub(1, std::memory_order_acq_rel);

        std::lock_guard<std::mutex> lock(mu_);
        received_[chunkIndex] = false;
        --inFlight_;

        // 回到"没收到"的状态而不是报错：调用方看到 kBusy 会暂停读连接，
        // 稍后重试时这一块还能被正常接收。
        return Status::Fail(ErrorCode::kBusy, "worker pool full, retry chunk " + std::to_string(chunkIndex));
    }

    return Status::Ok();
}

void TransferReceiver::onChunkWritten(ReceivedChunk chunk)
{
    std::lock_guard<std::mutex> lock(mu_);

    if (chunk.chunkIndex < written_.size())
    {
        // written_ 是"可以告诉了对方"的唯一依据。received_ 保持 true：
        // 下次对端重传这一块时，acceptChunk 靠它判重（走到 written_ 那条
        // 分支也一样，两条都返回 kAlreadyExists，留着不影响正确性，
        // 但留着的成本只是 1 bit）。
        written_[chunk.chunkIndex] = true;
    }

    if (inFlight_ > 0)
    {
        --inFlight_;
    }
}

void TransferReceiver::onChunkWriteFailed(std::uint32_t chunkIndex, Status error)
{
    {
        std::lock_guard<std::mutex> lock(mu_);

        // 写失败：把 received_ 清掉，让重传还能进来。
        //
        // 不清的话这一块会被判为"在写"，对端重传时返回 kAlreadyExists，
        // 于是永远没有人写它——而发送方会因为收不到 ACK 一直重传，
        // 表现为"卡住不动且不报错"。
        if (chunkIndex < received_.size() && !(chunkIndex < written_.size() && written_[chunkIndex]))
        {
            received_[chunkIndex] = false;
        }

        if (inFlight_ > 0)
        {
            --inFlight_;
        }

        if (writeError_.ok())
        {
            writeError_ = std::move(error);
        }
    }
}

ChunkAck TransferReceiver::buildCurrentAck() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return buildAck(written_, totalChunks_);
}

bool TransferReceiver::allChunksReceived() const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (totalChunks_ == 0)
    {
        // 空文件：协议上算"全部收到"。chunkCountFor 对 0 字节返回 0 块，
        // 而空文件的 Merkle 根是 SHA256("")，finalize 会照常走一遍。
        return true;
    }
    return std::all_of(written_.begin(), written_.end(), [](bool b) { return b; });
}

std::uint32_t TransferReceiver::writtenChunks() const
{
    // 不加 noexcept：里面要加锁，lock_guard 在极端情况下会抛
    // （std::system_error）。标了 noexcept 就变成直接 terminate，
    // 而这个函数只是"报个数"，不值得为它冒终止进程的风险。
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<std::uint32_t>(std::count(written_.begin(), written_.end(), true));
}

Status TransferReceiver::writeError() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return writeError_;
}

bool TransferReceiver::hasPendingWrites() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return inFlight_ > 0;
}

TransferReceiver::FinalizeResult TransferReceiver::finalize(std::string_view expectedRootHex)
{
    FinalizeResult out;

    if (!opened_)
    {
        out.status = CompleteStatus::kShapeMismatch;
        out.error = Status::Fail(ErrorCode::kUnknown, "finalize before open");
        return out;
    }

    // 还有块在写：现在读残片会读到半截数据，算出来的根必然不对，而那个
    // "根不对"会把排查引向"数据传错了"——完全错误的方向。
    if (hasPendingWrites())
    {
        out.status = CompleteStatus::kWriteFailed;
        out.error = Status::Fail(ErrorCode::kBusy, "finalize while writes are still in flight");
        return out;
    }

    if (auto st = writeError(); !st.ok())
    {
        out.status = CompleteStatus::kWriteFailed;
        out.error = std::move(st);
        return out;
    }

    // 形状自检：残片长度必须正好等于声明的总长。不等就是缺块或多块，
    // 这时候算根没有意义（MerkleBuilder 也会因为最后一块长度不对而拒绝）。
    const auto st = store_.stat(partPath_);
    if (!st.exists)
    {
        out.status = CompleteStatus::kWriteFailed;
        out.error = Status::Fail(ErrorCode::kNotFound, "part file disappeared: " + partPath_);
        return out;
    }
    if (st.size != totalSize_)
    {
        out.status = CompleteStatus::kShapeMismatch;
        out.error = Status::Fail(ErrorCode::kIoError, "part file size " + std::to_string(st.size) + " != declared " +
                                                          std::to_string(totalSize_));
        return out;
    }

    // 重读残片算根。为什么不在写的时候顺手攒叶子：写盘是乱序的，攒叶子
    // 得按块号拼回去，那等于在内存里再留一份完整文件。重读一遍是顺序读，
    // 代价换来"内存占用与文件大小无关"。
    Status readStatus;
    auto in = store_.openRead(partPath_, readStatus);
    if (!in)
    {
        out.status = CompleteStatus::kWriteFailed;
        out.error =
            readStatus.ok() ? Status::Fail(ErrorCode::kIoError, "open part file for verify failed") : readStatus;
        return out;
    }

    MerkleBuilder builder(chunkSize_);
    std::vector<char> buf(chunkSize_);
    std::uint64_t remaining = totalSize_;

    while (remaining > 0)
    {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf.size()));

        std::size_t got = 0;
        while (got < want)
        {
            Status rs;
            const auto n = in->read(buf.data() + got, want - got, rs);
            if (!rs.ok())
            {
                out.status = CompleteStatus::kWriteFailed;
                out.error = std::move(rs);
                return out;
            }
            if (n == 0)
            {
                // 上面已经比过长度了，走到这里说明文件在比对之后变短了
                // （另一个进程在动它）。当作写失败，让上层重来。
                out.status = CompleteStatus::kWriteFailed;
                out.error = Status::Fail(ErrorCode::kIoError, "part file shrank during verify");
                return out;
            }
            got += n;
        }

        if (auto bs = builder.addChunk(buf.data(), want); !bs.ok())
        {
            out.status = CompleteStatus::kShapeMismatch;
            out.error = std::move(bs);
            return out;
        }
        remaining -= want;
    }

    out.actualRootHex = toHex(builder.root());

    // 读流必须在这里关掉，不能等 finalize 返回。
    //
    // Windows 上 MoveFileEx 对"仍有打开句柄的源文件"会失败（ERROR_SHARING_VIOLATION），
    // 而下面的 atomicReplace 正是移动这个残片。POSIX 上未关闭的读句柄不挡 rename，
    // 所以这个 bug 只在 Windows 上出现——表现是"校验全过、就是改不了名"。
    //
    // 显式 reset 而不是靠作用域：后面还有比根、改名的逻辑，作用域要拖到最后。
    in.reset();

    // 根比对。大小写不敏感：十六进制在描述里规定是小写，但对方实现
    // 若给了大写，拒绝它没有意义——那是同一份内容。
    if (out.actualRootHex != expectedRootHex)
    {
        out.status = CompleteStatus::kRootMismatch;
        out.error = Status::Fail(ErrorCode::kProtocol, "merkle root mismatch: expected " +
                                                           std::string(expectedRootHex) + ", got " + out.actualRootHex);
        return out;
    }

    // 校验通过才改名。顺序反过来（先改名再校验）会在校验失败时留下一个
    // 名字正确、内容错误的文件——那比传输失败严重得多。
    if (auto rs = store_.atomicReplace(partPath_, targetPath_); !rs.ok())
    {
        out.status = CompleteStatus::kWriteFailed;
        out.error = std::move(rs);
        return out;
    }

    out.status = CompleteStatus::kOk;
    return out;
}

void TransferReceiver::abort()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (aborted_)
        {
            return;
        }
        aborted_ = true;
    }

    // 删除残片。这里的顺序不能换：先置 aborted_（让新块进不来），再删文件。
    // 反过来的话，一个正在 acceptChunk 的调用方可能刚通过检查、正要提交
    // 写盘任务，而残片已经没了——那个任务会在 open 时失败，报一个莫名其妙的
    // 错误。虽然结果也是"没写进去"，但错误信息离根因很远。
    //
    // 已经在写的任务拦不住，它们会往刚被删掉（或已不存在的）文件里写。
    // MemFileStore 上这会重建出一个新节点，StdFileStore 上会失败——两种都
    // 无害，因为 aborted_ 之后没人再看这块数据。
    (void)store_.remove(partPath_);
}

}  // namespace relayer::core::transfer
