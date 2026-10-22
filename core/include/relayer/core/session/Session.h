#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include "relayer/common/Status.h"
#include "relayer/core/auth/Handshake.h"
#include "relayer/core/protocol/Frame.h"
#include "relayer/core/session/SessionPolicy.h"
#include "relayer/core/transport/Connection.h"
#include "relayer/core/transport/Heartbeat.h"

// 会话：把 Connection（收发）、Handshake（认证）、HeartbeatPolicy（活性）、
// SessionPolicy（判定）四件事装到一起。
//
// 本层**不写新规则**。所有"该不该"的判断都已在前三层做完；这里只负责
// 按顺序调用它们，并把结论翻译成动作（发帧 / 断开 / 回调上层）。
//
// 这条边界是刻意的。组装层最容易变成"顺手加个 if"的地方，而一旦规则
// 有了两处实现，就再也说不清哪处是权威。所以本文件里出现任何
//
//     if (f.type == kAskFileList) ...
//
// 这类业务判断都应该是错的——那属于 SessionPolicy 或上层。
//
// 一次会话的全部事件都跑在 Connection 的 strand 上，因此这里**不需要加锁**，
// 但也**不能做阻塞操作**（写文件、同步 DNS、等锁）。传输引擎要落盘的
// 部分必须走异步接口，或者投递到别的线程池。

namespace relayer::core::session
{

// 前置声明，供 Session 的友元声明使用。
struct SessionAccess;

using protocol::Frame;
using protocol::FrameType;
using transport::Connection;
using transport::ConnectionOptions;
using transport::HeartbeatPolicy;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

struct SessionConfig
{
    Role role = Role::kServer;

    // 服务端：认证配置（口令、是否允许不认证）。
    // 客户端：password 为口令，allowNoAuth 表示"明知服务端关了认证"。
    auth::ServerConfig server;
    std::string clientPassword;

    // 握手超时。与心跳超时分开：握手是"从连上到认证完成"，心跳是"认证之后"。
    //
    // 为什么必须单独一个值：握手卡住时对端可能还活着（正在算 PBKDF2，
    // 20000 次迭代在中低端机器上可能上百毫秒，慢机器上更久），此时
    // 心跳一直在正常应答，永远不会判失活。没有握手超时的话，一个卡在
    // 半路的状态机可以永远占着一条连接。
    std::chrono::seconds handshakeTimeout{std::chrono::seconds(30)};

    // 服务端自己的 id（PROTOCOL.md §11.3：to 等于它时按本地受理）。
    // 客户端留空。
    std::string serverId;

    // 服务端分配给对端的 id。留空表示"认证完成后由上层通过
    // onAuthenticated 回调注入"——core 不认识 id 怎么生成。
    std::string initialPeerId;

    // 底层连接参数透传
    ConnectionOptions connection;
};

// ---------------------------------------------------------------------------
// 回调
// ---------------------------------------------------------------------------

struct SessionHandler
{
    // 认证完成，可以走业务帧了。
    //
    // assignedId：服务端分配（或客户端被告知）的身份。这里的 id 由上层
    // 注入——core 不认识"客户端 id 怎么分配"这种业务规则。
    std::function<void(std::string assignedId)> onAuthenticated;

    // 收到一帧业务帧（认证完成之后）。已通过 SessionPolicy 的入站检查，
    // 也已经按 §11.4 做了出站断言**之前**的状态——也就是说 from 是
    // 对端自称的，转发时仍须调用 applyIdentityBinding()。
    std::function<void(const Frame &)> onFrame;

    // 会话结束。保证只调用一次。
    //
    // 与 Connection::onClosed 的区别：Connection 只在 TCP 层关闭时通知，
    // 而会话可能先因为认证失败 / 握手超时 / 协议违规而结束——那之后
    // 还要主动断开底层连接。上层只需要关心这一个"结束"。
    std::function<void(Status)> onClosed;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

class Session : public std::enable_shared_from_this<Session>
{
public:
    // 客户端：主动建连并启动握手。
    [[nodiscard]] static std::shared_ptr<Session> connect(asio::io_context &ctx, const std::string &host,
                                                          std::uint16_t port, SessionConfig config,
                                                          SessionHandler handler);

    // 服务端：接管一个已建立的 socket。
    static std::shared_ptr<Session> accept(asio::io_context &ctx, asio::ip::tcp::socket socket, SessionConfig config,
                                           SessionHandler handler);

    ~Session();

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // 发送一帧业务帧。
    //
    // 会先过 checkOutbound()：服务端发出带第三方 from 的帧会被拒。这是
    // 转发路径漏调 bindSenderIdentity() 的兜底断言，不是可选的额外检查。
    [[nodiscard]] Status send(const Frame &frame);

    // 转发一帧：改写身份后发出。这是服务端转发他人帧的**唯一**入口。
    //
    // 之所以不暴露成"你自己调 applyIdentityBinding 再 send"，是因为那样
    // 调用方就有了两件事要记得做对。合成一个函数之后，"转发"这件事
    // 只有一条代码路径。
    [[nodiscard]] Status forward(Frame frame, std::string_view authenticatedId);

    void close();

    // 握手完成后的身份。未完成时为空。
    [[nodiscard]] const std::string &assignedId() const noexcept
    {
        return assignedId_;
    }
    [[nodiscard]] bool authenticated() const noexcept
    {
        return authenticated_;
    }
    [[nodiscard]] Role role() const noexcept
    {
        return config_.role;
    }

    // 对端地址，日志用。
    [[nodiscard]] std::string peer() const;

    // 诊断计数器
    [[nodiscard]] std::uint64_t framesIn() const noexcept
    {
        return framesIn_;
    }
    [[nodiscard]] std::uint64_t framesOut() const noexcept
    {
        return framesOut_;
    }
    [[nodiscard]] std::uint64_t framesRejected() const noexcept
    {
        return framesRejected_;
    }
    [[nodiscard]] std::uint64_t handshakeTimeouts() const noexcept
    {
        return handshakeTimeouts_;
    }
    [[nodiscard]] std::uint64_t heartbeatTimeouts() const noexcept
    {
        return heartbeatTimeouts_;
    }

    // 心跳策略（测试与诊断用）。
    //
    // 直接暴露引用而不是包一层：HeartbeatPolicy 本身是纯逻辑且已有自己的
    // 测试，包一层只会多一份要维护的转发代码。
    [[nodiscard]] const HeartbeatPolicy &heartbeatPolicy() const noexcept
    {
        return heartbeat_;
    }

    // 测试用：立即跑一次心跳判定。
    //
    // 生产路径由 asio 定时器驱动；这条只是为了在测试里跳过 20 秒等待。
    // 之所以还留着一个 public 方法而不是测试直接操作策略，是因为"判定
    // 为超时之后会话要做什么"（断开 + 计数 + 通知上层）也是要测的。
    void pollHeartbeatNow();

private:
    // makeHandler() 在匿名命名空间里，需要访问上面三个私有入口。
    // 友元声明放在这里而不是把三个方法改成 public：外部没有任何理由
    // 直接调 notifyEstablished()——那等于伪造一次"连接就绪"。
    friend struct SessionAccess;

    Session(asio::io_context &ctx, SessionConfig config, SessionHandler handler);

    void notifyEstablished();
    void handleInbound(const Frame &frame);
    void handleTransportClosed(Status status);

    // 真正写到连接上的唯一出口。send() / forward() 各自做完断言后调它。
    //
    // 单独拆出来是为了让"计数"和"写出"只有一处实现——否则加第三个发送
    // 入口时很容易漏掉 ++framesOut_，而那类 bug 在日志里表现为"帧数和
    // 实际流量对不上"，很难排查。
    [[nodiscard]] Status deliver(const Frame &frame);

    // 把一帧交给握手状态机，并把它吐出来的帧发出去。
    //
    // 返回握手状态机自己的 Status（失败意味着这条连接不能再用了）。
    [[nodiscard]] Status stepHandshake(const Frame &in);

    // 认证成功后进入业务相位：初始化心跳基线、取消握手超时、通知上层。
    void enterAuthenticated();

    void startHandshakeTimer();
    void finish(Status status, bool closeTransport);
    void sendHeartbeatAnswer();

    asio::io_context &ctx_;
    SessionConfig config_;
    SessionHandler handler_;

    std::shared_ptr<Connection> conn_;
    std::unique_ptr<auth::ServerHandshake> serverAuth_;
    std::unique_ptr<auth::ClientHandshake> clientAuth_;

    HeartbeatPolicy heartbeat_;
    asio::steady_timer handshakeTimer_;

    bool authenticated_ = false;
    bool finished_ = false;
    std::string assignedId_;
    std::string selfId_;  // 服务端自己的 id，来自配置

    std::uint64_t framesIn_ = 0;
    std::uint64_t framesOut_ = 0;
    std::uint64_t framesRejected_ = 0;
    std::uint64_t handshakeTimeouts_ = 0;
    std::uint64_t heartbeatTimeouts_ = 0;
};

}  // namespace relayer::core::session
