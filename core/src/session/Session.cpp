#include "relayer/core/session/Session.h"

#include <utility>

#include <asio/post.hpp>

#include "relayer/core/session/PeerTable.h"

namespace relayer::core::session
{

// 访问桥。
//
// 存在的唯一理由是：Connection 的回调要从匿名命名空间里访问 Session 的
// 私有入口。这些入口不设成 public——外部没有任何理由伪造一次"连接就绪"
// 或"收到帧"。
//
// 必须定义在匿名命名空间**之外**：定义在里面就成了
// `session::{anonymous}::SessionAccess`，与头文件里 friend 声明的
// `session::SessionAccess` 不是同一个类型，友元关系不成立。
struct SessionAccess
{
    static void established(Session &s)
    {
        s.notifyEstablished();
    }
    static void inbound(Session &s, const Frame &f)
    {
        s.handleInbound(f);
    }
    static void transportClosed(Session &s, Status st)
    {
        s.handleTransportClosed(std::move(st));
    }
};

namespace
{

using transport::ConnectionHandler;
using transport::HeartbeatAction;
using Clock = transport::heartbeat::Clock;
using TimePoint = transport::heartbeat::TimePoint;

// 构造一条"会话已结束"的 Status。
//
// 统一走这里而不是各处手写 Fail(...)，是为了让语义固定：会话结束的原因
// 一定落在 kProtocol / kAuthFailed / kTimeout / kCancelled 这几类里，
// 上层按 code 分派时不用猜。
Status failStatus(ErrorCode code, std::string message)
{
    return Status::Fail(code, std::move(message));
}

// 把 Session 的成员函数接到 Connection 的回调上。
//
// 为什么必须是 weak_ptr：Session 持有 conn_，conn_ 的回调若强引用 Session，
// 就形成环——两者引用计数永远不归零，连接和会话一起泄漏。反过来，回调里
// 拿不到强引用说明 Session 已经销毁，直接返回就是正确的行为。
ConnectionHandler makeHandler(const std::weak_ptr<Session> &weak)
{
    ConnectionHandler ch;

    ch.onEstablished = [weak]
    {
        if (auto self = weak.lock())
        {
            SessionAccess::established(*self);
        }
    };
    ch.onFrame = [weak](const Frame &f)
    {
        if (auto self = weak.lock())
        {
            SessionAccess::inbound(*self, f);
        }
    };
    ch.onClosed = [weak](Status s)
    {
        if (auto self = weak.lock())
        {
            SessionAccess::transportClosed(*self, std::move(s));
        }
    };
    return ch;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与启动
// ---------------------------------------------------------------------------

Session::Session(asio::io_context &ctx, SessionConfig config, SessionHandler handler)
    : ctx_(ctx)
    , config_(std::move(config))
    , handler_(std::move(handler))
    , handshakeTimer_(ctx)
    , heartbeatTimer_(ctx)
    , selfId_(config_.serverId)
{
}

Session::~Session() = default;

std::shared_ptr<Session> Session::connect(asio::io_context &ctx, const std::string &host, std::uint16_t port,
                                          SessionConfig config, SessionHandler handler)
{
    config.role = Role::kClient;
    auto session = std::shared_ptr<Session>(new Session(ctx, std::move(config), std::move(handler)));

    session->clientAuth_ = std::make_unique<auth::ClientHandshake>(
        session->config_.clientPassword,
        session->config_.server.allowNoAuth ? auth::ClientMode::kNoAuth : auth::ClientMode::kRequireAuth);

    // 客户端立刻启动状态机。kRequireAuth 模式下它不产帧（等服务端先发
    // Challenge），但相位要先进 kWaitChallenge——否则收到 Challenge 时
    // 会被判成"握手还没开始"。
    const auto st = session->clientAuth_->start();
    if (!st.ok())
    {
        // 启动失败几乎只可能是 CSPRNG 不可用。这种环境下继续跑没有意义，
        // 但要通过 onClosed 通知而不是抛出去——上层只有一条结束路径。
        session->finish(st, false);
        return session;
    }

    session->startHandshakeTimer();

    session->conn_ =
        Connection::connect(ctx, host, port, makeHandler(std::weak_ptr<Session>(session)), session->config_.connection);
    return session;
}

std::shared_ptr<Session> Session::accept(asio::io_context &ctx, asio::ip::tcp::socket socket, SessionConfig config,
                                         SessionHandler handler)
{
    config.role = Role::kServer;
    auto session = std::shared_ptr<Session>(new Session(ctx, std::move(config), std::move(handler)));

    session->serverAuth_ = std::make_unique<auth::ServerHandshake>(session->config_.server);

    // 注意这里**不**预置 assignedId_。
    //
    // 之前写过 `session->assignedId_ = config.initialPeerId;`，那是个 bug：
    // enterAuthenticatedServer() 用"assignedId_ 为空"判断该不该调
    // allocatePeerId，一旦这里先填了，allocator 就永远不会被调用——而它的
    // 存在恰恰说明上层想动态分配。现在把"从哪个来源取 id"的优先级集中在
    // enterAuthenticatedServer() 一处：
    //
    //     initialPeerId（显式指定） > allocatePeerId（动态分配） > 空
    //
    // 空身份是安全的默认值：identityToBind 返回空 → 转发被拒。忘了配 id
    // 的部署不会静默变成"可以任意冒充他人"。

    session->conn_ = Connection::adopt(ctx, std::move(socket), makeHandler(std::weak_ptr<Session>(session)),
                                       session->config_.connection);
    return session;
}

// ---------------------------------------------------------------------------
// 连接就绪
// ---------------------------------------------------------------------------

void Session::notifyEstablished()
{
    if (finished_)
    {
        return;
    }

    // 心跳基线必须在"连接可用于收发"这一刻建立，而不是在构造时。
    //
    // 构造到连上之间可能隔几秒（DNS + TCP 握手）。若用构造时刻做基线，
    // 一上来就"已经空闲很久"，连接刚建立就发心跳——对端收到一个毫无意义
    // 的 ping，而真正该测的"连上之后有没有动静"根本没测到。
    heartbeat_.reset(Clock::now());

    if (config_.role == Role::kClient && clientAuth_ && clientAuth_->established())
    {
        // 客户端配置成 kNoAuth：start() 时已处于 kEstablished，
        // 不需要等服务端的任何帧。
        enterAuthenticated();
        return;
    }

    if (config_.role == Role::kServer)
    {
        // 服务端发第一帧 Challenge（PROTOCOL.md §7.1）。
        //
        // 放在这里而不是构造函数里：构造函数执行时框架还没接管 socket。
        // 放在 onEstablished 能让两个方向的启动路径完全一致——少一条分支
        // 就少一处可能忘记维护的地方。
        Frame challenge;
        bool hasOut = false;
        const auto st = serverAuth_->start(challenge, hasOut);
        if (!st.ok())
        {
            finish(st, true);
            return;
        }

        if (!hasOut)
        {
            // allowNoAuth 为真：服务端不发 Challenge，直接进入已认证相位。
            enterAuthenticated();
            return;
        }

        const auto sent = conn_->send(challenge);
        if (!sent.ok())
        {
            finish(sent, true);
            return;
        }
        ++framesOut_;
    }

    // 心跳从"连接可用"这一刻才开始。放在 notifyEstablished 的末尾而不是
    // 开头：上面若握手启动失败会走 finish()，之后不该再排定时器。
    startHeartbeatTimer();
}

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------

Status Session::stepHandshake(const Frame &in)
{
    Frame reply;
    bool hasOut = false;

    Status st = Status::Ok();
    if (config_.role == Role::kServer)
    {
        st = serverAuth_->handleFrame(in, reply, hasOut);
    }
    else
    {
        st = clientAuth_->handleFrame(in, reply, hasOut);
    }

    // 无论成功失败，状态机都可能吐出一帧（Challenge / Response / Result / Abort）。
    // 失败路径上的 abort 尤其重要：不发它，对端只能靠超时才知道失败——那
    // 是 30 秒的空白，用户看到的是"卡住了"而不是"口令错了"。
    if (hasOut)
    {
        const auto sent = conn_->send(reply);
        if (!sent.ok())
        {
            return sent;
        }
        ++framesOut_;
    }

    return st;
}

// ---------------------------------------------------------------------------
// 入站分派
// ---------------------------------------------------------------------------

void Session::handleInbound(const Frame &frame)
{
    if (finished_)
    {
        return;
    }

    ++framesIn_;

    // 任何入站字节都是生命迹象（PROTOCOL.md §8.5 规则 1）。放在最前面，
    // 保证"因为协议违规被拒的帧"也能证明对端还活着——那帧本身是否合法，
    // 与"对端是否还在响应"是两个独立问题。
    heartbeat_.onBytesReceived(Clock::now());

    // 策略层判定。所有"该不该收"的判断都在那里，本层不重复。
    const auto decision = checkInbound(config_.role, authenticated_, frame);

    switch (decision.action)
    {
    case InboundAction::kAnswerHeart:
        sendHeartbeatAnswer();
        return;

    case InboundAction::kIgnore:
        // 心跳应答：transport 层已经因为它收到字节而清掉了未应答标记，
        // 这里不需要再做任何事。
        return;

    case InboundAction::kReject:
        ++framesRejected_;
        finish(failStatus(ErrorCode::kProtocol, decision.reason), true);
        return;

    case InboundAction::kAccept:
        break;
    }

    if (!authenticated_)
    {
        const auto st = stepHandshake(frame);
        if (!st.ok())
        {
            finish(st, true);
            return;
        }

        const bool nowEstablished =
            (config_.role == Role::kServer) ? serverAuth_->established() : clientAuth_->established();
        if (nowEstablished)
        {
            enterAuthenticated();
        }
        return;
    }

    // 已认证的业务帧。
    //
    // 这里**不改写 from**——改写属于转发路径（forward()），由调用方显式
    // 决定目标。把改写塞在这里等于"所有业务帧都当成转发"，本地受理的帧
    // （问 id、问家目录）会被错误地署名成对端。
    //
    // 但认证帧在已认证相位出现仍要走一遍状态机：它会把这种情况判成重放
    // 并返回失败。这就是"认证后收到 Challenge"的处理路径，不需要在这里
    // 单独写一个 if。
    if (protocol::isAuthType(frame.type))
    {
        const auto st = stepHandshake(frame);
        if (!st.ok())
        {
            finish(st, true);
        }
        return;
    }

    // 客户端的 id 应答：服务端在认证后主动告诉我们叫什么。
    //
    // 在这里拦下来而不是交给上层，是因为 assignedId() 是 Session 自己的
    // 状态；让上层去调 setAssignedId 就等于把"谁维护这个字段"搞成两处。
    if (config_.role == Role::kClient && frame.type == FrameType::kAnswerId)
    {
        std::uint8_t status = 1;
        std::string id;
        std::string err;
        if (!decodeAnswerId(frame.body, status, id, err))
        {
            ++framesRejected_;
            finish(failStatus(ErrorCode::kProtocol, "malformed answer_id: " + err), true);
            return;
        }
        if (status == 0 && assignedId_ != id)
        {
            assignedId_ = id;
            // 身份变了要再通知一次：上层可能要在 UI 上显示它，或者
            // 用它做路由。只在真的变化时通知，避免重复回调。
            if (handler_.onAuthenticated)
            {
                handler_.onAuthenticated(assignedId_);
            }
        }
        return;
    }

    if (handler_.onFrame)
    {
        handler_.onFrame(frame);
    }
}

void Session::enterAuthenticated()
{
    if (authenticated_)
    {
        return;
    }
    authenticated_ = true;

    // 握手已经结束，握手超时定时器必须取消。
    //
    // 忘了取消的后果很隐蔽：连接工作得好好的，30 秒后突然被断开，日志上
    // 写着 "handshake timeout"，而握手其实早就成功了。这类 bug 在跑不到
    // 30 秒的测试里永远不会出现。
    handshakeTimer_.cancel();

    if (config_.role == Role::kServer)
    {
        enterAuthenticatedServer();
        return;
    }

    // 客户端：认证刚通过，此时**还不知道**自己的 id。服务端会在收到
    // kAskId 之后回 kAnswerId。这里先把相位切好，让业务帧可以流动，
    // 身份在 handleInbound 里收到应答时补上。
    //
    // 顺序很重要：先 anonymous 通知一次（让上层知道"这条连接可用了"），
    // 而不是等 id 到位再通知。否则"服务端被配置成不回答 kAskId"这种
    // 情况下，客户端会永远收不到认证通知。
    if (handler_.onAuthenticated)
    {
        handler_.onAuthenticated(assignedId_);
    }
}

void Session::enterAuthenticatedServer()
{
    // 身份来源的优先级，只在这一处决定
    // （PROTOCOL.md §11.6）：
    //
    //     1. allocatePeerId —— 动态分配，返回非空即采用。
    //     2. initialPeerId —— allocator 没配、或返回空时的兜底。
    //     3. 空 —— 安全默认值：identityToBind 返回空，转发一律被拒。
    //
    // 为什么 allocator 优先：配了它就意味着"上层知道谁该叫什么"，它看到
    // 的是整张 peer 表，而 initialPeerId 只是一个静态字符串。两者都配时
    // 若让静态值赢，动态分配就永远不会发生，而"配了不生效"是比"按文档
    // 规则生效"更难排查的一类问题。
    //
    // 分配发生在这个时点（认证成功之后）而不是更早，是因为分配意味着
    // "占了 peer 表里的一个号"，而握手失败的连接不该占号。之前放在
    // constructor 里试过：失败路径要额外补一次"还号"，那条路径一旦
    // 漏掉就是号段泄漏。现在只有一条入口。
    if (config_.allocatePeerId)
    {
        assignedId_ = config_.allocatePeerId();
    }
    if (assignedId_.empty() && !config_.initialPeerId.empty())
    {
        assignedId_ = config_.initialPeerId;
    }

    // 把 id 告诉对端（PROTOCOL.md §11.6）。
    //
    // 这一步失败不该断开：身份没送达时对端只是不知道自己叫什么，
    // 而"它能做什么"由服务端的绑定决定。让它继续跑，上层可以选择
    // 在一次往返之后再检查一遍。
    Frame answer;
    answer.type = FrameType::kAnswerId;
    if (encodeAnswerId(assignedId_, answer.body).ok())
    {
        (void)deliver(answer);
    }

    if (handler_.onAuthenticated)
    {
        handler_.onAuthenticated(assignedId_);
    }
}

void Session::startHandshakeTimer()
{
    if (config_.handshakeTimeout.count() <= 0)
    {
        return;
    }

    handshakeTimer_.expires_after(config_.handshakeTimeout);
    handshakeTimer_.async_wait(
        [weak = weak_from_this()](const asio::error_code &ec)
        {
            if (ec)
            {
                // operation_aborted：定时器被取消（握手成功或会话已结束）。
                // 这不是错误，正常返回。
                return;
            }
            auto self = weak.lock();
            if (!self || self->finished_ || self->authenticated_)
            {
                return;
            }

            ++self->handshakeTimeouts_;
            self->finish(failStatus(ErrorCode::kTimeout, "handshake did not complete in time"), true);
        });
}

void Session::startHeartbeatTimer()
{
    if (heartbeatTimerStarted_ || finished_)
    {
        return;
    }
    heartbeatTimerStarted_ = true;
    scheduleNextHeartbeat();
}

void Session::scheduleNextHeartbeat()
{
    if (finished_)
    {
        return;
    }

    heartbeatTimer_.expires_after(transport::heartbeat::kTickInterval);
    heartbeatTimer_.async_wait(
        [weak = weak_from_this()](const asio::error_code &ec)
        {
            if (ec)
            {
                // operation_aborted：会话已结束，定时器被 cancel。正常退出。
                return;
            }
            auto self = weak.lock();
            if (!self || self->finished_)
            {
                return;
            }

            self->pollHeartbeatNow();

            // 必须先判 finished_ 再排下一轮：pollHeartbeatNow() 可能已经
            // 因为超时把会话关掉了。不判的话，已结束的会话会继续留一个
            // 每秒唤醒的定时器，进程里所有连接都这么泄漏的话空转开销很可观。
            self->scheduleNextHeartbeat();
        });
}

// ---------------------------------------------------------------------------
// 心跳
// ---------------------------------------------------------------------------

void Session::sendHeartbeatAnswer()
{
    Frame answer;
    answer.type = FrameType::kAnswerHeart;

    // 心跳帧不带身份：它是连接层的活性探测，与身份无关（PROTOCOL.md §11.2）。
    // 带上 from 反而会触发服务端的出站断言，还得解释"为什么心跳可以例外"。
    const auto st = conn_->send(answer);
    if (st.ok())
    {
        ++framesOut_;
    }
    // 发不出去不单独处理：连接已经不可用时，下一次读/写回调自然会把连接
    // 关掉并走到 finish()。在这里再判一次只会多一条重复的关闭路径。
}

void Session::pollHeartbeatNow()
{
    if (finished_)
    {
        return;
    }

    switch (heartbeat_.check(Clock::now()))
    {
    case HeartbeatAction::kNone:
        return;

    case HeartbeatAction::kSendPing:
    {
        Frame ping;
        ping.type = FrameType::kAskHeart;
        const auto st = conn_->send(ping);
        if (!st.ok())
        {
            finish(st, true);
            return;
        }
        ++framesOut_;
        return;
    }

    case HeartbeatAction::kTimedOut:
        ++heartbeatTimeouts_;
        finish(failStatus(ErrorCode::kTimeout, "peer stopped responding to heartbeats"), true);
        return;
    }
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

Status Session::send(const Frame &frame)
{
    if (finished_)
    {
        return failStatus(ErrorCode::kCancelled, "session is closed");
    }
    if (!authenticated_)
    {
        // 未认证就发业务帧：上层调用顺序错了。挡在本地，而不是让帧发出去
        // 被对端断开——错误发生在本地就在本地报，日志才能指到真正的调用点。
        return failStatus(ErrorCode::kAuthFailed, "cannot send business frame before authentication");
    }

    const auto check = checkOutbound(config_.role, frame, config_.serverId);
    if (!check.allowed)
    {
        ++framesRejected_;
        // **不**断开连接：这是本地实现 bug，对端没做错事。断开只会把问题
        // 推给对端，让排查方向跑偏。
        return failStatus(ErrorCode::kProtocol, check.reason);
    }

    return deliver(frame);
}

Status Session::forward(Frame frame, std::string_view authenticatedId)
{
    if (finished_)
    {
        return failStatus(ErrorCode::kCancelled, "session is closed");
    }
    if (!authenticated_)
    {
        return failStatus(ErrorCode::kAuthFailed, "cannot forward before authentication");
    }

    // 唯一落笔点：先问策略该绑成谁，需要绑就交给 bindSenderIdentity()。
    // 这一步漏掉就是"任何已认证客户端可以冒充任意其他客户端"，所以转发
    // 只有这一个入口，不暴露"自己调 applyIdentityBinding 再 send"。
    const auto bound = applyIdentityBinding(config_.role, authenticated_, frame, authenticatedId);

    // 转发帧的命运在这里就定了：改写后的 from 必须恰好等于发送者的认证身份。
    //
    // 为什么不能复用 send() 里的 checkOutbound()：那个断言允许的集合是
    // "空或服务端自己的 id"，而转发帧的 from 恰恰是别人的 id——两者互不
    // 相交，直接复用会把**正确的转发**也拒掉。
    //
    // 但这里仍然要断言，而不是直接发：applyIdentityBinding 在"未认证"
    // 或"客户端侧"会返回空串且不改写，那样 from 仍是对方的自称。漏掉这个
    // 检查，第 484 行的返回值就白算了。
    if (bound.empty() || bound != authenticatedId)
    {
        ++framesRejected_;
        return failStatus(ErrorCode::kProtocol, "forward requires an authenticated identity to bind");
    }

    if (frame.to.empty())
    {
        // 转发必须有目标。空 to 是"发给服务端自己"的写法（§11.3），
        // 那类帧走 send() 而不是 forward()。
        ++framesRejected_;
        return failStatus(ErrorCode::kProtocol, "forward requires a non-empty target");
    }

    return deliver(frame);
}

Status Session::deliver(const Frame &frame)
{
    // 真正落到连接上的唯一出口。两个公开的发送入口都先做完各自的断言再
    // 调它，这样"计数"和"实际写出"只有一处，不会出现"一个入口忘了计数"。
    const auto st = conn_->send(frame);
    if (st.ok())
    {
        ++framesOut_;
    }
    return st;
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------

void Session::close()
{
    finish(Status::Ok(), true);
}

void Session::handleTransportClosed(Status status)
{
    // 会话可能已经因为认证失败等原因 finish 过了。那时保留原有的原因，
    // 不要被"紧接着的底层关闭"覆盖成成功——上层看到"成功结束"就不知道
    // 口令是错的。
    finish(std::move(status), false);
}

void Session::finish(Status status, bool closeTransport)
{
    if (finished_)
    {
        return;
    }
    finished_ = true;

    handshakeTimer_.cancel();
    heartbeatTimer_.cancel();

    if (closeTransport && conn_)
    {
        conn_->close();
    }

    // 把 handler 拷出来再通知：上层很可能在这个回调里丢掉最后一个
    // Session 引用，那一刻 this 就没了。与 Connection::doClose 里的做法
    // 是同一个理由。
    auto notify = handler_.onClosed;
    if (!notify)
    {
        return;
    }

    // 投递而不是同步调用。finish() 有很多调用点（包括在 Connection 的帧
    // 回调里），同步通知会让上层在"会话还在处理这一帧"的栈上就把会话
    // 拆掉，之后本函数剩下的语句会访问已销毁的对象。
    asio::post(ctx_, [n = std::move(notify), s = std::move(status)]() mutable { n(std::move(s)); });
}

std::string Session::peer() const
{
    return conn_ ? conn_->peer() : std::string{};
}

}  // namespace relayer::core::session
