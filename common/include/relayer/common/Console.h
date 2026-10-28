#pragma once

#include <string_view>

// 终端输出适配（命名空间 relayer::console）。
//
// 问题背景：Windows 控制台按代码页（中文系统为 GBK）解释字节，直接写
// UTF-8 会乱码；类 Unix 系统本来就认 UTF-8。服务端与 CLI 的中文报错
// 都按 UTF-8 字节串书写（与 core 侧的 string 约定一致），所以需要一层
// 适配把"UTF-8 字节"变成"控制台认得的宽字符"。
//
// 解决方案：init() 把 std::cout / std::cerr / std::clog 换成 UTF-8 适配的
// streambuf —— 连着控制台时内部转宽字符走 WriteConsoleW；被重定向到
// 文件/管道时**字节原样转发**，一个字节都不改（e2e 抓 stdout 的契约行
// 因此不受影响）。
//
// 约定：所有 string 参数与输出内容均为 UTF-8 字节串。
namespace relayer::console {

/// 初始化终端：Windows 下接管 std::cout / std::cerr / std::clog，
/// 并开启 ANSI 转义支持；其他平台什么都不做。幂等，可重复调用。
///
/// 必须在任何标准流输出之前调用（通常放 main() 开头）。
/// 注意：只对「C++ 标准流」生效。printf 系仍按字节写出，
/// 需配合 use_utf8_locale() 使用——服务端与 CLI 现在两套都用，
/// 所以 main() 里两个都要调。
void init();

/// 只开启 Windows 控制台的 ANSI 转义（VT 序列处理），不接管标准流。幂等。
void enable_ansi();

/// 把 C 运行时 locale 设为 UTF-8（Win10 1803+ 的 UCRT 支持），
/// 让 printf 系 / 宽窄转换按 UTF-8 处理。其他平台为空操作。
void use_utf8_locale();

/// 标准输出当前是否连着终端（用于决定要不要输出 ANSI 颜色）。
bool stdout_is_console();

/// 输出 UTF-8 文本到标准输出并刷新。
void write(std::string_view text);

/// 输出 UTF-8 文本到标准错误并刷新（提示与错误信息，重定向时不会混进结果）。
void write_error(std::string_view text);

}   // namespace relayer::console
