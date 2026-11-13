# relayer —— 项目长期记忆

跨网络的点对点文件传输工具。v2 完全重写自 `D:/Code/relayFile`（v1）。
仓库：`git@github.com:taynpg/relayer.git`，主分支 `main`。

## 关键决策（用户明确要求过的）

- **服务端只做转发**，不落任何用户数据、不链接 filestore。见 `docs/DESIGN.md` §3.1。
- **CI 只在打 tag 时触发**（`v*`），不在每次 push/PR 跑。见 `.github/workflows/ci.yml`。
- **`third_party/` 整目录入库**（含 xz/miniz/catch2/asio/fmt/spdlog/cereal/nlohmann），
  理由是"编译简单是重点，仓库大点没关系"。
- **不着急出成果，慢工出细活**——宁可多写测试也不要留隐患。
- **控制连接 + 数据连接（两条 TCP）——2026-10 改口，现在就是这样**。曾评估
  "单连接 + 小块 + 写队列优先级 + 写超时"够用（`DESIGN.md` §4.2 留着那次结论），
  实机否掉了：只要还在同一条 TCP 上，大文件就把带宽占满，列举/取消仍然排队。
  现在 `core/net/ControlLink`（常驻、请求号配对、多请求并发、各自超时）+
  `core/net/TransferLink`（首次传输时建、之后保留），`cli::Client` 内部已换
  过去，公共 API 一行未改。**别再提议合并回单连接**。
- **默认分块 256 KiB（不是 4 MiB 上限）**。块大小只决定"控制帧最坏等多久"，
  吞吐由在飞字节数决定（`windowBytes=4 MiB` 换算成块数）。改块大小必须
  同时确认每块固定开销已被消掉，否则会吞吐腰斩。
- **id 必须是稳定身份，客户端自己声明**（`DESIGN.md` §3.2，已实现 `2e343aa`；
  `--id` 是**声明**不是断言）。理由：对照表按 id 固定投递，id 漂移会发错人
  （用户真实遇到过）。服务端只校验占用，撞名**拒绝连接并提示**——不自动
  改名、不顶掉先到者（两种做法都会让表悄悄指向别人）。
- **数据连接的身份是 `<控制连接 id>~d`**，用 `kClaimId` 声明，重连后名字不变
  （对端缓存的 id 才不过期）；它**不出现在在线列表**里——服务端按 `~d`
  **后缀**判定（见坑区"命名约定"一条）。

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
- 代码风格：`.clang-format`（LLVM 底 + 130 列 + 4 空格 + 指针靠左 +
  只给函数/类换行的大括号）。**2026-10-28 换过风格**，旧的是 Allman +
  120 列 + 指针靠右；改风格必须全量重排一次，否则后续提交混入无关 diff
- 格式化命令：`clang-format -i <files>`，提交前必跑
- 产物统一在 `build/bin` 与 `build/lib`

## 文档即契约

- `docs/PROTOCOL.md` 是线协议的**唯一权威说明**，必须能脱离代码读懂。
- 它与 `core/include/relayer/core/protocol/Frame.h` 的一致性由
  `tools/check_protocol_doc.py` 在 CTest 里逐项比对（测试名 `protocol_doc`）。
  加帧类型忘了改文档 → 测试红。
- 写文档时别吹没实现的东西。阶段性成果要如实标"未开始"，不要写成已完成。

## 构建与验证

**统一用 `cmk`**（2026-10-29 用户指定，别再手写 `cmake -G Ninja ...`）：

- **版本 2.0.0**，`RELAYER_BUILD_GUI/SERVER/CLI/TESTS` 四个开关，产物在 `build/bin`。

```bash
cmk build -t Debug     # build/，Debug，配置里已含 RELAYER_BUILD_TESTS=ON
cmk build -t Release   # build-rel/，Release + RELAYER_WERROR + tests（提交前跑一次）
cmk                    # 无参数 = 重放上次 build（日常增量）
cd build && ctest --output-on-failure   # 8 项，约 155 秒
```

- cmk 配置 `.cmk/cmk_config` **已入库**（2026-10-29 用户要求，两个
  configuration 块里 Debug/Release 都有 `-DRELAYER_BUILD_TESTS=ON`，
  Release 另有 `-DRELAYER_WERROR=ON`）。`cmk_cache`/`cmk_temp` 忽略。
  配置含本机编译器路径（C:/msys64），换机器按需改。
- 不要动 `.cmk/cmk_cache`、`.cmk/cmk_temp`；重置用 `cmk clean cache|temp|all`。
- 没有 Qt 的机器/CI 要显式 `-DRELAYER_BUILD_GUI=OFF`。

## 当前进度

- ✅ 阶段 A 地基（仓库骨架、CMake、CI、文档）
- ✅ 阶段 B filestore（StdFileStore / MemFileStore / PathPolicy / tar.xz 归档）
- ✅ 阶段 C protocol + crypto + auth + 状态机
  - ✅ `common/` 基础层（Status/ErrorCode + crypto 原语）
  - ✅ `core/protocol` 帧编解码（26 用例 / 443 断言）
  - ✅ `docs/PROTOCOL.md` + 文档一致性检查
  - ✅ `core/auth` 握手状态机（39 用例 / 291 断言）
  - ✅ `core/transfer/StateMachine` 转移表（25 用例 / 547 断言）
- ✅ 阶段 D transport + transfer + 三进程集成测试（**已结项**）
  - `core/transport`（FrameReader / Connection / Heartbeat）
  - `core/session`（SessionPolicy / PeerTable / Session）
  - `core/transfer`（StateMachine / Merkle / Payload / WorkPool / SendWindow
    / TransferSender / TransferReceiver / TransferPeer）
  - `server/`（relayerd）、`cli/`（relayer-cli）
  - `tests/e2e/run_e2e.py`：10 个三进程场景，完整跑 295 秒全过
- 🚧 **阶段 E 进行中**（Qt 前端，`app/`）
  - ✅ E1 骨架 + 9 个 `.ui` 迁移；E2 `bridge/CoreService`（唯一 Qt 边界）
  - ✅ E3.1 InfoDrop / CheckableComboBox；E3.2 OwnTableWidget 重写
  - ✅ E3.3 ExplorerControl（本地浏览；远端面板要协议扩展，属阶段 F）
  - ✅ E3.4 ComparisonControl（JSON 持久化 + 相对目录落盘）
  - ✅ E3.5 RelayTaskControl 任务表单（停止/重传/批次失败规则）
  - ✅ 身份稳定（连接=常驻接收会话 + sessionId 解复用）、性能三杠杆
  - ✅ E3.6 Setting 对话框（压缩/加密组故意不迁，v2 无对应物；QSettings 键
    单一来源 `app/ui/SettingsKeys.h`）；E3.7 ReplaceArea 作用域（v1 的
    useCase/useRegex 是假选项，v2 做成真的）；E3.8 ProgressTracker（清单 #11）
  - ⬜ 剩余：E4 真实链路验收（两台机器经中继；本机 e2e 已 20+ 场景）
测试基线（2026-11-13 实测）：`ctest` **8 项**全过，141 秒。common 22 用例 /
1136 断言、filestore 118 / 1726、core 436 / 6047（合计 **576 用例 / 8909 断言**）；
`e2e_quick` 111 秒、`gui_smoke` 13 秒。e2e 共 **27 个场景**（含 2 个 slow：
200MB / 1GB，`--quick` 跳过；完整跑约 5 分钟）。

**功能状态：齐了，缺真机验收。** 已实装：请求号配对（`core/protocol/Pairing`）、
控制连接（`core/net/ControlLink`）、数据连接（`core/net/TransferLink`）、
断线重连与按残片续传（`ClientConfig::ReconnectPolicy`，CLI 默认关、`--reconnect`
打开；GUI 常开）、载荷加密（`--encrypt`，`protocol::PayloadCipher`）、块载荷
压缩（`--compress`，逐帧 deflate）、远端列举/取回/文件操作、全路径访问（无共享根）。
吞吐 200-300 MB/s（`73b1ad2` 六个原因叠加，之前 25-48 MB/s）。

**缺的**：① E4 真机验收——两台真实机器经中继跑一遍，至今只在本机中继 + loopback
验过；② 行级下载 / 详细信息 / 跨面板拖拽三项只有代码审查，没有自动化用例；
③ P2P 直连（用户明确不做）、G 发布。

**文档只有两份**（2026-10-26 收敛，提交 `d6f8245`）：
`docs/PROTOCOL.md` 是线协议的唯一权威；`docs/DESIGN.md` 是架构、硬约束、
关键决策、已否决方案与决策记录。**重写计划与交接文档已删除**——过程记录在
git 历史里，别再重建。

改代码前先读 `DESIGN.md` §2（硬约束）与 §4（否决清单）；要推翻某个决策，
先在 §6 决策记录里补一条（§4.2 就是"双连接"被推翻后的样子）。

## 定位

**v2 仓库是 `D:/Code/relayer`**。`D:/Code/relayFile` 是 v1 的老仓库，
它的 `v2-main` 分支名有误导性，内容其实是 v1 历史。开工前先确认在这两个
目录中的哪一个，搞错会误判"v2 代码丢了"。

## 容易踩的坑

- **命名约定要端到端核对"方向"**：数据连接叫 `<控制连接 id>~d`（**后缀**），
  而过滤函数按 `id.front() == '~'` 判（**前缀**），两条规则永不相交 → 在线列表
  里每个客户端多一条 `<id>~d`，两个客户端看起来像四个在线端（用户就是这么
  报的）。常量名 `kInternalIdPrefix`、注释、文档 §11.7 全都写着"前缀"，读代码
  完全看不出矛盾。**约定写了前后缀，就必须找到真正拼接那一行核对**
  （`cli/src/Client.cpp` 的 `dataIdFor`），别相信常量名。
- **"列表里没有 X" 必须配一条反证**：只断言输出不含 `~d`，在"数据连接压根
  没建"时也是绿的（空转）。`online-list` 场景用"抢 `nodeA~d` 这个名字必须被
  拒绝"来证明那条连接真的注册着。
- **每块 open/close 一次文件 = 每块 ~9ms**（Windows）。接收端曾每块
  `openWrite` 残片，1024 块就是 9 秒，还会引发超时重传。写盘流要整段复用，
  在锁内 seek/write/flush；`finalize`/`abort` 前必须显式关闭（Windows 上
  重命名/删除打开中的文件会失败）。
- **"大块吞吐更高"在单 TCP 上是错觉**：块大小只影响控制帧的最坏等待，
  吞吐看的是每块固定开销。
- **优先级/顺序类测试要先确认能证伪**：把控制帧发在数据帧之前的用例，
  FIFO 实现也会通过。改完实现要临时退回旧行为跑一次，看用例是否真的红。
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
- **链接报 `ld returned 1` 却没有任何诊断 = 目标 exe 正被占用**。测试留下的
  `relayerd.exe` 没退出时就这样，`taskkill //F //IM relayerd.exe` 后重编即可。
  别去怀疑编译选项。
- **测试脚手架的反压会伪装成产品故障**：`subprocess.PIPE` 有容量上限（Windows
  几 KB），只等其中一个进程时另一个没人读，写满就阻塞在 `write` 上、事件循环
  停摆，症状是"传输卡在 94%"。e2e 的 `Process` 已改成输出直连文件。
  写这类测试时：**先确认自己在读所有子进程的输出**。
- **`--idle-timeout` 只该在传输开始后计时**。`TransferPeer::started()` 是
  "现在沉默是否正常"的权威判据（发送方=收到 AnswerSend，接收方=open 成功），
  不要在调用层按角色分别判——漏一边就会出现"接收端在发送端算摘要时把自己掐了"。
- **两个构建目录可能被留在 `RELAYER_BUILD_GUI=OFF`**（Release 验证后忘了改回），
  症状是 build/bin 里没有 relayer.exe。重新 configure 带 GUI 即可。
- **v1 的 GUI 也有假选项**（ReplaceArea 的 useCase/useRegex 传进 lambda 但
  从未使用、Setting 的压缩阈值照抄默认）。迁移 v1 逻辑时先读消费方代码，
  确认选项真的被用到了再照搬。
