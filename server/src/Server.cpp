#include "relayer/server/Server.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include <asio/ip/tcp.hpp>

#include "relayer/core/session/SessionPolicy.h"

namespace relayer::server
{

namespace
{

using protocol::Frame;
using protocol::FrameType;
using session::PeerInfo;
using session::planRoute;
using session::RouteAction;
using session::RouteConfig;
using session::RoutePlan;

}  // namespace

Server::Server(asio::io_context &ctx, ServerConfig config)
    : ctx_(ctx)
    , config_(std::move(config))
    , acceptor_(ctx)
    , peers_(config_.maxPeers)
{
}

Server::~Server() = default;

Status Server::start()
{
    if (accepting_)
    {
        return Status::Fail(ErrorCode::kBusy, "server already started");
    }

    std::error_code ec;
    const auto addr = asio::ip::make_address(config_.listenAddress, ec);
    if (ec)
    {
        return Status::Fail(ErrorCode::kInvalidPath, "bad listen address: " + config_.listenAddress);
    }

    acceptor_.open(asio::ip::tcp::v4(), ec);
    if (ec)
    {
        return Status::Fail(ErrorCode::kIoError, "acceptor open failed: " + ec.message());
    }

    // 服务端重启时端口可能还在 TIME_WAIT。不设这个的话，测试里连续
    // 起停同一个端口会莫名其妙地 bind 失败。
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);

    acceptor_.bind(asio::ip::tcp::endpoint(addr, config_.port), ec);
    if (ec)
    {
        return Status::Fail(ErrorCode::kIoError,
                            "bind failed on port " + std::to_string(config_.port) + ": " + ec.message());
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
    {
        return Status::Fail(ErrorCode::kIoError, "listen failed: " + ec.message());
    }

    accepting_ = true;
    doAccept();
    return Status::Ok();
}

void Server::stop()
{
    if (!accepting_)
    {
        return;
    }
    accepting_ = false;

    // 先关 acceptor 再动会话：不关的话一个正在被 accept 的连接会在下面
    // 这批 close 之后才进来，于是留下一条没人管的活连接。
    std::error_code ec;
    acceptor_.close(ec);

    // 复制一份再关：close() 会触发 onClosed，而那里面会从 sessions_ 里
    // 摘条目——直接迭代原容器等于边遍历边删。另外 onClosed 是**投递**
    // 执行的，所以下面显式清表，不指望回调来完成清理。
    std::vector<std::shared_ptr<session::Session>> all;
    all.reserve(sessions_.size());
    for (auto &kv : sessions_)
    {
        all.push_back(kv.second);
    }
    for (auto &s : all)
    {
        s->close();
    }

    sessions_.clear();
    peers_.clear();
}

std::uint16_t Server::port() const
{
    std::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

void Server::doAccept()
{
    if (!accepting_)
    {
        return;
    }

    acceptor_.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket)
        {
            if (!accepting_)
            {
                // stop() 之后到达的连接直接丢弃。不记 rejected——那不是
                // 对端的错，是我们自己在关。
                return;
            }

            if (ec)
            {
                ++stats_.rejected;
                // 单次 accept 失败（EMFILE 之类）不该让服务端永久停止服务。
                // 继续排下一次，下一个连接也许就能成。
                doAccept();
                return;
            }

            ++stats_.accepted;

            session::SessionConfig sc;
            sc.role = session::Role::kServer;
            sc.serverId = config_.serverId;
            sc.server.password = config_.password;
            sc.server.allowNoAuth = config_.allowNoAuth;
            sc.handshakeTimeout = config_.handshakeTimeout;

            // 分配 id 通过回调注入：core 不知道 id 怎么生成，也不该知道。
            sc.allocatePeerId = [this] { return allocate(); };

            std::error_code e2;
            const auto ep = socket.remote_endpoint(e2);
            const std::string addrText =
                e2 ? std::string{} : ep.address().to_string() + ":" + std::to_string(ep.port());

            session::SessionHandler h;

            // onAuthenticated 时才知道 id，所以这里用 Session& 反查不到；
            // 改用"回调里现注册"：把 id 与这条会话登记进 sessions_。
            h.onAuthenticated = [this, addrText](session::Session &self, std::string id)
            { registerPeer(self, id, addrText); };

            h.onFrame = [this](session::Session &self, const Frame &f) { onFrame(self, f); };

            h.onClosed = [this](std::string id, Status st) { onClosed(std::move(id), std::move(st)); };

            auto s = session::Session::accept(ctx_, std::move(socket), std::move(sc), std::move(h));

            // accept() 返回前已在握手，认证可能还没完成。这里先把指针
            // 暂存下来：认证回调发生在事件循环上，届时才有 id。
            pending_.push_back(s);

            doAccept();
        });
}

void Server::registerPeer(session::Session &self, const std::string &id, const std::string &addrText)
{
    // 改名会再触发一次这个回调（assignPeerId → onAuthenticated），
    // 但"认证成功"只发生了一次，计数不能跟着涨。
    if (!renaming_)
    {
        ++stats_.authenticated;
    }

    // 表满或重名：拒绝这条连接。
    //
    // 分配器已经保证 id 不重复（allocateId 会跳过已占用），所以重名只可能
    // 出现在"同一连接被通知了两次认证完成"这种异常情况下。即便如此也不
    // 该把它塞进表——那会让两条连接共用一个身份。
    const auto r = peers_.add(PeerInfo{id, addrText});
    if (r != session::RegisterResult::kOk)
    {
        ++stats_.rejected;
        self.close();
        return;
    }

    // Session& 只在这里转成 shared_ptr：Server 需要长期持有这条会话，
    // 而回调给的是引用。enable_shared_from_this 正是为这种场景准备的。
    auto sp = self.shared_from_this();
    sessions_[id] = sp;

    // 从待定列表里摘掉。比较的是对象地址而不是 shared_ptr 本身：同一个
    // 对象只可能有一条控制块，但这里比地址更直白地表达"就是这条会话"。
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const std::shared_ptr<session::Session> &p) { return p.get() == &self; }),
                   pending_.end());
}

void Server::onFrame(session::Session &self, const Frame &frame)
{
    ++stats_.localFrames;

    RouteConfig rc;
    rc.serverId = config_.serverId;

    const auto plan = planRoute(session::Role::kServer, true, rc, self.assignedId(), frame);

    switch (plan.action)
    {
    case RouteAction::kAcceptLocal:
        handleLocal(self, frame);
        return;

    case RouteAction::kForward:
    {
        // 测试注入的丢包：在中继这一跳丢掉，等价于"目标端没收到"。
        //
        // 放在查目标之前，是因为它模拟的是链路问题，与目标是否在线无关。
        // 计数在**所有**数据帧上递增（含被丢的和没丢的），这样测试给
        // 的序号就是"全局第几个数据帧"，不会因为前面的帧被丢而错位。
        if (frame.type == FrameType::kChunkRequestData && !config_.dropDataFrames.empty())
        {
            ++dataFrameSeq_;
            if (std::find(config_.dropDataFrames.begin(), config_.dropDataFrames.end(), dataFrameSeq_) !=
                config_.dropDataFrames.end())
            {
                ++stats_.droppedForTest;
                return;
            }
        }

        const auto it = sessions_.find(plan.to);
        if (it == sessions_.end())
        {
            // 目标不在线。不回错误帧——协议里没有"投递失败"这一类
            // （§9 的传输帧靠超时自己发现）。记一次计数，让运维能看出
            // "有人在找不存在的对端"。
            ++stats_.unroutable;
            return;
        }

        // 转发**必须**走 forward()：它会做身份绑定。直接 send() 会让
        // 服务端发出的帧带着对端自称的 from，那正是 §11.4 要防的。
        //
        // 返回值必须检查。早先这里是 (void) 丢弃的，代价是一次真实事故：
        // 对端的写队列满时 forward() 会返回 kNoSpace **并关闭那条连接**，
        // 而服务端继续按"转发成功"计数。于是中继侧统计一切正常、发送方
        // 却永远等不到确认，排查方向被引向网络丢包。
        //
        // 现在只计数不改行为：中继没有"投递失败"这类帧可回（§9 靠超时
        // 自己发现），但至少运维能从 forwardFailed 看出问题在对端而不是
        // 在自己。
        if (!it->second->forward(frame, self.assignedId()).ok())
        {
            ++stats_.forwardFailed;
            return;
        }
        ++stats_.forwarded;
        return;
    }

    case RouteAction::kReject:
    default:
        ++stats_.unroutable;
        return;
    }
}

void Server::handleLocal(session::Session &self, const Frame &frame)
{
    if (frame.type == FrameType::kClaimId)
    {
        handleClaimId(self, frame);
        return;
    }

    // 目前只有一件事需要服务端在本地回答：问在线列表。
    //
    // 问 id（kAskId）由 core 的握手阶段自动应答，走不到这里；心跳同理。
    // 剩下那些（kAskHome、kAskPeerInfo……）属于阶段 F 的目录服务，
    // 现在一律不回——不存在的功能应当表现为"没有回应"，而不是回一个
    // 编造的空答案，那会让客户端以为"这台机器没有家目录"。
    if (frame.type != FrameType::kAskClientList)
    {
        return;
    }

    std::vector<char> body;
    if (!session::encodeClientList(peers_.list(), body).ok())
    {
        return;
    }

    Frame out;
    out.type = FrameType::kAnswerClientList;
    out.to = self.assignedId();
    out.body = std::move(body);
    (void)self.send(out);
}

// 客户端声明标识（PROTOCOL.md §11.7）。
//
// 三种拒绝、一种幂等、一种接受：
//   - 结构坏 / id 不合法（空、超长、含 NUL）/ 撞服务端自己的 id → kInvalid
//   - 已被别人占用                                                  → kTaken
//   - 就是自己现在这个名字                                          → 不作答（它已经知道了）
//   - 空闲                                                          → 迁表 + assign
//
// 拒绝**不断开**（见头文件注释：先发后关会把理由吃掉）。本项目客户端
// 收到非零 status 会自己关闭；不听话的客户端也就停在临时 id 上，
// 不构成权限问题。
void Server::handleClaimId(session::Session &self, const Frame &frame)
{
    auto reject = [&](session::IdAnswerStatus status)
    {
        Frame answer;
        answer.type = FrameType::kAnswerId;
        std::vector<char> body;
        if (session::encodeAnswerId(status, std::string(), body).ok())
        {
            answer.body = std::move(body);
            (void)self.send(answer);
        }
    };

    std::string id;
    std::string err;
    if (!session::decodeClaimId(frame.body, id, err))
    {
        ++stats_.idConflicts;
        reject(session::IdAnswerStatus::kInvalid);
        return;
    }

    // 已经是这个名字：幂等返回。客户端重连后重复声明、或用户连点两次
    // 都会走到这里，不该被当成冲突。
    if (id == self.assignedId())
    {
        return;
    }

    if (!session::isValidPeerId(id) || id == config_.serverId)
    {
        // 撞服务端 id 必须单独拦：planRoute 里"to == 服务端 id"优先于
        // "to == 自己 id"，一个叫 relayerd 的客户端永远收不到转发帧，
        // 症状是"对方在线但发不过去"。
        ++stats_.idConflicts;
        reject(session::IdAnswerStatus::kInvalid);
        return;
    }

    if (peers_.contains(id))
    {
        // 撞名：拒绝这次声明。**不**顶掉先到者（那等于允许后来者踢人），
        // 也**不**自动改名（用户的对照表按 id 固定投递，改名比连不上危险）。
        ++stats_.idConflicts;
        reject(session::IdAnswerStatus::kTaken);
        return;
    }

    // 迁表：先摘旧号，再让 assignPeerId → onAuthenticated → registerPeer
    // 把新号登记进去。登记逻辑只保留一处（registerPeer），否则 peers_ /
    // sessions_ 的一致性会有两条各自演化的维护路径。
    const std::string oldId = self.assignedId();
    peers_.remove(oldId);
    sessions_.erase(oldId);

    renaming_ = true;
    const Status st = self.assignPeerId(id);
    renaming_ = false;
    if (!st.ok())
    {
        // 走到这里说明编码失败之类的内部问题（id 已经校验过）。会话此时
        // 既没旧号也没新号，继续留着只会变成"在线但不可达"，直接断开。
        self.close();
    }
}

void Server::onClosed(std::string id, Status status)
{
    if (id.empty())
    {
        // 没认证就断开（口令错、握手超时）。这类连接从未进表。
        //
        // 它们还留在 pending_ 里，要在这里清掉——不清的话 pending_ 会随
        // 每次失败的登录尝试长一点，一个扫描端口的脚本就能把它撑大。
        //
        // 这里找的是"已经 finish 的会话"。用 use_count 判不出来（本容器
        // 与事件循环里的异步操作都在持有它），所以改成问会话自己：已经
        // 结束的留下没有意义。注意 onClosed 是**投递**执行的，此刻调用
        // 发生在事件循环上，会话状态已稳定，读 finished() 是安全的。
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [](const std::shared_ptr<session::Session> &p) { return !p || p->finished(); }),
                       pending_.end());
        return;
    }

    peers_.remove(id);
    sessions_.erase(id);
    (void)status;
}

std::string Server::allocate()
{
    return session::allocateId(peers_, nextIdHint_);
}

}  // namespace relayer::server
