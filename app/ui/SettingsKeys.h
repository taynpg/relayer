#pragma once

// GUI 侧 QSettings 键的单一来源。连接面板（connector/*）、接收相关
// （receiver/*）的键都集中在这里——设置项散成多处字符串字面量后，
// 改名或改默认值必然漏一处。
//
// 默认值约定：写入方（Setting 对话框）与读取方（MainWindow /
// CoreService / LogControl）必须使用同一份常量，不允许各自手写字符串。

namespace relayer::app {
namespace settings {
// 主窗口几何（QMainWindow::saveGeometry 的原始字节，含还原态矩形、
// 位置与最大化/全屏状态）。存成 QByteArray 而不是宽高两个 int：v1 的
// 旧格式只有宽高，最大化时保存的宽高接近整屏，恢复时窗口会右下出屏。
inline constexpr const char* kWindowGeometry = "window/geometry";

// 连接面板
inline constexpr const char* kConnectorNoAuth = "connector/noAuth";
inline constexpr const char* kConnectorRecent = "connector/recent";
inline constexpr const char* kConnectorLast = "connector/last";
// 本机标识（声明给中继的名字）。空 = 让中继分配。
inline constexpr const char* kConnectorMyId = "connector/myId";

// 接收
inline constexpr const char* kReceiverDir = "receiver/dir";
// 共享目录（对端可列举的范围，PROTOCOL.md §11.8）。空 = 不响应列举。
// 发送前检查（对端已有同名文件时逐项询问是否覆盖）。默认开——v1 的
// 基本检查就是默认行为，关掉它意味着"照发不误、静默覆盖对方的文件"。
inline constexpr const char* kSendCheckBeforeSend = "sender/checkBeforeSend";
inline constexpr bool kSendCheckDefault = true;
// 自动接收失败重试（Setting 对话框；MainWindow 消费）
inline constexpr const char* kReceiverAutoRetry = "receiver/autoRetry";           // bool
inline constexpr const char* kReceiverRetryInterval = "receiver/retryInterval";   // 秒
inline constexpr const char* kReceiverRetryMax = "receiver/retryMax";             // 次，0=一直

// 上述键的默认值
inline constexpr bool kAutoRetryDefault = true;
inline constexpr int kRetryIntervalDefault = 3;   // 秒
inline constexpr int kRetryMaxDefault = 3;        // 次
}   // namespace settings
}   // namespace relayer::app
