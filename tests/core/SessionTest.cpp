#include "relayer/core/session/Session.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include "relayer/core/auth/Handshake.h"
#include "relayer/core/protocol/Frame.h"

using namespace relayer::core::session;
using namespace relayer::protocol;
using relayer::ErrorCode;
using relayer::Status;

// 这些测试跑真实 socket。会话层要验证的正是"回调顺序 + 相位推进 + 关闭
// 时机"，纯字节测试抓不到。
//
// 单独跑：core_test.exe "[session]"

namespace
{

constexpr const char *kPassword = "correct horse battery staple";

// 把 io_context 跑一段时间。
//
// 不用 run() / run_one()：它们在没有就绪任务但有挂起异步操作时会**阻塞**，
// 而测试里"连接还开着、断言已经做完"是常态。run_for 有明确上限，跑满即返回。
void pump(asio::io_context &ctx, std::chrono::milliseconds duration)
{
    ctx.restart();
    ctx.run_for(duration);
}

// 回环上的一对已连接 socket。测试"服务端单侧"行为时用，省掉连接建立。
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

SessionConfig serverConfig(bool allowNoAuth = false, std::string peerId = "client-1")
{
    SessionConfig c;
    c.role = Role::kServer;
    c.server.password = kPassword;
    c.server.allowNoAuth = allowNoAuth;
    c.serverId = "relayd";
    c.initialPeerId = std::move(peerId);
    return c;
}

SessionConfig clientConfig(const std::string &password = kPassword, bool noAuth = false)
{
    SessionConfig c;
    c.role = Role::kClient;
    c.clientPassword = password;
    c.server.allowNoAuth = noAuth;
    return c;
}

// 一次会话里的观测结果。用 shared_ptr 持有，因为回调会活到测试作用域之后。
struct Observation
{
    std::vector<Frame> frames;
    int authenticated = 0;
    std::string assignedId;
    int closed = 0;
    Status closeStatus;
    bool closeStatusSet = false;
};

SessionHandler makeHandler(const std::shared_ptr<Observation> &obs)
{
    SessionHandler h;
    h.onAuthenticated = [obs](std::string id)
    {
        ++obs->authenticated;
        obs->assignedId = std::move(id);
    };
    h.onFrame = [obs](const Frame &f) { obs->frames.push_back(f); };
    h.onClosed = [obs](Status s)
    {
        ++obs->closed;
        obs->closeStatus = std::move(s);
        obs->closeStatusSet = true;
    };
    return h;
}

Frame makeFrame(FrameType type, const std::string &from = {}, const std::string &to = {})
{
    Frame f;
    f.type = type;
    f.from = from;
    f.to = to;
    return f;
}

}  // namespace

// ===========================================================================
// 认证往返
// ===========================================================================

TEST_CASE("handshake succeeds over loopback", "[session][integration]")
{
    // 走两条不同入口：服务端 accept（socket 由 acceptor 交出），
    // 客户端 connect（自己建连）。不是拿两个 accept 假装是客户端。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    REQUIRE(client != nullptr);

    pump(ctx, std::chrono::seconds(3));

    REQUIRE(server != nullptr);
    REQUIRE(server->authenticated());
    REQUIRE(client->authenticated());

    // onAuthenticated 只该来一次。来两次会让上层把对端重复登记到 peer 表。
    REQUIRE(serverObs->authenticated == 1);
    REQUIRE(clientObs->authenticated == 1);

    // 服务端用配置里的 id；客户端还不知道自己的 id，此时为空。
    REQUIRE(serverObs->assignedId == "client-1");
    REQUIRE(clientObs->assignedId.empty());

    REQUIRE(serverObs->closed == 0);
    REQUIRE(clientObs->closed == 0);
}

TEST_CASE("wrong password fails fast with auth error", "[session][integration]")
{
    // 口令错时服务端回 kAuthAbort，客户端据此立即失败。
    //
    // 关键点：客户端不能"卡住"。没有 abort 帧的话，它只能等 30 秒握手
    // 超时——用户看到的是"连不上"，而不是"口令错了"。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig("wrong password"), makeHandler(clientObs));

    pump(ctx, std::chrono::seconds(3));

    REQUIRE(server != nullptr);
    REQUIRE_FALSE(client->authenticated());
    REQUIRE_FALSE(server->authenticated());

    // 两端都要结束，原因必须是认证失败——不能是超时，更不能是成功。
    REQUIRE(clientObs->closed == 1);
    REQUIRE(serverObs->closed == 1);
    REQUIRE(clientObs->closeStatus.code == ErrorCode::kAuthFailed);
    REQUIRE(serverObs->closeStatus.code == ErrorCode::kAuthFailed);

    // 而且不该走握手超时那条路（那条路会记 30 秒，测试只跑了 3 秒）
    REQUIRE(client->handshakeTimeouts() == 0);
    REQUIRE(server->handshakeTimeouts() == 0);
}

// ===========================================================================
// 业务帧
// ===========================================================================

TEST_CASE("business frames pass in both directions after auth", "[session][integration]")
{
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    pump(ctx, std::chrono::seconds(2));
    REQUIRE(client->authenticated());
    REQUIRE(server->authenticated());

    // 客户端 -> 服务端
    REQUIRE(client->send(makeFrame(FrameType::kAskId, "client-1")).ok());
    pump(ctx, std::chrono::milliseconds(500));
    REQUIRE(serverObs->frames.size() == 1);
    REQUIRE(serverObs->frames[0].type == FrameType::kAskId);

    // 服务端 -> 客户端
    REQUIRE(server->send(makeFrame(FrameType::kAnswerId, {}, "client-1")).ok());
    pump(ctx, std::chrono::milliseconds(500));
    REQUIRE(clientObs->frames.size() == 1);
    REQUIRE(clientObs->frames[0].type == FrameType::kAnswerId);
}

TEST_CASE("send before auth is refused locally", "[session][integration]")
{
    // 未认证就发业务帧是上层调用顺序错了。必须在本地挡住，不能发出去让
    // 对端断开——错误发生在本地就在本地报，这样日志能指到真正的调用点。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto server = Session::accept(ctx, std::move(pair.a), serverConfig(), makeHandler(obs));

    const auto st = server->send(makeFrame(FrameType::kAskFileList));
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kAuthFailed);
}

// ===========================================================================
// 转发与身份绑定
// ===========================================================================

TEST_CASE("forward rewrites a forged from to the authenticated id", "[session][integration][identity]")
{
    // v1 伪造漏洞的端到端回归：客户端自称 alice，服务端认证身份是
    // client-1，转发后 from 必须是 client-1 而不是 alice。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    pump(ctx, std::chrono::seconds(2));
    REQUIRE(server->authenticated());

    Frame f = makeFrame(FrameType::kFileRequestSend, "alice", "client-9");
    const auto fwdSt = server->forward(f, server->assignedId());
    INFO("forward failed: " << fwdSt.message << " / assignedId=" << server->assignedId());
    REQUIRE(fwdSt.ok());

    pump(ctx, std::chrono::milliseconds(500));
    REQUIRE(clientObs->frames.size() == 1);
    REQUIRE(clientObs->frames[0].from == "client-1");
    REQUIRE(clientObs->frames[0].to == "client-9");
}

TEST_CASE("server cannot send a frame claiming a third party", "[session][integration][identity]")
{
    // send() 的出站断言：服务端直接发 from="alice" 必须被拒。
    // 这是"转发时该调 forward()、结果调了 send()"这类错误的兜底。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    pump(ctx, std::chrono::seconds(2));
    REQUIRE(server->authenticated());

    const auto before = server->framesRejected();
    const auto st = server->send(makeFrame(FrameType::kAskFileList, "alice", "client-9"));
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kProtocol);
    REQUIRE(server->framesRejected() == before + 1);

    // 关键：拒绝之后连接**不该**断开——这是本地 bug，对端没做错事。
    // 断开只会把问题推给对端，让排查方向跑偏。
    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(serverObs->closed == 0);
    REQUIRE(server->authenticated());
}

TEST_CASE("forward and send have disjoint identity rules", "[session][integration][identity]")
{
    // 这两个入口的合法 from 集合**互不相交**：
    //
    //   send()    → 空，或服务端自己的 id
    //   forward() → 恰好是发送者的认证身份
    //
    // 合并成一个的话，必然有一边被误拒：转发帧的 from 是别人的 id，
    // 正好落在 send() 的禁止集合里。这个冲突在实现时真出现过，所以留一条
    // 测试钉住，防止后人"顺手把两个入口合并"。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    pump(ctx, std::chrono::seconds(2));
    REQUIRE(server->authenticated());

    // 同一个 from="client-1"：send() 拒绝，forward() 接受。
    const auto viaSend = server->send(makeFrame(FrameType::kAskFileList, "client-1", "client-9"));
    REQUIRE_FALSE(viaSend.ok());

    const auto viaForward = server->forward(makeFrame(FrameType::kAskFileList, "alice", "client-9"), "client-1");
    REQUIRE(viaForward.ok());

    pump(ctx, std::chrono::milliseconds(500));
    REQUIRE(clientObs->frames.size() == 1);
    REQUIRE(clientObs->frames[0].from == "client-1");
}

TEST_CASE("forward without an authenticated identity is refused", "[session][integration][identity]")
{
    // 上游若传空 id（表示"此人身份未定"），转发必须失败而不是原样发出。
    // 原样发出的后果是把对方的自称身份直接送到了第三方。
    asio::io_context ctx;

    asio::ip::tcp::acceptor acc(ctx);
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    const auto port = acc.local_endpoint().port();

    auto serverObs = std::make_shared<Observation>();
    auto clientObs = std::make_shared<Observation>();

    std::shared_ptr<Session> server;
    acc.async_accept(
        [&](const asio::error_code &ec, asio::ip::tcp::socket sock)
        {
            if (ec)
            {
                return;
            }
            server = Session::accept(ctx, std::move(sock), serverConfig(), makeHandler(serverObs));
        });

    auto client = Session::connect(ctx, "127.0.0.1", port, clientConfig(), makeHandler(clientObs));
    pump(ctx, std::chrono::seconds(2));
    REQUIRE(server->authenticated());

    const auto st = server->forward(makeFrame(FrameType::kAskFileList, "alice", "client-9"), "");
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == ErrorCode::kProtocol);

    pump(ctx, std::chrono::milliseconds(300));
    REQUIRE(clientObs->frames.empty());
}

// ===========================================================================
// 未认证的业务帧
// ===========================================================================

TEST_CASE("unauthenticated business frame closes the session", "[session][integration]")
{
    // 服务端还在等 Response，对端却发来业务帧 → 相位不一致，必须断开。
    //
    // 用裸 socket 手工构造：Session 对 Session 走不到这个状态，因为
    // 客户端的 send() 自己就会拒绝未认证发送。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto server = Session::accept(ctx, std::move(pair.a), serverConfig(), makeHandler(obs));

    // 先跑一小段，让服务端发出 Challenge
    pump(ctx, std::chrono::milliseconds(100));

    Frame bad = makeFrame(FrameType::kAskFileList, "x", "relayd");
    const auto wire = packFrame(bad);
    REQUIRE_FALSE(wire.empty());
    asio::write(pair.b, asio::buffer(wire));

    pump(ctx, std::chrono::milliseconds(500));

    REQUIRE(obs->closed == 1);
    REQUIRE(obs->closeStatus.code == ErrorCode::kProtocol);
    REQUIRE(server->framesRejected() == 1);
}

// ===========================================================================
// 心跳
// ===========================================================================

TEST_CASE("heartbeat ask is answered before auth", "[session][integration][heartbeat]")
{
    // PROTOCOL.md §11.2：心跳检测的是 TCP 活性、与身份无关，未认证也应答。
    //
    // 这个行为把"TCP 断了"和"TCP 通但握手卡住"区分开——两种故障的排查
    // 方向完全相反。用裸 socket 在握手未完成时发一个 kAskHeart，验证能收回
    // 一个 kAnswerHeart。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto server = Session::accept(ctx, std::move(pair.a), serverConfig(), makeHandler(obs));

    // 先让服务端发出 Challenge，并把它从 socket 里读掉——否则下面读到的是
    // Challenge 而不是心跳应答。
    pump(ctx, std::chrono::milliseconds(100));
    std::vector<char> scratch(4096);
    asio::error_code ec;
    pair.b.non_blocking(true);
    const auto n = pair.b.read_some(asio::buffer(scratch), ec);
    REQUIRE(n > 0);
    pair.b.non_blocking(false);

    Frame ask;
    ask.type = FrameType::kAskHeart;
    const auto wire = packFrame(ask);
    REQUIRE_FALSE(wire.empty());
    asio::write(pair.b, asio::buffer(wire));

    pump(ctx, std::chrono::milliseconds(300));

    // 会话没被断开，而且发出了应答
    REQUIRE(obs->closed == 0);
    REQUIRE_FALSE(server->authenticated());
    REQUIRE(server->framesOut() == 2);  // Challenge + AnswerHeart
}

TEST_CASE("session does not ping while idle clock is fresh", "[session][integration][heartbeat]")
{
    // 会话把策略结论翻译成动作这一层。策略本身的时序由 HeartbeatTest 覆盖，
    // 这里只验证"刚连上不该立刻发心跳"。
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto cfg = serverConfig();
    cfg.handshakeTimeout = std::chrono::seconds(0);  // 关掉握手超时，单独看心跳
    auto server = Session::accept(ctx, std::move(pair.a), cfg, makeHandler(obs));
    pump(ctx, std::chrono::milliseconds(100));

    const auto framesBefore = server->framesOut();
    server->pollHeartbeatNow();
    REQUIRE(server->framesOut() == framesBefore);
    REQUIRE(server->heartbeatTimeouts() == 0);
    REQUIRE(obs->closed == 0);
}

// ===========================================================================
// 关闭
// ===========================================================================

TEST_CASE("close notifies exactly once", "[session][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto server = Session::accept(ctx, std::move(pair.a), serverConfig(), makeHandler(obs));
    pump(ctx, std::chrono::milliseconds(100));

    server->close();
    server->close();  // 幂等
    pump(ctx, std::chrono::milliseconds(300));

    // 只通知一次。上方 peer 表之类的资源释放依赖这个承诺。
    REQUIRE(obs->closed == 1);
    REQUIRE(obs->closeStatus.ok());
}

TEST_CASE("peer disconnect surfaces as a closed session", "[session][integration]")
{
    asio::io_context ctx;
    auto pair = makeSocketPair(ctx);

    auto obs = std::make_shared<Observation>();
    auto server = Session::accept(ctx, std::move(pair.a), serverConfig(), makeHandler(obs));
    pump(ctx, std::chrono::milliseconds(100));

    asio::error_code ignored;
    pair.b.close(ignored);

    pump(ctx, std::chrono::milliseconds(500));

    REQUIRE(obs->closed == 1);
    // 对端正常关闭（EOF）不是错误
    REQUIRE(obs->closeStatus.ok());
}
