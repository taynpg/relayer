# relayer

跨网络的点对点文件传输工具。一端选文件，另一端收，中间的路径由 relayer 自己找。

> **状态：可用**。中继、认证、传输引擎、Qt 客户端与命令行客户端均已实装，
> 本机三进程端到端测试覆盖收发、中断续传、伪造残片、乱序丢包、固定标识、
> 自投、远端浏览/取回/打包等场景。尚未做：P2P 直连、载荷加密（见下）。
>
> 文档：[`docs/DESIGN.md`](docs/DESIGN.md) 是架构与决策，
> [`docs/PROTOCOL.md`](docs/PROTOCOL.md) 是线协议（唯一权威）。

[![CI](https://github.com/taynpg/relayer/actions/workflows/ci.yml/badge.svg)](https://github.com/taynpg/relayer/actions/workflows/ci.yml)

CI 只在打 tag 时运行（`v*`），也支持手动触发。

---

## 它解决什么问题

同一局域网里传文件，办法很多。真正麻烦的是这三种情况：

| 场景 | 常见做法的痛点 | relayer 的做法 |
| --- | --- | --- |
| 两台机器在不同 NAT 后面 | 要么装内网穿透，要么走第三方网盘 | 一台可达的中继做转发；两端都只发起出站连接，不用改防火墙 |
| 文件几个 GB，网络不稳 | 断了从头再来 | 分块传输 + 断点续传；残片可疑时用分块 Merkle 校验定位到具体哪一块 |
| 不想把文件留在别人的机器上 | 网盘中转意味着文件落到对方磁盘 | 中继只转发字节：不落盘、不缓存分片、不解析内容 |

---

## 设计取向

这几条是硬约束，写在 [`docs/DESIGN.md`](docs/DESIGN.md) 里，也由
`tools/check_arch.py` 在 CTest 上强制执行。

**核心库零 Qt。** `core/` 只依赖标准库与 vendored 的 header-only 库，
能单独编译进任何 C++ 项目。GUI 只是它的一种前端。

**服务端只做转发。** `relayerd` 不链接 `filestore`、不读写用户数据、不解析
传输载荷。这条边界让"中继看不到你的文件"成为结构性事实，而不是靠纪律维持。

> **载荷目前不加密。** 协议里留了 `kFlagEncrypted` 位与 AES-256-GCM 的载荷
> 布局（[`PROTOCOL.md`](docs/PROTOCOL.md) §6），但发送路径还没有实装加密，也
> 没有密钥协商——中继能看到明文字节。需要保密时请套隧道（VPN / SSH 端口转发）。

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
core/          通信库，零 Qt。协议、认证、连接、会话、传输引擎
filestore/     文件子系统，零 Qt。IFileStore 与各实现、归档格式
app/           Qt GUI，唯一允许使用 Qt 的模块
server/        无界面服务端
cli/           命令行客户端
tests/         单元测试 + 架构约束与文档一致性检查
tools/         开发脚本
docs/          设计文档与线协议
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
