#pragma once

#include <string>
#include <string_view>

// 远端路径的**文本**规范化（DESIGN.md §3.8）。
//
// 为什么需要它：远端路径是"另一台机器上的路径"，分隔符可能是 `/` 也可能是
// `\`，而拼接、上游填写、协议回包三者的写法并不一致。不统一就会出现
// `D:\/Download` 这种既不像 Windows 也不像 Unix 的混合作物——它能用（
// Windows 两种都认），但显示出来一眼就是 bug，而且再拼一次还会继续叠加。
//
// 约定（与 v1 一致）：**内部一律用 `/`**。需要在本地落盘/打开时再由调用方
// 转成 native 形式（Qt 用 `QDir::toNativeSeparators`）。这样"对端是 Unix、
// 本机是 Windows"这类跨平台组合也不会产生歧义。
//
// 不做的判断：这里**不判断平台**。远端是谁的机器不由本机决定，所以 `\`
// 一律当分隔符处理——即使对端是 Unix、文件名里真的含 `\`（合法字符），
// 那种文件本来也没法在远端浏览里正确双击进入，宁可显示成 `/` 让用户看出
// 异常，也不要在协议层给两个平台留两套规则。
namespace relayer::core::session {

// 规范成 `/` 分隔：`\`→`/`、合并重复分隔符、处理 `.` 与 `..`、去掉末尾
// 分隔符（根除外）。空输入返回空。
//
// 根的形式保留：
//   `/`            Unix 根
//   `D:/`          盘符根（输入 `D:`、`D:\`、`D:/` 都归一成 `D:/`）
//   `//server/share` UNC（前两个斜杠不合并）
std::string normalizePathText(std::string_view path);

// 拼接：base 是根列表条目那种完整路径时直接用 name。
// 结果总是规范化过的，所以 `joinPathText("D:\\", "Download")` 得到
// `D:/Download` 而不是 `D:\/Download`。
std::string joinPathText(std::string_view base, std::string_view name);

// 上一级。返回空 = 已经在根列表之上（调用方据此回到根列表）。
//   `D:/a/b` → `D:/a`
//   `D:/a`   → `D:/`
//   `D:/`    → ``（没有更上一级）
//   `/a`     → `/`
//   `/`      → ``
std::string parentPathText(std::string_view path);

}   // namespace relayer::core::session
