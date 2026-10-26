#include "relayer/core/session/SessionPolicy.h"

namespace relayer::core::session
{

std::string_view toString(Role r) noexcept
{
    switch (r)
    {
    case Role::kServer:
        return "server";
    case Role::kClient:
        return "client";
    }
    return "unknown";
}

std::string_view toString(InboundAction a) noexcept
{
    switch (a)
    {
    case InboundAction::kAccept:
        return "accept";
    case InboundAction::kAnswerHeart:
        return "answer_heart";
    case InboundAction::kReject:
        return "reject";
    case InboundAction::kIgnore:
        return "ignore";
    }
    return "unknown";
}

std::string_view toString(RouteAction a) noexcept
{
    switch (a)
    {
    case RouteAction::kForward:
        return "forward";
    case RouteAction::kAcceptLocal:
        return "accept_local";
    case RouteAction::kReject:
        return "reject";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 入站
// ---------------------------------------------------------------------------

InboundDecision checkInbound(Role role, bool established, const Frame &f) noexcept
{
    // --- 第一步：心跳 ---
    //
    // 放在认证检查之前，理由见头文件：心跳检测的是"TCP 还活着"，与身份无关。
    // 未认证连接上收到心跳仍然应答，这样"TCP 通了但握手卡住"能被观测成
    // 一个明确的认证超时，而不是笼统的"连接莫名其妙断了"。
    if (f.type == FrameType::kAskHeart)
    {
        return {InboundAction::kAnswerHeart, {}};
    }

    // 对端回的心跳应答。它的价值在 transport 层的 HeartbeatPolicy 里已经被
    // 用掉了（收到任何字节都会清掉未应答标记），到这里就不用再做什么。
    // 明确写出来而不是落到下面的"未认证就拒"分支：未认证连接上收到应答
    // 同样合法。
    if (f.type == FrameType::kAnswerHeart)
    {
        return {InboundAction::kIgnore, {}};
    }

    if (established)
    {
        return {InboundAction::kAccept, {}};
    }

    // --- 未认证：只认认证帧 ---
    if (protocol::isAuthType(f.type))
    {
        return {InboundAction::kAccept, {}};
    }

    // 能发业务帧说明对端自认为握手已经结束，而我们没有。相位不一致，
    // 断开比继续等更快让用户看到问题。
    //
    // 注意这里是 kReject 而不是 kIgnore：静默丢弃会让对端一直重试，
    // 而双方对"握手到哪一步了"的认知差异不会自己消失。
    std::string reason = "unauthenticated peer sent non-auth frame ";
    reason.append(protocol::toString(f.type));
    reason.append(" on ");
    reason.append(toString(role));
    return {InboundAction::kReject, std::move(reason)};
}

// ---------------------------------------------------------------------------
// 身份绑定
// ---------------------------------------------------------------------------

std::string_view identityToBind(Role role, bool established, std::string_view assignedId) noexcept
{
    if (role != Role::kServer)
    {
        // 客户端不转发别人的帧，也就没有"别人的身份"需要绑定。
        // 它自己发的帧的 from 由服务端在入站侧覆盖。
        return {};
    }
    if (!established)
    {
        // 还没认证就没有可信身份。此时 from 只能是对方的自称，
        // 任何"信任它"的代码路径都不该存在（PROTOCOL.md §7.6）。
        return {};
    }
    return assignedId;
}

std::string_view applyIdentityBinding(Role role, bool established, Frame &f, std::string_view assignedId) noexcept
{
    const auto id = identityToBind(role, established, assignedId);
    if (id.empty())
    {
        // 不动 from。注意这里也不要"顺手清空"它：清空会掩盖上游到底
        // 有没有把自称身份带上，而那个信息在排查伪造行为时是证据。
        return {};
    }
    auth::bindSenderIdentity(f, id);
    return id;
}

// ---------------------------------------------------------------------------
// 出站
// ---------------------------------------------------------------------------

OutboundDecision checkOutbound(Role role, const Frame &f, std::string_view serverId)
{
    // 客户端：不检查。它只能代表自己，越权的那一步发生在服务端入站侧覆盖，
    // 而不是在这里补一道让调用方困惑的规则。
    if (role != Role::kServer)
    {
        return {true, {}};
    }

    // 服务端：不允许以自己的名义发出别人的帧。
    //
    // 这条规则存在的原因是：转发时若漏调 bindSenderIdentity()，帧会带着
    // 发送方的自称身份原样出去。入站侧的检查拦不住这种情况（入站时确实
    // 是合法的），所以必须有一个出站侧的断言。
    //
    // 判断方式很直接：服务端自己发出的帧，from 要么为空（服务端主动发起
    // 的请求/应答，它不代表任何客户端），要么就是服务端自己的 id。
    if (f.from.empty())
    {
        return {true, {}};
    }
    if (f.from == serverId)
    {
        return {true, {}};
    }

    std::string reason = "server is about to emit frame ";
    reason.append(protocol::toString(f.type));
    reason.append(" with foreign from=\"");
    reason.append(f.from);
    reason.append("\" (server id is \"");
    reason.append(serverId);
    reason.append("\"); identity binding was probably skipped");
    return {false, std::move(reason)};
}

// ---------------------------------------------------------------------------
// 选路
// ---------------------------------------------------------------------------

RoutePlan planRoute(Role role, bool established, const RouteConfig &config, std::string_view selfId, const Frame &f)
{
    RoutePlan plan;

    // 这里的 selfId 是**对端在这条连接上的身份**，不是 f.from。
    //
    // 混淆这两个东西会导致一个很隐蔽的漏洞：f.from 是对端"自称"的，
    // 它可以随便写别人的名字。若拿 f.from 判断"是不是发给自己"，攻击者
    // 只要把 from 写成自己的名字，就能让本该转发的帧变成"发给自己"从而
    // 被本地受理。所以 selfId 只能来自认证阶段分配的身份。

    if (role != Role::kServer)
    {
        // 客户端上没有转发概念：它发出的每一帧都是给服务端的。
        plan.action = RouteAction::kAcceptLocal;
        plan.from = identityToBind(role, established, selfId);
        return plan;
    }

    // --- 以下都是服务端 ---

    // 空 to：发给服务端自己。问 id、问家目录、心跳这些都属于这一类。
    //
    // 为什么把"空 to"当成本地而不是"无法投递"：客户端在拿到自己的 id
    // 之前根本不知道服务端叫什么（kAskId 就是用来问这个的），所以这些
    // 帧必须允许 to 为空。要求客户端先填 to 会形成鸡生蛋问题。
    if (f.to.empty())
    {
        plan.action = RouteAction::kAcceptLocal;
        plan.from = identityToBind(role, established, selfId);
        return plan;
    }

    if (!config.serverId.empty() && f.to == config.serverId)
    {
        plan.action = RouteAction::kAcceptLocal;
        plan.from = identityToBind(role, established, selfId);
        return plan;
    }

    if (!selfId.empty() && f.to == selfId)
    {
        // 发给自己的帧：**转发回这条连接**，不是拒绝，也不是本地受理。
        //
        // 这是 v1 支持、用户明确要求保留的场景（"本机自测"：一个实例把
        // 文件发给自己，用来验证整条链路——中继、认证、传输、落盘——不用
        // 第二台机器）。早期版本把它判成 kReject，理由是"只可能是拼错了
        // to"，那是把猜测当成了规则：用户真的会这么用。
        //
        // 安全性：这里的 selfId 来自认证阶段，不是 f.from，所以发送者
        // 只可能把帧绕回**自己**，偷不到别人的帧、也干扰不到别人；转发
        // 时 from 仍会被绑成真实身份（见 forward()），伪造依然不可能。
        //
        // 注意不能用 kAcceptLocal：本地受理等于交给服务端处理，而服务端
        // 不做文件业务（docs/DESIGN.md §3.1），帧会在这里凭空消失——比
        // 明确拒绝更难排查。
        plan.action = RouteAction::kForward;
        plan.to = f.to;
        plan.from = identityToBind(role, established, selfId);
        return plan;
    }

    plan.action = RouteAction::kForward;
    plan.to = f.to;
    // 落笔仍然由调用方走 applyIdentityBinding()。这里返回的 from 就是
    // 那个函数会写进去的值——两处不一致会立刻被测试抓到。
    plan.from = identityToBind(role, established, selfId);
    return plan;
}

}  // namespace relayer::core::session
