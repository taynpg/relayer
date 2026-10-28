#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "relayer/core/session/PeerTable.h"
#include "relayer/core/session/Session.h"

// relayerd：把连接、认证、转发三件事装起来。
//
// 本层刻意**不认识文件**：不链接 filestore、不读盘、不看传输载荷里的
// 内容。传输的 kFileRequestSend / kChunkRequestData 等帧一律按 to 字段
// 转发，服务端不知道也不关心它们是什么意思（docs/DESIGN.md §2.3：
// "服务端不链接 Qt，也不链接文件系统"）。
//
// 这样切的好处不只是省钱：服务端看不见文件内容，就没法把它落盘——这是
// "服务端只做转发"这条承诺的**结构性**保证，而不是靠纪律维持。
//
// 线程模型：全部在单个 io_context 上，一条连接的帧由它自己的 strand 串行
// 处理。因此下面的成员都不加锁。

namespace relayer::server {

// 服务端只用到 core 的会话与在线表这两块。写全限定名会让签名长到读不动，
// 而 `using` 到命名空间级别是安全的——它不会像 using namespace 那样把
// 整个 core 的名字灌进来（这两个命名空间里没有重名）。
namespace session = ::relayer::core::session;

struct ServerConfig {
    std::string listenAddress = "0.0.0.0";
    std::uint16_t port = 0;   // 0 = 由系统分配（测试用）

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

    // **测试专用**：按"第几个转发的传输数据帧"丢弃，序号从 1 开始。
    // 空表示一个都不丢（生产默认）。
    //
    // 只作用于 kChunkRequestData。丢控制帧（握手、请求、收尾）测的是
    // 另一类问题——"请求被吞"与"块丢失"的失败表现都是超时，混在一起
    // 会让失败原因无法归因。
    //
    // 为什么在服务端注入而不是在 socket 层制造真实丢包：真实丢包不可
    // 复现，测试只能断言"最终成功"，而无法断言"只补了丢失的那几块"。
    // 精确丢第 3、7 块，才能同时验证 SACK 位图与"不是全量重传"。
    std::vector<std::uint64_t> dropDataFrames;
};

// 一次服务端的运行统计。给日志与测试用。
struct ServerStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;   // 表满 / 重名，直接断开
    std::uint64_t authenticated = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t forwardFailed = 0;   // 目标在线但投递失败（对端写队列满、连接已关闭）
    std::uint64_t unroutable = 0;      // 目标不在线
    std::uint64_t localFrames = 0;

    // kClaimId 被拒的次数（标识被占用 / 非法 / 撞服务端自己的 id）。
    // 运营上值得盯：正常用户不会反复抢同一个名字，非零往往意味着
    // 两台机器配了同一个标识，或者有人在试探。
    std::uint64_t idConflicts = 0;

    // 测试注入丢掉的数据帧数。生产环境恒为 0；非 0 就说明这个服务端
    // 不是按生产配置跑起来的——日志里必须能看出来。
    std::uint64_t droppedForTest = 0;
};

class Server
{
public:
    Server(asio::io_context& ctx, ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 开始监听。返回后端口已确定（config.port 为 0 时可用 port() 取实际值）。
    [[nodiscard]] Status start();

    // 停止接受新连接并断开所有现有连接。
    void stop();

    // 实际监听的端口。
    [[nodiscard]] std::uint16_t port() const;

    [[nodiscard]] const ServerStats& stats() const noexcept
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
    void registerPeer(session::Session& self, const std::string& id, const std::string& addrText);

    void onFrame(session::Session& self, const protocol::Frame& frame);
    void onClosed(std::string id, Status status);

    // 处理"发给服务端自己"的帧（问 id、问列表、心跳之外的东西）。
    void handleLocal(session::Session& self, const protocol::Frame& frame);

    // 处理 kClaimId（客户端声明自己的标识，PROTOCOL.md §11.7）。
    //
    // 撞名时的语义来自 v1：**拒绝这次声明**，不顶掉先到者、不自动改名。
    // 拒绝只回 kAnswerId(status) 而**不立刻断开**——Connection::doClose
    // 会清空写队列，先发后关等于把拒绝理由吃掉，客户端只能看到"连接断了"。
    // 由客户端收到拒绝后自己断开（本项目的 CLI/GUI 都会）。
    void handleClaimId(session::Session& self, const protocol::Frame& frame);

    [[nodiscard]] std::string allocate();

    asio::io_context& ctx_;
    ServerConfig config_;

    asio::ip::tcp::acceptor acceptor_;
    bool accepting_ = false;

    session::PeerTable peers_;
    std::uint64_t nextIdHint_ = 1;

    // 正在执行"声明改名"的登记。registerPeer 据此区分"第一次认证"与
    // "改名后的重新登记"，避免把同一条连接计两次 authenticated。
    // 只在 handleClaimId → assignPeerId 的同步回调链上为 true。
    bool renaming_ = false;

    // 已转发的传输数据帧序号（从 1 开始），配合 config_.dropDataFrames
    // 决定"这一帧丢不丢"。与 nextIdHint_ 一样只在单线程上访问。
    std::uint64_t dataFrameSeq_ = 0;

    // id -> 会话。与 peers_ 分开维护：PeerTable 只存公开信息（可被 UI 与
    // 日志安全读取），路由要的 Session 指针留在这里。合成一张表会让
    // "把裸指针暴露给谁"变成一个随时可能出错的判断。
    std::map<std::string, std::shared_ptr<session::Session>, std::less<>> sessions_;

    // 已 accept 但还没通过认证的会话。
    //
    // **必须存强引用**。这一点我们踩过一次：早先存 weak_ptr，理由是"这些
    // 连接可能永远不认证（口令错、扫描端口），强引用会让服务端保活它们"。
    // 那个理由是错的，因为反方向的问题更严重——
    //
    //   Connection 的 handler 是通过 weak_ptr<Session> 回调 Session 的
    //   （见 Session.cpp 的 makeHandler）。Session 一旦没有外部强引用，
    //   它的析构会释放 Connection，于是所有异步回调都静默消失：socket
    //   被关掉，但**没有任何人收到关闭通知**，客户端看到的只是"连上了，
    //   然后什么都不发生"，直到自己的握手超时。
    //
    // 而 Connection 自己只持有自己（async_read_some 的 self=shared_from_this），
    // 并不回指 Session——它不知道 Session 存在。所以"保活会话"这件事只能
    // 由上层的这个容器负责。
    //
    // 那么"不认证的连接占资源"怎么办？答案是**握手超时**，不是弱引用：
    // Session 在 accept 时就起了 handshakeTimeout（默认 30 秒），到点必定
    // finish 并把连接关掉。扫描器每建一条连接，最多占它 30 秒，之后自动回收。
    // 真有洪泛攻击的话，需要的是 accept 侧的速率限制，而不是让正常但慢的
    // 客户端在握手途中莫名消失。
    std::vector<std::shared_ptr<session::Session>> pending_;

    ServerStats stats_;
};

}   // namespace relayer::server
