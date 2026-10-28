#include "relayer/core/transfer/TransferReceiver.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "relayer/common/crypto/Sha256.h"
#include "relayer/core/transfer/MerkleTree.h"
#include "relayer/filestore/MemFileStore.h"
#include "relayer/filestore/StdFileStore.h"
#include "support/TempDir.h"

using namespace relayer;
using namespace relayer::core::transfer;

// 接收侧引擎的测试策略与发送侧一致：**不让线程时序进来**。
//
// 写盘真的跑在 WorkerPool 上，但投递用同步版 PostFn（直接执行回调），
// 于是"受理 → 写盘 → 回调"在一次调用内顺序完成。这样断言能精确说出
// "现在有几块落盘了"，而不是"等 100ms 后大概怎样"。
//
// 接收侧比发送侧更需要这一点：这里的断言是"文件里现在是什么内容"，
// 而半写状态下的内容是没有意义的。

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kChunk = 1024;   // 最小合法块大小，方便多切几块

PostFn inlinePost()
{
    return [](std::function<void()> fn) { fn(); };
}

std::string makeContent(std::size_t len, unsigned seed = 0)
{
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>((i * 31 + seed * 17 + 7) & 0xFF);
    }
    return s;
}

// 等条件成立。同步 PostFn 下多数用例不需要，但并发/真实线程池的用例要。
bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

// 等所有在写的块落盘。
//
// 为什么必须显式等：inlinePost 只保证**回调**在提交线程上执行，不保证
// 写盘已经完成——写盘永远在 WorkerPool 上。所以"acceptChunk 返回了"和
// "数据到文件了"是两件事，后者是异步的。
//
// 这一点与发送侧完全一样（TransferSenderTest 里的 drainUntil）。第一次
// 写接收侧测试时我也假设了 inlinePost 会同步整条链路，结果 8 条用例
// 一起红——失败的断言是 writtenChunks()==0，很容易误以为实现没写。
void settle(TransferReceiver& r, std::chrono::milliseconds timeout = 5000ms)
{
    REQUIRE(waitUntil([&] { return !r.hasPendingWrites(); }, timeout));
}

// 读回一个文件（测试断言用）。
std::string readAll(filestore::IFileStore& store, const std::string& path)
{
    Status st;
    auto in = store.openRead(path, st);
    if (!in) {
        return {};
    }
    std::string out;
    char buf[4096];
    for (;;) {
        Status rs;
        const auto n = in->read(buf, sizeof(buf), rs);
        if (!rs.ok() || n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

// 构造一个能被 open() 接受的描述。
TransferDesc makeDesc(const std::string& content, std::uint32_t chunkSize, const std::string& root = "out.bin")
{
    TransferDesc d;
    d.displayName = "out.bin";
    d.totalSize = content.size();
    d.chunkSize = chunkSize;
    d.root = root;

    crypto::Sha256 sha;
    sha.update(content.data(), content.size());
    d.sha256Hex = crypto::Sha256::hex(sha.digest());

    // fingerprint 只要求非空（validateDesc 检查的是格式），用 sha256 顶上。
    d.fingerprintHex = d.sha256Hex;
    return d;
}

// 把 content 按 chunkSize 切块后全部喂给 receiver，并**等它们落盘**。
//
// 结尾等一次是刻意的：调用方紧接着几乎总是断言"文件内容对不对"或
// "一共落了几块"，那两件事都要等写盘完成。让 feedAll 兜住这一步，
// 每个用例就不用各自记得加 settle ——漏加的症状是"偶发 0 块"。
void feedAll(TransferReceiver& r, const std::string& content, std::uint32_t chunkSize, const PostFn& post = inlinePost())
{
    const auto total = chunkCountFor(content.size(), chunkSize);
    for (std::uint32_t i = 0; i < total; ++i) {
        const auto off = static_cast<std::size_t>(i) * chunkSize;
        const auto len = static_cast<std::size_t>(chunkLengthAt(i, content.size(), chunkSize));
        REQUIRE(r.acceptChunk(i, content.data() + off, len, post).ok());
    }
    REQUIRE(waitUntil([&] { return !r.hasPendingWrites(); }));
}

}   // namespace

// ===========================================================================
// open
// ===========================================================================

TEST_CASE("receiver open creates a part file and starts from zero", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(3 * kChunk + 100);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 999;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    REQUIRE(resumeFrom == 0);
    REQUIRE(r.totalChunks() == 4);
    REQUIRE(r.chunkSize() == kChunk);
    REQUIRE(r.totalSize() == content.size());

    // 残片建出来了，正式文件还没有
    REQUIRE(store.stat("out.bin.relaypart").exists);
    REQUIRE_FALSE(store.stat("out.bin").exists);
}

TEST_CASE("receiver open rejects a bad chunk size", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(kChunk);
    auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;

    desc.chunkSize = 100;   // 小于 kMinChunkSize
    REQUIRE_FALSE(r.open(desc, "out.bin", resumeFrom).ok());

    desc.chunkSize = kChunk + 1;   // 未对齐
    REQUIRE_FALSE(r.open(desc, "out.bin", resumeFrom).ok());
}

TEST_CASE("receiver open reports an existing target with the same size", "[transfer][receiver][resume]")
{
    // 目标已存在且长度一致 → kAlreadyExists，让上层回 kTargetExists。
    // 注意这**不是失败**：上层要据此问用户"要不要覆盖"。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    Status wst;
    auto out = store.openWrite("out.bin", true, wst);
    REQUIRE(out != nullptr);
    REQUIRE(out->write(content.data(), content.size()).ok());
    REQUIRE(out->flush().ok());
    out.reset();

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    const auto st = r.open(desc, "out.bin", resumeFrom);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kAlreadyExists);
}

TEST_CASE("receiver open resumes from an existing part file", "[transfer][receiver][resume]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(4 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    // 预置一块残片：前 2 块
    Status wst;
    auto out = store.openWrite("out.bin.relaypart", true, wst);
    REQUIRE(out != nullptr);
    REQUIRE(out->write(content.data(), 2 * kChunk).ok());
    REQUIRE(out->flush().ok());
    out.reset();

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());
    REQUIRE(resumeFrom == 2);
}

TEST_CASE("receiver truncates a part file that ends mid chunk", "[transfer][receiver][resume]")
{
    // 残片停在半块上（写到 1.5 块时断电）。必须截到 1 块，让第 1 块
    // 整块重传——不截的话第 1 块永远缺后半截，而长度看起来"接近完整"。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(4 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    Status wst;
    auto out = store.openWrite("out.bin.relaypart", true, wst);
    REQUIRE(out != nullptr);
    REQUIRE(out->write(content.data(), kChunk + kChunk / 2).ok());
    REQUIRE(out->flush().ok());
    out.reset();

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    REQUIRE(resumeFrom == 1);
    REQUIRE(store.stat("out.bin.relaypart").size == kChunk);
}

TEST_CASE("receiver ignores a part file longer than the target", "[transfer][receiver][resume]")
{
    // 残片比目标还长：块大小或文件改过，不是这次的产物。从头来。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    Status wst;
    auto out = store.openWrite("out.bin.relaypart", true, wst);
    REQUIRE(out != nullptr);
    const auto junk = makeContent(10 * kChunk, 99);
    REQUIRE(out->write(junk.data(), junk.size()).ok());
    REQUIRE(out->flush().ok());
    out.reset();

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());
    REQUIRE(resumeFrom == 0);
}

// ===========================================================================
// acceptChunk
// ===========================================================================

TEST_CASE("receiver writes a chunk and reports it written", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    REQUIRE(r.acceptChunk(0, content.data(), content.size(), inlinePost()).ok());
    settle(r);
    REQUIRE(r.writtenChunks() == 1);
    REQUIRE(r.hasChunk(0));
    REQUIRE(readAll(store, "out.bin.relaypart") == content);
}

TEST_CASE("receiver accepts chunks out of order", "[transfer][receiver]")
{
    // 乱序是常态，不是异常。写进去的位置必须跟着块号走。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(4 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 3, 0, 2, 1
    const std::vector<std::uint32_t> order = {3, 0, 2, 1};
    for (auto i : order) {
        REQUIRE(r.acceptChunk(i, content.data() + i * kChunk, kChunk, inlinePost()).ok());
    }

    settle(r);
    REQUIRE(r.writtenChunks() == 4);
    REQUIRE(readAll(store, "out.bin.relaypart") == content);
}

TEST_CASE("receiver rejects a chunk with the wrong length", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 块 0 应该是 kChunk 字节
    const auto st = r.acceptChunk(0, content.data(), kChunk - 1, inlinePost());
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kProtocol);
    REQUIRE(r.writtenChunks() == 0);
}

TEST_CASE("receiver rejects a chunk index out of range", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    const auto st = r.acceptChunk(2, content.data(), kChunk, inlinePost());
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kProtocol);
}

TEST_CASE("receiver drops a duplicate chunk without writing twice", "[transfer][receiver]")
{
    // 重传是正常路径：对端没收到 ACK 就会重发。回应是"再 ACK 一次"，
    // 不是"再写一遍"，更不是报错。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    REQUIRE(r.acceptChunk(0, content.data(), content.size(), inlinePost()).ok());
    settle(r);
    REQUIRE(r.writtenChunks() == 1);

    const auto again = r.acceptChunk(0, content.data(), content.size(), inlinePost());
    REQUIRE_FALSE(again.ok());
    REQUIRE(again.code == ErrorCode::kAlreadyExists);
    REQUIRE(r.writtenChunks() == 1);   // 没有重复计数
}

TEST_CASE("receiver reports kBusy when the buffer is full", "[transfer][receiver][backpressure]")
{
    // 背压：到上限就返回 kBusy，上层据此暂停读连接。**不能当失败处理。**
    //
    // 用不投递的 PostFn 让块停在"已受理但没写盘"的状态。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(8 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver::Config cfg;
    cfg.maxBufferedChunks = 2;
    cfg.maxWriteInFlight = 2;

    TransferReceiver r(store, pool, cfg);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 一个"投了不执行"的 PostFn：任务跑了但完成回调不回来，
    // 于是 inFlight_ 一直占着。
    PostFn dropPost = [](std::function<void()>) {};

    REQUIRE(r.acceptChunk(0, content.data(), kChunk, dropPost).ok());
    REQUIRE(r.acceptChunk(1, content.data() + kChunk, kChunk, dropPost).ok());

    const auto third = r.acceptChunk(2, content.data() + 2 * kChunk, kChunk, dropPost);
    REQUIRE_FALSE(third.ok());
    REQUIRE(third.code == ErrorCode::kBusy);

    // 到上限之后 hasCapacity 必须说实话
    REQUIRE_FALSE(r.hasCapacity());
}

TEST_CASE("receiver releases a chunk slot after a write failure", "[transfer][receiver]")
{
    // 写失败后这一块必须能被重传进来。不释放的话对端重传会拿到
    // kAlreadyExists，于是永远没人写它——表现是"卡住不动且不报错"。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 直接走失败回调模拟"写盘任务失败了"
    REQUIRE(r.acceptChunk(0, content.data(), kChunk, [](std::function<void()>) {}).ok());
    r.onChunkWriteFailed(0, Status::Fail(ErrorCode::kNoSpace, "disk full"));

    REQUIRE_FALSE(r.writeError().ok());
    REQUIRE(r.writeError().code == ErrorCode::kNoSpace);

    // 再收一次同一块：应当被受理（而不是 kAlreadyExists）
    REQUIRE_FALSE(r.hasChunk(0));
}

// ===========================================================================
// ACK
// ===========================================================================

TEST_CASE("receiver ack counts only written chunks", "[transfer][receiver][ack]")
{
    // "先落盘后 ACK"的落点：在写的块不能算进 ACK。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(4 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 全部受理但都不写盘
    PostFn dropPost = [](std::function<void()>) {};
    for (std::uint32_t i = 0; i < 4; ++i) {
        REQUIRE(r.acceptChunk(i, content.data() + i * kChunk, kChunk, dropPost).ok());
    }

    const auto ack = r.buildCurrentAck();
    REQUIRE(ack.ackThrough == 0);   // 一块都没落盘
    REQUIRE(ack.bitmap.empty());
}

TEST_CASE("receiver ack advances through a contiguous prefix", "[transfer][receiver][ack]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(6 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    // 收 0,1,2 和 4（跳过 3）
    for (std::uint32_t i : {0u, 1u, 2u, 4u}) {
        REQUIRE(r.acceptChunk(i, content.data() + i * kChunk, kChunk, inlinePost()).ok());
    }

    settle(r);
    const auto ack = r.buildCurrentAck();
    // 连续前缀是 3（0,1,2 到了），4 落在位图里
    REQUIRE(ack.ackThrough == 3);

    // 位 i 表示"块 ackThrough+i 已收到"。
    //   位 0 = 块 3 → 没收到 → false
    //   位 1 = 块 4 → 已收到 → true
    REQUIRE_FALSE(bitmapTest(ack.bitmap, 0));
    REQUIRE(bitmapTest(ack.bitmap, 1));
    // 位 2 = 块 5 → 没收到
    REQUIRE_FALSE(bitmapTest(ack.bitmap, 2));
}

// ===========================================================================
// finalize
// ===========================================================================

TEST_CASE("receiver finalize verifies the root and renames", "[transfer][receiver][finalize]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(3 * kChunk + 37);   // 末尾短块
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    feedAll(r, content, kChunk);
    REQUIRE(r.allChunksReceived());

    const auto rootHex = toHex(rootOfBuffer(content.data(), content.size(), kChunk));
    const auto res = r.finalize(rootHex);

    INFO("finalize error: " << res.error.message);
    REQUIRE(res.status == CompleteStatus::kOk);

    // 正式文件在了，残片没了
    REQUIRE(store.stat("out.bin").exists);
    REQUIRE_FALSE(store.stat("out.bin.relaypart").exists);
    REQUIRE(readAll(store, "out.bin") == content);
}

TEST_CASE("receiver finalize reports a root mismatch and discards the part", "[transfer][receiver][finalize]")
{
    // 根不对时**不能**改名。先改名再校验会留下一个名字正确、内容错误的
    // 文件——那比传输失败严重得多。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    feedAll(r, content, kChunk);

    // 一个格式合法但对不上的根
    const auto wrongRoot = std::string(64, 'a');
    const auto res = r.finalize(wrongRoot);

    REQUIRE(res.status == CompleteStatus::kRootMismatch);
    REQUIRE_FALSE(store.stat("out.bin").exists);

    // 残片**必须删掉**，不能"留着下次续传"。
    //
    // 这条断言原先写的是 REQUIRE(store.stat(...).exists)，理由是"残片留着，
    // 可续传"。那个理由在残片内容真实可信时成立，但这里的残片已经被证明
    // **内容不对**——留着它只会让下一次传输从错误的偏移接着收，于是每次
    // 都传满 100%、每次都报 root_mismatch，重试永远救不回来。
    //
    // 触发这一改动的真实事故：伪造残片（前 16MB 对、
    // 后 16MB 是 0）能通过 open() 阶段的一切检查——描述里只有整文件 sha256
    // 和一个只覆盖前 64KB 的 fingerprint，而伪造残片的前 64KB 恰好是对的。
    // 唯一能识破它的地方就是算完真实根的这一刻，所以此刻必须自清。
    REQUIRE_FALSE(store.stat("out.bin.relaypart").exists);

    // 实际根要给出来，便于说明差在哪
    REQUIRE(res.actualRootHex == toHex(rootOfBuffer(content.data(), content.size(), kChunk)));
}

TEST_CASE("receiver finalize refuses while writes are in flight", "[transfer][receiver][finalize]")
{
    // 还在写就 finalize 会读到半截数据，算出来的根必然不对——而那个
    // "根不对"会把排查引向"数据传错了"，方向完全错。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    PostFn dropPost = [](std::function<void()>) {};
    REQUIRE(r.acceptChunk(0, content.data(), kChunk, dropPost).ok());
    REQUIRE(r.hasPendingWrites());

    const auto res = r.finalize(std::string(64, '0'));
    REQUIRE(res.status == CompleteStatus::kWriteFailed);
    REQUIRE(res.error.code == ErrorCode::kBusy);
}

TEST_CASE("receiver finalize detects a short part file", "[transfer][receiver][finalize]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(3 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    feedAll(r, content, kChunk);

    // 人为把残片截短（模拟"某块没写进去但被误报成功"这类实现 bug）
    REQUIRE(store.resize("out.bin.relaypart", kChunk).ok());

    const auto res = r.finalize(toHex(rootOfBuffer(content.data(), content.size(), kChunk)));
    REQUIRE(res.status == CompleteStatus::kShapeMismatch);
}

TEST_CASE("receiver handles an empty file", "[transfer][receiver][finalize]")
{
    // 0 字节文件是边界：0 块，但根不是"空树"，是 SHA256("")。
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const std::string content;
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());

    REQUIRE(r.totalChunks() == 0);
    REQUIRE(r.allChunksReceived());

    const auto res = r.finalize(toHex(rootOfBuffer(content.data(), 0, kChunk)));
    INFO("finalize error: " << res.error.message);
    REQUIRE(res.status == CompleteStatus::kOk);
    REQUIRE(readAll(store, "out.bin").empty());
}

// ===========================================================================
// abort
// ===========================================================================

TEST_CASE("receiver abort removes the part file", "[transfer][receiver]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1);

    const auto content = makeContent(2 * kChunk);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver r(store, pool);
    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, "out.bin", resumeFrom).ok());
    REQUIRE(store.stat("out.bin.relaypart").exists);

    r.abort();
    REQUIRE_FALSE(store.stat("out.bin.relaypart").exists);

    // 取消之后不该再接受块
    REQUIRE_FALSE(r.acceptChunk(0, content.data(), kChunk, inlinePost()).ok());
}

// ===========================================================================
// 真实线程池
// ===========================================================================

TEST_CASE("receiver works with a real worker pool", "[transfer][receiver][threaded]")
{
    // 前面所有用例都用同步 PostFn 排除了线程时序。这一条反过来：用真池子
    // + 真文件系统，确认"并发写盘不会写坏文件"。乱序 + 并发是这个引擎最
    // 容易出错的地方，必须有至少一条用例真的并发跑。
    //
    // 这里**必须**用 StdFileStore 而不是 MemFileStore：后者头文件里写明
    // 了"不是线程安全的，并发测试请用真实现"。用内存实现跑这条用例的话，
    // 测的是 MemFileStore 的缺陷，而不是接收引擎的正确性。
    test::TempDir dir;
    filestore::StdFileStore store;
    WorkerPool pool(4);

    const auto content = makeContent(32 * kChunk + 123);
    const auto desc = makeDesc(content, kChunk);

    TransferReceiver::Config cfg;
    cfg.maxBufferedChunks = 32;
    cfg.maxWriteInFlight = 4;

    TransferReceiver r(store, pool, cfg);
    const auto target = dir.path("out.bin");

    std::uint32_t resumeFrom = 0;
    REQUIRE(r.open(desc, target, resumeFrom).ok());

    const auto total = r.totalChunks();

    // 倒序喂：让块号与到达顺序完全相反，最大化"写错位置"的暴露概率。
    // post 用 inlinePost：回调在提交线程上跑（这里就是主线程），
    // 但写盘仍在线程池上——正是要验证的组合。
    for (std::uint32_t i = total; i-- > 0;) {
        const auto off = static_cast<std::size_t>(i) * kChunk;
        const auto len = static_cast<std::size_t>(chunkLengthAt(i, content.size(), kChunk));

        // 背压：满了就等。这正是上层该做的事。
        REQUIRE(waitUntil([&] { return r.hasCapacity(); }));
        REQUIRE(r.acceptChunk(i, content.data() + off, len, inlinePost()).ok());
    }

    REQUIRE(waitUntil([&] { return !r.hasPendingWrites(); }));
    REQUIRE(r.writtenChunks() == total);

    const auto rootHex = toHex(rootOfBuffer(content.data(), content.size(), kChunk));
    const auto res = r.finalize(rootHex);
    INFO("finalize error: " << res.error.message);
    REQUIRE(res.status == CompleteStatus::kOk);
    REQUIRE(readAll(store, target) == content);
}
