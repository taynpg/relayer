#include "relayer/core/net/ControlLink.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "relayer/core/protocol/Pairing.h"
#include "relayer/core/session/Session.h"
#include "support/IoPump.h"

using namespace relayer::core;
using namespace relayer::core::net;
using namespace relayer::protocol;
using relayer::test::pumpUntil;

// 这些用例跑真实 socket：要验证的正是"多个请求同时在飞、应答乱序返回时
// 还能各自配对"——纯字节测试抓不到顺序问题。
//
// 单独跑：core_test.exe "[controllink]"

namespace {

constexpr const char* kPassword = "correct horse battery staple";

session::SessionConfig serverConfig()
{
    session::SessionConfig c;
    c.role = session::Role::kServer;
    c.server.password = kPassword;
    c.serverId = "relayd";
    c.initialPeerId = "client-1";
    return c;
}

ControlLink::Config linkConfig(std::uint16_t port, std::chrono::seconds askTimeout = std::chrono::seconds(5))
{
    ControlLink::Config c;
    c.host = "127.0.0.1";
    c.port = port;
    c.password = kPassword;
    c.askTimeout = askTimeout;
    return c;
}

// 一个只负责"收下并攒着"的服务端：应答时机由用例决定，这样才能造出
// 乱序 / 不回 / 提前断等时序。
struct ServerStub {
    std::shared_ptr<session::Session> session;
    std::vector<Frame> received;

    void answer(const Frame& req, std::vector<char> body)
    {
        const auto type = answerForAsk(req.type);
        REQUIRE(type.has_value());
        Frame ans;
        ans.type = *type;
        ans.to = req.from;
        ans.index = req.index;   // 回显请求号：配对的唯一依据
        ans.body = std::move(body);
        (void)session->send(ans);
    }
};

std::uint16_t listen(asio::ip::tcp::acceptor& acc)
{
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    return acc.local_endpoint().port();
}

session::SessionHandler stubHandler(std::shared_ptr<ServerStub> stub)
{
    session::SessionHandler h;
    h.onAuthenticated = [stub](session::Session&, std::string) {};
    h.onFrame = [stub](session::Session&, const Frame& f) { stub->received.push_back(f); };
    h.onClosed = [](std::string, relayer::Status) {};
    return h;
}

std::string bodyText(const std::vector<char>& b)
{
    return std::string(b.begin(), b.end());
}

}   // namespace

TEST_CASE("控制连接认证后可用", "[controllink][integration]")
{
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto stub = std::make_shared<ServerStub>();
    acc.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        stub->session = session::Session::accept(ctx, std::move(sock), serverConfig(), stubHandler(stub));
    });

    ControlLink::Handler h;
    std::string readyId;
    int readyCount = 0;
    h.onReady = [&](std::string id) {
        ++readyCount;
        readyId = std::move(id);
    };

    auto link = ControlLink::connect(ctx, linkConfig(port), h);
    REQUIRE(link != nullptr);
    REQUIRE(pumpUntil(
        ctx, [&] { return link->ready() && stub->session && stub->session->authenticated(); }, std::chrono::seconds(5)));

    REQUIRE(link->assignedId() == "client-1");
    REQUIRE(readyId == "client-1");
    // Session 会通知两次（先空 id、后真实 id），ControlLink 原样转出。
    // 这里不钉死次数（那是 Session 的实现细节），只要求至少一次。
    REQUIRE(readyCount >= 1);
    REQUIRE(link->inFlight() == 0);
}

TEST_CASE("两个请求同时在飞，应答乱序返回也各自配对", "[controllink][integration]")
{
    // 这是 uuid 配对存在的**唯一理由**。没有它，两个请求只能串行；
    // 而串行会让"列目录 + 查元信息"这种天然并行的操作变成两倍往返。
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto stub = std::make_shared<ServerStub>();
    acc.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        stub->session = session::Session::accept(ctx, std::move(sock), serverConfig(), stubHandler(stub));
    });

    auto link = ControlLink::connect(ctx, linkConfig(port), {});
    REQUIRE(pumpUntil(
        ctx, [&] { return link->ready() && stub->session && stub->session->authenticated(); }, std::chrono::seconds(5)));

    std::string firstAnswer;
    std::string secondAnswer;
    bool firstOk = false;
    bool secondOk = false;
    REQUIRE(link->ask(FrameType::kAskFileList, "client-1", {'A'}, [&](const ControlLink::Answer& a) {
        firstOk = a.ok;
        firstAnswer = bodyText(a.body);
    }));
    REQUIRE(link->ask(FrameType::kAskSha256, "client-1", {'B'}, [&](const ControlLink::Answer& a) {
        secondOk = a.ok;
        secondAnswer = bodyText(a.body);
    }));
    REQUIRE(link->inFlight() == 2);

    REQUIRE(pumpUntil(ctx, [&] { return stub->received.size() == 2; }, std::chrono::seconds(5)));

    // 故意先回后到的那个。
    stub->answer(stub->received[1], {'b'});
    stub->answer(stub->received[0], {'a'});

    REQUIRE(pumpUntil(ctx, [&] { return !firstAnswer.empty() && !secondAnswer.empty(); }, std::chrono::seconds(5)));
    REQUIRE(firstOk);
    REQUIRE(secondOk);
    REQUIRE(firstAnswer == "a");   // 先发的那个拿到 'a'
    REQUIRE(secondAnswer == "b");
    REQUIRE(link->inFlight() == 0);
}

TEST_CASE("对端不回应答时按超时收尾", "[controllink][integration]")
{
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto stub = std::make_shared<ServerStub>();
    acc.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        stub->session = session::Session::accept(ctx, std::move(sock), serverConfig(), stubHandler(stub));
    });

    // 1 秒超时：用例要快，但也要留够认证的时间——所以先等到 ready 再发请求。
    auto link = ControlLink::connect(ctx, linkConfig(port, std::chrono::seconds(1)), {});
    REQUIRE(pumpUntil(
        ctx, [&] { return link->ready() && stub->session && stub->session->authenticated(); }, std::chrono::seconds(5)));

    bool done = false;
    bool ok = true;
    relayer::ErrorCode code = relayer::ErrorCode::kUnknown;
    REQUIRE(link->ask(FrameType::kAskFileList, "client-1", {}, [&](const ControlLink::Answer& a) {
        done = true;
        ok = a.ok;
        code = a.status.code;
    }));

    REQUIRE(pumpUntil(ctx, [&] { return done; }, std::chrono::seconds(3)));
    REQUIRE_FALSE(ok);
    REQUIRE(code == relayer::ErrorCode::kTimeout);
    REQUIRE(link->inFlight() == 0);

    // 服务端确实收到了请求——失败是因为它没回，不是因为没发出去。
    REQUIRE(stub->received.size() == 1);
}

TEST_CASE("应答类型与请求不匹配时立即失败", "[controllink][integration]")
{
    // 请求号对上、类型不对：说明对端回错了。让它挂到超时是最糟的处理——
    // 用户看到的是"网络慢"，而真实原因是协议实现有 bug。
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto stub = std::make_shared<ServerStub>();
    acc.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        stub->session = session::Session::accept(ctx, std::move(sock), serverConfig(), stubHandler(stub));
    });

    auto link = ControlLink::connect(ctx, linkConfig(port), {});
    REQUIRE(pumpUntil(
        ctx, [&] { return link->ready() && stub->session && stub->session->authenticated(); }, std::chrono::seconds(5)));

    bool done = false;
    bool ok = true;
    REQUIRE(link->ask(FrameType::kAskFileList, "client-1", {}, [&](const ControlLink::Answer& a) {
        done = true;
        ok = a.ok;
    }));
    REQUIRE(pumpUntil(ctx, [&] { return stub->received.size() == 1; }, std::chrono::seconds(5)));

    // 回一个类型不对的应答，但请求号是对的。
    Frame wrong;
    wrong.type = FrameType::kAnswerSha256;
    wrong.to = stub->received[0].from;
    wrong.index = stub->received[0].index;
    (void)stub->session->send(wrong);

    REQUIRE(pumpUntil(ctx, [&] { return done; }, std::chrono::seconds(3)));
    REQUIRE_FALSE(ok);
    REQUIRE(link->inFlight() == 0);
}

TEST_CASE("关闭连接时挂起的请求全部失败", "[controllink][integration]")
{
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto stub = std::make_shared<ServerStub>();
    acc.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        stub->session = session::Session::accept(ctx, std::move(sock), serverConfig(), stubHandler(stub));
    });

    auto link = ControlLink::connect(ctx, linkConfig(port), {});
    REQUIRE(pumpUntil(
        ctx, [&] { return link->ready() && stub->session && stub->session->authenticated(); }, std::chrono::seconds(5)));

    int failed = 0;
    int succeeded = 0;
    REQUIRE(
        link->ask(FrameType::kAskFileList, "client-1", {}, [&](const ControlLink::Answer& a) { a.ok ? ++succeeded : ++failed; }));
    REQUIRE(
        link->ask(FrameType::kAskSha256, "client-1", {}, [&](const ControlLink::Answer& a) { a.ok ? ++succeeded : ++failed; }));
    REQUIRE(link->inFlight() == 2);

    link->close();
    // close 是同步收尾的：调用返回时挂起的请求已经全部失败，不必再等事件循环。
    REQUIRE(failed == 2);
    REQUIRE(succeeded == 0);
    REQUIRE(link->inFlight() == 0);
    REQUIRE_FALSE(link->ready());
}

TEST_CASE("未就绪时发请求直接拒绝，不产生挂起项", "[controllink]")
{
    // 没有真实连接：connect 会失败，ready 永远是 false。
    // 这里只验证"没连上就别把请求塞进表里"——否则请求会挂在那里等一个
    // 永远不会来的应答，直到超时才报错。
    asio::io_context ctx;
    auto link = ControlLink::connect(ctx, linkConfig(1), {});   // 端口 1 必然连不上
    REQUIRE(link != nullptr);
    REQUIRE_FALSE(link->ready());

    bool called = false;
    REQUIRE_FALSE(link->ask(FrameType::kAskFileList, "client-1", {}, [&](const ControlLink::Answer&) { called = true; }));
    REQUIRE_FALSE(called);
    REQUIRE(link->inFlight() == 0);
}

TEST_CASE("非请求帧不能走 ask", "[controllink]")
{
    asio::io_context ctx;
    auto link = ControlLink::connect(ctx, linkConfig(1), {});
    // 数据帧 / 认证帧都不是"请求-应答"语义，传进来就是调用方用错了类型。
    REQUIRE_FALSE(link->ask(FrameType::kChunkRequestData, "x", {}, [](const ControlLink::Answer&) {}));
    REQUIRE_FALSE(link->ask(FrameType::kAuthChallenge, "x", {}, [](const ControlLink::Answer&) {}));
}
