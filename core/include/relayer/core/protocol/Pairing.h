#pragma once

#include <optional>

#include "relayer/core/protocol/Frame.h"

// 请求与应答的配对关系（PROTOCOL.md §4.5）。
//
// 为什么需要一张显式的表：帧号里**没有**可靠的规律可以推出配对关系。
// 100/101、104/105 看起来是"偶数问、奇数答"，但 135/136（`kAskFilePull`
// / `kAnswerFilePull`）正好相反，而 `kClaimId`(134) 的应答是 `kAnswerId`
// (101)——一个请求可以被两种请求共用同一个应答类型。靠奇偶或加减一推断
// 的代码会在这些地方出错，且错得很安静：应答被当成未知帧丢掉，请求方
// 等满超时。
//
// 这张表是"协议事实"的单一来源：ControlLink 用它决定"这一帧该交给
// 请求表还是交给业务"，文档检查工具也能核对它有没有漏项。
namespace relayer::protocol {

// 这个类型是不是"请求"（会收到应答）。
[[nodiscard]] bool isAskType(FrameType t) noexcept;

// 这个类型是不是"应答"。
[[nodiscard]] bool isAnswerType(FrameType t) noexcept;

// 请求 → 应答。不是请求时返回 nullopt。
//
// `kAskArchivePull` 与 `kAskFilePull` 都映射到 `kAnswerFilePull`：两者
// 的结论是同一类（接受 / 不存在 / 拒绝），多开一个应答类型只是多一处
// 需要同步的代码。
[[nodiscard]] std::optional<FrameType> answerForAsk(FrameType ask) noexcept;

}   // namespace relayer::protocol
