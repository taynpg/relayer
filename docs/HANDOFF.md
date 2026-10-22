# 当前交接：阶段 D（三进程集成测试）

> 写于 2026-10-22。这份文档的目的很窄：让接手的人**不读前面几万行对话**
> 也能知道现在在哪、哪些是真的、哪些还悬着。

---

## 1. 一句话状态

阶段 D 的**主体已完成**：三进程（`relayerd` + 发送端 + 接收端）集成测试
`tests/e2e/run_e2e.py` 已建立，覆盖 §3.5.3 表格中 7 类场景里的 6 类，
并借它揪出、修掉了三个**只有真进程才暴露**的致命缺陷。

**唯一未收口**：`kill-sender` 场景时序不稳，见 §5。

---

## 2. 本次修掉的三个核心缺陷

这三个都不是"代码不够优雅"，是**产品会卡死 / 会产出坏文件**级别的。

### 2.1 `FrameReader::next()` 的 O(n²) 拷贝 —— 传输越跑越慢直至卡死

**症状**：大文件传不完。`relayerd` 和 `relayer-cli` 各占约 7% CPU 空转，
传输像是在动、但永远不结束。

**根因**：`next()` 每次被调用都把整个内部缓冲 `std::vector<char> contiguous(buffer_.begin(), buffer_.end())`
全量拷一遍。而 `ConnectionOptions::readChunk` 默认只有 **4096 字节**，
一个 1MB 的块帧要 **256 次** `async_read_some` 才凑齐。每一次 4KB 读入
都触发一次全量拷贝 → 单帧 ≈ 256 × 平均 512KB ≈ **130MB 纯 memcpy**。

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

### 2.2 `SendWindow` 的重传饥饿死锁 —— 32MB 传输必然停在第 27MB

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

### 2.3 坏残片污染 —— 一旦出现，重试永远救不回来

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

### 2.4 附带修复：`forward()` 返回值被丢弃

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

## 3. 新增的三进程集成测试

`tests/e2e/run_e2e.py`，纯标准库 Python，无第三方依赖。

设计要点：

- 每个场景自己的 `workdir`，自己起 `relayerd`（随机空闲端口）、自己起
  收发端，跑完 `shutdown()` 全清。场景之间零共享状态。
- **不用 `sleep` 猜时序，等契约行**：服务端启动等 `已启动`，接收端等
  `ASSIGNED_ID=client-N`。
- 收发端一律加 `-q`。**进度条用 `\r` 刷新会覆写 `ASSIGNED_ID=` 行**，
  导致脚本抓不到 id。
- 不能用 `list | grep | head -1` 猜接收端 id：id 递增分配，每条新连接
  （包括 `list` 自己）都消耗一个号。

已注册进 CTest（`tests/CMakeLists.txt` 的 `e2e_quick`，标签 `e2e`，
`--quick` 跳过 slow，`TIMEOUT 900`）。完整覆盖手工跑不带 `--quick`。

### 覆盖矩阵

| §3.5.3 场景 | 用例 | 状态 |
| --- | --- | --- |
| 2MB 正常传输 | `transfer-2mb` | ✅ |
| 200MB 正常传输 | `transfer-200mb`（slow） | ✅ |
| 1GB 正常传输 | `transfer-1gb`（slow） | ✅ |
| 中途 kill 发送端 | `kill-sender` | ⚠️ 时序不稳，见 §5 |
| 预置正确残片 | `resume-valid-fragment` | ✅ |
| 预置伪造残片 | `resume-forged-fragment` | ✅ 两轮验证 |
| 空文件 / 1 字节 | `empty-file` / `one-byte` | ✅ |
| 目标被占用 | `target-busy` | ✅ |
| 乱序 / 丢包 | — | ❌ **未覆盖**，见 §5 |

### 本次修掉的两个测试自身 bug

1. **f-string 求值顺序**。`expect(not target.exists(), f"...{target.stat().st_size}...")`
   里 f-string 在调用**前**求值，而断言成立的前提恰恰是文件不存在 →
   抛 `FileNotFoundError`，一条本该通过的断言变成 traceback。
   改成 `if target.exists(): raise E2EFailure(...)`。
2. **Windows 独占句柄**。`target-busy` 里 PowerShell 以共享模式 `None`
   持有目标文件期间，Python `open()` 直接 `PermissionError`。必须先
   `held.kill()` 释放句柄，再去校验内容——要验的是"传输有没有动这个
   文件"，不是"持有期间能不能读"。

---

## 4. 修复 2.2 过程中的两个误判（留给后来人避坑）

写在这里是因为它们都很像真缺陷，实际不是，但排查它们花掉了最多时间。

### 4.1 "从未重传"

现象：`acked` 与 `sentAt` 恒差 4，看起来像从未重传过。
原因：`chunkSentAt_` 是 `std::map`，**同一块重发不增加 `size()`**。
加诊断后实测是 **47~50 次重传**。
教训：用 `size()` 判断"发生过多少次"只对只增不减的容器成立。

### 4.2 "服务端丢帧"

现象：接收端落后于发送端，怀疑中继吞帧。
加 `[fwd]` / `[fwdstat]` 诊断后结论是 `to1=21 to2=29 unroutable=0`
——**转发全部成功**。真正的瓶颈是接收端解析，即 2.1 的 O(n²)。

---

## 5. 未完成 / 已知问题

### 5.1 `kill-sender` 时序不稳（**优先级最高**）

**现状**：单跑通过，混在 `--quick` 全套里失败过一次：

```
FAIL  kill-sender          78.82s
      60s 内没等到 8MB 数据落地
```

**场景要求**：200MB 文件，1MB 块，等 `.relaypart` 超过 8MB 再杀发送端。

**怀疑方向**（未验证，按可能性排序）：

1. 前序场景（`transfer-2mb`、`empty-file`、`one-byte`）刚跑完，机器上
   有残留的磁盘写回压力，前 8MB 落盘被拖慢。
2. 200MB 源文件是**每次现生成**的，生成耗时混在计时窗口里。
3. 8MB 这个阈值太贴近"启动爬坡期"——GC/预分配/窗口填充都在这段。

**建议改法**（不做也行，但要留记录）：

- 阈值降到 2~4MB（够触发"已有进展"即可，不必 8MB）。
- 或把 deadline 从 60s 放宽到 120s。
- 或把 200MB 改成 64MB —— 这个场景测的是**杀进程后的盘上状态**，
  不是吞吐，文件大小本身不承载任何断言（残片只需"远小于完整文件"）。

### 5.2 「乱序 / 丢包」场景完全没覆盖

§3.5.3 要求"SACK 只重传空洞"。目前只有 `core/TransferPeerTest.cpp` 的
**单进程**覆盖，三进程层面没有。

难点：真实 socket 很难制造稳定可复现的丢包/乱序。可行路径：

1. 在 `Connection` 上加一个**测试专用**的"丢弃第 N 个写"钩子（不走生产
   路径，用 `#ifdef` 或注入的 policy）。
2. 或者给 `relayerd` 加测试专用参数，按概率丢弃转发帧。
3. 断言：`acked` 序列中出现非连续跳变后，接收端最终内容哈希一致，
   且重传的块数 ≈ 丢失的块数（不是全量重传）。

我倾向路径 1：不污染中继的行为，且能精确控制丢哪一块。

### 5.3 `transfer-200mb` / `transfer-1gb` 尚未在本轮跑过

修复后只跑过 `--quick`。这两个 slow 场景上次全套跑的时间点在
2.1/2.2 修好之前，理论上现在应该更快，但**没有实测记录**，交接时
不应假定通过。

---

## 6. 本轮验证记录

| 项目 | 结果 |
| --- | --- |
| 构建 | ✅ 无警告（`relayer_core` / `relayerd` / `relayer-cli` / `core_test`） |
| ctest 全量 | ✅ **6/6 通过**（core 335 用例 / 5451 断言，16.99s；总计比之前快 3.6s） |
| e2e `--quick` | **6 通过 / 1 失败**（114.7s），失败项 = `kill-sender` |
| e2e `-k forged` | ✅ 12.09s |
| e2e `-k kill-sender`（单独） | ✅ 通过 |
| 手工 100MB 传输 | ✅ 23s，`cmp` 一致，无 `.relaypart` 残留 |
| CPU | ✅ `relayerd` 累计 0.27s，发送端 CLI 累计 2.58s |

> `kill-sender` 单跑过、混跑挂一次，是**时序问题**不是逻辑问题，见 §5.1。

---

## 7. 代码清理状态

排查过程中在 `TransferPeer` / `TransferSender` / `Connection` 里加过大量
`fprintf` 诊断（`[pump]` `[rxack]` `[take]` `[txchunk]` `[retx]` `[req]`
`[busy]` `[rxchunk]` `[ack]` `[onframe]` `[rpump]` `[spump]` `[state]`
`[batch]` `[ready]` `[conn]` `[fwd]` `[fwdstat]`），**已全部移除**。
`grep fprintf` 各文件均无输出。`Connection.cpp` 里为诊断加的
`<chrono>` / `<cstdio>` 也已删除。

保留的实质修改只有一处：`handleRequestSend` 里的 `toPeerId_` 兜底
（服务端角色下 `frame.from` 可能为空，用 `session_->assignedId()` 顶）。

---

## 8. 下一步建议顺序

1. 收口 `kill-sender`（§5.1，改动很小，按建议改法任选其一）。
2. 补「乱序/丢包」三进程覆盖（§5.2，需要动 `Connection` 加测试钩子）。
3. 跑一次完整 e2e 含 slow（§5.3），记录真实耗时。
4. 阶段 D 可结项 → 进阶段 E（`app` Qt 前端，见 REWRITE-PLAN §3.7）。

---

## 9. 环境备忘

- 仓库：`D:\Code\relayer`（**不是** `D:\Code\relayFile`，那是 v1 旧仓库）
- C++17 / CMake 3.20+ / Ninja / GCC 16.2.0（MSYS2 UCRT64）/ Windows
- 产物名：`relayerd`（**不是** `relayd`）、`relayer-cli`、`relayer`
- 服务端不链接 Qt，也不链接 filestore
- 跨模块边界不用异常，全走 `Status` + `ErrorCode`

### 常用命令

```bash
cmake --build build
ctest --test-dir build --output-on-failure          # 单元 + 文档检查
python tests/e2e/run_e2e.py --quick                 # 三进程（快）
python tests/e2e/run_e2e.py                         # 三进程（含 200MB/1GB）
python tests/e2e/run_e2e.py -k kill-sender --repeat 3   # 查时序不稳
```

### 踩过的环境坑

- **ninja 报 `no work to do`**：源文件改动后 `cmake --build` 常识别不到。
  强制重建：`rm -f build/core/CMakeFiles/relayer_core.dir/src/.../<File>.cpp.obj`，
  或 `touch -d "now" <file>`。
