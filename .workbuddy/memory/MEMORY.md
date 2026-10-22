# relayer —— 项目长期记忆

跨网络的点对点文件传输工具。v2 完全重写自 `D:/Code/relayFile`（v1）。
仓库：`git@github.com:taynpg/relayer.git`，主分支 `main`。

## 关键决策（用户明确要求过的）

- **服务端只做转发**，不落任何用户数据、不链接 filestore。见 `docs/REWRITE-PLAN.md` 6.1。
- **CI 只在打 tag 时触发**（`v*`），不在每次 push/PR 跑。见 `.github/workflows/ci.yml`。
- **`third_party/` 整目录入库**（含 xz/miniz/catch2/asio/fmt/spdlog/cereal/nlohmann），
  理由是"编译简单是重点，仓库大点没关系"。
- **不着急出成果，慢工出细活**——宁可多写测试也不要留隐患。

## 架构约束

- 单向依赖：`app → core → filestore(接口) → common`，由 `tools/check_arch.py`
  在 CTest 里强制（R1-R6）。
- `common/` 是最底层：只有 `Status` / `ErrorCode` 与 crypto 原语（SHA-256、HMAC、
  PBKDF2、AES-256-GCM）。不许依赖任何上层模块。
- `core/` 只能 include `IFileStore.h`，不能 include 具体实现；可以 include `common/`。
- 跨模块边界**不用异常**，全部 `Status` + `ErrorCode`（定义在 `common/Status.h`，
  命名空间是 `relayer` 不是 `relayer::common`）。

## 工程约定

- C++17 / Qt6 / CMake 3.20+ / Ninja；MSYS2 UCRT64 下 GCC 16.2.0
- GCC 工具链路径：`C:\msys64\ucrt64\bin\`，`clang-format` 也在那里
- 代码风格：`.clang-format`（LLVM + Allman + 120 列 + 4 空格）
- 格式化命令：`clang-format -i <files>`，提交前必跑
- 产物统一在 `build/bin` 与 `build/lib`

## 文档即契约

- `docs/PROTOCOL.md` 是线协议的**唯一权威说明**，必须能脱离代码读懂。
- 它与 `core/include/relayer/core/protocol/Frame.h` 的一致性由
  `tools/check_protocol_doc.py` 在 CTest 里逐项比对（测试名 `protocol_doc`）。
  加帧类型忘了改文档 → 测试红。
- 写文档时别吹没实现的东西。阶段性成果要如实标"未开始"，不要写成已完成。

## 构建与验证

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure

# 带 -Werror 的 Release 验证（提交前跑一次）
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELAYER_WERROR=ON
cmake --build build-rel && cd build-rel && ctest
```

## 当前进度

- ✅ 阶段 A 地基（仓库骨架、CMake、CI、文档）
- ✅ 阶段 B filestore（StdFileStore / MemFileStore / PathPolicy / tar.xz 归档）
- ✅ 阶段 C protocol + crypto + auth + 状态机
  - ✅ `common/` 基础层（Status/ErrorCode + crypto 原语）
  - ✅ `core/protocol` 帧编解码（26 用例 / 443 断言）
  - ✅ `docs/PROTOCOL.md` + 文档一致性检查
  - ✅ `core/auth` 握手状态机（39 用例 / 291 断言）
  - ✅ `core/transfer/StateMachine` 转移表（25 用例 / 547 断言）
- 🚧 阶段 D transport + transfer
  - ✅ `core/transport/FrameReader` 帧切分（18 用例 / 678 断言）
  - ✅ `core/transport/Connection` asio 连接（13 用例 / 152 断言）
  - ✅ `core/transport/Heartbeat` 心跳策略（14 用例 / 766 断言）
  - ✅ `core/session/SessionPolicy` 会话策略（23 用例 / 776 断言）
  - ✅ `core/session/Session` 组装（36 用例 / 841 断言）
  - ⬜ 服务端 peer 表（id → Session，让转发真正有地方投递）
  - ⬜ 心跳 asio 定时器接线（`pollHeartbeatNow()` 已有，缺 `steady_timer`）
  - ⬜ `core/transfer` 传输引擎
- ⬜ 阶段 E ～ G（Qt 前端 / P2P / 发布）

测试基线（254 用例 / 4940 断言）：`common` 16/1111、`filestore` 117/1718、
`core` 121/2111。CTest 6 项：common / filestore / core / arch / protocol_doc /
transition_doc。

## 定位

**v2 仓库是 `D:/Code/relayer`**。`D:/Code/relayFile` 是 v1 的老仓库，
它的 `v2-main` 分支名有误导性，内容其实是 v1 历史。开工前先确认在这两个
目录中的哪一个，搞错会误判"v2 代码丢了"。

## 容易踩的坑

- **ninja 增量构建有时不重编 `.obj`**：改了源码却看到旧行为，先 `rm` 目标文件
- MSYS2 的 GNU tar 不认 `D:/xxx` 路径（当成远程主机名），要在目录内用相对路径
- `lzma_code` 会吞掉第一次"零进展"，截断检测必须多读一轮（详见 2026-10-22 日志）
- **测试向量必须实算，不能凭记忆写**。PBKDF2 用 `hashlib.pbkdf2_hmac`，
  AES-GCM 用 `cryptography` 库（装在
  `~/.workbuddy/binaries/python/envs/default`）。凭记忆填的向量几乎必然是错的。
- **`git push` 在沙箱内解析不了 github.com**，需要带 `dangerouslyDisableSandbox`
  再推一次。
- 大目录的 `rm -rf` 会被安全策略拦下（要求确认）。清构建目录时改用
  "在原目录上重新 configure"，或只删具体文件。
- **`#include` 有顺序依赖时会被 clang-format 打乱**：它按字母序重排，
  `windows.h` 必须先于 `bcrypt.h`，排反了一堆 `'LONG' does not name a type`。
  这类区块用 `// clang-format off` / `on` 包起来，并写清原因。
- **注释里的算术也要验算**：`"relayer/auth/v2"` 曾按"看起来很长"写成 24 字节，
  实际 15。长度、偏移这类可计算的事实尽量由字面量推导，别手算。
- **别用 `std::random_device` 做安全随机**：标准把质量留给实现，某些 MinGW
  版本是 mt19937 加固定种子。用 `BCryptGenRandom` / `getrandom` / `arc4random_buf`。
- **"确定性"测试别用含真随机的路径**：客户端每次握手都会生成新 nonce，
  拿两次完整握手比结果必然失败。要测确定性就把随机输入也固定下来。
- **asio 的 include 根目录是 `third_party/`**，不是 `third_party/asio/`：
  vendored asio 内部用 `#include "asio/detail/..."` 互引。另外上游没有
  `asio.hpp` 这个总头文件，别写 `<asio.hpp>`。
- **asio 目标必须标 `SYSTEM`**：header-only 会在我们的编译单元展开，
  上游代码不过本项目的 `-Werror -Wsign-conversion`。
- **asio 的 `run_one()` 在"没有就绪任务但有挂起异步操作"时会阻塞**，
  不是返回 0。测试里用 `run_for(时限)`，否则会永久挂死。
- **Windows 上连被拒端口前有 SYN 重传**，测试等待时间要 5 秒以上。
