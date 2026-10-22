# relayer

跨网络的点对点文件传输工具。一端选文件，另一端收，中间的路径由 relayer 自己找。

> **状态：重写中（v2）**。阶段 A-D 已完成（filestore / 协议与认证 / 传输引擎 /
> 三进程集成测试），阶段 E（Qt 前端）进行中：GUI 已能连接中继、稳定收 id、
> 自动接收、对照表批量发送。剩余项见 `docs/REWRITE-PLAN.md` §4.1。
> 可用的旧版本在 [`taynpg/relayFile`](https://github.com/taynpg/relayFile)（v0.5）。
> 设计文档见 [`docs/REWRITE-PLAN.md`](docs/REWRITE-PLAN.md)，当前交接见
> [`docs/HANDOFF.md`](docs/HANDOFF.md) §0。

[![CI](https://github.com/taynpg/relayer/actions/workflows/ci.yml/badge.svg)](https://github.com/taynpg/relayer/actions/workflows/ci.yml)

CI 只在打 tag 时运行（`v*`），也支持手动触发。

---

## 它解决什么问题

同一局域网里传文件，办法很多。真正麻烦的是这三种情况：

| 场景 | 常见做法的痛点 | relayer 的做法 |
| --- | --- | --- |
| 两台机器在不同 NAT 后面 | 要么装内网穿透，要么走第三方网盘 | 服务端只做信令与中继，能直连就直连 |
| 文件几个 GB，网络不稳 | 断了从头再来 | 分块传输 + 断点续传，残片可续 |
| 不想把文件传给第三方 | 网盘中转意味着文件过别人的机器 | 直连时数据不出两端；中继时端到端加密 |

---

## 设计取向

这几条是硬约束，写在 [`docs/REWRITE-PLAN.md`](docs/REWRITE-PLAN.md) 里，也由
`tools/check_arch.py` 在 CI 上强制执行。

**核心库零 Qt。** `core/` 只依赖标准库与 vendored 的 header-only 库，
能单独编译进任何 C++ 项目。GUI 只是它的一种前端。

**文件操作有接口。** `IFileStore` 把路径、读写、原子替换、目录遍历收成一组虚函数。
默认实现用 `std::filesystem`，出问题可以换。`core/` 只允许看到这个接口头，
看不到实现——这条规则由脚本检查，不是靠自觉。

**依赖全部 vendored。** 所有三方库源码入仓，`clone` 下来离线可编。
版本与许可证见 [`third_party/README.md`](third_party/README.md)。

**协议写在文档里，不是只写在代码里。** [`docs/PROTOCOL.md`](docs/PROTOCOL.md)
能脱离代码读懂：帧布局、字段语义、解包算法、握手时序、状态机、错误处理。
它和 `core/include/relayer/core/protocol/Frame.h` 的一致性由
`tools/check_protocol_doc.py` 在 CI 上逐项比对——加帧类型忘了改文档会红。

---

## 构建

| 项 | 要求 |
| --- | --- |
| 编译器 | MSVC 2022 / GCC 13+ / Clang 16+ |
| CMake | 3.20+ |
| Qt | 6.5+（仅 GUI 需要） |

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELAYER_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

只编核心库与测试（不出 GUI，秒级完成）：

```bash
cmake -S . -B build -G Ninja \
  -DRELAYER_BUILD_GUI=OFF -DRELAYER_BUILD_SERVER=OFF -DRELAYER_BUILD_CLI=OFF \
  -DRELAYER_BUILD_TESTS=ON
```

Windows（MSYS2 UCRT64）下的完整依赖安装见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。

### 构建选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `RELAYER_BUILD_GUI` | ON | Qt 图形客户端 |
| `RELAYER_BUILD_SERVER` | ON | 无界面服务端 `relayerd` |
| `RELAYER_BUILD_CLI` | ON | 命令行客户端 |
| `RELAYER_BUILD_TESTS` | OFF | 单元测试与架构检查 |
| `RELAYER_WERROR` | OFF | 告警即错误，提交前建议打开 |

---

## 目录

```
common/        最底层共用件，零 Qt。Status/ErrorCode 与密码学原语
core/          通信库，零 Qt。协议、加密、认证、传输、发现
filestore/     文件子系统，零 Qt。IFileStore 与各实现、归档格式
app/           Qt GUI，唯一允许使用 Qt 的模块
server/        无界面服务端
cli/           命令行客户端
tests/         单元测试 + 架构约束与文档一致性检查
tools/         开发脚本
docs/          设计文档
third_party/   三方库源码
```

依赖方向是单向的：

```
app → core → filestore(接口) → common
```

`common` 是最底层，不允许反向依赖任何上层模块。反向依赖会形成环，
由 `tools/check_arch.py` 的 R1-R6 规则在 CI 上拦下。

`common` 单独抽出来的原因：`filestore` 和 `core` 都要用 `Status` 与 SHA-256，
而两者之间不能互相依赖，否则会形成环。

---

## 参与

提 PR 前请读 [`CONTRIBUTING.md`](CONTRIBUTING.md)，里面有格式、命名、
错误处理的约定，以及提交前的三条自查命令。

## 许可证

[MIT](LICENSE)
