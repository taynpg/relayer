#pragma once

// GUI 侧 QSettings 键的单一来源。连接面板（connector/*）、接收相关
// （receiver/*）的键都集中在这里——设置项散成多处字符串字面量后，
// 改名或改默认值必然漏一处。
//
// 默认值约定：写入方（Setting 对话框）与读取方（MainWindow /
// CoreService / LogControl）必须使用同一份常量，不允许各自手写字符串。

namespace relayer::app
{
namespace settings
{
// 连接面板
inline constexpr const char *kConnectorNoAuth = "connector/noAuth";
inline constexpr const char *kConnectorRecent = "connector/recent";
inline constexpr const char *kConnectorLast = "connector/last";
// 本机标识（声明给中继的名字）。空 = 让中继分配。
inline constexpr const char *kConnectorMyId = "connector/myId";

// 接收
inline constexpr const char *kReceiverDir = "receiver/dir";
inline constexpr const char *kReceiverOverwrite = "receiver/overwrite";
// 共享目录（对端可列举的范围，PROTOCOL.md §11.8）。空 = 不响应列举。
inline constexpr const char *kReceiverShareRoot = "receiver/shareRoot";
inline constexpr bool kReceiverOverwriteDefault = true;
// 自动接收失败重试（Setting 对话框；MainWindow 消费）
inline constexpr const char *kReceiverAutoRetry = "receiver/autoRetry";          // bool
inline constexpr const char *kReceiverRetryInterval = "receiver/retryInterval";  // 秒
inline constexpr const char *kReceiverRetryMax = "receiver/retryMax";            // 次，0=一直

// 上述键的默认值
inline constexpr bool kAutoRetryDefault = true;
inline constexpr int kRetryIntervalDefault = 3;  // 秒
inline constexpr int kRetryMaxDefault = 3;       // 次
}  // namespace settings
}  // namespace relayer::app
