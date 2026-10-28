#include "relayer/core/transfer/TransferSender.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "relayer/common/crypto/Sha256.h"
#include "relayer/core/transfer/MerkleTree.h"

namespace relayer::core::transfer {

TransferSender::TransferSender(filestore::IFileStore& store, WorkerPool& pool) : TransferSender(store, pool, Config{})
{
}

TransferSender::TransferSender(filestore::IFileStore& store, WorkerPool& pool, Config config)
    : store_(store), pool_(pool), config_(config)
{
    // 窗口块数在 open() 里按 windowBytes 换算（那一刻才知道 chunkSize），
    // 这里只保证读盘上限至少为 1：0 会让 pump 一块也提交不出去。
    if (config_.maxReadInFlight == 0) {
        config_.maxReadInFlight = 1;
    }
}

TransferSender::~TransferSender()
{
    // 必须等在跑的任务退出：任务体里捕获了 this，越过本对象生命周期就是
    // use-after-free。
    //
    // 只等"已经开始"的（runningTasks_ 在任务体入口才加），不等还排在池子
    // 队列里的——那些由 WorkerPool 的 stop() 丢弃，符合"取消时不该再写盘"
    // 的语义。两个契约叠起来才既不悬空、又不拖时间。
    //
    // 注意这里不能先 stop() 整个池子：池子是调用方的，可能还在服务别的
    // 传输（多任务并行是设计目标）。本类只有"别让我的任务活着越过我"
    // 这一项责任。
    for (;;) {
        const auto n = runningTasks_.load(std::memory_order_acquire);
        if (n == 0) {
            break;
        }
        std::this_thread::yield();
    }
}

Status TransferSender::open(std::string_view sourcePath, std::uint32_t chunkSize)
{
    if (opened_) {
        return Status::Fail(ErrorCode::kInvalidPath, "sender already open");
    }

    // 块大小的合法性在这里判一次。validateDesc 里也有同样的规则，但那是给
    // "收到的描述"用的；这里是"自己构造描述"，两处规则必须一致——所以直接
    // 复用常量，不复制魔法数字。
    if (chunkSize < kMinChunkSize || chunkSize > kMaxChunkSize || chunkSize % kChunkSizeAlignment != 0) {
        return Status::Fail(ErrorCode::kInvalidPath, "chunk size out of range or misaligned");
    }

    const auto st = store_.stat(sourcePath);
    if (!st.exists) {
        return Status::Fail(ErrorCode::kNotFound, "source file does not exist");
    }
    if (st.isDir) {
        // 目录要由上层先打包。传输层只认单个文件，不在这里偷偷处理目录树。
        return Status::Fail(ErrorCode::kInvalidPath, "source is a directory, archive it first");
    }
    if (!st.readable) {
        return Status::Fail(ErrorCode::kPermissionDenied, "source file is not readable");
    }

    sourcePath_ = std::string(sourcePath);
    chunkSize_ = chunkSize;
    totalSize_ = st.size;
    totalChunks_ = chunkCountFor(totalSize_, chunkSize_);

    // 窗口块数：显式指定的优先，否则按字节配额换算。
    //
    // 换算放在这里而不是构造函数里：构造函数还不知道 chunkSize，而"4 MiB
    // 在飞"要在知道块大小之后才能落成块数。
    //
    // 上限 65536 是防御性的：windowBytes 是可配置的，块又能小到 1 KiB，
    // 两者相乘会让窗口大得毫无意义（并让 SendWindow 的位图失控）。
    std::uint32_t effectiveWindowChunks = config_.windowChunks;
    if (effectiveWindowChunks == 0) {
        const std::size_t byBytes = config_.windowBytes / chunkSize;
        const std::size_t clamped = std::min<std::size_t>(std::max<std::size_t>(byBytes, 1), 65536);
        effectiveWindowChunks = static_cast<std::uint32_t>(clamped);
    }

    window_ = std::make_unique<SendWindow>(totalChunks_, effectiveWindowChunks);
    opened_ = true;

    return Status::Ok();
}

std::uint32_t TransferSender::totalChunks() const noexcept
{
    return totalChunks_;
}

std::uint32_t TransferSender::chunkSize() const noexcept
{
    return chunkSize_;
}

std::uint64_t TransferSender::totalSize() const noexcept
{
    return totalSize_;
}

std::uint64_t TransferSender::offsetOf(std::uint32_t chunkIndex) const noexcept
{
    return static_cast<std::uint64_t>(chunkIndex) * chunkSize_;
}

std::uint32_t TransferSender::lengthOf(std::uint32_t chunkIndex) const noexcept
{
    // 一堆块的长度上限就是 chunkSize（≤ 4 MiB），所以收窄到 u32 是安全的。
    // 但 chunkIndex 越界时 chunkLengthAt 会返回 0，这个也要保住——不能
    // 因为收窄把"越界"变成一个奇怪的巨大长度。
    return static_cast<std::uint32_t>(chunkLengthAt(chunkIndex, totalSize_, chunkSize_));
}

TransferSender::DigestResult TransferSender::computeDigestSync()
{
    // 仅供测试与小文件使用：在当前线程上直接读一遍。
    //
    // 刻意**不复用** computeDigest 的异步路径：那条路会往线程池里丢任务
    // 然后阻塞等它回来，而"谁在等"取决于线程池还剩几个线程——线程池被
    // 别的东西占满时，这个等待就是死锁。同步版自己读，谁也不依赖。
    DigestResult out;

    if (!opened_) {
        out.status = Status::Fail(ErrorCode::kUnknown, "computeDigestSync before open");
        return out;
    }

    auto in = store_.openRead(sourcePath_, out.status);
    if (!in) {
        if (out.status.ok()) {
            out.status = Status::Fail(ErrorCode::kIoError, "open source for digest failed");
        }
        return out;
    }

    crypto::Sha256 sha;

    MerkleBuilder builder(chunkSize_);

    std::vector<char> buf(chunkSize_);
    std::uint64_t remaining = totalSize_;

    while (remaining > 0) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf.size()));

        // 短读要接着读，不能当成文件结束：磁盘读可能只给一部分。
        std::size_t got = 0;
        while (got < want) {
            Status st;
            const auto n = in->read(buf.data() + got, want - got, st);
            if (!st.ok()) {
                out.status = st;
                return out;
            }
            if (n == 0) {
                // 文件在传输过程中变短了。这是真错误：继续算下去得到的根
                // 和描述里声明的不一致，只会更难查。
                out.status = Status::Fail(ErrorCode::kIoError, "source file shrank during digest");
                return out;
            }
            got += n;
        }

        sha.update(buf.data(), want);
        if (auto st = builder.addChunk(buf.data(), want); !st.ok()) {
            out.status = st;
            return out;
        }
        remaining -= want;
    }

    out.sha256Hex = crypto::Sha256::hex(sha.digest());
    out.merkleRootHex = toHex(builder.root());
    out.status = Status::Ok();
    return out;
}

Status TransferSender::computeDigest(std::function<void(DigestResult)> onDone)
{
    if (!opened_) {
        return Status::Fail(ErrorCode::kUnknown, "computeDigest before open");
    }
    if (!onDone) {
        return Status::Fail(ErrorCode::kInvalidPath, "computeDigest needs a callback");
    }

    // 摘要计算要整读一遍文件，绝不能跑在 strand 上。它和读盘共用线程池，
    // 所以会在几个块之内和读盘任务交错——那没问题，两者都只读不写。
    const bool accepted = pool_.submit([this, onDone = std::move(onDone)] { onDone(computeDigestSync()); });

    if (!accepted) {
        // 队列满不是"摘要算不出来"，而是"现在排不上"。调用方可以稍后重试。
        return Status::Fail(ErrorCode::kBusy, "worker pool is full, retry computeDigest later");
    }

    return Status::Ok();
}

std::uint32_t TransferSender::pump(const PostFn& post)
{
    if (!opened_ || !window_ || !readError_.ok()) {
        return 0;
    }

    // 读盘在飞的任务数上限。超过它就不该再提交——那会把"磁盘排队"
    // 转成"内存里堆满未读的块"。
    if (reading_.size() >= config_.maxReadInFlight) {
        return 0;
    }

    const auto budget = config_.maxReadInFlight - static_cast<std::uint32_t>(reading_.size());
    auto batch = window_->takeNextBatch(budget);

    if (batch.empty()) {
        return 0;
    }

    std::uint32_t submitted = 0;

    for (auto idx : batch) {
        // 计数在**提交前**加：任务可能被另一个线程立刻取走开跑，
        // 提交后再加会让析构看到一个偏小的值。
        runningTasks_.fetch_add(1, std::memory_order_acq_rel);

        const auto accepted = pool_.submit([this, idx, post]() {
            // 任务体退出时把计数还回去。用 RAII 而不是在每个 return 前
            // 手动减：这条函数里有五个出口，漏一个就是永久等待。
            struct RunningGuard {
                std::atomic<std::uint32_t>& n;
                ~RunningGuard()
                {
                    n.fetch_sub(1, std::memory_order_acq_rel);
                }
            } guard{runningTasks_};

            ReadChunk chunk;
            chunk.chunkIndex = idx;
            chunk.data.resize(lengthOf(idx));

            // 失败路径统一走这里：记错误 + 归还窗口 + 通知上层。
            auto fail = [this, idx, &post](Status st) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (readError_.ok()) {
                        readError_ = std::move(st);
                    }
                }
                if (post) {
                    post([this, idx] { onChunkReadyInternalFailure(idx); });
                }
            };

            Status openStatus;
            auto in = store_.openRead(sourcePath_, openStatus);
            if (!in) {
                // 读完的块不投递，只把错误记到 readError_。上层在
                // takeReadyChunks 之后检查 readError()。
                fail(openStatus.ok() ? Status::Fail(ErrorCode::kIoError, "open source for read failed") : openStatus);
                return;
            }

            // 每个任务自己 seek。不能共用流位置：多个任务并发读同一个
            // 文件时，共享的偏移会被彼此踩乱——读到的是别人的数据，
            // 而且长度正确，只有最终哈希能发现。
            if (auto st = in->seek(offsetOf(idx)); !st.ok()) {
                fail(std::move(st));
                return;
            }

            std::size_t got = 0;
            while (got < chunk.data.size()) {
                Status st;
                const auto n = in->read(chunk.data.data() + got, chunk.data.size() - got, st);
                if (!st.ok()) {
                    fail(std::move(st));
                    return;
                }
                if (n == 0) {
                    fail(Status::Fail(ErrorCode::kIoError, "source file shrank while reading chunk " + std::to_string(idx)));
                    return;
                }
                got += n;
            }

            if (post) {
                post([this, c = std::move(chunk)]() mutable { onChunkReady(std::move(c)); });
            }
        });

        if (!accepted) {
            // 提交失败，这个任务不会执行，计数要还回去。
            runningTasks_.fetch_sub(1, std::memory_order_acq_rel);

            // 队列满：把**从这一块起**还没提交的都归还窗口，然后停手。
            //
            // 按顺序处理很重要：batch 是升序的，已经提交成功的前缀保持在
            // 在飞状态，失败位置及其之后全部退回待发。继续提交后面的块
            // 通常也会失败，白折腾一遍。
            std::vector<std::uint32_t> rest;
            for (std::size_t i = submitted; i < batch.size(); ++i) {
                rest.push_back(batch[i]);
            }
            window_->release(rest);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            reading_.push_back(idx);
        }
        ++submitted;
    }

    return submitted;
}

void TransferSender::onChunkReady(ReadChunk chunk)
{
    std::lock_guard<std::mutex> lock(mu_);

    // 从 reading_ 里摘掉。找不到说明这一块已经被 cancelAll 归还过了，
    // 那次取消之后回来的块直接丢弃——上层已经不关心它了。
    const auto it = std::find(reading_.begin(), reading_.end(), chunk.chunkIndex);
    if (it == reading_.end()) {
        return;
    }
    reading_.erase(it);

    ready_.push_back(std::move(chunk));
}

void TransferSender::onChunkReadyInternalFailure(std::uint32_t chunkIndex)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = std::find(reading_.begin(), reading_.end(), chunkIndex);
        if (it != reading_.end()) {
            reading_.erase(it);
        }
    }

    // 读失败的块退回待发状态。不退回的话它永远占着窗口，而窗口满之后
    // pump 什么也提交不了 —— 表现为"传输无声停住"，比直接失败更难查。
    //
    // window_ 只在 strand 上被访问，本函数也跑在 strand 上，所以不必加锁。
    if (window_ && window_->stateOf(chunkIndex) == ChunkState::kInflight) {
        window_->release({chunkIndex});
    }
}

std::vector<ReadChunk> TransferSender::takeReadyChunks()
{
    std::vector<ReadChunk> out;
    {
        std::lock_guard<std::mutex> lock(mu_);
        out.swap(ready_);
    }

    // 排序再交出去：读盘完成顺序和块号顺序无关，而上层发帧时按块号递增
    // 发送能让对端的 SACK 位图更早连成一片（连续前缀推进更快，窗口更早
    // 腾出来）。这是一个廉价但有效的优化，代价只是一次排序。
    std::sort(out.begin(), out.end(), [](const ReadChunk& a, const ReadChunk& b) { return a.chunkIndex < b.chunkIndex; });
    return out;
}

bool TransferSender::hasWork() const
{
    if (!opened_ || !window_) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!readError_.ok()) {
            return false;
        }
        if (!reading_.empty() || !ready_.empty()) {
            return true;
        }
    }

    if (window_->done()) {
        return false;
    }
    return window_->freeSlots() > 0;
}

void TransferSender::applyAck(const ChunkAck& ack)
{
    if (window_) {
        (void)window_->applyAck(ack);
    }
}

std::vector<std::uint32_t> TransferSender::outstandingChunks() const
{
    return window_ ? window_->outstanding() : std::vector<std::uint32_t>{};
}

std::uint32_t TransferSender::markRetransmit(const std::vector<std::uint32_t>& chunks)
{
    return window_ ? window_->markRetransmit(chunks) : 0;
}

void TransferSender::cancelAll()
{
    if (!window_) {
        return;
    }

    // 在读和在飞的都归还。已在 worker 里跑的那个拦不住，它完成时
    // onChunkReady 找不到对应的 reading_ 项，会自行丢弃。
    std::vector<std::uint32_t> toRelease;
    {
        std::lock_guard<std::mutex> lock(mu_);
        toRelease.reserve(reading_.size() + ready_.size());
        for (auto idx : reading_) {
            toRelease.push_back(idx);
        }
        for (const auto& c : ready_) {
            toRelease.push_back(c.chunkIndex);
        }
        reading_.clear();
        ready_.clear();
    }

    window_->release(toRelease);
}

bool TransferSender::done() const noexcept
{
    return window_ && window_->done();
}

std::uint32_t TransferSender::ackedChunks() const noexcept
{
    return window_ ? window_->acked() : 0;
}

std::uint32_t TransferSender::retransmittedChunks() const noexcept
{
    return window_ ? window_->retransmitted() : 0;
}

Status TransferSender::readError() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return readError_;
}

}   // namespace relayer::core::transfer
