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
            const std::string addrText = e2 ? std::string{}
                                            : ep.address().to_string() + ":" + std::to_string(ep.port());

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
    ++stats_.authenticated;

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

    // 从待定列表里摘掉。
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const std::weak_ptr<session::Session> &w)
                                  { return w.lock().get() == &self; }),
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
            (void)it->second->forward(frame, self.assignedId());
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

void Server::onClosed(std::string id, Status status)
{
    if (id.empty())
    {
        // 没认证就断开（口令错、握手超时）。这类连接从未进表。
        //
        // 它们还留在 pending_ 里，要在这里清掉——不清的话 pending_ 会随
        // 每次失败的登录尝试长一点，一个扫描端口的脚本就能把它撑大。
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [](const std::weak_ptr<session::Session> &w) { return w.expired(); }),
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
