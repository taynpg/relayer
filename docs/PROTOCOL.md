# relayer 帧协议（RFM1）

> 本文是 relayer 线协议的**唯一权威说明**。实现见 `core/src/protocol/Frame.cpp`，
> 但读到本文就应当能写出一个能互通的服务端或客户端，不需要读代码。
>
> 协议版本：`2`（帧头布局沿用 v1，帧类型与语义在 v2 有扩展）
> 实现状态：帧编解码已实现并测；认证与传输状态机在设计确认中。

---

## 1. 设计边界

### 1.1 这个协议做什么

在两个对等端之间（或客户端与中继服务端之间）传**带路由信息的定长头 + 变长载荷**的帧。

### 1.2 这个协议不做什么

- **不做可靠性**。它假定底层是 TCP（或语义等价的可靠有序流），不重传、不排重、不乱序重排。
- **不做分片**。一帧要么完整到达，要么还没有；分块由上层传输引擎负责（见 §8）。
- **不做向前兼容的协商**。版本号不匹配直接断开（见 §5.3），不做能力协商。
- **不含时间戳**。所有依赖时间的判断（超时、nonce 有效期）都在连接层做，
  协议层只保证字节正确。

### 1.3 字节序

**全部字段小端（little-endian）**，与主机字节序无关。

`magic` 是唯一例外需要说明的：它按"字节序列 `R`,`F`,`M`,`1`"定义，
按小端读成 `u32` 时值为 `0x314D4652`。这两个说法等价，实现时按字节序列写更不容易错。

---

## 2. 帧布局

### 2.1 固定头（36 字节）

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|-----:|-----:|------|------|------|
| 0 | 4 | `magic` | bytes | 固定 `52 46 4D 31`（ASCII `RFM1`） |
| 4 | 2 | `version` | u16 | 当前为 `2` |
| 6 | 2 | `flags` | u16 | 见 §3 |
| 8 | 2 | `type` | u16 | 帧类型，见 §4 |
| 10 | 2 | `reserved` | u16 | **必须为 0**。将来启用需提升版本号 |
| 12 | 8 | `sessionId` | i64 | 会话标识，同一连接可并行多个会话 |
| 20 | 8 | `index` | i64 | 帧序号，语义随 `type` 变化（见 §2.3） |
| 28 | 4 | `frameLen` | u32 | **整帧**字节数，含这 36 字节头 |
| 32 | 4 | `headerCrc` | u32 | CRC32 of bytes `[0, 32)` |

`headerCrc` 覆盖偏移 `0..31`，即**不含 `headerCrc` 自身**的那部分头部。

### 2.2 载荷

紧跟固定头，依次是三个带长度前缀的路由字段，然后是 body：

```
offset                          size       field
.frameLen relative
+0                              1          fromLen   (u8)
+1                              fromLen    from      (UTF-8, 不保证 NUL 结尾)
+1+fromLen                      1          toLen     (u8)
+...+toLen                      toLen      to        (UTF-8)
+...                            1          fuuidLen  (u8)
+...+fuuidLen                   fuuidLen   fuuid     (UTF-8)
+...                            bodyLen    body      (原始字节)
+bodyLen                        [4]        bodyCrc   (u32，仅当 flags 置了 kFlagBodyCrc)
```

`bodyLen = frameLen - 36 - 3 - fromLen - toLen - fuuidLen - (带 bodyCrc ? 4 : 0)`

三个路由字段各自上限 **255 字节**（长度前缀是 u8）。超长**不截断**，打包直接失败——
截断会把两个不同的 id 合成一个，比失败危险得多。

### 2.3 `index` 的语义

| 帧类别 | `index` 含义 |
|--------|-------------|
| 分块数据 / 确认帧 | 块号（从 0 开始） |
| 请求 / 应答帧 | 请求号；应答与对应请求同号 |
| 认证帧 | 恒为 0 |
| 其余控制帧 | 恒为 0 |

接收方**不应**用 `index` 做去重——去重是传输引擎的职责，不是协议的。

---

## 3. 标志位

| 位 | 名称 | 含义 |
|---:|------|------|
| 0 | `kFlagFromReceiver` | 该帧由「接收文件数据的一侧」发出 |
| 1 | `kFlagEncrypted` | body 已加密，布局见 §6 |
| 2 | `kFlagBodyCrc` | body 尾部附带 u32 CRC32 |

### 3.1 `kFlagFromReceiver`

发送侧与接收侧各自维护一份会话槽。这个位决定去哪个槽找会话。

**不要**用 `flags != 0` 判断方向。v1 曾把方向、加密挤在同一个字段的不同位上，
加上加密位后"按非零判方向"就取错了槽——这是 v2 把它显式拆开的直接原因。

### 3.2 `kFlagBodyCrc` 由谁决定

**由打包方根据 body 长度自动决定，调用方不能自己置位**：

- `body.size() <= 65536`（`kBodyCrcLimit`）→ 置位并写 CRC
- 否则 → 清除该位，不写 CRC

原因：大块数据的完整性由传输层的分块 Merkle 校验保证（§8.3），每帧多加 4 字节
收益很小；而且"带不带 CRC"会影响 `bodyLen` 的计算，让它依赖调用方的意志会让
解包侧的验证逻辑变复杂。

解包侧必须**相信标志位而不相信自己的推测**：带就校验，不带就不校验。

### 3.3 `kFlagEncrypted` 与 body CRC 的组合

两者可以同时置位。此时 `bodyCrc` 计算的是**密文**的 CRC，不是明文的。

理由：解包方在拿到帧的那一刻还没有密钥协商结果（或不想为了校验而解密），
用密文 CRC 可以先做一层廉价筛查，再解密。反过来若用明文 CRC，解包方不解密就
无法验证，等于白占 4 字节。

---

## 4. 帧类型

类型号是线格式的一部分，**发布后不可更改**。分段是为了让接收方能便宜地判断
"这帧归哪个子系统"，不用维护一张大表。

| 号段 | 用途 | 状态 |
|------|------|------|
| `0 .. 99` | 握手与认证 | v2 新增 |
| `100 .. 199` | 控制与会话信令 | 沿用 v1 |
| `200 .. 499` | **保留** | 不得占用 |
| `500 .. 549` | 文件传输会话管理 | 沿用 v1 |
| `550 .. 599` | 服务端直连文件通道 | 沿用 v1 |
| `600 .. 65535` | 分块数据与确认 | 沿用 v1 |

### 4.1 认证段（0..99）

| 值 | 名称 | 方向 | 载荷 |
|---:|------|------|------|
| 0 | `kAuthChallenge` | S→C | `salt[16] \|\| nonceS[32]` |
| 1 | `kAuthResponse` | C→S | `nonceC[32] \|\| proof[32]` |
| 2 | `kAuthResult` | S→C | `status(u8) \|\| serverProof[32]` |
| 3 | `kAuthAbort` | 双向 | `status(u8)` |

`status` 取值见 §7.2。

### 4.2 控制段（100..199）

| 值 | 名称 | 方向 | `body` |
|---:|------|------|--------|
| 100 | `kAskId` | C→S | 空 |
| 101 | `kAnswerId` | S→C | 分配的 id（UTF-8） |
| 102 | `kAskHome` | C→S | 空 |
| 103 | `kAnswerHome` | S→C | 家目录路径 |
| 104 | `kAskFileList` | C→S | 目标目录路径 |
| 105 | `kAnswerFileList` | S→C | 条目列表（见 §9.1） |
| 106 | `kAskClientList` | C→S | 空 |
| 107 | `kAnswerClientList` | S→C | 在线客户端列表 |
| 108 | `kAskFileMeta` | C→S | 路径 |
| 109 | `kAnswerFileMeta` | S→C | 元信息（见 §9.2） |
| 110 | `kAskDelete` | C→S | 路径 |
| 111 | `kAnswerDelete` | S→C | `status(u8)` |
| 112 | `kAskSha256` | C→S | 路径 |
| 113 | `kAnswerSha256` | S→C | `status(u8) \|\| digest[32]` |
| 114 | `kAskRename` | C→S | `oldLen(u16) \|\| old \|\| new` |
| 115 | `kAnswerRename` | S→C | `status(u8)` |
| 116 | `kAskCreateDir` | C→S | 路径 |
| 117 | `kAnswerCreateDir` | S→C | `status(u8)` |
| 118 | `kAskArchive` | C→S | 源路径列表 |
| 119 | `kAnswerArchive` | S→C | `status(u8) \|\| 归档路径` |
| 120 | `kAskUnarchive` | C→S | `归档路径 \|\| 目标目录` |
| 121 | `kAnswerUnarchive` | S→C | `status(u8)` |
| 122 | `kAskHomeAndDriver` | C→S | 空 |
| 123 | `kAnswerHomeAndDriver` | S→C | 家目录 + 盘符列表 |
| 124 | `kAskHeart` | 双向 | 空 |
| 125 | `kAnswerHeart` | 双向 | 空 |
| 126 | `kNotifyClientList` | S→C | 变化后的列表 |
| 127 | `kAnswerNotifyClientList` | C→S | `status(u8)` |
| 128 | `kAskFileSamples` | C→S | 路径 |
| 129 | `kAnswerFileSamples` | S→C | 头部字节样本 |
| 130 | `kAskArchivePack` | C→S | 打包请求 |
| 131 | `kAnswerArchivePack` | S→C | `status(u8)` |
| 132 | `kAskArchiveReady` | C→S | 查询就绪 |
| 133 | `kAnswerArchiveReady` | S→C | `status(u8) \|\| ready(u8)` |

### 4.3 文件会话段（500..549）

| 值 | 名称 | 说明 |
|---:|------|------|
| 500 | `kFileRequestSend` | 请求发送文件 |
| 501 | `kFileAnswerSend` | 应答 |
| 502 | `kFileRequestDown` | 请求下载 |
| 503 | `kFileAnswerDown` | 应答 |
| 504 | `kFileRequestComplete` | 传输完成 |
| 505 | `kFileAnswerComplete` | 完成确认 |
| 506 | `kFileRequestCancel` | 取消 |
| 507 | `kFileAnswerCancel` | 取消确认 |
| 508 | `kFileRequestStart` | 开始传数据 |
| 509 | `kFileAnswerStart` | 开始确认 |

### 4.4 直连段（550..599）

| 值 | 名称 | 说明 |
|---:|------|------|
| 550 | `kDirectRequestId` | P2P 直连时查询对端 id |
| 551 | `kDirectAnswerId` | 应答 |

### 4.5 分块段（600..）

| 值 | 名称 | `body` |
|---:|------|--------|
| 600 | `kChunkRequestAck` | 请求确认 |
| 601 | `kChunkAnswerAck` | 确认应答 |
| 602 | `kChunkRequestData` | 块数据 |
| 603 | `kChunkAnswerData` | 块数据应答 |

---

## 5. 解包算法

### 5.1 三态返回

解包函数返回三种结果之一：

| 结果 | 含义 | 调用方动作 |
|------|------|-----------|
| `kOk` | 解出一帧 | 消费 `consumed` 字节，处理帧 |
| `kNeedMoreData` | 输入不够 | 消费 `consumed` 字节，保留剩余，等更多数据 |
| `kProtocolError` | 流已不可恢复 | **断开连接** |

`consumed` 的语义在三种情况下都不同，必须严格按上表处理。

### 5.2 步骤

```
1. 找 magic
   从起始位置逐字节前进，直到找到 4 字节的 "RFM1"。
   - 没找到完整 magic：丢弃 avail-3 字节（末尾最多 3 字节可能是半个 magic），
     返回 kNeedMoreData
   - 找到了但不是第 0 字节：丢弃 magic 之前的垃圾，返回 kNeedMoreData
     （让调用方从 magic 处重新进来，实现更简单）

2. 固定头是否收全
   avail < 36 → kNeedMoreData，consumed = 0

3. 头部 sanity + CRC

   frameLen 不在 [39, 32MB] 内  → 跳过 1 字节，回到步骤 1
   headerCrc 不匹配               → 跳过 1 字节，回到步骤 1
   version != 2                  → 返回 kProtocolError
   avail < frameLen              → kNeedMoreData，consumed = 0

4. 拆路由字段
   三次 (u8 长度 + 内容)。任一次字段越界 → 跳过 1 字节，回到步骤 1

5. body 与 CRC
   带 kFlagBodyCrc 时校验；不匹配 → 丢掉整帧，返回 kProtocolError

6. 类型合法性
   type 落在未定义区间 → 丢掉整帧，返回 kProtocolError

7. 填帧，consumed = frameLen，返回 kOk
```

### 5.3 为什么"头 CRC 不对"和"版本不对"处理方式不同

- **头 CRC 不对**：最可能是 body 里恰好出现了 `RFM1` 四个字节（压缩数据、
  随机二进制里很常见）。这是**误判**，流本身没问题，所以跳 1 字节继续找。

- **版本不对**：头部 CRC 都对上了，说明对端确实是按协议打的包，只是版本号不同。
  这时候继续扫下去只会把整条流都当成垃圾，永远不会成功。所以直接报协议错误、
  让调用方断开并提示用户升级。

这个区分是 v1 的既有设计，v2 保留。

### 5.4 重同步的代价

最坏情况下（每 4 字节就出现一次假 magic 且 CRC 都不对），每前进 1 字节要做一次
32 字节 CRC 计算。32MB 的流最坏是 32768 次扫描，每次 32 字节 CRC —— 约 1 毫秒量级。
可以接受，不需要更复杂的同步策略。

### 5.5 空 body 也带 CRC

`bodyLen == 0` 时 `0 <= kBodyCrcLimit` 成立，所以空 body 的帧也会带 4 字节 CRC。
因此线上**实际最短帧是 43 字节**（36 + 3 + 4），而不是理论的 39 字节。

`kMinFrameSize = 39` 只是"不带 CRC 时的理论下限"，用于步骤 3 的范围检查。

---

## 6. 加密载荷布局

当 `kFlagEncrypted` 置位时，`body` 的布局是：

```
nonce[12] || ciphertext[N] || tag[16]
```

- 算法：AES-256-GCM
- `nonce`：12 字节，**每次加密必须唯一**（同一密钥下复用 nonce 会直接泄露明文异或值）
- `tag`：16 字节完整长度，不支持截断
- AAD：见 §6.1

### 6.1 AAD 用什么

AAD 参与认证但不加密，因此可以把"必须被保护、但不需要保密"的元数据放进去，
防止攻击者篡改它们而不被发现。本协议约定 AAD 为：

```
AAD = magic[4] || version[2] || flags[2] || type[2] || sessionId[8] || index[8]
```

也就是固定头的前 28 字节（不含 `frameLen` 与 `headerCrc`）。

这样攻击者即使重算 `headerCrc` 把 `type` 从 `kAskDelete` 改成 `kAskFileList`，
解密时 tag 也校验不过。

**`frameLen` 故意不在 AAD 里**：它是长度，改它会导致帧边界错乱，
在 TCP 层就会表现为解析失败，不需要 GCM 再保护一次。

---

## 7. 认证握手

### 7.1 时序

```
client                                          server
  │ ── TCP connect ──────────────────────────────▶│
  │◀── Challenge(salt[16], nonceS[32]) ───────────│
  │                                               │
  │   key   = PBKDF2-HMAC-SHA256(pw, salt, 20000, 32)
  │   proof = HMAC-SHA256(key, tag ‖ nonceS ‖ nonceC)
  │                                               │
  │── Response(nonceC[32], proof[32]) ───────────▶│
  │                                               │  校验 proof
  │                                               │  serverProof =
  │                                               │    HMAC(key, tag ‖ nonceC ‖ nonceS)
  │◀── Result(ok, serverProof[32]) ───────────────│
  │                                               │
  │  校验 serverProof                              │
  │── AskId ... ─────────────────────────────────▶│  此后才受理业务帧
```

`tag` 是一个固定的域分隔字符串，取值见 §7.3。

### 7.2 `status` 取值

| 值 | 名称 | 含义 |
|---:|------|------|
| 0 | `kOk` | 成功 |
| 1 | `kBadProof` | proof 不匹配（口令错） |
| 2 | `kReplay` | nonce 重复（重放攻击） |
| 3 | `kUnsupported` | 版本或算法不支持 |
| 4 | `kInternal` | 服务端内部错误 |

认证失败时**不要**区分"口令错"和"用户不存在"——那会给攻击者提供用户枚举能力。
本项目没有用户概念（只有一条共享口令），所以这条约束天然满足，但扩展时要注意。

### 7.3 域分隔 `tag`

```
tag = "relayer/auth/v2"        // 15 字节（ASCII，无 NUL 结尾）
```

作用是把这次 HMAC 与"任何其他用到同一密钥的 HMAC"区分开。没有它的话，
如果将来某个环节也用 `key` 做 MAC，攻击者可能把两个场景的输出互换。

### 7.4 为什么两个 proof 的 nonce 顺序相反

```
clientProof = HMAC(key, tag ‖ nonceS ‖ nonceC)
serverProof = HMAC(key, tag ‖ nonceC ‖ nonceS)
```

顺序相反保证**两个 proof 一定不相等**，否则一个中间人可以截获客户端发的
proof 然后原样回给客户端，冒充服务端。

### 7.5 nonce 与重放

- `nonceS` 由服务端每次连接生成（建议 CSPRNG），32 字节
- `nonceC` 由客户端每次连接生成，32 字节
- 双方都应当**记住最近收到的 nonce**，拒绝重复值

只靠时间窗口不够：攻击者可以在窗口内重放。必须维护一个已见 nonce 集合，
或者用单调计数器（本项目用随机 nonce + 集合，因为连接数不大）。

### 7.6 `authEnabled = false` 的降级

服务端可以配置成不认证。此时：

- 不发 `kAuthChallenge`，连接建立后直接受理业务帧
- 客户端如果配置了期待认证却收到业务帧，应当报错而不是静默降级

**降级只允许由服务端配置显式开启**，不允许"因为握手失败所以跳过"——
那等于把认证变成可选项。

---

## 8. 传输状态机

### 8.1 状态

```
Idle → Handshaking → Transferring → Finalizing → Done
                   ↘ Interrupted → (Resume) → Transferring
```

| 状态 | 含义 |
|------|------|
| `Idle` | 连接已建立，未开始任何会话 |
| `Handshaking` | 认证握手中 |
| `Transferring` | 正在传数据块 |
| `Finalizing` | 数据发完，等最终校验与落盘确认 |
| `Interrupted` | 中断（网络、用户取消、对端消失），可续传 |
| `Done` | 成功完成 |

### 8.2 事件与转移

| 从 | 事件 | 到 |
|----|------|----|
| `Idle` | `StartHandshake` | `Handshaking` |
| `Handshaking` | `AuthOk` | `Idle`（已认证，可开业务会话） |
| `Handshaking` | `AuthFail` | `Done`（作废） |
| `Idle` | `StartTransfer` | `Transferring` |
| `Transferring` | `AllChunksAcked` | `Finalizing` |
| `Transferring` | `Interrupt` | `Interrupted` |
| `Finalizing` | `VerifyOk` | `Done` |
| `Finalizing` | `VerifyFail` | `Interrupted` |
| `Interrupted` | `Resume` | `Transferring` |
| `Interrupted` | `Abort` | `Done` |

**其余所有 (状态, 事件) 组合都非法**，必须显式失败而不是静默忽略。

这条规则需要有测试遍历全部组合来保证，不能靠"实现的时候记得"。

实现与校验：

- 代码：`core/include/relayer/core/transfer/StateMachine.h` 的 `kTransitions[]`
- 单元测试遍历 6×10=60 个组合（`tests/core/StateMachineTest.cpp`）
- `tools/check_transition_doc.py` 在本节与代码之间做双向比对，接在 CTest 里

改本节表格或改代码表，任一侧漏改都会让 CI 变红。

### 8.3 分块与增量校验

传输大文件时按块（默认 4MB）发送，每块算一个 SHA-256。

- 块内偏移写在 `index` 里（块号）
- 接收方维护块级位图，已收到的块可以乱序落盘
- 全部收完后按块哈希合并成一棵 Merkle 树，树根与发送方事先告知的整文件哈希比对

**先落盘后 ACK**：接收方只有在数据确实写进文件之后才发确认。宁可慢一点，
也不要出现"确认了但重启后文件是坏的"。

中断续传时，接收方把位图发给发送方（SACK），发送方只重发缺失的块。

### 8.4 `.relaypart` 临时文件

接收中的文件写成 `<目标名>.relaypart`，完成后原子重命名。这样：

- 中断后能识别出"这是个未完成的下载"
- 不会出现半个文件被用户误当成完整文件

### 8.5 心跳与空闲超时

用 §4.2 定义的 `kAskHeart`(124) / `kAnswerHeart`(125)，两者 `body` 都为空。

**为什么不用 TCP keepalive**：keepalive 默认 2 小时才报死，而且只能检测
"连接被中间设备丢弃"，检测不到"对端进程卡死但 socket 还开着"。后者在文件
传输里代价很高——对端卡住十秒可能意味着一个 4MB 块白传。

#### 8.5.1 参数

| 参数 | 值 | 含义 |
|------|-----|------|
| `kIdleBeforePing` | 20 s | 连续这么久没收到任何字节，就发一个 `kAskHeart` |
| `kPingTimeout` | 10 s | 发了 `kAskHeart` 之后这么久没有应答，判定对端失联 |
| `kTickInterval` | 1 s | 检查上述两个时间戳的轮询间隔 |

最坏发现时间 = 20 + 10 = 30 秒。

这张表与 `core/include/relayer/core/transport/Heartbeat.h` 里的 `constexpr`
常量一一对应，`tests/core/HeartbeatTest.cpp` 把它们钉死防止被"顺手调一下"。

#### 8.5.2 规则

1. **任何入站字节都算生命迹象**，包括业务帧、`kAnswerHeart`、甚至重同步前
   的垃圾字节。心跳要回答的问题是"对端还有没有反应"，不是"对端是否遵循
   心跳协议"。一个只在发数据的对端显然活着，此时判它超时是错的。
2. **出站动作不算生命迹象**。发 `kAskHeart` 不重置空闲计时器——否则只要
   我们在发包，就永远不会超时，哪怕对端早已断开。
3. **一个未应答的 `kAskHeart` 在路上时，不重复发新的**。重复发送不带来新
   信息，只会让对端收到一串无用帧。
4. **未应答超过 `kPingTimeout` 判失联**，与是否继续有出站流量无关。
5. **收到 `kAskHeart` 必须立即回 `kAnswerHeart`**，该应答属于规则 1 意义上
   的入站字节，同时触发对方的"未应答"标记清除。
6. 超时判定用 `>=` 而非 `>`：恰好落在边界上必须触发，否则会出现"周期
   恰好等于阈值时永远不超时"。

#### 8.5.3 判定顺序

检查动作时**必须先判超时，再判是否该发新心跳**：

```
if 有未应答的 ping:
    if now - 发出时刻 >= kPingTimeout:  → 超时，断开
    else:                               → 什么都不做
elif now - 最后收字节时刻 >= kIdleBeforePing: → 发 kAskHeart
else:                                          → 什么都不做
```

顺序写反会导致已超时的连接继续发新心跳（"空闲超过 20 秒"这个条件在死连接
上恒成立），问题被掩盖成"一直在重试"，永远不断开。

#### 8.5.4 与传输状态机的关系

心跳**不改变** §8.1 的状态。它是连接层的活性检测，`Transferring` 期间同样
生效（虽然那时业务流量通常已让心跳处于静默）。判定超时后的断开动作，由上层
转为 `Interrupt` 事件进入 `Interrupted`，走 §8.3 的续传路径。

实现在 `core/include/relayer/core/transport/Heartbeat.h`，是**纯逻辑**——输入
时间点，输出动作，不做 I/O。这样测试可以把十分钟的行为在微秒内跑完，避免真
`sleep` 造成的 CI 偶发失败。

---

## 9. 载荷编码约定

以下约定适用于所有 `body` 内的结构（认证段与分块段的裸字节除外）。

### 9.1 通用原则

- 整数小端，宽度显式（`u8` / `u16` / `u32` / `u64`）
- 字符串：`u16 长度 || UTF-8 字节`，**不带 NUL 结尾**
  （路由字段例外，它们用 `u8` 长度前缀，因为上限只有 255）
- 列表：`u32 元素个数 || 元素...`
- 没有"可选字段"：需要可空时用单独的长度 0 表示

### 9.2 文件条目列表（`kAnswerFileList`）

```
u32 count
重复 count 次：
    u8  nameLen,  name[nameLen]        // 仅文件名，不含目录
    u8  type                            // 0=文件 1=目录 2=符号链接
    u64 size
    i64 mtime                           // Unix 秒
    u16 mode                            // POSIX 权限位
```

### 9.3 文件元信息（`kAnswerFileMeta`）

```
u8  status
若 status == 0：
    u8  nameLen,  name[nameLen]
    u64 size
    i64 mtime
    u16 mode
    u8  sha256Len, sha256[sha256Len]     // 十六进制字符串，通常是 64
```

---

## 10. 错误处理

### 10.1 协议层错误

| 情况 | 处理 |
|------|------|
| 头部 CRC 不匹配 | 当作误判，跳 1 字节重同步 |
| `frameLen` 越界 | 同上 |
| 路由字段越界 | 同上 |
| 版本不符 | `kProtocolError`，断开 |
| body CRC 不匹配 | `kProtocolError`，断开 |
| 类型号未定义 | `kProtocolError`，断开 |

### 10.2 业务层错误

业务失败**不走**协议错误，走应答帧里的 `status(u8)`。理由：

- 协议错误意味着"这条连接不可信了"，要断开
- 业务错误（文件不存在、权限不足）是正常结果，连接应当继续可用

对应到 v2 的 `ErrorCode`：

| `status` | `ErrorCode` |
|---------:|-------------|
| 0 | `kOk` |
| 1 | `kNotFound` |
| 2 | `kPermissionDenied` |
| 3 | `kAlreadyExists` |
| 4 | `kInvalidPath` |
| 5 | `kNoSpace` |
| 6 | `kIoError` |
| 7 | `kUnsupported` |

### 10.3 身份绑定

**服务端必须强制覆写 `frame->from`**，用连接建立时认证过的身份。

客户端在 `from` 里写的任何内容都不可信——否则客户端 A 可以伪造
`from = "B"` 删掉 B 的文件。这条约束在服务端实现，不在协议格式里表达，
因为格式无法表达"必须覆写"这种要求。

落笔只有一个入口：`auth::bindSenderIdentity()`。**不要**手写 `f.from = id`，
那个函数存在的全部意义就是让这条规则世间只有一处实现。

---

## 11. 会话与转发规则

本节规定一条连接上"哪些帧能过、过了要做什么骨架手术"。实现见
`core/include/relayer/core/session/SessionPolicy.h`，是纯逻辑（不做 I/O）。

### 11.1 相位

每条连接只有一个会话相位，由 §7 的握手决定：

| 相位 | 允许入站的帧 |
|------|-------------|
| 未认证 | 认证帧（0..3）+ 心跳（124 / 125） |
| 已认证 | 全部已定义类型 |

**未认证时收到业务帧必须断开，不能静默丢弃**。理由：能发业务帧说明对端
自认为握手已结束，而我们这边没结束——双方对"走到哪一步"的认知不一致。
静默丢弃会让对端一直重试，这个差异不会自己消失；断开是唯一能让用户立刻
看到问题的方式。

### 11.2 心跳是例外

心跳在认证完成前后**都应被应答**，因为它检测的是"这条 TCP 连接还活着"，
与身份无关。

这个例外的价值在于把两种故障区分开：

| 现象 | 结论 |
|------|------|
| 心跳超时（§8.5） | TCP 断了，或对端进程死了 |
| 心跳正常但认证不完成 | TCP 通，问题在握手（口令、版本、实现） |

如果未认证时不答心跳，第二种会表现成第一种，排查方向直接跑偏。

### 11.3 转发

服务端收到一帧后按 `to` 决定去向：

| `to` | 去向 |
|------|------|
| 空 | 本地受理（发给服务端自己） |
| 等于服务端 id | 本地受理 |
| 等于发送者自己的 id | 拒绝 |
| 其他 | 转发给 `to` |

**`to` 为空算本地受理**，不是"无法投递"：客户端在拿到自己的 id 之前
根本不知道服务端叫什么（`kAskId` 就是用来问这个的），要求它先填 `to`
会形成鸡生蛋问题。

**"等于发送者自己的 id" 里的发送者身份必须来自认证阶段，不能读 `from`。**
`from` 是对端自称的，它可以随便写别人的名字——若拿它做这个判断，攻击者
只要把 `from` 写成自己的名字，就能让本该转发的帧被本地受理。

### 11.4 出站侧的身份断言

服务端会发出三类帧，`from` 的合法取值各不相同：

| 帧的来源 | 合法 `from` |
|----------|-------------|
| 服务端主动发起（应答、通知） | 空，或服务端自己的 id |
| 本地受理后由业务层回复 | 空，或服务端自己的 id |
| **转发他人的帧** | 恰好是该帧**发送者**在认证时得到的 id |

前两类的断言是"不许出现第三方身份"：

> 服务端发出的帧，`from` 要么为空（不代表任何客户端），要么等于服务端
> 自己的 id。

原因是：转发时若漏了覆写，帧会带着发送方的自称身份原样出去，而入站侧
的检查**拦不住**这种情况（入站时 `from = "alice"` 完全合法）。这种失败
模式在功能测试里看起来完全正常——只是身份是错的——所以必须有一个显式的
拒绝点。

第三类必须单独说明，否则上面那条断言会把**正确的转发**也一起拒掉——
转发后的 `from` 恰恰是别人的 id。转发路径的合法集合是"发送者在认证时
得到的 id"，而不是"空或服务端 id"。

这两个集合互不相交（转发帧的 `from` 一定不是服务端 id），所以判定时
**必须显式区分"这是转发帧"还是"这是我自己的帧"**，不能只看 `from` 的值。
实现上由 `planRoute()/forward()` 与 `send()` 两个入口分开，调用方不需要
自己判断属于哪一类。

客户端的出站不做此检查：它只能代表自己，越权的那一步在服务端入站侧覆盖。

---

## 12. 与 v1 的差异

线格式（§2 的字节布局）**完全不变**。差异只在语义层：

| 项 | v1 | v2 |
|----|----|----|
| 帧类型枚举名 | `kMsgType_Ask_ID` 等 | `kAskId` 等（风格统一） |
| 类型号 | 0 / 500 / 550 / 600 分段 | 保留，新增认证段 0..99 |
| 认证 | 无 | Challenge/Response/Result 三次往返 |
| 方向判断 | `mark != 0` | `hasFlag(kFlagFromReceiver)` |
| 解包接口 | `UnPack(miniBuffer&, FramePtr&)` | `unpackFrame(data, len, frame, consumed)` |
| 帧所有权 | `shared_ptr<OneFrame>` | 值类型 `Frame` |
| `reserved` | 未使用 | 明确"必须为 0" |
| AAD | 无 | 固定头前 28 字节 |

**老客户端无法连 v2 服务端**：认证段是新加的，v2 服务端会先发 `kAuthChallenge`，
v1 客户端不认识类型 0，会按 v1 的逻辑丢弃或误解。这是预期行为——
版本号同为 2 是个历史包袱，实际上需要通过发布说明告知用户必须同时升级。

如果将来需要真正区分，应当提升 `version` 到 3 并保留一条"拒绝并提示升级"的路径。
