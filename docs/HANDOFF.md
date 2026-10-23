# 当前交接：阶段 D 已结项，阶段 E 进行中

> 写于 2026-10-22。这份文档的目的很窄：让接手的人**不读前面几万行对话**
> 也能知道现在在哪、哪些是真的、哪些还悬着。
>
> **先读 §0**（阶段 E 现状，最后一次更新 2026-10-22 晚）；§1-§11 是阶段 D
> 的结项记录，作为历史与踩坑库保留——里面的结论仍然有效，但"下一步"一节
> 已经过期。

---

## 0. 阶段 E 现状（新会话从这里开始）

### 0.1 一句话

阶段 D 结项后进 `app/`（Qt 前端）。**GUI 的最小可用闭环已经跑通**：连接中继、
拿到稳定 id、自动接收、对照表批量发送、任务选项卡看进度/停止/重传。
`main` 干净，ctest 7/7（52 秒）。

阶段 E 的代码工作已全部完成（Setting 对话框、替换作用域、速度显示三项于
2026-10-23 收尾）。**只剩 E4 的真实链路验收**——不是架构问题，是缺一次
两台机器经中继的实测记录，清单见 §0.8。

### 0.2 已落地（按依赖顺序）

| 子项 | 内容 |
| --- | --- |
| E1 | `app/` 骨架；9 个 `.ui` 自 v1 原样搬迁（只改 `<class>` 名）；Qt6 缺库时给可照做的报错 |
| E2 | `bridge/CoreService`：持有 io_context + io 线程，对 `cli::Client` 的调用全部 `asio::post`，回调 emit Qt 信号（跨线程 queued）。**不要在 onDone 回调栈里析构 Client**（见 0.6） |
| E3.1 | `Base/InfoDrop`（拖拽载荷，格式即契约）、`Control/CheckableComboBox` |
| E3.2 | `OwnTableWidget`：`ComColumn`/`ExpColumn` 枚举、列可编辑性收敛到 `isReadOnlyColumn()`、`ensureItem/setText/setReadOnly` 拆分、`ensureRowComplete` |
| E3.3 | `ExplorerControl`：本地浏览（导航/排序/筛选/拖出/右键动作表）。远端面板**故意不做**——v2 没有 list-dir 帧 |
| E3.4 | `ComparisonControl`：`rows_` + `viewToRow_` 投影、JSON 持久化（替 v1 的 Sqlite）、`selectedRows()`、动作表、去掉 `static bool inited` |
| E3.5 | `RelayTaskControl`（主窗口「任务」选项卡）：批次顺序推进、停止、单行重传；文件级失败继续、连接级失败终止 |
| 身份 | 连接/刷新/发送**共用同一条常驻会话**，id 只分配一次；发送在接收会话上挂第二个 `TransferPeer`，帧靠 `sessionId` 解复用 |
| 性能 | 默认块 256 KiB（上限仍 4 MiB）、控制帧插队（`isBulkFrame()`）、单帧写超时 60s、接收端写流复用 |

### 0.3 没做（下次接着做）

上次列的收尾三项已完成（2026-10-23，提交 beb6d63 / c118c06 / 32500f6）：

- **Setting 对话框**：`btnLock` 已解禁。重试策略（开关/间隔/次数，0=一直）+
  接收覆盖开关；v1 的压缩/加密两组故意不迁（v2 无对应物）。QSettings 键
  统一在 `app/ui/SettingsKeys.h`。
- **ReplaceArea 作用域**：对话框已接线 `ComparisonControl::onReplace()`；
  v1 的 useCase/useRegex 是假选项，v2 做成了真的。
- **ProgressTracker**（清单 #11）：`app/model/ProgressTracker.{h,cpp}`，
  任务表进度列显示「百分比 · 速度」。

剩下：

1. **E4 验收**：需要一次**真实链路**（两台机器经中继，不是本机回环）的完整收发记录。
   目前为止 GUI 的收发都是在本机中继 + loopback 上验证的。
   e2e 的 no-auth 缺口已补（2026-10-23，`no-auth` 场景覆盖"两端 --no-auth 可传 /
   口令客户端被拒 / 漏配被拒"三条断言）。
2. **阶段 F**：远端浏览/下载、对端打包 tar.xz、P2P 直连、多任务并行。前三个都要动协议，
   `PROTOCOL.md` 得先写帧定义再写代码。
3. **固定 id（客户端声明标识）**：**已实现**（2026-10-23）。设计与落地记录见
   `REWRITE-PLAN.md` §6.2；线协议见 `PROTOCOL.md` §11.7（`kClaimId`=134）。
   GUI 连接面板有「本机标识」输入框，CLI 是 `--id`；撞名会被拒且**不会**顶替
   先到者、**不会**自动改名。**别用 `kDirectRequestId`(550) 干这个**——那是
   P2P 直连查 id 的保留号。自投（发给自己）同样已实现，见 §11.3.1；
   CLI 可用 `recv --self-send <file>` 一个进程验证整条链路（同时也是 e2e
   场景 `self-send` 的实现方式）。

### 0.4 E 阶段定下的架构约束（别推翻，推翻要改协议和测试）

- **一条连接 = 一个常驻接收会话**。GUI 里所有"临时连接"都在消耗服务端递增 id
  并顶掉常驻会话，症状是「点一次刷新 id 涨一号」。这条已经用 `askListOnStart`
  和 `sendOnSession` 消掉了，不要再加"用完即弃"的连接。
- **发送不另建连接**：`Client::sendOnSession()` 在接收会话上挂第二个 `TransferPeer`，
  收到的帧按 `sessionId` 解复用（接收端空闲时 `sessionId=0`，只认领 `kFileRequestSend`）。
- **服务端纯转发**（§6.1 的老决策），id 由服务端分配，客户端不能自选。
- **`overwriteExisting`**：`kAlreadyExists` 时先删后重开。CLI 默认关（e2e 契约），
  GUI 默认开（日志右键可切）。
- **`--idle-timeout` 只在传输开始后计时**，判据统一是 `TransferPeer::started()`，
  不要按角色分别判。

### 0.5 编译与运行

```bash
# GUI 默认 ON，本机 Qt 6.11.2（MSYS2 UCRT64）能被 CMake 找到，不必再关
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRELAYER_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure     # 7 项，约 51 秒（e2e_quick 40s）

build/bin/relayerd.exe --port 9527 --password <口令>   # 中继（默认端口 9527）
build/bin/relayer.exe                                  # GUI
build/bin/relayer-cli.exe send/recv ...                # 对照参考实现、e2e 用
```

没有 Qt 的机器（或 CI）要显式 `-DRELAYER_BUILD_GUI=OFF`，否则 configure 会停在
「找不到 Qt6」的提示上——提示本身是可照做的，不是崩溃。

### 0.6 GUI 侧踩过的坑

- **`ld returned 1` 且无任何诊断 = `relayer.exe` 正被占用**。先
  `taskkill //F //IM relayer.exe`（中继同理）再编，别怀疑编译选项。
- **不能在 `onDone`/`finish` 的回调栈里 `reset(client_)`**：Client 的 pump 定时器
  回调返回后还会访问 Impl 成员，栈上析构就是 use-after-free。下次操作时在 post 的
  lambda 里替换实例。
- **`RelayTaskControl` 继承 `QDialog` 但被塞进 `QTabWidget`**：能跑，布局按 QWidget
  处理。若以后要模态，别直接 `exec()`（批次可能跑几分钟，会锁死界面）。
- **`QTableWidget::selectedItems().size()` 是 `qsizetype`**，`-Wconversion` 下必须显式转。
- **前向声明要带命名空间**：`class RelayTaskControl;` 在 `namespace relayer::app` 里声明，
  全局再声明一次就是两个不同类，链接期才炸。
- **`.ui` 的根类必须与 `.cpp` 一致**：`ComparisonControl.ui` 从 `QDialog` 改成 `QWidget`
  才能配 `setupUi(QWidget*)`。
- **【连接】/【断开】的语义（2026-10-23 定）**：【连接】只在 `!connected_ && !busy`
  时可用，拿到**非空** id 才算连上（空 id 是握手中间态）；【断开】只看 `connected_`。
  此前【断开】按 `busy` 禁用，而连上后 busy 恒为 true → 永远点不动，用户只能反复点
  【连接】（每次顶掉常驻会话、白耗一个 id）。
  **【断开】必须真的结束会话**：经 `disconnectRequested` 交给 MainWindow，由它先清
  `receiveWanted_` 再 `cancelCurrent()`。若在连接面板里直接 abort，`operationFinished`
  会走"接收会话失败"的自动重连路径，表现为"断开后自己又连上"。换中继地址的路径
  因此固定为 **断开 → 连接**。
- **no-auth 路径**：服务端 `--allow-no-auth` 时客户端必须显式 `serverAllowNoAuth`，
  不从空口令推断——漏填口令应报错，不该静默降级。CLI 侧"`--password` 与
  `--no-auth` 必须恰好给一个"的**后半条**曾缺失（只拦了"同时给"），导致漏配的
  客户端拿着空口令连上去、从协议错误里反推参数问题；2026-10-23 补上，由 e2e
  的 `no-auth` 场景看门（`parseArgs` 在匿名命名空间里，单测够不着）。
  口令客户端撞上无认证中继时，服务端会跳过握手直接发 `answer_id`，客户端按
  相位错误拒绝并追加"请加 --no-auth"的提示——这是**有意设计**（`Client.cpp`
  的 onClosed 分支），不是意外。

### 0.7 起手式

1. `git log --oneline -8` 看最近改动，`ctest` 确认基线（7/7）。
2. **终端中文乱码**先看 `common/include/relayer/common/Console.h`：`main()` 开头
   是否调了 `init()` + `use_utf8_locale()`。前者管 C++ 流（转宽字符写
   `WriteConsoleW`），后者管 `printf` 系；被重定向时两条路都不改字节。
   `common/` 里放它是因为 cli 与 server 都要——不要挪进任一上层。
2. 要改 GUI：`app/bridge/CoreService.{h,cpp}` 是唯一的 Qt 边界，先读它，再读
   `cli/include/relayer/cli/Client.h`（GUI 的 worker 就是它）。
3. 要改传输：`core/transfer/TransferPeer.h` 的 `Config` / `started()` / `Stats` 是契约面。
4. 要改协议：先改 `docs/PROTOCOL.md`，再改 `Frame.h`——`protocol_doc` 测试会逐项比对。
5. 每次提交前跑一次 Release + `-Werror`：`-DCMAKE_BUILD_TYPE=Release -DRELAYER_WERROR=ON`。
6. Debug 构建**不开** `-Werror`：变量遮蔽、`qsizetype` 转换这类问题在 Debug 全绿、
   Release 必红。别只跑 Debug 就提交。

### 0.8 E4 实链路验收清单（唯一剩余项）

目标：拿到一份**两台机器经中继**的收发记录（不是本机 loopback）。本机上的
`e2e` 已经覆盖协议与落盘正确性，这一步验的是"换到真实网络/真实用户操作还成立"。

前置：两台机器（A = 中继 + 发送方，B = 接收方），同一 `build/bin`，
B 的防火墙放行入站无要求（客户端只发起出站）；中继端口默认 9527。

1. **中继**（A）：`relayerd --port 9527 --password <口令>`，
   确认输出 `relayerd 已启动 … 认证=开启`。
2. **B 连上**：GUI 填 `A的IP:9527` + 口令 → 记下 `[身份] 本机 id = …`。
   - 连点三次「刷新」，id **不得变化**（"点一次刷新 id 涨一号"是回归红线）。
3. **A 连上**：同样连接，右键 B 的 id → 设为通信对象。
4. **传一个中等文件**（≥50MB，跨网段才看得出吞吐）：B 的日志出现
   `[接收] 等待接收，落点 …` → `[接收完成] …`；对端文件哈希与本机一致。
   - 任务/日志里的速度应是合理的量级（局域网千兆 ≈ 100 MiB/s 量级，
     公网看带宽）。速度列出现 `-` 只在传输开始的 200ms 内，属正常。
5. **批量**：对照表填 3 行（两个文件 + 一个目录），勾选 → 发送。
   逐行状态 → 完成；中断一行（故意填错对端）应为"失败"而**不中断整批**。
6. **停止/重传**：传输中点「停止」，当前行"已取消"、剩余行"已取消"；
   点「重传失败项」能重跑失败的项。
7. **Setting**：点「设定」→ 关闭自动重试 → 杀掉中继，B 的日志应报一次失败后
   停止重试（不再每 3 秒刷）。改回来。
8. **无认证路径**（可选）：中继改 `--allow-no-auth`，两端勾「无认证」，
   重复第 4 步；然后故意不勾、填口令连一次，应看到"请加 --no-auth"的提示。
9. **自投**（发给自己）：在 B 上右键**自己的** id → 发送文件。应正常传完、
   落到自己的接收目录（服务端把帧绕回本连接，§11.3.1）。列表里自己标红，
   选中自己不报错是**预期行为**。命令行等价物：
   `relayer-cli recv --to-dir <目录> --id me --self-send <文件> -p <端口> --password <口令>`
   —— 这条也是 e2e `self-send` 场景跑的东西，先跑它确认环境没问题。
10. **固定标识**：在 B 的「本机标识」填 `bob` 连接；再用第二个实例填 `bob`
    连接，应被拒并提示"已被占用"，而第一个实例仍能收发。
11. **共享目录 + 远端浏览**：两个浏览面板在窗口下半部**左右并排常驻**
    （左边"本地"，右边"远端"，v1 形状）。B 点连接面板的「设定」→「共享」
    选一个目录（**保存后要断开重连才生效**，会话启动时已拷走配置）；
    A 的对端表右键 →「浏览此对端的目录」，右侧分组标题应变成
    `远端 —— <对端名>`，路径显示 `bob:/`，状态行给出项数，双击目录进入、
    上一级返回、按钮"共享根"。
    三种失败都要**在状态行**看到红字（不只是日志）：对端没开共享 →
    "对端拒绝列举（未开启共享，或路径越界）"；对端离线 → 约 10 秒后
    "对端没有响应（可能已离线）"；没连接 → 弹窗 + 日志。
    远端模式右键只有"刷新"、不能拖出（载荷是本地路径），这是有意为之。
    命令行等价物：`relayer-cli recv --to-dir <目录> --share <目录> --self-browse`
    —— 这正是 e2e 的 `self-browse` 场景，先跑它确认环境没问题。
12. **下载（远端取回）**：右侧远端面板里右键一个**文件** →「下载到接收目录」。
    日志应出现 `[取回] 对端已开始发送，文件将落到接收目录`，随后
    `[接收完成] <路径>`，文件出现在本机接收目录里、内容一致。
    三种失败要在日志里看到原因：对方没开共享 →「对端拒绝取回」；文件不存在
    →「对端没有这个文件」；右键**目录**时该菜单项是灰的（归档未实现）。
    命令行等价物：`relayer-cli recv --to-dir <目录> --pull-from <对端id>
    --pull-path <相对路径> ...`，对应 e2e 的 `pull-file` 场景。
13. **下载目录（打包）**：右侧远端面板里右键一个**目录** →「下载目录（打包为 tar.xz）」。
    日志先出现 `[取回] 对端已开始发送`，随后 `[取回] 已解包到 <接收目录>\<目录名>`。
    接收目录里应出现**该目录本身**（含子目录与空目录），而**没有** `.tar.xz`
    ——包解开后已删除。层次要与对方一致：`<目录名>/a.txt`，不是散在接收目录里。
    右键**文件**时给的是「下载到接收目录」，两项不会同时出现。
    命令行等价物：`relayer-cli recv --to-dir <目录> --pull-from <对端id>
    --pull-dir <相对路径> ...`，对应 e2e 的 `pull-dir` 场景。
14. **压缩 / 解压缩**：左侧本地面板选中一个目录（或若干项）→ 右键「压缩为
    tar.xz…」→ 选保存位置，状态行显示"已压缩为 xxx.tar.xz"，列表刷新后
    出现该包。再选中这个 `.tar.xz` → 右键「解压缩」→ 应在同目录解出原来的
    目录，且**没有多套一层**（不应出现 `sub/sub/`）。
    注意两种包的形状不同：手动压缩的包**带顶层目录**，解压时解到父目录；
    取回目录的包**不带顶层**，解到按包名建的子目录。混用会多套一层。
15. **浏览面板的「传输」与 SHA256**：左侧本地面板里选中若干文件或目录 →
    右键「传输（发送到当前通信对象）」→ 任务表应开始发送（对端取连接面板里
    设的「当前通信对象」，没设会弹提示）。选中一个**文件**右键「SHA256」→
    状态行显示 64 位十六进制并已复制到剪贴板，日志也有一行。
    目录会递归展开；文件用"父目录 + 文件名"的形状。
16. **批量并发**：对照表里放 3 行，分别发往 **3 个不同对端**，点发送。
    三行应**同时**变成「传输中」，日志里三条 `[任务] N/M` 连续出现（不是
    一条完了才出下一条），三行的进度各自独立跳动（速度不一样）。
    然后把 3 行都指向**同一个对端**再发一次：这次应当**串行**——第二行
    要等第一行结束。原因：对方的接收端一次只收一个文件，同时发两个第二
    个会被拒。
    并发上限是 3（`kConcurrency`）；超出部分排队。
17. **检查远端**：
    A 的对端表右键 →「检查远端」对 B 的某个文件行执行，日志应给出
    "已存在且大小一致 / 大小不符（本地 X，对方 Y）/ 对方没有 / 无法确认（原因）"。
    命令行等价物：
    `relayer-cli recv --to-dir <目录> --id bob --share <共享目录> ...` +
    `relayer-cli ls --to bob --path <相对路径> ...`
    —— 这条对应 e2e 的 `remote-list` 场景，先跑它确认环境没问题。

记录格式：把每步的关键日志行贴进下一次的工作日志（`.workbuddy/memory/`），
并在本节末尾追加"已完成 + 日期"。**没跑过的步骤不要标 ✅**。

> 已在本机验证过的部分（可作为对照基线）：GUI 连接/刷新 id 稳定、接收、
> 对照表批量发送、任务暂停重传、Setting/ReplaceArea/ProgressTracker 三项
> 均在本机中继 + loopback 上跑通过；`e2e` 13 场景（含 1GB）全过。

---

## 1. 一句话状态

阶段 D 完成。三进程（`relayerd` + 发送端 + 接收端）集成测试
`tests/e2e/run_e2e.py` 覆盖 §3.5.3 表格的 8 类场景，全部通过；含 1GB 的
完整套件 **295 秒**跑完，10 个场景 0 失败。

这一层总共揪出**六个**只有真进程才暴露的缺陷，其中四个是产品缺陷
（会卡死 / 会毁数据）、两个是测试自身的缺陷。全部已修，见 §2。

---

## 2. 阶段 D 修掉的缺陷总表

| # | 症状 | 根因 | 性质 |
| --- | --- | --- | --- |
| 3.1 | 大文件传不完，两端各占 7% CPU 空转 | `FrameReader::next()` 每次 4KB 读入都全量拷贝缓冲，O(n²) | 产品 |
| 3.2 | 32MB/1MB 块必定停在第 27MB | 重传段被窗口配额卡住 + 定时器看不见，两头都以为没活干 | 产品 |
| 3.3 | 坏残片一旦出现，重试永远救不回来 | 根校验失败但不删残片，下一轮继续在坏数据上续传 | 产品 |
| 3.4 | 中继统计"转发成功"而实际失败 | `forward()` 返回值被 `(void)` 丢弃 | 产品（轻微） |
| 5.1 | 接收端 `--idle-timeout 8` 时在传输开始前自杀 | `checkIdle` 只给发送方的摘要阶段豁免，漏了接收方等首个请求 | 产品 |
| 5.2 | 200MB/1GB 稳定卡在 94% | e2e 先等发送端、无人读接收端管道，管道写满后接收端阻塞在 `write` | **测试** |

前四个见 §3，后两个见 §5。

---

## 3. 四个产品缺陷

### 3.1 `FrameReader::next()` 的 O(n²) 拷贝 —— 传输越跑越慢直至卡死

**症状**：大文件传不完。`relayerd` 和 `relayer-cli` 各占约 7% CPU 空转，
传输像是在动、但永远不结束。

**根因**：`next()` 每次被调用都把整个内部缓冲
`std::vector<char> contiguous(buffer_.begin(), buffer_.end())` 全量拷一遍。
而 `ConnectionOptions::readChunk` 默认只有 **4096 字节**，一个 1MB 的块帧
要 **256 次** `async_read_some` 才凑齐。每一次 4KB 读入都触发一次全量拷贝
→ 单帧 ≈ 256 × 平均 512KB ≈ **130MB 纯 memcpy**。

更糟的是它是正反馈：接收端一落后，累积缓冲变大，每次拷贝更贵 →
"越慢越慢" → 窗口彻底压死。

**修复**（`core/src/transport/FrameReader.cpp`）：改成两阶段。

1. 只拷固定头 **36 字节**（`kFixedHeaderSize`），从中读出帧长
   （`frameLen` 在头偏移 **28**，小端 u32，见 `kFrameLenOffset`）。
2. `avail < frameLen` 时**直接返回 false 等下一批字节，一个字节都不拷**。
3. 只有整帧收齐才拷那一次，交给 `unpackFrame()`。

**效果**：100MB / 1MB 块从"不可能完成"变成 **23 秒**（≈4.3MB/s）；
`relayerd` 累计 CPU 从持续 7% 降到 **0.27 秒**。core 单测也因为
`FrameReaderTest` 走同一路径，从 20.58s 降到 **16.99s**。

> 注意：`readU32` 原本在 `Frame.cpp` 的匿名 namespace 里，不对外。协议层
> 刻意只暴露 `unpackFrame` 一个入口，不让"直接摸帧头"变成公共 API。
> 所以 `FrameReader.cpp` 里复制了一个 `readU32LE` + 两个偏移常量。**改协议
> 布局时必须两处一起改**，好在 `header_crc` 那组测试会真的 pack 一帧再
> unpack，偏移错了必红。

### 3.2 `SendWindow` 的重传饥饿死锁 —— 32MB 传输必然停在第 27MB

**症状**：32MB 配 1MB 块，**必定**停在第 27MB（`acked=26/31, out=0`），
一直挂到 hard timeout。8MB 能过，100MB 反而能过（凑不出触发条件）。

**根因**：窗口约束是 `inflight + needRetransmit <= windowChunks`，而
`takeNextBatch` 用 `budget = min(maxCount, freeSlots())` 把**重传段也卡在
这个配额里**。当一批块同时超时：

```
inflight = 0, needRetransmit = 5, window = 4
→ byWindow = max(0, 4-5) = 0
→ freeSlots() = 0      ← 取重传块要 freeSlots > 0，而它恰好是 0
```

而 `checkRetransmit` 只扫 `outstanding()`（即 `kInflight`），也看不见这
5 个 `kNeedRetransmit` 块。**两头都以为没活干**，传输无声停住。

触发条件是"文件足够大 + 块足够大 + 一批块集中超时（3 秒内 5 块）"，
所以小文件全过、32MB 必炸、100MB 又蒙混过关。

**修复**（`core/src/transfer/SendWindow.cpp`）：把配额**分段**。

- **重传段**：不受窗口约束。`kNeedRetransmit` 本来就占着槽，取出来变回
  `kInflight` 只是"沿用原槽"，不消耗新配额。
- **新块段**：才受窗口约束（它们要**新占**槽）。

`freeSlots()` 同步改成 `needRetransmit() + min(byWindow, pending())`。
这两个函数必须逐条对齐——调用方 `hasWork()` 据它判断"有没有活干"，
不对齐就会重演同一个静默停住。

**验证**：8MB ✅ / 16MB ✅ / 20MB ✅（修复前必失败）/ 32MB ✅ / 100MB ✅。

### 3.3 坏残片污染 —— 一旦出现，重试永远救不回来

**症状**：`resume-forged-fragment` 场景，传输跑到 **100%**，然后报
`peer verify failed: root_mismatch`。发送端退出码 2。

**根因**：伪造残片（前 16MB 正确、后 16MB 是 `\x00`）被 `scanResumePoint()`
按**长度**当成有效半成品，从 16MB 处接着收。收满后算真实 merkle 根，发现
对不上——**但坏残片留在了盘上**。下一次传输又在它上面续传，于是永远
失败，症状一模一样（每次传满 100%，每次报 root_mismatch），运维会以为
"重试没用"。

协议层在 `open()` 阶段**没法**识破它：描述里只有整文件 sha256 和一个
**只覆盖前 64KB** 的 `fingerprintHex`，而伪造残片的前 64KB 恰好是对的。
唯一能发现的地方就是算完真实根的那一刻。

**修复**（`core/src/transfer/TransferReceiver.cpp` 的 `finalize()`）：
根校验失败时 `(void)store_.remove(partPath_)`，让下一次传输自愈。
错误文案补上 `partial file discarded, retry from scratch`。

**连带改了一条单元测试**：`TransferReceiverTest.cpp` 里原有用例名是
`... reports a root mismatch and **keeps** the part`，断言残片仍然存在，
理由是"留着可续传"。那个理由只在残片内容真实可信时成立；这里残片已被
证明内容不对，留着只会让下一次从错误偏移接着收。用例已改名为
`... and discards the part`，断言反转为 `REQUIRE_FALSE(...exists)`。
**这是语义变更，不是断言放宽** —— 以后看到这条测试不要"修回去"。

### 3.4 附带修复：`forward()` 返回值被丢弃

`server/src/Server.cpp` 里 `it->second->forward(frame, ...)` 的返回值原先
是 `(void)` 丢掉的。代价是一次真实误判：对端写队列满时 `forward()` 返回
`kNoSpace` **并关闭那条连接**，而服务端继续按"转发成功"计数——中继侧
统计一切正常，发送方却永远等不到确认，排查方向被引向网络丢包。

现在只计数不改行为（中继没有"投递失败"这类帧可回，§9 靠超时自己发现），
但运维能从 `stats_.forwardFailed` 看出问题在对端。`Server.h` 新增三个计数：

```cpp
std::uint64_t forwarded = 0;
std::uint64_t forwardFailed = 0;  // 目标在线但投递失败
std::uint64_t unroutable = 0;     // 目标不在线
```

---

## 4. 三进程集成测试

`tests/e2e/run_e2e.py`，纯标准库 Python，无第三方依赖。

设计要点：

- 每个场景自己的 `workdir`，自己起 `relayerd`（随机空闲端口）、自己起
  收发端，跑完 `shutdown()` 全清。场景之间零共享状态。
- **不用 `sleep` 猜时序，等契约行**：服务端启动等 `已启动`，接收端等
  `ASSIGNED_ID=client-N`，发送端结束时等 `STATS`（见 §4.3）。
- 收发端一律加 `-q`。**进度条用 `\r` 刷新会覆写 `ASSIGNED_ID=` 行**，
  导致脚本抓不到 id；同时它也是 §5.2 那个管道事故的燃料。
- 不能用 `list | grep | head -1` 猜接收端 id：id 递增分配，每条新连接
  （包括 `list` 自己）都消耗一个号。
- 子进程输出**直连文件**而不是 `subprocess.PIPE`，理由见 §5.2。

已注册进 CTest（`tests/CMakeLists.txt` 的 `e2e_quick`，标签 `e2e`，
`--quick` 跳过 slow，`TIMEOUT 900`）。完整覆盖手工跑不带 `--quick`。

### 4.1 覆盖矩阵

| §3.5.3 场景 | 用例 | 状态 |
| --- | --- | --- |
| 2MB 正常传输 | `transfer-2mb` | ✅ |
| 200MB 正常传输 | `transfer-200mb`（slow） | ✅ 37s |
| 1GB 正常传输 | `transfer-1gb`（slow） | ✅ 200s |
| 中途 kill 发送端 | `kill-sender` | ✅ 17.6s × 5 连跑 |
| 预置正确残片 | `resume-valid-fragment` | ✅ |
| 预置伪造残片 | `resume-forged-fragment` | ✅ 两轮验证 |
| 空文件 / 1 字节 | `empty-file` / `one-byte` | ✅ |
| 目标被占用 | `target-busy` | ✅（Windows 专属，其他平台 SKIP） |
| 乱序 / 丢包 | `drop-chunks` | ✅ 见下 |
| 无认证中继 | `no-auth` | ✅ 2026-10-23 补：可传 + 两类拒绝（见 §0.6 末条） |
| 固定标识 | `fixed-id` | ✅ 2026-10-23：声明生效 / 撞名被拒 / 先到者不受影响 / 断开后名字可重用 |
| 自投（发给自己） | `self-send` | ✅ 2026-10-23：帧绕回本连接，内容一致、无残片 |
| 远端目录列举 | `remote-list` | ✅ 2026-10-23：共享根可列 / 子目录可列 / `..` 越界被拒 / 未共享被拒 |
| 目录取回（打包） | `pull-dir` | ✅ 2026-10-23：对端打包 tar.xz（含空目录）/ 解开后内容一致 / 指向文件报 kNotFound / `..` 越界报 kRejected |
| 远端取回（下载） | `pull-file` | ✅ 2026-10-23：取回存在文件（内容一致）/ 不存在报 kNotFound / `..` 越界报 kRejected / 目录明确拒绝 |
| 会话内列举（GUI 路径） | `self-browse` | ✅ 2026-10-23：`recv --share --self-browse` 自投，覆盖 `browseOnSession`（搭在 kReceive 会话上，与 `ls` 的一次性连接不是同一份代码） |

### 4.2 `drop-chunks`：丢包场景怎么测的

真实 socket 上制造**稳定可复现**的丢包很难（`SO_RCVBUF`、`iptables`、
代理在 Windows 上都不可用，且丢哪一块完全不可控）。所以给 `relayerd`
加了测试专用参数：

```
relayerd --drop-data-frames 3,5     # 丢掉第 3、第 5 个转发的传输数据帧
```

- 只作用于 `kChunkRequestData`。丢控制帧测的是另一件事（请求被吞），
  两者失败表现都是超时，混在一起无法归因。
- 序号按**所有**数据帧递增（含被丢的），所以序号是"全局第几个"，不会
  因为前面的帧被丢而错位。
- 启动时打一条 `警告: 测试模式…`：一个悄悄丢帧的中继比一个崩溃的中继
  难查得多。

断言的核心**不是**"最终成功"（那只能证明没卡死），而是重传块数
**恰好等于**丢失块数：

```
STATS chunksSent=10 chunksRetransmitted=2   # 8 块文件，丢 2 块
```

- `== 丢的块数` → SACK 位图生效，只补空洞
- `≈ 总块数`   → 退化成整份重传。内容仍然正确，**只有这个数字能看出来**
- `0`          → 没发现丢包，等最终校验才失败

**篡改验证过**（证明断言不是恒真）：把丢帧改成 `1,2,3,4`，计数随之变成
`chunksSent=12 chunksRetransmitted=4`。改这个参数时记得跑一次对照。

### 4.3 `--stats`：可观测性是断言的前提

`TransferPeer::Stats` 新增 `chunksRetransmitted`（`SendWindow` 累计
"取出用于重传的块次"），`relayer-cli --stats` 在结论前打一行机器可读的：

```
STATS chunksSent=10 chunksReceived=0 chunksRetransmitted=2 duplicateChunks=0 acksSent=0 acksReceived=10
```

默认不打印——正常用户只需要"发送完成"一句，计数只会让人以为出了问题。

### 4.4 修掉的两个测试自身 bug

1. **f-string 求值顺序**。`expect(not target.exists(), f"...{target.stat().st_size}...")`
   里 f-string 在调用**前**求值，而断言成立的前提恰恰是文件不存在 →
   抛 `FileNotFoundError`，一条本该通过的断言变成 traceback。
   改成 `if target.exists(): raise E2EFailure(...)`。
2. **Windows 独占句柄**。`target-busy` 里 PowerShell 以共享模式 `None`
   持有目标文件期间，Python `open()` 直接 `PermissionError`。必须先
   `held.kill()` 释放句柄，再去校验内容——要验的是"传输有没有动这个
   文件"，不是"持有期间能不能读"。

---

## 5. 续接轮：两个"看着像产品 bug"的排查

### 5.1 接收端在传输开始前自杀（**产品缺陷**）

**症状**：`kill-sender` 从"偶发不稳"变成**稳定失败**（3/3、5/5）：接收端
起来后 60 秒内 `dst/` 一直是空的，发送端拿到 id 后毫无动静。

**根因**：`cli/src/Client.cpp` 的 `checkIdle()` 里，只给**发送方**在
"摘要阶段"开了豁免：

```cpp
if (action == Action::kSend && !peer->started()) { /* 不算空闲 */ }
```

接收方在等第一个 `kFileRequestSend` 时没有对应豁免。而 `TransferPeer.h`
里 `started()` 的契约本来就写着"发送方：收到 kFileAnswerSend 后为真；
**接收方：open 成功之后为真**"，注释还明说"上层需要这个信号来判断沉默
是不是正常，见 cli/Client.cpp 的 checkIdle"。**实现漏了接收方这一半。**

在 `--idle-timeout 8` 下，接收端认证完 8 秒就退出了；而 Debug 构建里
发送端光是算 200MB 的摘要就要超过这个时间，于是请求还没发出对端就没了。
发送方看到的是 `peer did not answer the send request`——看着像中继丢包。

**修复**：判据交给传输层，按角色分别判改为 `if (!peer->started())`。
`idleTimeout` 的语义收窄为"**传输开始后**多久没有进展"，`--hard-timeout`
才是"等多久没等到就放弃"。help 文本与 `Client.h` 的注释同步更新。

**验证**：`kill-sender` 5/5 通过，单次 17.6s（原本 79s 全失败）。

### 5.2 管道背压把接收端堵死（**测试缺陷**）

**症状**：完整套件里 200MB 与 1GB **稳定失败**，发送端退出码 3（超时），
两端进度都停在 94%，接收端 120 秒无进展。手工用同样参数跑 200MB 却
一次通过（763 块、零重传）。

**根因**：`Process` 原来用 `subprocess.PIPE` 收输出，而测试的写法是

```python
expect_eq(sender.wait_exit(600), 0, ...)   # 这期间没人读 recv
expect_eq(recv.wait_exit(120), 0, ...)
```

接收端每 10ms 打一行进度到 stderr。Windows 管道缓冲只有几 KB，跑满之后
接收端**阻塞在 `write` 调用里**，事件循环随之停摆——不收帧、不回 ACK。
发送端窗口满，两端一起静止，120 秒后发送端判 idle 超时。

验证方式很干脆：同样的参数加 `-q`（不打印进度）手工跑 → 通过。
`2MB` 能过是因为输出量小；`kill-sender` 在 8MB 处就杀了，也来不及堵。

**修复**：`Process` 改为把子进程 stdout/stderr **直连文件**
（`stdout=打开的文件句柄`），`pill()` 按偏移增量读文件。文件没有容量
上限，读不读只影响断言的实时性，永远不会反压被测进程。

同时加了 `Cluster.wait_all([sender, recv], timeout)`：串行等会把"先失败
的那一端"埋在后面的等待里（1GB 场景要白等三分钟）。

**这个缺陷的教训比修法重要**：测试脚手架的反压会伪装成产品故障，而且
伪装得很像——两端停顿、超时、日志里什么错都没有。以后看到"传输卡在
某个百分比"，先确认**测试自己在读输出**。

**顺带暴露的产品脆弱点**（本轮未修，记在 §7）。

---

## 6. 排查过程中踩的三个误判（留给后来人避坑）

### 6.1 "从未重传"

现象：`acked` 与 `sentAt` 恒差 4，看起来像从未重传过。
原因：`chunkSentAt_` 是 `std::map`，**同一块重发不增加 `size()`**。
加诊断后实测是 **47~50 次重传**。
教训：用 `size()` 判断"发生过多少次"只对只增不减的容器成立。

### 6.2 "服务端丢帧"

现象：接收端落后于发送端，怀疑中继吞帧。
加 `[fwd]` / `[fwdstat]` 诊断后结论是 `to1=21 to2=29 unroutable=0`
——**转发全部成功**。真正的瓶颈是接收端解析，即 3.1 的 O(n²)。

### 6.3 "链接器没有任何诊断"

现象：`cmake --build` 报

```
collect2.exe: error: ld returned 1 exit status
FAILED: bin/relayerd.exe
```

**完全没有** undefined reference 之类的诊断信息。

原因：目标 exe 正被一个还在跑的进程占用（测试留下的 `relayerd` 没有
退出），Windows 不允许覆盖正在执行的映像，ld 又没把这个错误传上来。

处理：`taskkill //F //IM relayerd.exe` 之后重编即可。
**看到"ld 静默失败"先查进程占用，不要怀疑编译选项。**

---

## 7. 已知限制（本轮不做）

1. **进度输出会阻塞事件循环**。CLI 在 io_context 线程上同步 `fprintf`
   stderr，如果 stderr 被接到一个慢消费者（`| less`、卡住的终端），传输
   会被一起拖停。§5.2 就是这个机制的极端形态。真修法是把输出移到独立
   线程或做非阻塞写，成本不低，且只在异常环境下暴露，暂不处理。
2. **`target-busy` 在非 Windows 平台 SKIP**。POSIX 没有强制独占语义，
   造不出同样条件——与其造假，不如明说没覆盖。
3. **1GB 场景 184 秒**，其中相当一部分是 Debug 构建里 SHA-256 / Merkle
   的开销。Release 下会快得多，但没有实测记录。

---

## 8. 验证记录（2026-10-22 续接轮）

| 项目 | 结果 |
| --- | --- |
| 构建 | ✅ 无警告（`relayer_core` / `relayerd` / `relayer-cli` / `core_test`） |
| ctest 全量 | ✅ **7/7**（core 335 用例 / 5451 断言；e2e_quick 64.9s；总计 87s） |
| e2e `--quick` | ✅ **8/8**，57.3s |
| e2e 完整（含 1GB） | ✅ **19/19**，192s（Release） |
| `kill-sender` × 5 | ✅ 全过，单次 17.6s |
| `drop-chunks` × 3 | ✅ 全过，单次 5.1s；篡改参数后计数随之改变 |
| 手工 200MB | ✅ 33s，`cmp` 一致，无 `.relaypart` 残留 |

---

## 9. 代码清理状态

排查过程中在 `TransferPeer` / `TransferSender` / `Connection` 里加过大量
`fprintf` 诊断（`[pump]` `[rxack]` `[take]` `[txchunk]` `[retx]` `[req]`
`[busy]` `[rxchunk]` `[ack]` `[onframe]` `[rpump]` `[spump]` `[state]`
`[batch]` `[ready]` `[conn]` `[fwd]` `[fwdstat]` `[pumpS]`），**已全部移除**。
`grep fprintf core/src/transfer cli/src` 只剩 CLI 的正常输出。

保留的实质修改：

- `handleRequestSend` 里的 `toPeerId_` 兜底（服务端角色下 `frame.from`
  可能为空，用 `session_->assignedId()` 顶）。
- `Server` 的 `--drop-data-frames`（测试专用，已在 help 与启动日志里
  标明）。

---

## 10. 下一步（写于阶段 D 结项时，**已完成**，现状见 §0）

阶段 D 结项，进**阶段 E：`app/` Qt 前端**（REWRITE-PLAN §3.7）。

进阶段 E 前先注意两件事：

1. `CMakeLists.txt` 里 `RELAYER_BUILD_GUI` 默认 **ON**，但 `app/` 目录
   还不存在 → 新克隆的机器 `cmake -S . -B build` 会直接失败。目前要么
   加 `-DRELAYER_BUILD_GUI=OFF`，要么先建目录。建议在 `app/` 落地时
   一起解决，不必单独改。
2. `cli/Client.{h,cpp}` 就是 GUI 侧 worker 的参考实现（不含 Qt），
   `TransferPeer` 的回调式接口可以原样接 Qt 信号。

---

## 11. 环境备忘

- 仓库：`D:\Code\relayer`（**不是** `D:\Code\relayFile`，那是 v1 旧仓库）
- C++17 / CMake 3.20+ / Ninja / GCC 16.2.0（MSYS2 UCRT64）/ Windows
- 产物名：`relayerd`（**不是** `relayd`）、`relayer-cli`、`relayer`
- 服务端不链接 Qt，也不链接 filestore
- 跨模块边界不用异常，全走 `Status` + `ErrorCode`

### 常用命令

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DRELAYER_BUILD_GUI=OFF -DRELAYER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure          # 单元 + 文档检查 + e2e_quick
python tests/e2e/run_e2e.py --quick                 # 三进程（快）
python tests/e2e/run_e2e.py                         # 三进程（含 200MB/1GB）
python tests/e2e/run_e2e.py -k kill-sender --repeat 3   # 查时序不稳
E2E_KEEP=1 python tests/e2e/run_e2e.py -k drop-chunks   # 保留现场看日志
```

### 踩过的环境坑

- **ninja 报 `no work to do`**：源文件改动后 `cmake --build` 常识别不到。
  强制重建：`rm -f build/core/CMakeFiles/relayer_core.dir/src/.../<File>.cpp.obj`，
  或 `touch -d "now" <file>`。
- **链接静默失败**：见 §6.3，先查进程占用。
- **GitHub 22 端口不通**：`git push` 报
  `Connection reset by ... port 22` / `Could not read from remote repository`。
  这是网络侧阻断 SSH 22，不是密钥问题（`ssh -T -p 443 git@ssh.github.com`
  能正常认证）。仓库已配好本地改写，`origin` 现在直接走 443：

  ```bash
  git config --local url."ssh://git@ssh.github.com:443/".insteadOf "git@github.com:"
  ```

  这条只在**本地**配置里，新克隆的机器需要重新执行一次。
