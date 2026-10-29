#pragma once

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/core/protocol/Frame.h"

namespace relayer::core::session {
class Session;
}

namespace relayer::core::net {

// 常驻控制连接（v1 `ControlSession` 的现代版）。
//
// 一条长连接负责所有**控制类**交互：在线表、目录列举、元信息、文件操作、
// 数据通道协商……每个请求带一个 64 位请求号（帧头的 `index`），应答回显
// 同一个号，按它配对回调。于是**多个请求可以同时在飞**——这正是 v2
// "一次一操作、忙就拒绝"最卡手的地方。
//
// 文件数据**不走这里**：那是 TransferLink 的事（双连接模型）。控制连接上
// 只有小帧，所以永远不会被大块数据堵在写队列里，传输期间照样能浏览、
// 能取消、能看对端列表。
//
// 线程模型：所有方法都只能在持有 io_context 的线程上调用（与 Session 的
// 单线程契约一致）。回调也都在那个线程上触发，所以回调里可以直接发下一个
// 请求。
//
// 与 v1 的差异：
//   - v1 用 QMap + 互斥锁 + 条件变量跨线程等结果；这里是纯 io 线程回调，
//     没有锁，也不阻塞（阻塞等结果会让心跳停摆）。
//   - v1 的请求号来自 C++ 侧自增的 `sessionId`，混在业务字段里；这里用
//     帧头已有的 `index`，协议层本来就规定它是"请求/应答同号"。
class ControlLink : public std::enable_shared_from_this<ControlLink>
{
public:
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 9527;
        std::string password;

        // 明知服务端关了认证时必须显式置位（与 CLI 的 --no-auth 同一条原则：
        // 不从"口令为空"推断，否则漏填口令会静默降级成不认证）。
        bool noAuth = false;

        // 要声明的身份（PROTOCOL.md §11.7）。空 = 由服务端分配。
        std::string requestedId;

        // 单个请求的应答超时。控制类请求都是小帧，10 秒足够；
        // 对端离线时靠它收尾，不能靠"连接还活着"——中继会静默丢弃
        // 投给不在线对端的帧。
        std::chrono::seconds askTimeout{10};

        // 同时在飞的请求上限。超了 ask() 直接返回 false，由调用方决定
        // 是排队还是提示。
        //
        // 不是越大越好：每个在飞请求占一个计时器，而且对端可能串行处理。
        // 256 远超 GUI 的实际并发（列举 + 若干元信息查询）。
        std::size_t maxInFlight = 256;
    };

    // 一次请求的结论。ok=false 时 status 说明原因（超时 / 连接断开 /
    // 应答类型不匹配）；ok=true 时 body 是应答体。
    struct Answer {
        bool ok = false;
        Status status;
        std::vector<char> body;
    };
    using AnswerFn = std::function<void(const Answer&)>;

    struct Handler {
        // 连接可用（认证完成）。
        //
        // **可能被调用多次**：Session 在认证刚过时会通知一次（此时 id 可能
        // 还是空的），拿到真实 id 后再通知一次。调用方应当幂等——只把
        // assignedId() 更新到界面，不要在这里做"只该发生一次"的动作。
        std::function<void(std::string assignedId)> onReady;

        // 连接结束（用户关闭、认证失败、中继断开、心跳超时）。保证只一次。
        std::function<void(Status why)> onClosed;

        // 对端发来的请求。填 answer 的 type 与 body 即可——`to` 与 `index`
        // 由 ControlLink 负责（回显请求号是配对的唯一依据，不能让业务漏填）。
        //
        // 返回 false = 不认识这条请求。此时**不回任何应答**：对端会等满它
        // 自己的超时，但它至少能确定"这台对端不认识"，而不是收到一个假的
        // 成功。任何模式下都必须应答的帧（列举、取文件）由上层决定要不要
        // 注册，不由本类替它决定。
        std::function<bool(const protocol::Frame& req, protocol::Frame& answer)> onPeerRequest;

        // 既不是请求也不是应答的帧（例如服务端的 kNotifyClientList）。
        std::function<void(const protocol::Frame& f)> onNotice;
    };

    static std::shared_ptr<ControlLink> connect(asio::io_context& ctx, Config config, Handler handler);
    ~ControlLink();

    ControlLink(const ControlLink&) = delete;
    ControlLink& operator=(const ControlLink&) = delete;

    // 发一个请求并等应答。
    //
    // 返回 false = **没有发出去**（未就绪 / 请求类型不是 ask / 超过并发
    // 上限 / 底层发送失败）。此时 done **不会被调用**。
    //
    // 返回 true 后 done 一定会被调用恰好一次：成功、超时、连接断开、应答
    // 类型不符，四条路径都归一到 done。
    [[nodiscard]] bool ask(protocol::FrameType askType, std::string to, std::vector<char> body, AnswerFn done);

    // 发一条不需要应答的帧（通知）。失败只表达"没写进发送队列"。
    [[nodiscard]] Status post(protocol::Frame frame);

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }
    [[nodiscard]] const std::string& assignedId() const noexcept
    {
        return assignedId_;
    }
    [[nodiscard]] std::size_t inFlight() const noexcept
    {
        return pending_.size();
    }

    // 主动关闭。挂起的请求全部以 kCancelled 收尾，onClosed 随后触发。
    void close();

private:
    ControlLink(asio::io_context& ctx, Config config, Handler handler);

    struct Pending {
        // steady_timer 必须带 io_context 构造，所以 Pending 没有默认构造。
        Pending(asio::io_context& ctx, protocol::FrameType expectedType, AnswerFn callback)
            : expected(expectedType), done(std::move(callback)), timer(ctx)
        {
        }

        protocol::FrameType expected;
        AnswerFn done;
        // 每个请求一个计时器：在飞请求很少（GUI 个位数），换来的是
        // 互不干扰的独立超时，比一条共享的扫描链简单得多。
        asio::steady_timer timer;
    };

    void onSessionReady(std::string assignedId);
    void onSessionFrame(const protocol::Frame& f);
    void onSessionClosed(Status why);
    void deliver(std::uint64_t id, bool ok, Status status, std::vector<char> body);
    void failAll(const Status& why);

    asio::io_context& ctx_;
    Config config_;
    Handler handler_;

    std::shared_ptr<session::Session> session_;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::uint64_t nextRequestId_ = 1;

    bool ready_ = false;
    bool closing_ = false;
    std::string assignedId_;
};

}   // namespace relayer::core::net
