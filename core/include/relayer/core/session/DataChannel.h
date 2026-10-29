#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// 数据通道协商（PROTOCOL.md §11.13）。
//
// 双连接模型下，一个客户端到中继有两条连接：
//
//   控制连接（常驻）—— 认证、列举、元信息、文件操作、通知
//   数据连接（按需）—— 只跑文件传输的帧（请求、分块、ACK、完成）
//
// 两条连接各自独立认证、各自一个 id。中继**按 `to` 里的 id 找连接**转发，
// 所以要让文件从 A 的数据连接流到 B 的数据连接，A 必须知道 B 的数据连接
// 叫什么名字。
//
// 这一对帧就是交换那个名字，一次往返换两边：
//
//   A ── kAskDataChannel(myDataId = aid) ──> B
//   A <── kAnswerDataChannel(peerDataId = bid) ── B
//
// 之后 A 在自己数据连接上发 `to = bid` 的传输帧，B 在自己数据连接上收到。
// 同时 B 从请求里拿到 aid，知道"那个正在推文件给我的人，它的数据连接是
// aid"——它需要这个映射来把传过来的文件和"控制连接上的哪个对端"对上。
//
// 为什么不能省掉 A 的 id：数据帧的 `from` 由中继绑定成**连接 id**，B 看到的
// 是 aid 而不是 A 的控制连接 id。没有映射的话，B 只能看到一个陌生 id 在推
// 文件。
namespace relayer::core::session {

// 应答状态。
enum class DataChannelStatus : std::uint8_t {
    kOk = 0,
    kNotReady = 1,   // 暂时建不起来（连接数到上限 / 网络问题），可以稍后重试
    kRejected = 2,   // 明确不接受（正在关闭、配置禁止等），重试也没用
};

// 数据连接 id 的字节上限。与帧头的路由字段一致（u8 长度前缀）。
inline constexpr std::size_t kMaxDataChannelIdBytes = 255;

// 请求：`u8 len || myDataId`。
//
// 允许空 id（len = 0）：含义是"我还没建好数据连接，先告诉你一声"。
// 对端可以据此回 kNotReady，或者建好自己的、等下一次协商。
[[nodiscard]] std::vector<char> encodeAskDataChannel(std::string_view myDataId);
[[nodiscard]] bool decodeAskDataChannel(const std::vector<char>& body, std::string& myDataId, std::string& err);

// 应答：`u8 status || u8 len || peerDataId`。非 kOk 时 id 为空。
[[nodiscard]] Status encodeAnswerDataChannel(DataChannelStatus status, std::string_view peerDataId, std::vector<char>& out);
[[nodiscard]] bool decodeAnswerDataChannel(const std::vector<char>& body, DataChannelStatus& status, std::string& peerDataId,
                                           std::string& err);

[[nodiscard]] const char* dataChannelStatusText(DataChannelStatus status) noexcept;

}   // namespace relayer::core::session
