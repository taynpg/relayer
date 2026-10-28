#include "relayer/core/transfer/TransferSender.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "relayer/common/crypto/Sha256.h"
#include "relayer/core/transfer/MerkleTree.h"
#include "relayer/filestore/MemFileStore.h"

using namespace relayer;
using namespace relayer::core::transfer;

// 发送侧引擎的测试策略：**不让线程时序进来**。
//
// 读盘确实跑在 WorkerPool 上，但投递用同步版 PostFn（直接执行回调），
// 于是"提交 → 读盘 → 回调"变成一次调用内的顺序流程。这样：
//
//   - 断言能精确说出"现在窗口里几个在飞"，而不是"等 100ms 后大概是什么样"
//   - 测试跑得快（全部毫秒级，不含 sleep）
//   - 挂了就是真挂了，不是"CI 机器慢"
//
// 真正需要线程的地方（并发读、背压）单独用同步/阻塞的 PostFn 覆盖。

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kChunk = 1024;   // 最小的合法块大小，测试用它能多切几块

// 同步投递：立刻在当前线程执行，不排队。
//
// 这条路径等同于"io_context 单线程 + 回调已经在该 strand 上"。它不保证
// 读盘已完成（读盘永远在线程池上），只保证回调不会跨线程乱跑。
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

// 等线程池把活干完。用同步 PostFn 时这一步不需要，但并发用例要。
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

// 反复 pump + 收块，直到收满 want 块或超时。
//
// 为什么需要它：pump() 提交的读盘任务**真的跑在线程池上**（那是设计目的），
// 所以"提交完立刻 takeReadyChunks()"拿到空是正常的，不是 bug。inlinePost
// 只保证回调在提交线程上执行，不保证读盘已经完成。
//
// autoAck：收满一批就自动确认一遍。窗口小于总块数时**必须**打开它，
// 否则窗口被收下来的块占满，永远发不出下一批——这不是引擎的 bug，
// 而是"只读不确认"本身就不构成一次完整传输。
std::vector<ReadChunk> drainUntil(TransferSender& sender, std::size_t want, bool autoAck = false,
                                  std::chrono::milliseconds timeout = 5000ms)
{
    std::vector<ReadChunk> out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
        if (autoAck && !out.empty()) {
            // 把已收到的块当成"对端已落盘"确认掉，腾出窗口。
            std::vector<bool> received(sender.totalChunks(), false);
            for (const auto& c : out) {
                received[c.chunkIndex] = true;
            }
            sender.applyAck(buildAck(received, sender.totalChunks()));
        }

        (void)sender.pump(inlinePost());
        for (auto& c : sender.takeReadyChunks()) {
            out.push_back(std::move(c));
        }
        if (out.size() < want) {
            std::this_thread::sleep_for(1ms);
        }
    }

    std::sort(out.begin(), out.end(), [](const ReadChunk& a, const ReadChunk& b) { return a.chunkIndex < b.chunkIndex; });
    return out;
}

}   // namespace

// ===========================================================================
// open 的前置校验
// ===========================================================================

TEST_CASE("open rejects missing file", "[transfer][sender]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);

    const auto st = sender.open("/nope.bin", kChunk);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kNotFound);
}

TEST_CASE("open rejects a directory", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putDir("/d");
    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);

    const auto st = sender.open("/d", kChunk);
    REQUIRE_FALSE(st.ok());
    // 目录必须由上层先打包。这里明确拒绝，而不是偷偷当成空文件。
    REQUIRE(st.code == ErrorCode::kInvalidPath);
}

TEST_CASE("open rejects illegal chunk sizes", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(4096));
    WorkerPool pool(1, 16);

    SECTION("below minimum")
    {
        TransferSender s(store, pool);
        REQUIRE_FALSE(s.open("/f.bin", 512).ok());
    }
    SECTION("above maximum")
    {
        TransferSender s(store, pool);
        REQUIRE_FALSE(s.open("/f.bin", kMaxChunkSize + kChunkSizeAlignment).ok());
    }
    SECTION("misaligned")
    {
        TransferSender s(store, pool);
        REQUIRE_FALSE(s.open("/f.bin", kChunk + 17).ok());
    }
    SECTION("minimum is fine")
    {
        TransferSender s(store, pool);
        REQUIRE(s.open("/f.bin", kChunk).ok());
    }
}

TEST_CASE("open twice is rejected", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(4096));
    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);

    REQUIRE(sender.open("/f.bin", kChunk).ok());
    REQUIRE_FALSE(sender.open("/f.bin", kChunk).ok());
}

TEST_CASE("empty file has zero chunks and is done", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/empty.bin", "");
    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);

    REQUIRE(sender.open("/empty.bin", kChunk).ok());
    REQUIRE(sender.totalChunks() == 0);
    REQUIRE(sender.done());
    REQUIRE_FALSE(sender.hasWork());
    REQUIRE(sender.pump(inlinePost()) == 0);
}

// ===========================================================================
// 分块几何
// ===========================================================================

TEST_CASE("chunk count rounds up", "[transfer][sender]")
{
    filestore::MemFileStore store;
    WorkerPool pool(1, 16);

    // 2500 字节 / 1024 = 2 整块 + 452 字节的尾巴 => 3 块。
    store.putFile("/a.bin", makeContent(2500));
    TransferSender a(store, pool);
    REQUIRE(a.open("/a.bin", kChunk).ok());
    REQUIRE(a.totalChunks() == 3);
    REQUIRE(a.totalSize() == 2500);

    // 正好整数块时不该多切一块。
    store.putFile("/b.bin", makeContent(3072));
    TransferSender b(store, pool);
    REQUIRE(b.open("/b.bin", kChunk).ok());
    REQUIRE(b.totalChunks() == 3);
}

TEST_CASE("pump reads chunks with correct content", "[transfer][sender]")
{
    const auto content = makeContent(2500);
    filestore::MemFileStore store;
    store.putFile("/f.bin", content);

    WorkerPool pool(2, 16);
    TransferSender::Config cfg;
    cfg.windowChunks = 8;
    cfg.maxReadInFlight = 8;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());
    REQUIRE(sender.totalChunks() == 3);

    auto chunks = drainUntil(sender, 3);
    REQUIRE(chunks.size() == 3);

    // 必须按块号升序交付：上层按序发帧能让对端的连续前缀更快推进。
    REQUIRE(chunks[0].chunkIndex == 0);
    REQUIRE(chunks[1].chunkIndex == 1);
    REQUIRE(chunks[2].chunkIndex == 2);

    REQUIRE(chunks[0].data.size() == kChunk);
    REQUIRE(chunks[1].data.size() == kChunk);
    REQUIRE(chunks[2].data.size() == 452);   // 尾巴

    // 拼起来必须与源文件逐字节相同。这是"块号 -> 偏移"映射的唯一真凭据：
    // 映射写错时每块长度都对，只有拼回去才看得出。
    std::string rebuilt;
    for (const auto& c : chunks) {
        rebuilt.append(c.data.data(), c.data.size());
    }
    REQUIRE(rebuilt == content);
}

TEST_CASE("chunks cover the file across chunk sizes", "[transfer][sender]")
{
    // 换几种块大小，覆盖"正好整除 / 差一字节"两类边界。
    const auto content = makeContent(5000);

    for (std::uint32_t chunk : {1024u, 2048u, 4096u}) {
        INFO("chunk size " << chunk);
        filestore::MemFileStore store;
        store.putFile("/f.bin", content);

        WorkerPool pool(2, 16);
        TransferSender::Config cfg;
        cfg.windowChunks = 16;
        cfg.maxReadInFlight = 16;
        TransferSender sender(store, pool, cfg);

        REQUIRE(sender.open("/f.bin", chunk).ok());
        const auto want = static_cast<std::size_t>(sender.totalChunks());
        auto chunks = drainUntil(sender, want);
        REQUIRE(chunks.size() == want);

        std::string rebuilt;
        for (const auto& c : chunks) {
            rebuilt.append(c.data.data(), c.data.size());
        }
        REQUIRE(rebuilt == content);
    }
}

// ===========================================================================
// 窗口与背压
// ===========================================================================

TEST_CASE("pump respects the window", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(10 * kChunk));

    WorkerPool pool(2, 64);
    TransferSender::Config cfg;
    cfg.windowChunks = 3;
    cfg.maxReadInFlight = 16;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());
    REQUIRE(sender.totalChunks() == 10);

    // 第一次只能提交 3 块：窗口就是 3。这是**提交数**，与"读没读完"无关，
    // 所以可以直接断言。
    REQUIRE(sender.pump(inlinePost()) == 3);

    // 一块都没确认，窗口仍然是满的，不能再发新块。
    REQUIRE(sender.pump(inlinePost()) == 0);

    // 收下这 3 块（读盘是异步的）。
    REQUIRE(drainUntil(sender, 3).size() == 3);

    // 窗口仍然满（3 块都在飞），还是发不出。
    REQUIRE(sender.pump(inlinePost()) == 0);

    // 确认前两块，腾出两个位置。
    ChunkAck ack;
    ack.ackThrough = 2;
    sender.applyAck(ack);

    REQUIRE(sender.pump(inlinePost()) == 2);
    auto more = drainUntil(sender, 2);
    REQUIRE(more.size() == 2);
    REQUIRE(more[0].chunkIndex == 3);
    REQUIRE(more[1].chunkIndex == 4);
}

TEST_CASE("pump respects maxReadInFlight", "[transfer][sender]")
{
    // 窗口开得很大，但读盘上限很小：限制必须来自 maxReadInFlight。
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(20 * kChunk));

    // 单线程池：一个读盘任务在跑，其余排队。这样 reading_ 会稳定停在 4。
    WorkerPool pool(1, 64);
    TransferSender::Config cfg;
    cfg.windowChunks = 100;
    cfg.maxReadInFlight = 4;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());

    // pump 只提交读盘任务；提交数是同步决定的，所以能直接断言。
    // 上限 4：即使窗口是 100，也只能同时有 4 块在读。
    REQUIRE(sender.pump(inlinePost()) == 4);

    // 立刻再 pump：reading_ 已经满了，一块都不该再提交。
    //
    // 这是 maxReadInFlight 的全部意义：窗口允许发 100 块，但磁盘上同时
    // 只排 4 个读请求。不设这个上限时，慢盘会把"磁盘排队"变成"内存里
    // 堆满未读的块"。
    REQUIRE(sender.pump(inlinePost()) == 0);

    // 收完之后，名额腾出来，可以继续提交。
    //
    // 注意"读完"不等于"腾出窗口"：块从 reading_ 移到 ready_ 之后仍然是
    // in-flight（等 ACK）。所以要先 ACK 掉这 4 块，窗口才真的有位置。
    REQUIRE(drainUntil(sender, 4).size() == 4);
    REQUIRE(sender.pump(inlinePost()) == 0);

    ChunkAck ack;
    ack.ackThrough = 4;
    sender.applyAck(ack);

    // ACK 之后窗口应当空出来 4 个位置，但读盘名额还卡在 reading_ 上：
    // 那 4 个任务虽然读完了，onChunkReady 也可能还没把它们从 reading_ 摘掉
    // （投递是异步的，inlinePost 只是"立即在同一线程执行"，而任务本身在
    // 别的线程上跑）。
    //
    // 所以这里要等到 reading_ 真的清空。用 hasWork/pump 的返回值间接观察：
    // 反复 pump 直到提交成功，而不是假设"ACK 完立刻就能发"。
    bool submittedAgain = false;
    for (int i = 0; i < 2000; ++i) {
        if (sender.pump(inlinePost()) == 4) {
            submittedAgain = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(submittedAgain);
}

TEST_CASE("worker pool back pressure returns chunks to the window", "[transfer][sender]")
{
    // 线程池队列只有 1 个位置：pump 想提交 4 块时必然有提交失败。
    // 失败的那些必须退回窗口，否则窗口会被"永远不会回来"的块占死。
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(10 * kChunk));

    WorkerPool pool(1, 1);   // 1 线程，队列容量 1
    TransferSender::Config cfg;
    cfg.windowChunks = 8;
    cfg.maxReadInFlight = 8;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());

    // 第一次 pump 提交不了 8 块——队列只有 1 个位置。
    const auto submitted = sender.pump(inlinePost());
    REQUIRE(submitted >= 1);
    REQUIRE(submitted < 8);

    // 关键断言：每块最终都要读出来。若失败提交后的块被窗口永久占用，
    // 这里会少于 10 块——而这在真实传输里的表现是"卡在 80% 不动"。
    auto chunks = drainUntil(sender, 10, /*autoAck=*/true, 10000ms);
    REQUIRE(chunks.size() == 10);

    for (std::uint32_t i = 0; i < 10; ++i) {
        INFO("chunk " << i);
        REQUIRE(chunks[i].chunkIndex == i);
        REQUIRE(chunks[i].data.size() == kChunk);
    }
}

// ===========================================================================
// 取消
// ===========================================================================

TEST_CASE("cancelAll returns chunks to the window", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(10 * kChunk));

    WorkerPool pool(1, 64);
    TransferSender::Config cfg;
    cfg.windowChunks = 4;
    cfg.maxReadInFlight = 4;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());

    REQUIRE(sender.pump(inlinePost()) == 4);

    // 取消：在读的 4 块都退回待发，就绪队列清空。
    sender.cancelAll();

    // 取消之后窗口必须全部可用——这是"取消后能重新开始"的前提。
    // 若有的块留在 in-flight，窗口会被永久占住，重新开始也拿不到块。
    REQUIRE(sender.pump(inlinePost()) == 4);

    // 从中途已经投递回来的块不该混进来：它们要么已经按新的 pump 重新读，
    // 要么被丢弃。总之块号必须是 0..3 这个前缀。
    auto chunks = drainUntil(sender, 4);
    REQUIRE(chunks.size() == 4);
    for (std::uint32_t i = 0; i < 4; ++i) {
        INFO("chunk " << i);
        REQUIRE(chunks[i].chunkIndex == i);
    }
}

// ===========================================================================
// 完成与摘要
// ===========================================================================

TEST_CASE("done only after every chunk is acked", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(3 * kChunk));

    WorkerPool pool(2, 16);
    TransferSender::Config cfg;
    cfg.windowChunks = 8;
    cfg.maxReadInFlight = 8;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());
    REQUIRE_FALSE(sender.done());

    REQUIRE(drainUntil(sender, 3).size() == 3);
    REQUIRE_FALSE(sender.done());

    ChunkAck ack;
    ack.ackThrough = 2;   // 差最后一块
    sender.applyAck(ack);
    REQUIRE_FALSE(sender.done());
    REQUIRE(sender.ackedChunks() == 2);

    ack.ackThrough = 3;
    sender.applyAck(ack);
    REQUIRE(sender.done());
    REQUIRE_FALSE(sender.hasWork());
}

TEST_CASE("digest matches an independent computation", "[transfer][sender]")
{
    // 摘要这块必须**独立算一遍**再比较：拿引擎自己算两次比是自欺欺人。
    const auto content = makeContent(5000);
    filestore::MemFileStore store;
    store.putFile("/f.bin", content);

    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);
    REQUIRE(sender.open("/f.bin", kChunk).ok());

    const auto d = sender.computeDigestSync();
    REQUIRE(d.status.ok());

    // 整文件 SHA-256：直接用 common 里的实现，走的是另一条代码路径。
    crypto::Sha256 sha;
    sha.update(content.data(), content.size());
    REQUIRE(d.sha256Hex == crypto::Sha256::hex(sha.digest()));

    // Merkle 根：用 rootOfBuffer 独立算，它不经过 MerkleBuilder。
    const auto expectedRoot = rootOfBuffer(content.data(), content.size(), kChunk);
    REQUIRE(d.merkleRootHex == toHex(expectedRoot));
}

TEST_CASE("digest of an empty file", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/empty.bin", "");

    WorkerPool pool(1, 16);
    TransferSender sender(store, pool);
    REQUIRE(sender.open("/empty.bin", kChunk).ok());

    const auto d = sender.computeDigestSync();
    REQUIRE(d.status.ok());

    // 空文件的 Merkle 根是 SHA256("")，不是全零。
    REQUIRE(d.merkleRootHex == toHex(emptyRoot()));

    crypto::Sha256 sha;
    REQUIRE(d.sha256Hex == crypto::Sha256::hex(sha.digest()));
}

TEST_CASE("digest callback form delivers a result", "[transfer][sender]")
{
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(3000));

    WorkerPool pool(2, 16);
    TransferSender sender(store, pool);
    REQUIRE(sender.open("/f.bin", kChunk).ok());

    std::atomic<bool> called{false};
    TransferSender::DigestResult got;
    Status submit = Status::Ok();

    submit = sender.computeDigest([&](TransferSender::DigestResult r) {
        got = std::move(r);
        called.store(true, std::memory_order_release);
    });
    REQUIRE(submit.ok());

    REQUIRE(waitUntil([&called] { return called.load(std::memory_order_acquire); }));
    REQUIRE(got.status.ok());
    REQUIRE(got.merkleRootHex.size() == 64);
    REQUIRE(got.sha256Hex.size() == 64);
}

// ===========================================================================
// 失败路径
// ===========================================================================

TEST_CASE("source vanishing after open is reported", "[transfer][sender]")
{
    // 真实场景：用户在传输过程中删掉了源文件。这必须是个明确错误，
    // 而不是"传完发现哈希不对"。
    filestore::MemFileStore store;
    store.putFile("/f.bin", makeContent(3 * kChunk));

    WorkerPool pool(1, 16);
    TransferSender::Config cfg;
    cfg.windowChunks = 4;
    cfg.maxReadInFlight = 4;
    TransferSender sender(store, pool, cfg);

    REQUIRE(sender.open("/f.bin", kChunk).ok());

    REQUIRE(store.remove("/f.bin").ok());

    // 提交一批读盘任务，然后等它们失败。
    (void)sender.pump(inlinePost());

    REQUIRE(waitUntil([&sender] { return !sender.readError().ok(); }));
    REQUIRE_FALSE(sender.hasWork());

    // 出错之后不该再提交新任务：那只会把同一个错误重复几十遍。
    REQUIRE(sender.pump(inlinePost()) == 0);
}
