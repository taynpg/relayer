#include "relayer/core/protocol/Pairing.h"

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

using relayer::protocol::answerForAsk;
using relayer::protocol::FrameType;
using relayer::protocol::isAnswerType;
using relayer::protocol::isAskType;

// 这张表是"应答该交给谁"的唯一依据。错了不会崩，只会让应答被当成未知帧
// 丢掉、请求方等满超时——所以用它自己的不变量来测，而不是逐条抄断言。

namespace {

// 全部"请求"帧。新增请求忘登记会在这里被要求补上（见最后一条用例）。
const std::vector<FrameType> kAllAsks = {
    FrameType::kAskId,          FrameType::kClaimId,         FrameType::kAskHome,       FrameType::kAskHomeAndDriver,
    FrameType::kAskFileList,    FrameType::kAskFileMeta,     FrameType::kAskPathMeta,   FrameType::kAskDelete,
    FrameType::kAskSha256,      FrameType::kAskRename,       FrameType::kAskCreateDir,  FrameType::kAskArchive,
    FrameType::kAskUnarchive,   FrameType::kAskFileSamples,  FrameType::kAskFilePull,   FrameType::kAskArchivePull,
    FrameType::kAskArchivePack, FrameType::kAskArchiveReady, FrameType::kAskClientList, FrameType::kAskHeart,
    FrameType::kAskDataChannel, FrameType::kDirectRequestId,
};

}   // namespace

TEST_CASE("每个请求都能映射到一个应答，且那个类型确实被认定为应答", "[pairing]")
{
    std::set<FrameType> answers;
    for (const auto ask : kAllAsks) {
        INFO("ask = " << relayer::protocol::toString(ask));
        REQUIRE(isAskType(ask));
        const auto ans = answerForAsk(ask);
        REQUIRE(ans.has_value());
        REQUIRE(isAnswerType(*ans));
        answers.insert(*ans);
    }
    REQUIRE_FALSE(answers.empty());
}

TEST_CASE("请求与应答的集合不相交", "[pairing]")
{
    // 一个类型不能既当请求又当应答：ControlLink 靠这个判断"这一帧是来问我的
    // 还是回我的"，两者都成立就永远分不清。
    for (const auto ask : kAllAsks) {
        REQUIRE_FALSE(isAnswerType(ask));
    }
}

TEST_CASE("非请求帧不能被当成请求", "[pairing]")
{
    // 认证帧有自己的状态机；传输会话管理帧由 TransferPeer 按 sessionId 认领；
    // 分块帧更不参与问答。它们进请求表就会被要求"回一个应答"，那是凭空
    // 造协议。
    for (const auto t : {FrameType::kAuthChallenge, FrameType::kAuthResponse, FrameType::kAuthResult, FrameType::kAuthAbort,
                         FrameType::kFileRequestSend, FrameType::kFileAnswerSend, FrameType::kChunkRequestData,
                         FrameType::kChunkAnswerData, FrameType::kNotifyClientList}) {
        INFO("type = " << relayer::protocol::toString(t));
        REQUIRE_FALSE(isAskType(t));
    }
}

TEST_CASE("两个请求共用一个应答的特例", "[pairing]")
{
    // 这两个是"看规律推配对"会踩的坑，单独钉住。
    REQUIRE(answerForAsk(FrameType::kClaimId) == FrameType::kAnswerId);
    REQUIRE(answerForAsk(FrameType::kAskArchivePull) == FrameType::kAnswerFilePull);

    // 135/136 正好和"偶数问奇数答"相反，靠奇偶推断的实现会在这里错。
    REQUIRE(answerForAsk(FrameType::kAskFilePull) == FrameType::kAnswerFilePull);
}

TEST_CASE("新增请求帧必须登记到配对表", "[pairing]")
{
    // 这条用例是给未来的人看的：`FrameType` 里加一个 kAskXxx 却忘了在
    // `answerForAsk` 里登记，ControlLink 会把它当成"对端请求"转给业务层，
    // 而业务层却在等应答——症状是"点了没反应，十秒后超时"。
    //
    // 全量扫描比人眼可靠：把 100..199 里所有以 Ask 开头的类型列出来，
    // 逐个要求它在表里。
    for (std::uint16_t raw = 100; raw <= 199; ++raw) {
        const auto t = static_cast<FrameType>(raw);
        const std::string name = std::string(relayer::protocol::toString(t));
        if (name.rfind("ask_", 0) == 0) {
            INFO("未登记的请求帧: " << name << " (#" << raw << ")");
            REQUIRE(answerForAsk(t).has_value());
        }
    }
}
