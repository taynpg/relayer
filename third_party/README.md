# 三方依赖

本目录下的库全部 **vendored**（源码入仓），保证 `clone + cmake` 即可编译，
不依赖包管理器、不需要联网。

## 清单

| 目录 | 库 | 版本 | 许可证 | 用途 |
| --- | --- | --- | --- | --- |
| `asio/` | Asio | 1.38.0 | Boost Software License 1.0 | 异步网络 I/O |
| `cereal/` | cereal | 1.3.2 | BSD-3-Clause | 消息序列化 |
| `fmt/` | {fmt} | 12.1.0 | MIT | 字符串格式化 |
| `spdlog/` | spdlog | 1.17.0 | MIT | 日志 |
| `nlohmann/` | JSON for Modern C++ | 3.12.0 | MIT | 配置与元数据 |
| `miniz/` | miniz | 3.1.0 | MIT / public domain | zip 读写 |
| `xz/` | XZ Utils (liblzma) | 5.8.4 | 0BSD / LGPL-2.1+ | tar.xz 压缩 |
| `catch2/` | Catch2 | 见 `catch2/VERSION` | BSL-1.0 | 单元测试框架 |

## 更新方式

1. 下载对应版本源码，整目录替换；
2. 更新本文件表格里的版本号；
3. 跑 `cmake --build` 与 `ctest`，全绿才提交。

## 结构说明

- **asio**：上游 `asio/include/asio/` 的内容被上提了一层，直接放在 `asio/` 下
  （与 v1 的布局一致）。因此 include 根目录是 `third_party/` 本身，
  代码里写 `#include <asio/io_context.hpp>`。
  **没有** `asio.hpp` 这个总头文件——上游本来也没有，不要写成 `<asio.hpp>`。
- **xz**：仅保留 `src/liblzma` 与顶层 CMake 脚本，构建选项在
  `cmake/Dependencies.cmake` 里统一关闭命令行工具、文档、NLS、测试。
- **miniz**：单文件 C 实现（`miniz.c` / `miniz.h`）。
- 其余均为 header-only，只加 include 路径，不产生编译产物。

## 为什么不用 FetchContent

`FetchContent` 让仓库体积小，但首次配置需要联网，且版本漂移不受控。
本项目的目标之一是「编译简单」，因此宁可仓库大一点，也要保证离线可编。
