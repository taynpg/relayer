#include "relayer/core/net/ControlLink.h"

#include <utility>

#include "relayer/core/protocol/Pairing.h"
#include "relayer/core/session/Session.h"

namespace relayer::core::net {

namespace {
using protocol::Frame;
using protocol::FrameType;

session::SessionConfig makeSessionConfig(const ControlLink::Config& cfg)
{
    session::SessionConfig sc;
    sc.role = session::Role::kClient;
    sc.clientPassword = cfg.password;
    sc.server.allowNoAuth = cfg.noAuth;
    return sc;
}
}   // namespace

ControlLink::ControlLink(asio::io_context& ctx, Config config, Handler handler)
    : ctx_(ctx), config_(std::move(config)), handler_(std::move(handler))
{
}

ControlLink::~ControlLink() = default;

std::shared_ptr<ControlLink> ControlLink::connect(asio::io_context& ctx, Config config, Handler handler)
{
    // 用 shared_ptr 而不是栈对象：Session 的回调会活到连接结束之后，
    // 必须有一个"它引用的东西可能已经死了"的安全表述——weak_from_this。
    //
    // 这里不用 make_shared：构造函数是私有的（要求调用方必须走 connect，
    // 否则连接还没建就有一个"半初始化"的 ControlLink 在外面飘）。静态成员
    // 函数内可以 new 私有构造。
    std::shared_ptr<ControlLink> self(new ControlLink(ctx, std::move(config), std::move(handler)));

    session::SessionHandler sh;
    std::weak_ptr<ControlLink> weak = self;
    sh.onAuthenticated = [weak](session::Session&, std::string id) {
        if (auto link = weak.lock()) {
            link->onSessionReady(std::move(id));
        }
    };
    sh.onFrame = [weak](session::Session&, const Frame& f) {
        if (auto link = weak.lock()) {
            link->onSessionFrame(f);
        }
    };
    sh.onClosed = [weak](std::string, Status s) {
        if (auto link = weak.lock()) {
            link->onSessionClosed(std::move(s));
        }
    };

    // 身份声明在认证完成后由 Session 自己发（claimPeerId），这里只需要
    // 把 id 带进配置。Session 的 Role::kClient 路径会处理 kAskId。
    auto cfg = self->config_;
    self->session_ = session::Session::connect(ctx, cfg.host, cfg.port, makeSessionConfig(cfg), std::move(sh));

    if (self->session_ != nullptr && !cfg.requestedId.empty()) {
        // 声明身份必须在认证之后。Session 的 connect 是异步的，这里排一次
        // 到事件循环里，等 onAuthenticated 到了再发——用 weak 保证连接
        // 已经没了时不动作。
        std::weak_ptr<ControlLink> w2 = self;
        asio::post(ctx, [w2] {
            auto link = w2.lock();
            if (link == nullptr || link->session_ == nullptr || !link->session_->authenticated()) {
                return;
            }
            (void)link->session_->claimPeerId(link->config_.requestedId);
        });
    }
    return self;
}

void ControlLink::onSessionReady(std::string assignedId)
{
    // Session 会通知两次（先空 id、后真实 id）。两次都要转出去：上层用
    // assignedId() 更新显示，重复通知是幂等的。
    ready_ = true;
    if (!assignedId.empty()) {
        assignedId_ = std::move(assignedId);
    }
    if (handler_.onReady) {
        handler_.onReady(assignedId_);
    }
}

void ControlLink::onSessionFrame(const Frame& f)
{
    // 分派顺序有讲究：**先判应答**。应答类型一定不会是"请求"，但如果先判
    // 请求，一个像应答的帧会被当成"对端在问我"，回一条无意义的应答出去。
    if (protocol::isAnswerType(f.type)) {
        const auto id = static_cast<std::uint64_t>(f.index);
        const auto it = pending_.find(id);
        if (it == pending_.end()) {
            // 无主的应答：最常见是"超时之后才到"。丢掉，不要塞给业务层
            // ——它没有上下文可以处理。
            return;
        }
        if (it->second->expected != f.type) {
            // 请求号对上了但类型不对：说明对端回错了（或我们记错了）。
            // 立刻以失败收掉，而不是让它挂着等满超时——那条路径会让人
            // 以为是网络慢。
            deliver(id, false, Status::Fail(ErrorCode::kProtocol, "应答类型与请求不匹配"), {});
            return;
        }
        deliver(id, true, Status{}, f.body);
        return;
    }

    if (protocol::isAskType(f.type)) {
        Frame answer;
        bool handled = false;
        if (handler_.onPeerRequest) {
            handled = handler_.onPeerRequest(f, answer);
        }
        if (!handled) {
            // 不认识的请求：静默丢弃（见头文件）。
            return;
        }
        // `to` 与 `index` 由本类填：回显请求号是配对的唯一依据，
        // 交给业务就意味着迟早有一处漏填。
        answer.to = f.from;
        answer.index = f.index;
        (void)session_->send(answer);
        return;
    }

    if (handler_.onNotice) {
        handler_.onNotice(f);
    }
}

void ControlLink::onSessionClosed(Status why)
{
    // 先让挂起的请求失败，再通知上层：上层在 onClosed 里通常会清空自己的
    // 状态，此时 pending_ 必须已经是空的，否则析构顺序会踩到已销毁的回调。
    failAll(why.ok() ? Status::Fail(ErrorCode::kCancelled, "控制连接已关闭") : why);
    ready_ = false;
    if (handler_.onClosed) {
        handler_.onClosed(std::move(why));
    }
}

bool ControlLink::ask(FrameType askType, std::string to, std::vector<char> body, AnswerFn done)
{
    if (!ready_ || closing_ || session_ == nullptr) {
        return false;
    }
    const auto expected = protocol::answerForAsk(askType);
    if (!expected) {
        return false;   // 不是请求帧：调用方拿错了类型
    }
    if (pending_.size() >= config_.maxInFlight) {
        return false;
    }

    const std::uint64_t id = nextRequestId_++;

    Frame f;
    f.type = askType;
    f.to = std::move(to);
    f.index = static_cast<std::int64_t>(id);
    f.body = std::move(body);
    if (const auto st = session_->send(f); !st.ok()) {
        return false;
    }

    auto p = std::make_shared<Pending>(ctx_, *expected, std::move(done));
    p->timer.expires_after(config_.askTimeout);

    std::weak_ptr<ControlLink> weak = weak_from_this();
    p->timer.async_wait([weak, id](const std::error_code& ec) {
        if (ec) {
            return;   // 被 cancel（应答已到 / 连接关闭）
        }
        if (auto link = weak.lock()) {
            link->deliver(id, false, Status::Fail(ErrorCode::kTimeout, "控制请求应答超时"), {});
        }
    });

    pending_.emplace(id, std::move(p));
    return true;
}

Status ControlLink::post(Frame frame)
{
    if (session_ == nullptr || closing_) {
        return Status::Fail(ErrorCode::kCancelled, "控制连接不可用");
    }
    return session_->send(frame);
}

void ControlLink::deliver(std::uint64_t id, bool ok, Status status, std::vector<char> body)
{
    const auto it = pending_.find(id);
    if (it == pending_.end()) {
        return;   // 已经收过（例如超时先到、应答后到）
    }
    // 先摘下来再回调：回调里可能立刻发下一个请求（很常见：拿到目录列表
    // 就去查元信息），那时 pending_ 必须已经干净。
    auto p = std::move(it->second);
    pending_.erase(it);

    p->timer.cancel();
    if (p->done) {
        p->done(Answer{ok, std::move(status), std::move(body)});
    }
}

void ControlLink::failAll(const Status& why)
{
    // 同样先整体摘出再逐个回调：回调里可能调用 ask()（会看到 pending_
    // 已经为空、ready_ 已经为 false），也可能直接销毁上层对象。
    auto pending = std::move(pending_);
    pending_.clear();

    for (auto& [id, p] : pending) {
        p->timer.cancel();
        if (p->done) {
            p->done(Answer{false, why, {}});
        }
    }
}

void ControlLink::close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    ready_ = false;
    if (session_ != nullptr) {
        session_->close();
    }
    // 不等 onClosed 投递：调用方调完 close 之后就不该再收到"成功"的应答，
    // 立刻把挂起的请求收掉更符合直觉。
    failAll(Status::Fail(ErrorCode::kCancelled, "控制连接已关闭"));
}

}   // namespace relayer::core::net
