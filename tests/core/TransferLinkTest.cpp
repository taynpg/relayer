#include "relayer/core/net/TransferLink.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "relayer/core/session/Session.h"
#include "relayer/filestore/StdFileStore.h"
#include "support/IoPump.h"
#include "support/TempDir.h"

using namespace relayer;
using namespace relayer::core;
using namespace relayer::core::net;
using namespace relayer::core::session;
using relayer::protocol::Frame;
using relayer::test::pumpUntil;

// 这些用例跑真实 socket，而且刻意**起一个迷你的"中继"**：两个 TransferLink
// 各自建连、互相看不见，帧必须经过中间那一跳才能到达对方——这正是双连接
// 的真实形态（两条 TCP，各自一个 id）。如果只用两条直连的 Session 对接，
// "按 id 路由"这一层就被绕过去了，测不到真东西。
//
// 单独跑：core_test.exe "[transferlink]"

namespace {

constexpr const char* kPassword = "correct horse battery staple";
// 与 pumpUntil 的 timeout 参数同类型（milliseconds）：跨精度用 constexpr
// 常量传参时，宏展开里那一步隐式转换会失败，报出来的却是 lambda 无法
// 转成 std::function——离真正的原因很远。
constexpr std::chrono::milliseconds kWait{15000};

SessionConfig relayServerConfig(std::string peerId)
{
    SessionConfig c;
    c.role = Role::kServer;
    c.server.password = kPassword;
    c.serverId = "relayd";
    c.initialPeerId = std::move(peerId);
    return c;
}

TransferLink::Config linkConfig(std::uint16_t port, std::filesystem::path receiveDir = {})
{
    TransferLink::Config c;
    c.host = "127.0.0.1";
    c.port = port;
    c.password = kPassword;
    c.receiveDir = receiveDir.string();
    c.chunkSize = 4096;   // 小到能切出多块
    return c;
}

// 迷你中继：接受两条连接，按来源把帧转给另一条，并把 from 绑定成来源的 id。
//
// 真实的 relayerd 按 `to` 找连接；这里只有两条连接，所以"另一条"就是答案。
// 保留的语义是**身份绑定**——不绑的话接收方算不出回信地址（TransferPeer
// 用 frame.from 当 to）。
struct MiniRelay {
    std::shared_ptr<Session> a;
    std::shared_ptr<Session> b;
    int accepted = 0;
};

SessionHandler relayHandler(const std::shared_ptr<MiniRelay>& relay, bool isFirst)
{
    SessionHandler h;
    h.onAuthenticated = [](Session&, std::string) {};
    h.onFrame = [relay, isFirst](Session& self, const Frame& f) {
        const auto& other = isFirst ? relay->b : relay->a;
        if (!other || other->finished()) {
            return;
        }
        // 必须走 forward()：服务端角色的会话不允许 send() 带第三方 from
        // （checkOutbound 会拒），而"把 A 的帧发给 B"恰恰就是这件事。
        // forward 会做身份绑定，与 relayerd 的转发路径完全一致。
        Frame g = f;
        (void)other->forward(std::move(g), self.assignedId());
    };
    h.onClosed = [](std::string, Status) {};
    return h;
}

void startRelay(asio::io_context& ctx, asio::ip::tcp::acceptor& acc, const std::shared_ptr<MiniRelay>& relay)
{
    acc.async_accept([&ctx, &acc, relay](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        if (ec) {
            return;
        }
        const bool first = (relay->accepted == 0);
        ++relay->accepted;
        auto s = Session::accept(ctx, std::move(sock), relayServerConfig(first ? "dataA" : "dataB"), relayHandler(relay, first));
        if (first) {
            relay->a = s;
        } else {
            relay->b = s;
        }
        if (relay->accepted < 2) {
            startRelay(ctx, acc, relay);
        }
    });
}

std::uint16_t listen(asio::ip::tcp::acceptor& acc)
{
    acc.open(asio::ip::tcp::v4());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    acc.listen();
    return acc.local_endpoint().port();
}

std::string makeContent(std::size_t len, unsigned seed = 0)
{
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>((i * 131 + seed * 17 + 7) & 0xFF);
    }
    return s;
}

void writeFile(const std::filesystem::path& p, const std::string& content)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}   // namespace

TEST_CASE("双连接：文件从 A 的数据连接传到 B 的数据连接", "[transferlink][integration]")
{
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto relay = std::make_shared<MiniRelay>();
    startRelay(ctx, acc, relay);

    relayer::test::TempDir tmp;
    const auto root = tmp.native();
    const auto recvDir = root / "recv";
    const auto srcDir = root / "src";
    std::filesystem::create_directories(recvDir);
    std::filesystem::create_directories(srcDir);

    const std::string content = makeContent(200000, 3);   // 约 49 块
    const auto srcFile = srcDir / "payload.bin";
    writeFile(srcFile, content);

    bool bDone = false;
    transfer::TransferResult bResult;
    TransferLink::Handler hb;
    hb.onFinished = [&](const TransferLink::JobInfo&, const transfer::TransferResult& r) {
        bResult = r;
        bDone = true;
    };
    filestore::StdFileStore storeB;
    auto linkB = TransferLink::connect(ctx, linkConfig(port, recvDir), storeB, hb);

    bool aDone = false;
    transfer::TransferResult aResult;
    TransferLink::Handler ha;
    ha.onFinished = [&](const TransferLink::JobInfo&, const transfer::TransferResult& r) {
        aResult = r;
        aDone = true;
    };
    filestore::StdFileStore storeA;
    auto linkA = TransferLink::connect(ctx, linkConfig(port), storeA, ha);

    REQUIRE(pumpUntil(ctx, [&] { return linkA->ready() && linkB->ready(); }, kWait));
    REQUIRE_FALSE(linkA->assignedId().empty());
    REQUIRE_FALSE(linkB->assignedId().empty());
    REQUIRE(linkA->assignedId() != linkB->assignedId());

    // toPeerId 填 B 的**数据连接 id**——这是 §11.13 协商出来的那个值。
    const auto jobId = linkA->sendFile(srcFile.string(), "sub/payload.bin", linkB->assignedId());
    REQUIRE(jobId != 0);

    REQUIRE(pumpUntil(ctx, [&] { return aDone && bDone; }, kWait));

    REQUIRE(aResult.outcome == transfer::TransferOutcome::kCompleted);
    REQUIRE(bResult.outcome == transfer::TransferOutcome::kCompleted);

    // 落点由发送方的描述（desc.root = "sub/payload.bin"）决定，落在 B 的
    // 接收目录之下。
    const auto dst = recvDir / "sub" / "payload.bin";
    REQUIRE(std::filesystem::exists(dst));
    REQUIRE(readFile(dst) == content);
}

TEST_CASE("取消：在跑的传输以 kLocalCancelled 收尾，不是静默消失", "[transferlink][integration]")
{
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto relay = std::make_shared<MiniRelay>();
    startRelay(ctx, acc, relay);

    relayer::test::TempDir tmp;
    const auto root = tmp.native();
    const auto recvDir = root / "recv";
    std::filesystem::create_directories(recvDir);

    // 8 MB / 4 KiB = 2000 块，足够在取消之前跑不完。
    const std::string content = makeContent(8u * 1024 * 1024);
    const auto srcFile = tmp.native() / "big.bin";
    writeFile(srcFile, content);

    bool aDone = false;
    transfer::TransferResult aResult;
    TransferLink::Handler ha;
    ha.onFinished = [&](const TransferLink::JobInfo&, const transfer::TransferResult& r) {
        aResult = r;
        aDone = true;
    };

    TransferLink::Handler hb;
    filestore::StdFileStore storeB;
    auto linkB = TransferLink::connect(ctx, linkConfig(port, recvDir), storeB, hb);
    filestore::StdFileStore storeA;
    auto linkA = TransferLink::connect(ctx, linkConfig(port), storeA, ha);

    REQUIRE(pumpUntil(ctx, [&] { return linkA->ready() && linkB->ready(); }, kWait));

    const auto jobId = linkA->sendFile(srcFile.string(), "big.bin", linkB->assignedId());
    REQUIRE(jobId != 0);

    // 等它真的开始（否则可能在还没发第一块时就取消，测不到"中途取消"）。
    REQUIRE(pumpUntil(
        ctx,
        [&] {
            const auto jobs = linkA->activeJobs();
            return jobs > 0 && !linkB->assignedId().empty();
        },
        kWait));
    relayer::test::pump(ctx, std::chrono::milliseconds(50));

    REQUIRE(linkA->cancel(jobId));
    REQUIRE(pumpUntil(ctx, [&] { return aDone; }, kWait));
    REQUIRE(aResult.outcome == transfer::TransferOutcome::kLocalCancelled);

    // 界面靠这个把行标成"已取消"而不是一直"传输中"。
    REQUIRE(linkA->activeJobs() == 0);
}

TEST_CASE("数据连接关闭时，在跑的传输有结论", "[transferlink][integration]")
{
    // 与上一条的区别：不是"用户点了取消"，而是"连接没了"。上层必须能收到
    // 一个结论，否则进度条永远停在原地。
    asio::io_context ctx;
    asio::ip::tcp::acceptor acc(ctx);
    const auto port = listen(acc);

    auto relay = std::make_shared<MiniRelay>();
    startRelay(ctx, acc, relay);

    relayer::test::TempDir tmp;
    const auto root = tmp.native();
    const auto recvDir = root / "recv";
    std::filesystem::create_directories(recvDir);

    const std::string content = makeContent(8u * 1024 * 1024);
    const auto srcFile = tmp.native() / "big.bin";
    writeFile(srcFile, content);

    bool aDone = false;
    transfer::TransferResult aResult;
    TransferLink::Handler ha;
    ha.onFinished = [&](const TransferLink::JobInfo&, const transfer::TransferResult& r) {
        aResult = r;
        aDone = true;
    };

    filestore::StdFileStore storeB;
    auto linkB = TransferLink::connect(ctx, linkConfig(port, recvDir), storeB, {});
    filestore::StdFileStore storeA;
    auto linkA = TransferLink::connect(ctx, linkConfig(port), storeA, ha);

    REQUIRE(pumpUntil(ctx, [&] { return linkA->ready() && linkB->ready(); }, kWait));
    REQUIRE(linkA->sendFile(srcFile.string(), "big.bin", linkB->assignedId()) != 0);
    relayer::test::pump(ctx, std::chrono::milliseconds(50));

    linkA->close();
    REQUIRE(pumpUntil(ctx, [&] { return aDone; }, kWait));
    REQUIRE(aResult.outcome != transfer::TransferOutcome::kCompleted);
    REQUIRE(linkA->activeJobs() == 0);
}

TEST_CASE("未就绪时发文件直接拒绝", "[transferlink]")
{
    asio::io_context ctx;
    filestore::StdFileStore store;
    auto link = TransferLink::connect(ctx, linkConfig(1), store, {});   // 端口 1 连不上
    REQUIRE(link != nullptr);
    REQUIRE_FALSE(link->ready());
    REQUIRE(link->sendFile("whatever.bin", "x.bin", "peer") == 0);
}
