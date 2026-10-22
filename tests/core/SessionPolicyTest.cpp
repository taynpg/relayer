#include "relayer/core/session/SessionPolicy.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "relayer/core/auth/Handshake.h"

using namespace relayer::core::session;
using namespace relayer::protocol;

// 这一层是"漏一个分支就是安全漏洞"的地方，所以测试不追求覆盖率的数字，
// 而追求**把每个分支的后果钉住**：每条 REQUIRE 都能回答"如果这里改错了，
// 会发生什么坏事"。

namespace
{

Frame makeFrame(FrameType type, const std::string &from = {}, const std::string &to = {})
{
    Frame f;
    f.type = type;
    f.from = from;
    f.to = to;
    return f;
}

RouteConfig serverConfig()
{
    RouteConfig c;
    c.serverId = "relayd";
    return c;
}

}  // namespace

// ===========================================================================
// 入站：未认证连接
// ===========================================================================

TEST_CASE("unauthenticated peer may only send auth frames", "[session][inbound]")
{
    // 认证段的四个帧必须放行，否则握手根本没法进行。
    for (const auto t :
         {FrameType::kAuthChallenge, FrameType::kAuthResponse, FrameType::kAuthResult, FrameType::kAuthAbort})
    {
        const auto d = checkInbound(Role::kServer, false, makeFrame(t));
        REQUIRE(d.action == InboundAction::kAccept);
        REQUIRE(d.accepted());
    }
}

TEST_CASE("unauthenticated business frame is rejected, not ignored", "[session][inbound]")
{
    // 关键点：kReject 而不是 kIgnore。
    //
    // 静默丢弃会让对端一直重试，而"我以为握手完了、你以为没完"这个认知
    // 差异不会自己消失。断开是唯一能让用户立刻看到问题的方式。
    const auto d = checkInbound(Role::kServer, false, makeFrame(FrameType::kAskFileList));
    REQUIRE(d.action == InboundAction::kReject);
    REQUIRE_FALSE(d.accepted());
    REQUIRE_FALSE(d.reason.empty());

    // 拒绝原因要写清楚是哪一帧、在哪一侧，否则排查时只能靠猜。
    // 注意这里用的是线格式名（ask_file_list），不是枚举名（kAskFileList）：
    // protocol::toString() 返回的是线格式名，日志里也是这个。
    REQUIRE(d.reason.find("ask_file_list") != std::string::npos);
    REQUIRE(d.reason.find("server") != std::string::npos);
}

TEST_CASE("every non-auth frame type is rejected before auth", "[session][inbound]")
{
    // 逐个类型过一遍，而不是只抽查一两个。
    //
    // 只测一两个的话，"某个类型被误放进 isAllowedBeforeAuth"这类 bug
    // 完全测不出来，而那正好是"不提供口令就能枚举文件列表"的入口。
    for (std::uint16_t raw = 0; raw <= 700; ++raw)
    {
        const auto t = static_cast<FrameType>(raw);
        if (!isKnownType(raw))
        {
            continue;
        }
        const auto d = checkInbound(Role::kServer, false, makeFrame(t));

        if (isAuthType(t) && t != FrameType::kAskHeart && t != FrameType::kAnswerHeart)
        {
            REQUIRE(d.action == InboundAction::kAccept);
        }
        else if (t == FrameType::kAskHeart)
        {
            REQUIRE(d.action == InboundAction::kAnswerHeart);
        }
        else if (t == FrameType::kAnswerHeart)
        {
            REQUIRE(d.action == InboundAction::kIgnore);
        }
        else
        {
            REQUIRE(d.action == InboundAction::kReject);
        }
    }
}

// ===========================================================================
// 入站：心跳的例外
// ===========================================================================

TEST_CASE("heartbeat is answered regardless of auth state", "[session][inbound][heartbeat]")
{
    // 心跳检测的是"这条 TCP 还活着"，与身份无关。
    //
    // 未认证时也应答，是为了把两种故障区分开：
    //   - TCP 断了          → 心跳超时，transport 判失联
    //   - TCP 通但握手卡住   → 心跳正常，但认证超时
    // 如果未认证时不答心跳，第二种会表现成第一种，排查方向完全错了。
    for (const bool established : {false, true})
    {
        for (const auto role : {Role::kServer, Role::kClient})
        {
            const auto d = checkInbound(role, established, makeFrame(FrameType::kAskHeart));
            REQUIRE(d.action == InboundAction::kAnswerHeart);
            REQUIRE(d.accepted());
        }
    }
}

TEST_CASE("heartbeat answer is ignored, never rejected", "[session][inbound][heartbeat]")
{
    // 应答本身不需要业务处理——transport 层的 HeartbeatPolicy 已经因为
    // "收到字节"而清掉了未应答标记。但它也绝不能走到"未认证就拒"分支。
    for (const bool established : {false, true})
    {
        const auto d = checkInbound(Role::kServer, established, makeFrame(FrameType::kAnswerHeart));
        REQUIRE(d.action == InboundAction::kIgnore);
        // kIgnore 不算 accepted（上层不该把它交给业务），但也不该断开。
        REQUIRE_FALSE(d.accepted());
    }
}

// ===========================================================================
// 入站：已认证
// ===========================================================================

TEST_CASE("established connection accepts business frames", "[session][inbound]")
{
    for (const auto t : {FrameType::kAskId, FrameType::kFileRequestSend, FrameType::kChunkRequestData})
    {
        const auto d = checkInbound(Role::kServer, true, makeFrame(t));
        REQUIRE(d.action == InboundAction::kAccept);
        REQUIRE(d.accepted());
    }
}

TEST_CASE("established connection still accepts re-auth frames", "[session][inbound]")
{
    // 已认证之后又收到 Challenge：状态机会在自己的相位检查里拒掉它
    // （ServerHandshake 在 kEstablished 相位收到非 Response 帧直接失败）。
    //
    // 策略层在这里放行是有意的：相位判定属于状态机，两处都判会让"到底谁
    // 负责拒"变得模糊，出问题时不知道该看哪一层。
    const auto d = checkInbound(Role::kServer, true, makeFrame(FrameType::kAuthChallenge));
    REQUIRE(d.action == InboundAction::kAccept);
}

// ===========================================================================
// 身份绑定
// ===========================================================================

TEST_CASE("only an established server binds identity", "[session][identity]")
{
    // 四种组合里只有一种该绑。
    REQUIRE(identityToBind(Role::kServer, true, "client-7") == "client-7");

    // 未认证的服务端：没有可信身份可言。
    REQUIRE(identityToBind(Role::kServer, false, "client-7").empty());

    // 客户端无论认证与否都不绑——它没有"别人的身份"可用。
    REQUIRE(identityToBind(Role::kClient, true, "client-7").empty());
    REQUIRE(identityToBind(Role::kClient, false, "client-7").empty());
}

TEST_CASE("bind result equals what bindSenderIdentity writes", "[session][identity]")
{
    // 策略层算出"该绑成谁"，实际落笔走 bindSenderIdentity()。
    // 这两处一旦不一致，就会出现"判定说绑了、实际没绑"的漏洞。
    const auto id = identityToBind(Role::kServer, true, "client-7");
    REQUIRE_FALSE(id.empty());

    Frame f = makeFrame(FrameType::kAskFileList, "forged-id", "other");
    relayer::core::auth::bindSenderIdentity(f, id);

    REQUIRE(f.from == "client-7");
    REQUIRE(f.from == id);
}

TEST_CASE("applyIdentityBinding overwrites forged from", "[session][identity]")
{
    Frame f = makeFrame(FrameType::kAskFileList, "alice", "client-9");
    const auto written = applyIdentityBinding(Role::kServer, true, f, "bob");
    REQUIRE(written == "bob");
    REQUIRE(f.from == "bob");
}

TEST_CASE("applyIdentityBinding leaves from untouched when unauthenticated", "[session][identity]")
{
    // 关键：不改，也不清空。
    //
    // 清空看起来"更安全"，但会毁掉排查证据——"对端自称 alice"这件事本身
    // 在分析伪造行为时是有用的信息。而且清空会让"到底有没有绑过"变得
    // 无法判断。
    Frame f = makeFrame(FrameType::kAskFileList, "alice", "client-9");
    const auto written = applyIdentityBinding(Role::kServer, false, f, "bob");
    REQUIRE(written.empty());
    REQUIRE(f.from == "alice");
}

// ===========================================================================
// 出站
// ===========================================================================

TEST_CASE("server may emit frames with empty or own from", "[session][outbound]")
{
    // 服务端主动发起的帧（应答、通知）不代表任何客户端，from 为空是正常的。
    REQUIRE(checkOutbound(Role::kServer, makeFrame(FrameType::kAnswerId), "relayd").allowed);
    // 显式署名服务端自己也行。
    REQUIRE(checkOutbound(Role::kServer, makeFrame(FrameType::kAnswerId, "relayd"), "relayd").allowed);
}

TEST_CASE("server must not emit frames with a foreign from", "[session][outbound]")
{
    // 这是"转发时漏调 bindSenderIdentity"的兜底断言。
    //
    // 入站侧拦不住这种情况：入站时 from="alice" 完全合法。问题出在把它
    // 原样发给了 C——C 会认为文件来自 alice，而实际上它来自 bob（bob 只是
    // 把自己的 from 写成了 alice）。
    const auto d = checkOutbound(Role::kServer, makeFrame(FrameType::kAskFileList, "alice"), "relayd");
    REQUIRE_FALSE(d.allowed);
    REQUIRE(d.reason.find("alice") != std::string::npos);
    REQUIRE(d.reason.find("relayd") != std::string::npos);
}

TEST_CASE("client outbound is not identity-checked", "[session][outbound]")
{
    // 客户端的越权在服务端入站侧被覆盖，不在这里补一道让调用方困惑的规则。
    REQUIRE(checkOutbound(Role::kClient, makeFrame(FrameType::kAskFileList, "anything"), "relayd").allowed);
    REQUIRE(checkOutbound(Role::kClient, makeFrame(FrameType::kAskFileList, ""), "relayd").allowed);
}

// ===========================================================================
// 选路
// ===========================================================================

TEST_CASE("empty to means the server itself", "[session][route]")
{
    // 客户端在拿到自己的 id 之前根本不知道服务端叫什么（kAskId 就是用来
    // 问这个的），所以这些帧必须允许 to 为空。
    const auto plan =
        planRoute(Role::kServer, true, serverConfig(), "client-7", makeFrame(FrameType::kAskId, "client-7"));
    REQUIRE(plan.action == RouteAction::kAcceptLocal);
    REQUIRE(plan.to.empty());
}

TEST_CASE("explicit server id also routes locally", "[session][route]")
{
    const auto plan = planRoute(Role::kServer, true, serverConfig(), "client-7",
                                makeFrame(FrameType::kAskHome, "client-7", "relayd"));
    REQUIRE(plan.action == RouteAction::kAcceptLocal);
}

TEST_CASE("frame for another client is forwarded", "[session][route]")
{
    const auto plan = planRoute(Role::kServer, true, serverConfig(), "client-7",
                                makeFrame(FrameType::kFileRequestSend, "client-7", "client-9"));
    REQUIRE(plan.action == RouteAction::kForward);
    REQUIRE(plan.to == "client-9");
    REQUIRE(plan.from == "client-7");
}

TEST_CASE("routing to self is rejected", "[session][route]")
{
    // client-7 让服务端把帧转给 client-7：只可能是拼错了 to，或者在试探
    // 服务端会不会把帧绕回去。两种都不该转发。
    const auto plan = planRoute(Role::kServer, true, serverConfig(), "client-7",
                                makeFrame(FrameType::kFileRequestSend, "client-7", "client-7"));
    REQUIRE(plan.action == RouteAction::kReject);
    REQUIRE_FALSE(plan.reason.empty());
}

TEST_CASE("forwarding never preserves a claimed third-party identity", "[session][route][identity]")
{
    // 这是 v1 的伪造漏洞的最小复现：bob 把自己写成 alice，服务端若原样
    // 转发，client-9 就会相信文件来自 alice。
    const auto plan = planRoute(Role::kServer, true, serverConfig(), "bob",
                                makeFrame(FrameType::kFileRequestSend, "alice", "client-9"));
    REQUIRE(plan.action == RouteAction::kForward);
    // 目标照原样，但 from 必须是 bob——不是 alice。
    REQUIRE(plan.to == "client-9");
    REQUIRE(plan.from == "bob");
}

TEST_CASE("route self-check uses authenticated id, not claimed from", "[session][route][identity]")
{
    // 这条测试钉的是一个非常隐蔽的漏洞：
    //
    // 如果 planRoute 用 f.from 判断"是不是发给自己"，攻击者只要把 from
    // 写成自己的名字，就能让本该转发的帧被本地受理。
    //
    // 这里 bob 声称自己是 carol，同时 to 也写 carol。正确行为仍然是**转发**
    // （因为真实身份 bob ≠ carol），而不是判成"发给自己"。
    const auto plan =
        planRoute(Role::kServer, true, serverConfig(), "bob", makeFrame(FrameType::kFileRequestSend, "carol", "carol"));
    REQUIRE(plan.action == RouteAction::kForward);
    REQUIRE(plan.to == "carol");
    // 而 from 必须被覆写成真实身份 bob，不能是它自称的 carol。
    REQUIRE(plan.from == "bob");
}

TEST_CASE("forwarding before auth cannot bind any identity", "[session][route][identity]")
{
    // 未认证时 identityToBind 返回空，plan.from 也就为空。
    // 调用方据此知道"不该调用 bindSenderIdentity"，而应该直接拒绝转发。
    const auto plan = planRoute(Role::kServer, false, serverConfig(), "bob",
                                makeFrame(FrameType::kFileRequestSend, "alice", "client-9"));
    REQUIRE(plan.from.empty());
}

TEST_CASE("client route plan is always local", "[session][route]")
{
    // 客户端上没有转发概念。
    const auto plan =
        planRoute(Role::kClient, true, serverConfig(), "", makeFrame(FrameType::kAskFileList, "", "relayd"));
    REQUIRE(plan.action == RouteAction::kAcceptLocal);
}

// ===========================================================================
// 名称稳定性
// ===========================================================================

TEST_CASE("session policy names are stable", "[session]")
{
    // 这些字符串会进日志。改了它们会让既有的日志检索规则失效。
    REQUIRE(toString(Role::kServer) == "server");
    REQUIRE(toString(Role::kClient) == "client");
    REQUIRE(toString(InboundAction::kAccept) == "accept");
    REQUIRE(toString(InboundAction::kAnswerHeart) == "answer_heart");
    REQUIRE(toString(InboundAction::kReject) == "reject");
    REQUIRE(toString(InboundAction::kIgnore) == "ignore");
    REQUIRE(toString(RouteAction::kForward) == "forward");
    REQUIRE(toString(RouteAction::kAcceptLocal) == "accept_local");
    REQUIRE(toString(RouteAction::kReject) == "reject");
}
