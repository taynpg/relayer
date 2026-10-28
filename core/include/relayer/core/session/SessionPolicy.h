#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "relayer/common/Status.h"
#include "relayer/core/auth/Handshake.h"
#include "relayer/core/protocol/Frame.h"

// 会话策略：一条连接上"哪些帧能过、过了要做什么手术"（PROTOCOL.md §7.6 / §8.6）。
//
// 和 HeartbeatPolicy 一样，这里**不含 I/O**：输入是收到的帧或要发出的帧，
// 输出是判定结果。谁读 socket、判定失败由谁断开、日志写哪里——全在
// Session（会做 I/O）与更上层的 app。
//
// 这样切的理由和握手状态机一样，而且更硬：
//
//   转发路径上的判定是"漏一个分支就是安全漏洞"的地方。混在 socket 回调里
//   写，测试就得起端口、造对端、等超时——于是没人写全部分支的测试。
//   抽成纯函数之后，每一种（相位 × 帧类型 × 字段）组合都能单独钉住。
//
// 本文件回答四个问题：
//
//   1. 这个帧现在能不能收？        checkInbound()
//   2. 收下的业务帧要不要改写身份？  shouldBindIdentity()
//   3. 发出去的帧能不能带假身份？    checkOutbound()
//   4. 这个帧能不能转发、t0 是谁？  planRoute()
//
// 四者都刻意做成**独立且有名字的**判定，而不是塞进一个大的 if-else：
// 每个都有"写反就是漏洞"的具体后果，值得各自被测试点名。

namespace relayer::core::session {

using auth::AuthStatus;
using auth::ClientPhase;
using auth::ServerPhase;
using protocol::Frame;
using protocol::FrameType;

// ---------------------------------------------------------------------------
// 连接方向
// ---------------------------------------------------------------------------
//
// 为什么不分"服务端模式 / 客户端模式"两套策略：两个方向的规则至少三处
// 不同（心跳谁回、身份绑定谁做、未认证时谁先说话），分两套就会不可避免地
// 出现"改了一边忘了另一边"。用方向参数化一个策略，差异集中在一处。
enum class Role {
    kServer,   // 接受连接的一方：负责握手、转发、身份绑定
    kClient,   // 发起连接的一方：只发自己的业务帧，不转发
};

[[nodiscard]] std::string_view toString(Role r) noexcept;

// t0 的取值。服务端自己的 id 由上层配置给出（"服务端在协议里的身份"）。
struct RouteConfig {
    std::string serverId;   // Role::kServer 时必填
};

// ---------------------------------------------------------------------------
// 入站判定
// ---------------------------------------------------------------------------

enum class InboundAction {
    kAccept,        // 收下：交给上层业务，或交给握手状态机
    kAnswerHeart,   // 心跳询问，回一个应答（不需要经过业务层）
    kReject,        // 协议违规：发 abort（若能发）并断开
    kIgnore,        // 静默丢弃，不断开
};

[[nodiscard]] std::string_view toString(InboundAction a) noexcept;

// 判定结果。reason 只在 kReject / kIgnore 时有内容，用于日志。
struct InboundDecision {
    InboundAction action = InboundAction::kReject;
    std::string reason;

    [[nodiscard]] bool accepted() const noexcept
    {
        return action == InboundAction::kAccept || action == InboundAction::kAnswerHeart;
    }
};

// 收到的这一帧能不能处理。
//
// 相位语义：
//   kServer: ServerPhase::kEstablished 之前只允许认证帧
//   kClient: ClientPhase::kEstablished 之前只允许认证帧
//
// 为什么未认证时收到业务帧要**断开**而不是丢弃：能发业务帧说明对端已经
// 认为握手结束了。我们这边还没结束，说明两人的相位不一致——继续等下去
// 只会让双方各说各话。断开是最快让用户看到"连不上"的方式。
//
// 唯一的例外是心跳：心跳在握手完成前后都该被应答，因为它检测的是"这条
// TCP 连接还活着"，与身份无关。让未认证连接也能维持心跳，可以把
// "TCP 通但认证卡住"和"TCP 断了"区分开——这两种故障的排查方向完全不同。
[[nodiscard]] InboundDecision checkInbound(Role role, bool established, const Frame& f) noexcept;

// ---------------------------------------------------------------------------
// 身份绑定
// ---------------------------------------------------------------------------

// 执行 bindSenderIdentity 的身份。
//
// 两件事必须同时为真：
//   1. 我们是服务端——客户端不该转发别人的帧，它没有可信的"别人"可自称；
//   2. 我们已经完成认证——没认证就没有可信身份可言，此时 from 只能按
//      "对端自称"处理，而 PROTOCOL.md §7.6 要求这种自称不得进入转发路径。
//
// 返回空串表示不需要绑定（调用方不应改动 from）。
//
// 注意：调用方拿到非空结果后**必须**调 auth::bindSenderIdentity()，不要
// 自己写 f.from = x。那个函数存在的意义就是让这条规则只有一个落点。
[[nodiscard]] std::string_view identityToBind(Role role, bool established, std::string_view assignedId) noexcept;

// 按身份绑定规则改写帧的 from。
//
// 这是转发路径唯一该调用的落笔入口：内部先问 identityToBind()，需要绑就
// 交给 auth::bindSenderIdentity()。之所以还要这一层，是因为"该不该绑"
// 和"绑成谁"是两个独立决策，前者有单独测试；把它们合成一个函数体之后
// 就再也测不了"未认证时有没有偷偷绑"这类问题。
//
// 返回值是最终写进 f.from 的内容（空串表示没改）。
[[nodiscard]] std::string_view applyIdentityBinding(Role role, bool established, Frame& f, std::string_view assignedId) noexcept;

// ---------------------------------------------------------------------------
// 出站判定
// ---------------------------------------------------------------------------

struct OutboundDecision {
    bool allowed = false;
    std::string reason;
};

// 要发出去的帧能不能带它现在这个 from。
//
// 拦的是"从服务端流向客户端、却自称第三方"的帧：
//
//   A ──from=A──▶ 服务端 ──from=X──▶ B
//
// 服务端若把 A 的帧改写成 from=X 发出去，B 就会认为文件来自 X。这正是
// v1 的伪造漏洞，只是换了个位置实现——所以出站侧也要有一道判定，而不是
// 只靠"转发时记得改写"。
//
// 客户端的出站不检查身份：它只能代表自己，服务端会在入站侧覆盖。在客户端
// 再加一道检查只会让"我该填什么 from"变成一个要查文档的问题。
[[nodiscard]] OutboundDecision checkOutbound(Role role, const Frame& f, std::string_view serverId);

// ---------------------------------------------------------------------------
// 转发选路
// ---------------------------------------------------------------------------

enum class RouteAction {
    kForward,       // 转发给 route.to
    kAcceptLocal,   // 不需要转发，交给本地业务处理
    kReject,        // 无处可去
};

[[nodiscard]] std::string_view toString(RouteAction a) noexcept;

struct RoutePlan {
    RouteAction action = RouteAction::kReject;
    std::string to;   // kForward 时的目标 id
    std::string from;
    std::string reason;
};

// 这一帧该往哪去、from 该写成谁。
//
// 服务端的转发规则（PROTOCOL.md §8.6）：
//
//   to 为空          → kAcceptLocal（发给服务端自己：问 id、问家目录、心跳等）
//   to == 服务端 id   → kAcceptLocal（同上，只是显式写了地址）
//   to == 自己的 id   → kReject（自己转发给自己，只可能是 bug 或试探）
//   其余              → kForward
//
// 还有一条：`from` 最后**一定**由 identityToBind() 决定，RoutePlan::from
// 只是把"应该写成谁"算出来，真正落笔仍要走 bindSenderIdentity()。
[[nodiscard]] RoutePlan planRoute(Role role, bool established, const RouteConfig& config, std::string_view selfId,
                                  const Frame& f);

}   // namespace relayer::core::session
