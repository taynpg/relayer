#include "relayer/core/transfer/TransferPeer.h"

#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "relayer/core/session/Session.h"
#include "relayer/core/transfer/MerkleTree.h"
#include "relayer/core/transfer/TransferPayload.h"
#include "relayer/filestore/MemFileStore.h"
#include "relayer/filestore/StdFileStore.h"
#include "support/IoPump.h"
#include "support/TempDir.h"

using namespace relayer;
using namespace relayer::core::transfer;
using namespace relayer::core::session;
using relayer::protocol::Frame;
using relayer::protocol::FrameType;

// TransferPeer 把 sender / receiver 挂到真实 Session 上，跑"收帧 → 解码 →
// 分发 → 回帧"的完整回路。
//
// 这些用例刻意**跑真实 socket**：本层要验证的正是"帧序列对不对、状态机的
// 推进时机对不对"，用假的 Session 替身只会把 bug 藏到替身里。回环上的两个
// 进程内连接足够，还省掉了一个三进程测试脚本。
//
// 单独跑：core_test.exe "[peer]"

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kChunk = 4096;   // 小到能切出多块，大到不至于帧数爆掉

// 跑真实 socket，所以每条用例都要推进事件循环；超时给宽一点，
// 但不至于让失败用例拖住整批。
constexpr auto kWait = std::chrono::seconds(10);

std::string makeContent(std::size_t len, unsigned seed = 0)
{
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>((i * 131 + seed * 17 + 7) & 0xFF);
    }
    return s;
}

// 两个 Store 的写读辅助。测试里既要用内存实现（快），
// 也要用真实文件实现（验证 seek / 落盘 / 原子替换）。
std::string readAllStore(filestore::IFileStore& store, const std::string& path)
{
    Status st;
    auto in = store.openRead(path, st);
    if (!in || !st.ok()) {
        return {};
    }
    std::string out;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        Status rs;
        const auto n = in->read(buf.data(), buf.size(), rs);
        if (!rs.ok()) {
            return {};
        }
        if (n == 0) {
            break;
        }
        out.append(buf.data(), n);
    }
    return out;
}

bool writeAllStore(filestore::IFileStore& store, const std::string& path, std::string_view content)
{
    Status st;
    auto out = store.openWrite(path, true, st);
    if (!out || !st.ok()) {
        return false;
    }
    return out->write(content.data(), content.size()).ok() && out->flush().ok();
}

// ---------------------------------------------------------------------------
// 双端测试台
// ---------------------------------------------------------------------------

// 把两条真实 Session 和一对 TransferPeer 接起来。
//
// 生命周期刻意做成"成员顺序即析构顺序"：peer 先死、再是 session、最后是
// ctx。TransferPeer 的回调里会碰 session_，反过来 session 的 onFrame 会碰
// peer——两边都不能比对方活得久。
struct Harness {
    asio::io_context ctx;
    filestore::StdFileStore storeA;   // 发送方
    filestore::StdFileStore storeB;   // 接收方
    // 两端各一个池：一个池同时跑"发送侧读盘"和"接收侧写盘"会让两者
    // 互相挤占队列，而本层没有任何跨侧调度策略来避免这种饿死。
    WorkerPool sendPool{2};
    WorkerPool recvPool{2};

    std::shared_ptr<Session> client;
    std::shared_ptr<Session> server;

    std::shared_ptr<TransferPeer> sender;
    std::shared_ptr<TransferPeer> receiver;

    // 结果。用 shared_ptr 是因为回调可能活到测试作用域之后（比如超时用例）。
    struct Sink {
        int calls = 0;
        TransferResult result;
    };
    std::shared_ptr<Sink> senderSink = std::make_shared<Sink>();
    std::shared_ptr<Sink> receiverSink = std::make_shared<Sink>();

    // 传输层发出的、没带 to 的业务帧。
    //
    // 两侧直连时 to 没用，漏填也一切正常——所以这些缺陷只有跨中继才暴露。
    // 记下来并在用例末尾断言为空：断言成本几乎为零，而它能挡住"又有一类
    // 帧忘了填收件人"这种回归（历史上出过两次：应答帧和 ACK 帧）。
    std::vector<FrameType> framesWithoutTo;

    // 用例末尾统一调用。失败时把帧类型打出来——Catch2 默认不展开枚举
    // 容器，"哪个类型漏了"恰恰是唯一有用的信息。
    void requireEveryFrameAddressed() const
    {
        if (framesWithoutTo.empty()) {
            return;
        }
        std::string bad;
        for (const auto t : framesWithoutTo) {
            bad += std::to_string(static_cast<unsigned>(t));
            bad += ' ';
        }
        FAIL("有业务帧没填 to，类型: " << bad);
    }

    // 两端的事件循环推进。Session 里每条连接的帧都要经过它。
    bool pumpUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = kWait)
    {
        return relayer::test::pumpUntil(ctx, pred, timeout);
    }

    // 转一次事件循环，让排队的帧有机会被处理。
    void tick(std::chrono::milliseconds d = 1ms)
    {
        relayer::test::pump(ctx, d);
    }
};

// 建一对已认证的回环会话。callback 拿到两侧 session 后再挂 peer——
// 因为 peer 需要 session 指针，而 session 的 onFrame 又要转发给 peer。
template <typename Fn> void connectPair(Harness& h, Fn&& onReady)
{
    asio::ip::tcp::acceptor acc(h.ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    SessionConfig serverCfg;
    serverCfg.role = Role::kServer;
    serverCfg.server.password = "pw";
    serverCfg.serverId = "relayd";
    serverCfg.initialPeerId = "client-1";

    SessionConfig clientCfg;
    clientCfg.role = Role::kClient;
    clientCfg.clientPassword = "pw";
    clientCfg.server.allowNoAuth = false;

    // 心跳默认 10s 超时，比本文件的用例超时（10s）长，不会插入干扰。

    // 两侧的 onFrame 先占位，peer 建好后再改指它们。
    // std::function 可赋值，是这里最简单可靠的二次绑定方式。
    // 记录规则见 Harness::framesWithoutTo 的注释。
    //
    // 只检查**业务帧**：认证阶段的帧由 Session 层自己填 to（或者说，
    // 那些帧本来就不该有 to——它们是发给服务端/对端的握手，语义由帧类型
    // 决定），而 TransferPeer 只负责文件会话帧。把认证帧也算进去，
    // 这个断言就永远不空，等于没写。
    auto noteRecipient = [&h](const Frame& f) {
        if (!relayer::protocol::isAuthType(f.type) && f.to.empty()) {
            h.framesWithoutTo.push_back(f.type);
        }
    };

    auto serverHandler = std::make_shared<SessionHandler>();
    serverHandler->onFrame = [&h, noteRecipient](Session&, const Frame& f) {
        // 这一侧收到的是**发送方**发出的帧（request / chunk / finish）。
        noteRecipient(f);
        if (h.receiver) {
            h.receiver->onFrame(f);
        }
    };

    acc.async_accept([&h, serverCfg, serverHandler](std::error_code ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        h.server = Session::accept(h.ctx, std::move(sock), serverCfg, *serverHandler);
    });

    auto clientHandler = std::make_shared<SessionHandler>();
    clientHandler->onFrame = [&h, noteRecipient](Session&, const Frame& f) {
        // 这一侧收到的是**接收方**发出的帧（answer / ack / 收尾应答）。
        // 历史上漏填 to 的正是这一类——发送方的 to 在构造时就填了，
        // 接收方的 to 要靠收到请求后才学到手，最容易忘。
        noteRecipient(f);
        if (h.sender) {
            h.sender->onFrame(f);
        }
    };

    h.client = Session::connect(h.ctx, "127.0.0.1", port, clientCfg, *clientHandler);

    REQUIRE(h.pumpUntil([&h] { return h.client && h.server && h.client->authenticated() && h.server->authenticated(); }, 5s));

    onReady();
}

TransferPeer::Config peerConfig(std::uint32_t chunkSize = kChunk)
{
    TransferPeer::Config c;
    c.chunkSize = chunkSize;
    c.retransmitTimeout = 500ms;   // 测试里别等 3 秒
    c.sender.windowChunks = 8;
    c.sender.maxReadInFlight = 8;
    c.receiver.maxBufferedChunks = 16;
    c.receiver.maxWriteInFlight = 4;
    return c;
}

// 每 1ms 推进一次循环，直到 done() 为真或超时。
//
// 不用固定时长 pump：一次 8 块的传输在回环上只需要几毫秒，而 pump(1s)
// 会老老实实跑满 1 秒——二十条用例累积起来就是 CI 的 timeout。
bool driveUntil(Harness& h, const std::function<bool()>& done, std::chrono::milliseconds timeout = kWait)
{
    return h.pumpUntil(
        [&h, &done] {
            // peer 的推进由外部驱动（本层不带定时器），见 TransferPeer::pump 的注释。
            if (h.sender) {
                h.sender->pump();
            }
            if (h.receiver) {
                h.receiver->pump();
            }
            return done();
        },
        timeout);
}

}   // namespace

// ===========================================================================
// 正常传输
// ===========================================================================

TEST_CASE("peer transfers a file end to end", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(64 * kChunk + 777);
    const auto srcPath = srcDir.utf8() + "/payload.bin";
    const auto dstPath = dstDir.utf8() + "/received.bin";
    REQUIRE(writeAllStore(h.storeA, srcPath, content));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 7, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });

        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "payload.bin", "client-1", 7,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));

    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kCompleted);

    // 内容一致是唯一说得过去的验收标准。块数对、ACK 数对、但内容错了
    // 的情况在"块号错位"这类 bug 下真实存在。
    REQUIRE(readAllStore(h.storeB, dstPath) == content);

    // 残片不该留下。它留在盘上说明收尾没走完——即便内容对，
    // 下次同名传输也会被它干扰。
    REQUIRE_FALSE(h.storeB.stat(dstPath + ".relaypart").exists);

    // 全部块都走了一次完整往返，没有重传。
    REQUIRE(h.sender->stats().chunksSent >= h.sender->progress().chunksTotal);
    REQUIRE(h.sender->stats().duplicateChunks == 0);
    REQUIRE(h.sender->stats().protocolErrors == 0);

    // 跨中继回归：任何业务帧漏填 to，中继都会把它当成"发给服务端自己"
    // 本地吃掉，对端收不到、发起方在原地等超时。
    //
    // 这条断言在两侧直连的 Harness 里为什么还有效：Harness 虽然不走
    // planRoute，但两侧的 onFrame 是**各自**收到的帧，检查的就是"发出去
    // 的帧里有没有 to"。真正需要在意的只有收件人字段在不在，与中间那跳
    // 走不走得通无关——所以这里能拦住，只是拦不住"to 填错了"。
    //
    // 它不是纸面条款：第一次加上时就抓到了真缺陷——接收方在直连下算不出
    // 发送方是谁（帧的 from 被发送方有意清空，等中继绑定），于是它发出的
    // 每一帧都缺 to。
    h.requireEveryFrameAddressed();
}

TEST_CASE("peer handles an empty file", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    // 零长文件是边界：块数为 0，窗口立刻就 done()。这条路径最容易
    // 在"等第一个 ACK"上死锁——永远不会有 ACK 来。
    const auto srcPath = srcDir.utf8() + "/empty.bin";
    const auto dstPath = dstDir.utf8() + "/empty.out";
    REQUIRE(writeAllStore(h.storeA, srcPath, ""));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 1, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "empty.bin", "client-1", 1,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);

    REQUIRE(h.storeB.stat(dstPath).exists);
    REQUIRE(h.storeB.stat(dstPath).size == 0);

    h.requireEveryFrameAddressed();
}

TEST_CASE("peer handles a single byte file", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    // 一块、且块长小于 chunkSize。最后一块的"不满"逻辑与空文件是两条路。
    const auto srcPath = srcDir.utf8() + "/one.bin";
    const auto dstPath = dstDir.utf8() + "/one.out";
    REQUIRE(writeAllStore(h.storeA, srcPath, "x"));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 2, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "one.bin", "client-1", 2,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));
    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(readAllStore(h.storeB, dstPath) == "x");

    h.requireEveryFrameAddressed();
}

// ===========================================================================
// 断点续传
// ===========================================================================

TEST_CASE("peer resumes from a valid partial file", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(32 * kChunk + 100);
    const auto srcPath = srcDir.utf8() + "/resume.bin";
    const auto dstPath = dstDir.utf8() + "/resume.out";

    // 预置一个"前 10 块已经落好"的残片。这正是崩溃后重来时的盘上状态。
    auto partial = content.substr(0, 10 * kChunk);
    REQUIRE(writeAllStore(h.storeA, srcPath, content));
    REQUIRE(writeAllStore(h.storeB, dstPath + ".relaypart", partial));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 3, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "resume.bin", "client-1", 3,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    // 续传生效的直接证据：**对端应答之后、第一块数据发出之前**，
    // 发送方就应当把前 10 块算作已完成。
    //
    // 不用"总发送次数 <= 总数 - 10"来断言：chunksSent 含重传，而丢包/背压
    // 造成的重传会让这个数变大，与"是否省了前 10 块"无关。那样的断言在
    // 负载轻时碰巧通过，一有重传就红——测的是环境而不是逻辑。
    REQUIRE(h.pumpUntil(
        [&] {
            h.sender->pump();
            h.receiver->pump();
            return h.sender->progress().chunksDone >= 10;
        },
        3s));

    // 此时前 10 块不该有任何一块在"已发送"集合里被发过两次以上，
    // 而这正是续传点认错了才会出现的情况。用总量兜一个宽范围：
    // 真正全量重传时这里是 33 块，续传后无论如何都该明显更少。
    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));
    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(readAllStore(h.storeB, dstPath) == content);
    REQUIRE(h.sender->stats().chunksSent < h.sender->progress().chunksTotal);

    h.requireEveryFrameAddressed();
}

TEST_CASE("peer rejects a forged partial file and refetches everything", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(24 * kChunk);
    const auto srcPath = srcDir.utf8() + "/forged.bin";
    const auto dstPath = dstDir.utf8() + "/forged.out";

    // 残片长度与"前 8 块"吻合，但内容是别的——模拟残片被外部程序篡改，
    // 或者上一次传输写坏了。长度对得上是最容易骗过"存在即续传"的地方。
    auto forged = makeContent(8 * kChunk, 99);
    REQUIRE(writeAllStore(h.storeA, srcPath, content));
    REQUIRE(writeAllStore(h.storeB, dstPath + ".relaypart", forged));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 4, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "forged.bin", "client-1", 4,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));

    // 结果必须正确。残片的内容是错的，如果它被当成已传部分直接续上，
    // 最终文件是坏，而这个坏**通不过**最终校验——但更糟的情形是它通过了
    // 某种弱校验。这里断言的就是"要么全量重传、要么报错"，不能默默产出坏文件。
    if (h.receiverSink->result.outcome == TransferOutcome::kCompleted) {
        REQUIRE(readAllStore(h.storeB, dstPath) == content);
    } else {
        // 报错也是可接受的结果：残片不可信就该要求上层显式丢弃。
        // 但不该留下一个"看起来完成"的目标文件。
        REQUIRE_FALSE(h.storeB.stat(dstPath).exists);
    }

    h.requireEveryFrameAddressed();
}

// ===========================================================================
// 目标已存在
// ===========================================================================

TEST_CASE("peer reports an existing target instead of hanging", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(8 * kChunk);
    const auto srcPath = srcDir.utf8() + "/dup.bin";
    const auto dstPath = dstDir.utf8() + "/dup.out";

    REQUIRE(writeAllStore(h.storeA, srcPath, content));
    // 目标已存在且内容不同。接收方应当拒绝，而不是覆盖。
    REQUIRE(writeAllStore(h.storeB, dstPath, "something else entirely"));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 5, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "dup.bin", "client-1", 5,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    // 关键是"有结果"，不是"卡住"。一个不返回的拒绝路径在 UI 上就是
    // 永远转圈的进度条。
    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));

    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kFailed);
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kFailed);

    // 原文件不能被碰过。
    REQUIRE(readAllStore(h.storeB, dstPath) == "something else entirely");

    h.requireEveryFrameAddressed();
}

TEST_CASE("peer overwrites an existing target when the policy allows it", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(8 * kChunk + 123);
    const auto srcPath = srcDir.utf8() + "/dup.bin";
    const auto dstPath = dstDir.utf8() + "/dup.out";

    REQUIRE(writeAllStore(h.storeA, srcPath, content));
    // 目标已存在且内容不同。overwriteExisting 开着：应当删旧收新，
    // 而不是像上一条用例那样拒绝。"反复把固定文件发到对方同一目录"
    // 的核心场景靠这条行为活着。
    REQUIRE(writeAllStore(h.storeB, dstPath, "stale content"));

    auto cfg = peerConfig();
    cfg.overwriteExisting = true;

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 8, cfg,
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "dup.bin", "client-1", 8,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));

    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kCompleted);

    // 旧内容必须被完整替换成新内容——删了但没收到、收到一半改名失败，
    // 都会在这里露馅。
    REQUIRE(readAllStore(h.storeB, dstPath) == content);
    REQUIRE_FALSE(h.storeB.stat(dstPath + ".relaypart").exists);

    h.requireEveryFrameAddressed();
}

TEST_CASE("peer lands on an absolute root path instead of the receive directory", "[peer][integration]")
{
    // 全路径语义（DESIGN.md §3.8）：desc.root 是绝对路径时，落点就是它，
    // 不再拼接收目录。对照表的"远端目录"那一列靠这条活着——它写的是
    // 对端的绝对目录。
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(4 * kChunk + 7);
    const auto srcPath = srcDir.utf8() + "/abs-src.bin";
    const auto inbox = dstDir.utf8() + "/inbox";
    REQUIRE(writeAllStore(h.storeA, srcPath, content));
    REQUIRE(h.storeB.mkdirs(inbox));

    // 目标绝对路径，中间目录还不存在：接收方要在打开文件前建齐。
    const std::string absTarget = dstDir.utf8() + "/deep/nested/abs-dst.bin";

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, inbox, 6, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, absTarget, "client-1", 6,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }));

    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kCompleted);

    // 落在绝对路径上，内容一致，接收目录里不多出东西。
    REQUIRE(readAllStore(h.storeB, absTarget) == content);
    std::vector<relayer::filestore::DirEntry> inInbox;
    REQUIRE(h.storeB.list(inbox, true, inInbox).ok());
    REQUIRE(inInbox.empty());

    h.requireEveryFrameAddressed();
}

// ===========================================================================
// 取消
// ===========================================================================

TEST_CASE("local cancel notifies the peer", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto content = makeContent(64 * kChunk);
    const auto srcPath = srcDir.utf8() + "/cancel.bin";
    const auto dstPath = dstDir.utf8() + "/cancel.out";
    REQUIRE(writeAllStore(h.storeA, srcPath, content));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 6, peerConfig(),
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "cancel.bin", "client-1", 6,
                                              peerConfig(), [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    // 先让它跑起来一点，再取消——一上来就取消测不到"传输中途"。
    REQUIRE(h.pumpUntil([] { return true; }, 1ms));
    REQUIRE(driveUntil(h, [&] { return h.sender->progress().chunksDone > 0 || h.senderSink->calls > 0; }, 3s));

    h.sender->cancel("user pressed stop");

    // 两端都要收到结论。发送方是本地的 kLocalCancelled，接收方是从帧里
    // 读到的 kPeerCancelled。
    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }, 3s));
    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kLocalCancelled);
    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kPeerCancelled);

    // 取消不该留下一个"看起来完整"的目标文件。
    REQUIRE_FALSE(h.storeB.stat(dstPath).exists);

    h.requireEveryFrameAddressed();
}

// ===========================================================================
// 错误路径
// ===========================================================================

TEST_CASE("sender fails fast when the source is missing", "[peer]")
{
    // 这条不需要 socket：源文件不存在是本地错误，一个往返都不该走。
    //
    // session 传 nullptr 是**刻意的**：正是要证明这条路径根本碰不到
    // session。若某次改动让它在报错前先去问 session（取 id、发帧），
    // 这里会立刻段错误而不是安静地通过。
    asio::io_context ctx;
    filestore::StdFileStore store;
    WorkerPool pool{1};

    int calls = 0;
    TransferResult got;
    auto peer = TransferPeer::createSender(ctx, nullptr, store, pool, "/definitely/not/here.bin", "x.bin", "peer", 1,
                                           peerConfig(), [&](const TransferResult& r) {
                                               ++calls;
                                               got = r;
                                           });

    // createSender **同步**就该报错。若它交给异步路径，这条断言会先在
    // calls == 0 上失败——那正是我们要的"立刻知道"。
    REQUIRE(calls == 1);
    REQUIRE(got.outcome == TransferOutcome::kFailed);
    REQUIRE_FALSE(got.status.ok());
    REQUIRE(got.status.code == ErrorCode::kNotFound);
    REQUIRE(peer->finished());
}

TEST_CASE("peer ignores frames from another session", "[peer]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto srcPath = srcDir.utf8() + "/f.bin";
    const auto dstPath = dstDir.utf8() + "/f.out";
    REQUIRE(writeAllStore(h.storeA, srcPath, makeContent(kChunk)));

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 100, peerConfig(),
                                                  [](const TransferResult&) {});
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "f.bin", "client-1", 100,
                                              peerConfig(), [](const TransferResult&) {});
    });

    // sessionId 不匹配的帧必须被拒。同一个 Session 上会并行跑多个传输，
    // 靠 sessionId 分派——收错了会让另一条传输的块被写进这个文件。
    Frame other;
    other.type = FrameType::kChunkRequestData;
    other.sessionId = 999;
    other.index = 0;
    REQUIRE_FALSE(h.receiver->onFrame(other));

    // 角色不对的帧也要被拒：接收方不该处理 ACK。
    Frame wrongType;
    wrongType.type = FrameType::kChunkRequestAck;
    wrongType.sessionId = 100;
    REQUIRE_FALSE(h.receiver->onFrame(wrongType));

    h.requireEveryFrameAddressed();
}

TEST_CASE("sender fails instead of waiting forever on a silent peer", "[peer][integration]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const auto srcPath = srcDir.utf8() + "/silent.bin";
    REQUIRE(writeAllStore(h.storeA, srcPath, makeContent(4 * kChunk)));

    connectPair(h, [&] {
        auto cfg = peerConfig();
        // 收尾等待压到 300ms：默认 60 秒是给大文件算根留的余量，
        // 在测试里等不起。
        cfg.answerTimeout = 300ms;
        cfg.finalizeTimeout = 300ms;
        cfg.retransmitTimeout = 200ms;

        // 对端**不在**。没有它就没有 kFileAnswerSend，也没有任何
        // ACK——发送方会卡在"等一个不会来的应答"上。这正是要验的：
        // 这种情况必须有终点。
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "silent.bin", "client-1", 200, cfg,
                                              [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    // 结论必须是"失败且已结束"，而不是无限等下去。
    //
    // 注意这里验的不是某一条具体超时（等应答 / 等收尾各有各的路径），
    // 而是**整体契约**：对端不在时，本层一定在有限时间内给出结论。
    // 一条传输永远挂着，在 UI 上就是一个永远转圈的进度条。
    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0; }, 10s));
    REQUIRE(h.senderSink->result.outcome != TransferOutcome::kCompleted);
    REQUIRE_FALSE(h.senderSink->result.status.ok());
    REQUIRE(h.sender->finished());

    h.requireEveryFrameAddressed();
}

// 吞吐基准：**不经中继**的直连传输。
//
// 为什么单独留一条：线上每次都经 relayerd 转发，而"慢"到底慢在 core 的
// 编解码/落盘，还是慢在中继那一跳，光看端到端数字分不清。这条把中继摘掉，
// 于是两次测量的差就是中继的代价。
//
// 标签前加点 = 默认不跑（Catch2 的隐藏用例），手工执行：
//     core_test.exe "[.throughput]"
//
// 注意它比生产快的原因里有两条是测试环境特有的，比较时要心里有数：
//   1. pump 由 driveUntil 驱动（毫秒级），而生产由 TransferLink 的
//      tickInterval(10ms) 驱动；
//   2. 两端在同一个 io_context 上，没有跨进程的 syscall 与唤醒。
TEST_CASE("吞吐基准：直连（不经中继）", "[.throughput]")
{
    test::TempDir srcDir;
    test::TempDir dstDir;
    Harness h;

    const std::size_t kSize = 128u * 1024 * 1024;
    const auto content = makeContent(kSize);
    const auto srcPath = srcDir.utf8() + "/payload.bin";
    const auto dstPath = dstDir.utf8() + "/received.bin";
    REQUIRE(writeAllStore(h.storeA, srcPath, content));

    // **默认配置**，只定块大小——要量的是引擎默认值下的表现，
    // 不是某个调优组合。
    TransferPeer::Config cfg;
    cfg.chunkSize = 256u * 1024;

    connectPair(h, [&] {
        h.receiver = TransferPeer::createReceiver(h.ctx, h.server, h.storeB, h.recvPool, dstPath, 7, cfg,
                                                  [sink = h.receiverSink](const TransferResult& r) {
                                                      ++sink->calls;
                                                      sink->result = r;
                                                  });
        h.sender = TransferPeer::createSender(h.ctx, h.client, h.storeA, h.sendPool, srcPath, "payload.bin", "client-1", 7, cfg,
                                              [sink = h.senderSink](const TransferResult& r) {
                                                  ++sink->calls;
                                                  sink->result = r;
                                              });
    });

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(driveUntil(h, [&] { return h.senderSink->calls > 0 && h.receiverSink->calls > 0; }, std::chrono::seconds(180)));
    const auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE(h.senderSink->result.outcome == TransferOutcome::kCompleted);
    REQUIRE(h.receiverSink->result.outcome == TransferOutcome::kCompleted);

    const double mb = static_cast<double>(kSize) / (1024.0 * 1024.0);
    std::printf("\n[throughput] 直连(无中继) %.0f MB: %.2fs => %.1f MB/s\n", mb, dt, mb / dt);
    std::fflush(stdout);
}
