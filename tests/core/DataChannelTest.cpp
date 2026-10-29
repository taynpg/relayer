#include "relayer/core/session/DataChannel.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using relayer::core::session::DataChannelStatus;
using relayer::core::session::decodeAnswerDataChannel;
using relayer::core::session::decodeAskDataChannel;
using relayer::core::session::encodeAnswerDataChannel;
using relayer::core::session::encodeAskDataChannel;
using relayer::core::session::kMaxDataChannelIdBytes;

// 这几条用例守的是同一个原则：**协商结果不能有歧义**。
// "成功但没给 id"这种半截状态一旦被放过，发送方会拿空串去路由，
// 中继静默丢弃，最后表现为"发送方等满超时"——离真正的原因隔了三层。

TEST_CASE("数据通道请求往返保真", "[datachannel]")
{
    const auto body = encodeAskDataChannel("client-7");
    REQUIRE_FALSE(body.empty());

    std::string id;
    std::string err;
    REQUIRE(decodeAskDataChannel(body, id, err));
    REQUIRE(id == "client-7");
}

TEST_CASE("数据通道请求允许空 id（= 我还没建好）", "[datachannel]")
{
    // 空 id 是合法的语义："先报个到，我的数据连接还没起来"。
    // 与"结构坏了"必须区分开——解码成功但 id 为空。
    const auto body = encodeAskDataChannel("");
    REQUIRE_FALSE(body.empty());

    std::string id = "stale";
    std::string err;
    REQUIRE(decodeAskDataChannel(body, id, err));
    REQUIRE(id.empty());
}

TEST_CASE("数据通道请求拒绝超长 id", "[datachannel]")
{
    REQUIRE(encodeAskDataChannel(std::string(kMaxDataChannelIdBytes + 1, 'x')).empty());

    // 恰好到上限仍然合法：边界不能差一。
    REQUIRE_FALSE(encodeAskDataChannel(std::string(kMaxDataChannelIdBytes, 'x')).empty());

    std::string id;
    std::string err;
    REQUIRE_FALSE(decodeAskDataChannel({}, id, err));
}

TEST_CASE("数据通道应答往返保真", "[datachannel]")
{
    std::vector<char> body;
    REQUIRE(encodeAnswerDataChannel(DataChannelStatus::kOk, "client-9", body).ok());

    DataChannelStatus status = DataChannelStatus::kRejected;
    std::string id;
    std::string err;
    REQUIRE(decodeAnswerDataChannel(body, status, id, err));
    REQUIRE(status == DataChannelStatus::kOk);
    REQUIRE(id == "client-9");
}

TEST_CASE("数据通道应答成功却不带 id 必须编码失败", "[datachannel]")
{
    std::vector<char> body;
    REQUIRE_FALSE(encodeAnswerDataChannel(DataChannelStatus::kOk, "", body).ok());
    REQUIRE(body.empty());
}

TEST_CASE("数据通道应答的解码侧同样拒绝成功无 id", "[datachannel]")
{
    // 手搓一个"status=0, 空 id"的包：编码器不会产出它，但网络对面可能是
    // 一个更宽松的实现。这种包必须当坏包，不能当成"成功"。
    const std::vector<char> forged = {0, 0};

    DataChannelStatus status = DataChannelStatus::kOk;
    std::string id;
    std::string err;
    REQUIRE_FALSE(decodeAnswerDataChannel(forged, status, id, err));
    REQUIRE(err.find("empty") != std::string::npos);
}

TEST_CASE("数据通道应答的非成功状态不带 id", "[datachannel]")
{
    for (const auto status : {DataChannelStatus::kNotReady, DataChannelStatus::kRejected}) {
        std::vector<char> body;
        // 传了 id 也写不进去：失败就是失败，别让上层拿着半个结果去用。
        REQUIRE(encodeAnswerDataChannel(status, "ignored", body).ok());

        DataChannelStatus got = DataChannelStatus::kOk;
        std::string id = "stale";
        std::string err;
        REQUIRE(decodeAnswerDataChannel(body, got, id, err));
        REQUIRE(got == status);
        REQUIRE(id.empty());
    }
}

TEST_CASE("数据通道应答缺状态字节判失败", "[datachannel]")
{
    DataChannelStatus status = DataChannelStatus::kOk;
    std::string id;
    std::string err;
    REQUIRE_FALSE(decodeAnswerDataChannel({}, status, id, err));
}

TEST_CASE("数据通道状态文案覆盖全部枚举", "[datachannel]")
{
    for (const auto status : {DataChannelStatus::kOk, DataChannelStatus::kNotReady, DataChannelStatus::kRejected}) {
        const std::string text = relayer::core::session::dataChannelStatusText(status);
        REQUIRE_FALSE(text.empty());
        REQUIRE(text != "未知状态");
    }
}
