#include "relayer/core/transport/Connection.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>

#include "relayer/core/protocol/Frame.h"

using namespace relayer::core::transport;
using namespace relayer::protocol;
using relayer::Status;

// 这些测试用回环地址跑真实 socket。
//
// 它们比纯字节测试慢（每条几十毫秒），但能抓到 FrameReader 测试抓不到的
// 问题：回调顺序、close 与回调的竞态、发送队列串行化。这些恰恰是 v1 里
// 最难排查的一类 bug。
//
// 标签 [integration] 让 CI 的快速通道可以选择跳过。

namespace
{

// 把 io_context 跑一段时间。
//
// 为什么不用 run() 或 run_one() 循环：
//
//   asio 的 run_one() 在"没有任何就绪任务、但有挂起的异步操作"（比如一个
//   还在等的 async_read）时会**阻塞等待**，而不是返回 0。测试里经常出现
//   "连接还开着、断言已经做完"的情况，那时候 run_one() 会永久阻塞。
//
//   run_for 有明确的上限，跑满即返回。测试挂死不给任何信息，比失败更糟。
void pump(asio::io_context &ctx, std::chrono::milliseconds duration)
{
    ctx.restart();
    ctx.run_for(duration);
}

// 回环上的一个简单 acceptor，返回监听的端口。
std::uint16_t listenOn(asio::ip::tcp::acceptor &acc)
{
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));  // 0 = 让系统分配
    acc.listen();
    return acc.local_endpoint().port();
}

// 构造 handler 的辅助。
//
// 不用指定初始化器（Handler{.onFrame = ..., .onClosed = ...}）：那是 C++20
// 的语法，本项目用 C++17。设计一个按位置传参的小函数比到处写三行赋值清楚。
ConnectionHandler handler(std::function<void(const Frame &)> onFrame, std::function<void(Status)> onClosed)
{
    ConnectionHandler h;
    h.onFrame = std::move(onFrame);
    h.onClosed = std::move(onClosed);
    return h;
}

// 只关心收到什么、不关心关闭的常见组合

Frame makeFrame(FrameType type, const std::string &body)
{
    Frame f;
    f.type = type;
    f.from = "sender";
    f.to = "receiver";
    f.body.assign(body.begin(), body.end());
    return f;
}

}  // namespace

// ===========================================================================
// 显式双端：用 adopt 两端，直接验证收发
// ===========================================================================
//
// 这一组绕开 connect()，直接用已经配好对的 socket。connect() 的异步建连
// 路径另有专门测试（见文件末尾的"建连失败路径"与"connect 成功路径"）。

namespace
{

// 一对已连好的 socket。用同步 connect/accept 建立，然后各交给 Connection。
struct SocketPair
{
    asio::ip::tcp::socket a;
    asio::ip::tcp::socket b;
};

SocketPair makeSocketPair(asio::io_context &ctx)
{
    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();

    asio::ip::tcp::socket client(ctx);
    client.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), acc.local_endpoint().port()));

    asio::ip::tcp::socket server(ctx);
    acc.accept(server);

    return SocketPair{std::move(client), std::move(server)};
}

}  // namespace

TEST_CASE("two connections exchange one frame", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    std::vector<Frame> got;
    bool closedCalled = false;

    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    auto b =
        Connection::adopt(ctx, std::move(pair.b),
                          handler([&](const Frame &f) { got.push_back(f); }, [&](Status) { closedCalled = true; }));

    REQUIRE(b->state() == ConnectionState::kOpen);

    const auto st = a->send(makeFrame(FrameType::kAskId, "hello from a"));
    REQUIRE(st.ok());

    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].type == FrameType::kAskId);
    REQUIRE(std::string(got[0].body.begin(), got[0].body.end()) == "hello from a");
    REQUIRE(a->framesSent() == 1);
    REQUIRE_FALSE(closedCalled);
}

TEST_CASE("connection preserves frame order", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    std::vector<std::int64_t> indices;

    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    auto b = Connection::adopt(ctx, std::move(pair.b),
                               handler([&](const Frame &f) { indices.push_back(f.index); }, [](Status) {}));

    // 连发 50 帧，index 各不相同
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i)
    {
        Frame f = makeFrame(FrameType::kChunkRequestData, "x");
        f.index = i;
        REQUIRE(a->send(f).ok());
    }

    pump(ctx, std::chrono::seconds(2));
    REQUIRE(indices.size() == kCount);
    // 顺序必须与发送顺序完全一致：TCP 保证字节序，而"逐帧写入"的队列
    // 保证帧边界不被交错。任何一处实现错误（比如并发 async_write）
    // 都会在这里暴露成乱序或解析失败。
    for (int i = 0; i < kCount; ++i)
    {
        REQUIRE(indices[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("connection carries a large body", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    std::vector<char> gotBody;
    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    auto b =
        Connection::adopt(ctx, std::move(pair.b), handler([&](const Frame &f) { gotBody = f.body; }, [](Status) {}));

    // 1MB，跨越多个 readChunk（默认 4KB）
    std::string body(1024 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        body[i] = static_cast<char>(i % 253);
    }
    REQUIRE(a->send(makeFrame(FrameType::kAskFileSamples, body)).ok());
    pump(ctx, std::chrono::seconds(5));

    REQUIRE(gotBody.size() == body.size());
    REQUIRE(std::equal(gotBody.begin(), gotBody.end(), body.begin()));
    REQUIRE(a->bytesSent() > body.size());
    REQUIRE(b->bytesReceived() > body.size());
}

TEST_CASE("bidirectional exchange works", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    std::vector<FrameType> aGot;
    std::vector<FrameType> bGot;

    auto a = Connection::adopt(ctx, std::move(pair.a),
                               handler([&](const Frame &f) { aGot.push_back(f.type); }, [](Status) {}));
    auto b = Connection::adopt(ctx, std::move(pair.b),
                               handler([&](const Frame &f) { bGot.push_back(f.type); }, [](Status) {}));

    REQUIRE(a->send(makeFrame(FrameType::kAskId, "1")).ok());
    REQUIRE(b->send(makeFrame(FrameType::kAnswerId, "2")).ok());
    REQUIRE(a->send(makeFrame(FrameType::kAskHome, "3")).ok());

    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(bGot.size() == 2);
    REQUIRE(bGot[0] == FrameType::kAskId);
    REQUIRE(bGot[1] == FrameType::kAskHome);
    REQUIRE(aGot.size() == 1);
    REQUIRE(aGot[0] == FrameType::kAnswerId);
}

// ===========================================================================
// 关闭语义
// ===========================================================================

TEST_CASE("closing one side notifies the other", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    int aClosed = 0;
    int bClosed = 0;
    Status aReason;
    Status bReason;

    auto a = Connection::adopt(ctx, std::move(pair.a),
                               handler([](const Frame &) {},
                                       [&](Status s)
                                       {
                                           ++aClosed;
                                           aReason = std::move(s);
                                       }));
    auto b = Connection::adopt(ctx, std::move(pair.b),
                               handler([](const Frame &) {},
                                       [&](Status s)
                                       {
                                           ++bClosed;
                                           bReason = std::move(s);
                                       }));

    a->close();
    REQUIRE(a->state() == ConnectionState::kClosed);

    pump(ctx, std::chrono::milliseconds(300));

    // 本地主动关：双方都收到一次通知
    REQUIRE(aClosed == 1);
    REQUIRE(bClosed == 1);
    REQUIRE(aReason.ok());  // 主动关闭不是错误
}

TEST_CASE("onClosed is called exactly once", "[transport][connection][integration]")
{
    // 这是本层对上层唯一的强承诺。重复通知会让上层对同一份资源释放两次。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    int bClosed = 0;
    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    auto b = Connection::adopt(ctx, std::move(pair.b), handler([](const Frame &) {}, [&](Status) { ++bClosed; }));

    // 触发多路关闭：先本地 close，再让它自然断开
    b->close();
    b->close();
    b->close();

    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(bClosed == 1);

    // 再看对端关闭时的重复
    a->close();
    a->close();
    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(bClosed == 1);  // 没有因为 a 的关闭再来一次
}

TEST_CASE("send after close fails", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    a->close();

    const auto st = a->send(makeFrame(FrameType::kAskId, "x"));
    REQUIRE_FALSE(st.ok());
}

TEST_CASE("peer disconnect is observed", "[transport][connection][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    int closedCount = 0;
    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [&](Status) { ++closedCount; }));
    {
        auto b = Connection::adopt(ctx, std::move(pair.b), handler([](const Frame &) {}, [](Status) {}));
        b->close();
    }  // b 析构

    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(closedCount == 1);
    REQUIRE(a->state() == ConnectionState::kClosed);
}

// ===========================================================================
// 非法帧
// ===========================================================================

TEST_CASE("sending an oversized field is rejected without closing", "[transport][connection][integration]")
{
    // 字段超长是调用方的 bug，不是连接的错。连接应当保持可用。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    bool closed = false;
    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [&](Status) { closed = true; }));

    Frame bad = makeFrame(FrameType::kAskId, "x");
    bad.from = std::string(300, 'f');  // 超过 kMaxRouteField(255)

    const auto st = a->send(bad);
    REQUIRE_FALSE(st.ok());
    REQUIRE(closed == false);
    REQUIRE(a->state() == ConnectionState::kOpen);

    // 连接仍然可以正常发帧
    REQUIRE(a->send(makeFrame(FrameType::kAskId, "ok")).ok());
    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(a->framesSent() == 1);  // 只算成功的那一次
}

// ===========================================================================
// 建连路径
// ===========================================================================

TEST_CASE("connect succeeds and can send the first frame immediately", "[transport][connection][integration]")
{
    // 这条测试覆盖"客户端拿到 Connection 指针就能 send"这个契约：
    // 第一帧通常就是握手帧，调用方不应该需要先等一个"连上了"的回调。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    const std::uint16_t port = listenOn(acc);

    std::vector<FrameType> serverGot;
    Status serverCloseReason;
    int serverClosed = 0;
    std::shared_ptr<Connection> serverConn;  // 在外面持有，让服务端连接活到测试结束

    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            REQUIRE_FALSE(ec);
            ConnectionHandler h;
            h.onFrame = [&](const Frame &f) { serverGot.push_back(f.type); };
            h.onClosed = [&](Status s)
            {
                serverCloseReason = std::move(s);
                ++serverClosed;
            };
            serverConn = Connection::adopt(ctx, std::move(sock), std::move(h));
        });

    bool clientClosed = false;
    auto client = Connection::connect(ctx, "127.0.0.1", port,
                                      handler([](const Frame &) {}, [&](Status) { clientClosed = true; }));
    REQUIRE(client != nullptr);

    // 立刻发——此时还没连上，帧应当进队列
    REQUIRE(client->send(makeFrame(FrameType::kAuthResponse, "early")).ok());

    pump(ctx, std::chrono::seconds(2));
    REQUIRE(serverGot.size() == 1);
    REQUIRE(serverGot[0] == FrameType::kAuthResponse);
    REQUIRE_FALSE(clientClosed);
    REQUIRE(client->state() == ConnectionState::kOpen);
}

TEST_CASE("connecting to a closed port reports failure", "[transport][connection][integration]")
{
    asio::io_context ctx;

    bool closedCalled = false;
    Status reason;

    // 先占一个端口再释放，拿到一个大概率没人监听的端口号
    std::uint16_t port = 0;
    {
        asio::ip::tcp::acceptor tmp(ctx);
        port = listenOn(tmp);
    }

    // 保留返回的指针：Connection 内部用 shared_from_this 维持存活，
    // 但外部若一个引用都不留，连接对象会在建连完成前被销毁。
    auto conn = Connection::connect(ctx, "127.0.0.1", port,
                                    handler([](const Frame &) {},
                                            [&](Status s)
                                            {
                                                closedCalled = true;
                                                reason = std::move(s);
                                            }));
    REQUIRE(conn != nullptr);

    // Windows 上连接被拒之前会先重试几次（SYN 重传），2 秒不够。
    // 这是系统的行为，不是实现的问题，所以测试要给足时间。
    pump(ctx, std::chrono::seconds(8));
    REQUIRE(closedCalled);
    REQUIRE_FALSE(reason.ok());
}

TEST_CASE("resolving a bogus host reports failure", "[transport][connection][integration]")
{
    asio::io_context ctx;

    bool closedCalled = false;
    Status reason;

    auto conn = Connection::connect(ctx, "this-host-does-not-exist.invalid", 12345,
                                    handler([](const Frame &) {},
                                            [&](Status s)
                                            {
                                                closedCalled = true;
                                                reason = std::move(s);
                                            }));
    REQUIRE(conn != nullptr);

    pump(ctx, std::chrono::seconds(5));
    REQUIRE(closedCalled);
    REQUIRE_FALSE(reason.ok());
}

// ===========================================================================
// 连接就绪通知
// ===========================================================================

TEST_CASE("adopt reports established before any frame", "[transport][connection][integration]")
{
    // 顺序断言是这条测试的重点。
    //
    // 会话层要靠 onEstablished 启动握手状态机（服务端发 Challenge）。
    // 如果 onEstablished 来的比第一帧晚，服务端就会在"还没开始握手"的
    // 状态里收到 Response，直接被判相位错误；如果干脆不来，握手永远不启动。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    std::vector<std::string> events;

    ConnectionHandler hb;
    hb.onEstablished = [&] { events.emplace_back("established"); };
    hb.onFrame = [&](const Frame &) { events.emplace_back("frame"); };
    hb.onClosed = [&](Status) { events.emplace_back("closed"); };

    auto a = Connection::adopt(ctx, std::move(pair.a), handler([](const Frame &) {}, [](Status) {}));
    auto b = Connection::adopt(ctx, std::move(pair.b), hb);

    // adopt 里投递的是异步通知，所以此刻还没跑过回调
    REQUIRE(events.empty());

    REQUIRE(a->send(makeFrame(FrameType::kAskId, "x")).ok());
    pump(ctx, std::chrono::milliseconds(500));

    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "established");
    REQUIRE(events[1] == "frame");
}

TEST_CASE("connect reports established once connect succeeds", "[transport][connection][integration]")
{
    // 客户端侧：建连是异步的，onEstablished 必须等真的连上才来。
    // 而且它不能重复来——重复会让会话层启动两次握手。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    int establishedCount = 0;
    bool closedCalled = false;

    ConnectionHandler hc;
    hc.onEstablished = [&] { ++establishedCount; };
    hc.onClosed = [&](Status) { closedCalled = true; };

    auto client = Connection::connect(ctx, "127.0.0.1", port, hc);
    REQUIRE(client != nullptr);

    // 建连还没完成时不该有任何通知
    REQUIRE(establishedCount == 0);

    // acceptor 侧必须真的 accept，否则三次握手只能走内核 backlog。
    // 把 socket 移动进回调（asio 1.38 的定型签名要求）——留下一个已移动的
    // 局部变量只会掩盖"accept 到底有没有成功"这件事。
    //
    // 连接要保留住不能析构：服务端 socket 一旦关闭，客户端立刻看到 EOF 并
    // 正常关闭，我们就没法区分"连接健康"和"连接已经结束"了。
    bool accepted = false;
    std::shared_ptr<asio::ip::tcp::socket> serverSock;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (!ec && sock.is_open())
            {
                accepted = true;
                serverSock = std::make_shared<asio::ip::tcp::socket>(std::move(sock));
            }
        });

    pump(ctx, std::chrono::seconds(2));

    REQUIRE(accepted);
    REQUIRE(establishedCount == 1);
    REQUIRE_FALSE(closedCalled);
    REQUIRE(client->state() == ConnectionState::kOpen);

    // 服务端主动断开，客户端才应该关闭
    serverSock->close();
    pump(ctx, std::chrono::seconds(1));
    REQUIRE(closedCalled);
}

// ===========================================================================
// 状态名称
// ===========================================================================

TEST_CASE("connection state names are stable", "[transport][connection]")
{
    REQUIRE(toString(ConnectionState::kConnecting) == "connecting");
    REQUIRE(toString(ConnectionState::kOpen) == "open");
    REQUIRE(toString(ConnectionState::kClosing) == "closing");
    REQUIRE(toString(ConnectionState::kClosed) == "closed");
}
