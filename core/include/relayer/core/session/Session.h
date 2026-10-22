#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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

// SessionHandler 在 Session 之前定义（Session 要拿它做成员），
// 而它的回调签名里带 Session&，所以这里必须先声明类名。
class Session;

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

    // 服务端分配给对端的 id。留空表示"认证完成后由上层的 allocatePeerId
    // 分配"——core 不认识 id 怎么生成。
    //
    // 与 allocatePeerId 的关系：allocator 优先，它返回空串时才用这里的值。
    // 也就是说本字段是**兜底**，不是"显式指定就绝对赢"。理由见
    // enterAuthenticatedServer() 里的注释。
    std::string initialPeerId;

    // 认证通过后给对端分配 id 的回调（PROTOCOL.md §11.6）。
    //
    // 为什么是回调而不是让 Session 认识 PeerTable：Session 只该知道
    // "我有个对端、它叫什么"，而"表里谁占了哪个号"是服务端编排层的事。
    // 直接依赖会让 Session 无法在单条连接的测试里使用（每个测试都得
    // 先建一张表）。
    //
    // 返回空串表示"分配失败"——此时退回 initialPeerId；若它也空，就用
    // 空身份继续（客户端拿不到 id，服务端侧转发会被拒，但连接还能用）。
    // 上层可以选择直接断开而不是让它继续。
    //
    // 与 initialPeerId 的关系：本回调优先，返回空串时退回那个字段。
    std::function<std::string()> allocatePeerId;

    // 底层连接参数透传
    ConnectionOptions connection;
};

// ---------------------------------------------------------------------------
// 回调
// ---------------------------------------------------------------------------

struct SessionHandler
{
    // 三个回调都以 `Session &self` 开头。
    //
    // 为什么需要它：服务端同时管着几十上百条连接，回调触发时必须知道
    // "这是哪一条"。不用闭包捕获，是因为构造 Session 之前就要传 handler
    // ——那时拿不到自己的 shared_ptr，鸡生蛋。也不让调用方在 handler 里
    // 存一个"当前会话"的临时变量：那是全局状态，并发下必然出错。
    //
    // 传引用而不是 shared_ptr：回调期间 Session 一定活着（它自己正在
    // 调用回调），而传 shared_ptr 会让调用方很容易不小心长期持有，
    // 把一个该断开的连接拖住不放。

    // 认证完成，可以走业务帧了。
    //
    // assignedId：服务端分配（或客户端被告知）的身份。这里的 id 由上层
    // 注入——core 不认识"客户端 id 怎么分配"这种业务规则。
    std::function<void(Session &self, std::string assignedId)> onAuthenticated;

    // 收到一帧业务帧（认证完成之后）。已通过 SessionPolicy 的入站检查，
    // 也已经按 §11.4 做了出站断言**之前**的状态——也就是说 from 是
    // 对端自称的，转发时仍须调用 applyIdentityBinding()。
    std::function<void(Session &self, const Frame &)> onFrame;

    // 会话结束。保证只调用一次。
    //
    // `assignedId` 是这条会话的身份（未认证就断开时为空串）。
    //
    // 不传 Session& 而是传 id，原因是这个回调是**投递**执行的（见
    // Session::finish）：从投递到执行之间，上层可能已经丢掉了最后一个
    // 引用，会话对象已经销毁。而"哪条连接断了"这件事必须能告诉上层——
    // 服务端要据此把该 id 从在线表里摘掉。id 是值语义，活得过对象。
    //
    // 与 Connection::onClosed 的区别：Connection 只在 TCP 层关闭时通知，
    // 而会话可能先因为认证失败 / 握手超时 / 协议违规而结束——那之后
    // 还要主动断开底层连接。上层只需要关心这一个"结束"。
    std::function<void(std::string assignedId, Status)> onClosed;
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

    // 让服务端在认证完成后为对端分配 id。
    //
    // 时序上有个陷阱，必须写清楚：accept() 同步调用 Connection::adopt()，
    // 而 adopt() 已经向事件循环投递了 onEstablished。也就是说，如果调用方
    // 在 `Session::accept(...)` **返回之后**才调这个函数、而此前的调用栈
    // 从未让出过（例如在一次同步 accept 循环里连开多条连接），那么
    // onEstablished 的回调还排在队列里，本次设置来得及生效；但只要
    // 调用栈中途让出过一次——比如在 async_accept 回调里先 connect 一条
    // 对端会话——排队中的回调就可能先跑完，此时认证分配已经用旧的
    // initialPeerId 完成了。
    //
    // 正因为"来得及"完全取决于调度顺序，**正确的用法是在构造前配置**：
    //
    //     cfg.allocatePeerId = [this] { return allocateId(table_, hint_); };
    //     auto s = Session::accept(ctx, std::move(sock), std::move(cfg), h);
    //
    // 这个方法只保留给"会话已经建好但我确实想在认证前补一个分配器"的
    // 场景，且只在 assignedId_ 尚未落定时才接受新值——认证一旦完成，
    // id 已经announce 出去了，再改就得通知所有人（那是注销后重新注册，
    // 应当由上层显式做）。设置被忽略时返回 false。
    [[nodiscard]] bool setPeerIdAllocator(std::function<std::string()> alloc)
    {
        if (authenticated_)
        {
            return false;
        }
        config_.allocatePeerId = std::move(alloc);
        return true;
    }

    // 握手完成后的身份。未完成时为空。
    [[nodiscard]] const std::string &assignedId() const noexcept
    {
        return assignedId_;
    }
    [[nodiscard]] bool authenticated() const noexcept
    {
        return authenticated_;
    }

    // 会话是否已经结束（正常关闭、认证失败、超时都算）。
    //
    // 暴露它是因为上层需要一个**幂等的**判据来清理自己持有的会话集合：
    // onClosed 是投递执行的，从"会话结束"到"回调执行"之间，上层手里的
    // 指针指向的已经是一个死会话。没有这个访问器，上层就只能靠
    // use_count 猜——而事件循环里的异步操作也在持有它，那个数永远大于 1。
    [[nodiscard]] bool finished() const noexcept
    {
        return finished_;
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

    // 认证成功后进入业务相位：取消握手超时、通知上层。
    void enterAuthenticated();

    // 服务端侧的后半段：分配（或取用配置的）对端 id、把 id 告诉对端。
    //
    // 与 enterAuthenticated 分开是因为客户端没有"分配"这一步——它的
    // id 是**被告知**的。合成一个函数就得靠 role 分支区分，而那个分支
    // 会让"客户端误走了分配路径"这种错误变得不易察觉。
    void enterAuthenticatedServer();

    void startHandshakeTimer();

    // 心跳轮询。固定在 kTickInterval 触发一次，检查完再排下一次。
    //
    // 用"固定间隔轮询 + 时间戳比较"而不是"按到期时间设一个长定时器"：
    // 后者要在每次收到数据时重置，频繁 cancel/expires 本身有成本，而且
    // 漏掉任何一处重置就会让空闲判定失效。轮询的代价是每秒一次空转，
    // 相对于 20 秒的阈值可以忽略。
    void startHeartbeatTimer();
    void scheduleNextHeartbeat();

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
    asio::steady_timer heartbeatTimer_;

    bool authenticated_ = false;
    bool finished_ = false;
    bool heartbeatTimerStarted_ = false;
    std::string assignedId_;
    std::string selfId_;  // 服务端自己的 id，来自配置

    std::uint64_t framesIn_ = 0;
    std::uint64_t framesOut_ = 0;
    std::uint64_t framesRejected_ = 0;
    std::uint64_t handshakeTimeouts_ = 0;
    std::uint64_t heartbeatTimeouts_ = 0;
};

}  // namespace relayer::core::session
