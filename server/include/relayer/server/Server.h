#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "relayer/core/session/PeerTable.h"
#include "relayer/core/session/Session.h"

// relayerd：把连接、认证、转发三件事装起来。
//
// 本层刻意**不认识文件**：不链接 filestore、不读盘、不看传输载荷里的
// 内容。传输的 kFileRequestSend / kChunkRequestData 等帧一律按 to 字段
// 转发，服务端不知道也不关心它们是什么意思（REWRITE-PLAN 2.3：
// "服务端不链接 Qt，也不链接文件系统"）。
//
// 这样切的好处不只是省钱：服务端看不见文件内容，就没法把它落盘——这是
// "服务端只做转发"这条承诺的**结构性**保证，而不是靠纪律维持。
//
// 线程模型：全部在单个 io_context 上，一条连接的帧由它自己的 strand 串行
// 处理。因此下面的成员都不加锁。

namespace relayer::server
{

// 服务端只用到 core 的会话与在线表这两块。写全限定名会让签名长到读不动，
// 而 `using` 到命名空间级别是安全的——它不会像 using namespace 那样把
// 整个 core 的名字灌进来（这两个命名空间里没有重名）。
namespace session = ::relayer::core::session;

struct ServerConfig
{
    std::string listenAddress = "0.0.0.0";
    std::uint16_t port = 0;  // 0 = 由系统分配（测试用）

    // 服务端在协议里的身份（PROTOCOL.md §11.3）。
    std::string serverId = "relayerd";

    // 认证口令。留空表示关闭认证——这会打一条 warning：
    // 一个忘记配口令就上线的服务端等于任何人都能接入的开放中继。
    std::string password;
    bool allowNoAuth = false;

    // 分配给对端的 id 前缀，结果是 "<prefix><n>"。
    std::string idPrefix = "client-";

    std::size_t maxPeers = session::PeerTable::kDefaultCapacity;

    std::chrono::seconds handshakeTimeout{30};
};

// 一次服务端的运行统计。给日志与测试用。
struct ServerStats
{
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;  // 表满 / 重名，直接断开
    std::uint64_t authenticated = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t unroutable = 0;  // 目标不在线
    std::uint64_t localFrames = 0;
};

class Server
{
public:
    Server(asio::io_context &ctx, ServerConfig config);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // 开始监听。返回后端口已确定（config.port 为 0 时可用 port() 取实际值）。
    [[nodiscard]] Status start();

    // 停止接受新连接并断开所有现有连接。
    void stop();

    // 实际监听的端口。
    [[nodiscard]] std::uint16_t port() const;

    [[nodiscard]] const ServerStats &stats() const noexcept
    {
        return stats_;
    }

    [[nodiscard]] std::size_t peerCount() const noexcept
    {
        return peers_.size();
    }

    // 当前在线列表（测试用）。
    [[nodiscard]] std::vector<session::PeerInfo> peers() const
    {
        return peers_.list();
    }

private:
    void doAccept();

    // 认证完成时把会话登记进在线表。
    //
    // 入参是 Session&（回调约定）：Server 需要长期持有的 shared_ptr 由
    // enable_shared_from_this 取。addrText 在 accept 时就能拿到，存下来
    // 是因为注册发生在之后，那时 socket 已经交出去了。
    void registerPeer(session::Session &self, const std::string &id, const std::string &addrText);

    void onFrame(session::Session &self, const protocol::Frame &frame);
    void onClosed(std::string id, Status status);

    // 处理"发给服务端自己"的帧（问 id、问列表、心跳之外的东西）。
    void handleLocal(session::Session &self, const protocol::Frame &frame);

    [[nodiscard]] std::string allocate();

    asio::io_context &ctx_;
    ServerConfig config_;

    asio::ip::tcp::acceptor acceptor_;
    bool accepting_ = false;

    session::PeerTable peers_;
    std::uint64_t nextIdHint_ = 1;

    // id -> 会话。与 peers_ 分开维护：PeerTable 只存公开信息（可被 UI 与
    // 日志安全读取），路由要的 Session 指针留在这里。合成一张表会让
    // "把裸指针暴露给谁"变成一个随时可能出错的判断。
    std::map<std::string, std::shared_ptr<session::Session>, std::less<>> sessions_;

    // 已 accept 但还没通过认证的会话。
    //
    // 存 weak_ptr 不存 shared_ptr：这些连接可能永远不认证（口令错、扫描
    // 端口）。用强引用会让一个不认证的连接被服务端一直保活——最坏情况下
    // 每 30 秒的握手超时才回收，而扫描器每秒能建几十条。
    std::vector<std::weak_ptr<session::Session>> pending_;

    ServerStats stats_;
};

}  // namespace relayer::server
