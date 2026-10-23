# relayer 重写方案

> 本文是整个重写工程的纲领性文档。所有实现决策以此为准；如需偏离，先在本文
> 「决策记录」里补一条，再动代码。
>
> 来源项目：`D:\Code\relayFile`（下称 v1），参考基线 `v0.5` + `4f85be5`。
> 目标项目：`D:\Code\relayer`（下称 v2）。

---

## 1. 目标与非目标

### 1.1 目标

让 relayer 达到**可以公开的开源项目质量**：陌生人 clone 下来能在 5 分钟内跑起来，
愿意读代码，愿意提 PR，敢在别的项目里复用其中的模块。

具体到可验收的六条：

1. **构建简单**：从干净 clone 到出可执行文件，三步命令，零手工传参。
2. **依赖可控**：所有三方库 vendored 或标准 `find_package`，注明版本与许可证。
3. **结构清晰**：每个模块职责单一，依赖方向单向，能用一句话说清它做什么。
4. **测试覆盖**：协议、状态机、文件、路径、认证都有自动化测试，CI 上跑。
5. **风格一致**：命名、错误处理、日志、注释不依赖个人习惯，有工具约束。
6. **可替换**：文件系统、传输、认证都有接口，能换实现、能 mock。

### 1.2 非目标

- **不追求新功能数量**。v2 新增的认证 / 多任务并行 / P2P 是为了补上 v1 的结构性缺陷，
  不是为了堆特性。
- **不改 UI 布局**。9 个 `.ui` 文件原样搬迁，排版零改动。
- **不保留 v1 源码兼容**。协议可以演进，不背历史包袱。
- **不做跨平台之外的平台适配**。Windows 与 Linux 为主，macOS 尽力而为。

---

## 2. 总体架构

### 2.1 分层

```
┌─────────────────────────────────────────────────────────────┐
│  app/         Qt 前端（唯一允许 #include <Q...> 的地方）      │
│  ├─ ui/       9 个 .ui + 控件类（布局复用，逻辑重写）          │
│  └─ bridge/   Core → Qt 适配层                               │
└───────────────────────────┬─────────────────────────────────┘
                            │ 只依赖稳定接口
┌───────────────────────────▼─────────────────────────────────┐
│  core/        通信库（零 Qt，可独立复用）                     │
│  ├─ protocol  帧编解码、消息、序列化                          │
│  ├─ crypto    AES-256-GCM、SHA-256、PBKDF2                   │
│  ├─ auth      挑战-响应握手                                   │
│  ├─ transport asio 连接、会话、监听                           │
│  ├─ transfer  传输引擎（窗口/ACK/续传/Merkle 校验）           │
│  └─ discovery 客户端目录、信令、P2P 地址交换                  │
└───────────┬─────────────────────────────┬───────────────────┘
            │ 依赖接口，不依赖实现          │
┌───────────▼─────────────────┐  ┌────────▼──────────────────┐
│  filestore/   文件子系统     │  │  third_party/             │
│  ├─ IFileStore  抽象接口     │  │  asio / cereal / fmt      │
│  ├─ StdFileStore 标准库实现  │  │  spdlog / nlohmann        │
│  ├─ QtFileStore  可选实现    │  │  miniz / liblzma / catch2 │
│  └─ archive      zip / tar.xz│  └───────────────────────────┘
└─────────────────────────────┘
```

### 2.2 依赖方向（硬约束）

```
app  ──▶ core ──▶ filestore(接口)
 │        │            ▲
 │        └────────────┘  （core 只 include IFileStore.h）
 └──────▶ filestore(具体实现，由 app 组装注入)
```

三条禁止规则，用脚本在 CI 上卡死：

- `core/**` 不得 `#include <Q` 或 `#include <Qt`
- `core/**` 不得 `#include` `filestore/` 下除 `IFileStore.h` 以外的头
- `filestore/**` 不得 `#include` `core/` 下任何头

### 2.3 产物

| 产物 | 说明 | 依赖 |
| --- | --- | --- |
| `relayer` | GUI 客户端 | core + filestore + Qt |
| `relayerd` | 服务端 | core（**不需要 filestore、不需要 Qt**） |
| `relayer-cli` | 命令行客户端 | core + filestore |
| `relayer-tests` | 单元/集成测试 | 全部 |

关键点：**服务端不链接 Qt，也不链接文件系统**。它只做转发、认证、目录。

---

## 3. 模块设计

### 3.1 filestore：文件子系统

#### 3.1.1 为什么先做

它风险最低、收益最快，且能解决 v1 最大的测试缺口：v1 里「写盘失败要中断传输」
这条逻辑根本无法单测。有了接口，可以注入 `MemFileStore` 模拟短写、磁盘满、文件被占用。

#### 3.1.2 `IFileStore` 接口

> **实现状态（2026-10-22）**：接口已落地，与本节的草案有三处偏离，
> 以实际代码 `filestore/include/relayer/filestore/IFileStore.h` 为准：
>
> 1. `Status` 增加了 `ErrorCode` 枚举（草案只有 `bool ok`）。原因是只有布尔
>    的话，调用方无法区分"文件被占用"和"磁盘满"，而这两者的处理方式完全不同
>    （前者重试、后者报错）。
> 2. `WriteMode` 改成 `bool truncate`。实际只需要两个状态，
>    额外造一个枚举没有收益。
> 3. 路径方法从 5 个（`normalize/join/parent/fileName/isAbsolute`）收成 3 个
>    （`normalize/join/resolveWithin`）。纯字符串逻辑抽到独立的 `PathPolicy`，
>    接口只保留需要"实现可能有差异"的部分，并新增 `resolveWithin`
>    作为归档路径穿越的唯一防线。

```cpp
namespace filestore
{

struct FileStat
{
    bool          exists{};
    bool          isDir{};
    bool          isSymlink{};
    std::uint64_t size{};       // 目录为 0
    std::int64_t  mtime{};      // Unix epoch 秒
    bool          readable{};
    bool          writable{};
    bool          executable{};
};

enum class ErrorCode
{
    kOk = 0, kNotFound, kPermissionDenied, kAlreadyExists,
    kNotADirectory, kIsADirectory, kNoSpace, kBusy,
    kTooManyHandles, kInvalidPath, kIoError, kUnsupported, kUnknown,
};

struct Status
{
    ErrorCode   code = ErrorCode::kOk;
    std::string message;

    [[nodiscard]] bool ok() const noexcept;
    explicit operator bool() const noexcept;
    static Status Ok();
    static Status Fail(ErrorCode c, std::string msg);
};

class IFileStore
{
public:
    virtual ~IFileStore() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // ---- 路径（纯逻辑，实现退化为委托 PathPolicy）----
    [[nodiscard]] virtual std::string normalize(std::string_view path) const = 0;
    [[nodiscard]] virtual std::string join(std::string_view a, std::string_view b) const = 0;
    // 归档条目落点校验：返回空串表示必须拒绝
    [[nodiscard]] virtual std::string resolveWithin(std::string_view root, std::string_view child) const = 0;

    // ---- 元信息 ----
    [[nodiscard]] virtual FileStat stat(std::string_view path) const = 0;
    [[nodiscard]] virtual Status probeWritable(std::string_view path) const = 0;

    // ---- 读写 ----
    virtual std::unique_ptr<IReadStream>  openRead(std::string_view path, Status& st) const = 0;
    virtual std::unique_ptr<IWriteStream> openWrite(std::string_view path, bool truncate, Status& st) const = 0;

    // ---- 目录 ----
    virtual Status list(std::string_view dir, bool recursive, std::vector<DirEntry>& out) const = 0;
    virtual Status mkdirs(std::string_view dir) const = 0;

    // ---- 变更 ----
    virtual Status remove(std::string_view path) const = 0;
    virtual Status resize(std::string_view path, std::uint64_t size) const = 0;
    virtual Status atomicReplace(std::string_view from, std::string_view to) const = 0;

    [[nodiscard]] virtual std::string quickFingerprint(std::string_view path,
                                                      std::int64_t sizeOverride = -1) const = 0;
};

}   // namespace filestore
```

设计要点：

- **路径一律 UTF-8 的 `std::string`**。`StdFileStore` 在 Windows 内部转 `std::wstring`
  （`MultiByteToWideChar`），**不用 `u8path`**（C++20 起返回类型变更，跨标准版本会炸）。
- **不用异常穿越接口**：文件操作失败是常态（被占用、无权限、磁盘满），全部走 `Status`。
- **`atomicReplace` 语义写死在注释里**：目标存在必须覆盖。
- **`probeWritable` 单独成方法**：把平台差异关进实现，引擎只读结果。
- **路径规则集中在 `PathPolicy`**：`PathStyle::kPosix` / `kWindows` 可显式指定，
  因此同一套路径用例在两种平台都能跑，不依赖宿主。`resolveWithin` 用**段比较**
  而非字符串前缀，天然带分隔符边界——v1 的 zip-slip 就出在前缀比较没有边界上。

#### 3.1.3 实现

| 实现 | 用途 | 状态 |
| --- | --- | --- |
| `StdFileStore` | 默认，`std::filesystem` + C stdio 流 | 已实现，已测 |
| `MemFileStore` | 测试，内存文件树 + 故障注入 | 已实现，已测 |
| `QtFileStore` | 可选，仅当 `std::filesystem` 出坑时切换 | 暂不实现 |

`MemFileStore` 的能力（阶段 B 实际落地）：

| 能力 | 接口 | 用途 |
| --- | --- | --- |
| 容量上限 | `setCapacity(n)` | 模拟磁盘满，写超限返回 `kNoSpace` |
| 路径前缀故障 | `FaultInjection::pathPrefix` | 模拟"任务 A 写入失败而任务 B 正常" |
| 写到一半失败 | `FaultInjection::afterOps` | 前 N 次写成功、之后失败 |
| 一次性故障 | `FaultInjection::once` | 模拟"文件被占用一会儿后释放" |
| 崩溃丢缓冲 | `setFlushOnClose(false)` | 写流未 flush 就被销毁 |

**指纹一致性由测试强制**：`MemFileStore::quickFingerprint` 必须与
`StdFileStore` 完全一致，否则所有基于内存实现写的续传测试都在验证假的东西。
对应用例 `两个实现的指纹算法一致`。

#### 3.1.4 归档

**状态：tar.xz 已实现并测；zip 未实现。**

层级（全部在 `filestore/` 内，不单独立 `archive/` 目录——它和文件子系统
共用 `IFileStore`，分成两个库反而要多一层依赖）：

| 文件 | 职责 |
| --- | --- |
| `detail/XzCodec.{h,cpp}` | 包装 liblzma 的流式 LZMA2 编解码 |
| `detail/TarCodec.{h,cpp}` | ustar 头读写 + PAX/GNU 长名字 |
| `IArchive.h` | 归档接口 + `sanitizeEntryName` |
| `TarXzArchiver.{h,cpp}` | tar + xz，接 `IFileStore` |

**路径校验**（v1 的 zip-slip 防线在这里重写）：

- 打包侧：`sanitizeEntryName` 逐段检查。含 `..` / 带盘符（`C:` 和 `C:x`
  两种形式）/ 清洗后为空 → 拒绝，记进 `skipped`。**前导斜杠不拒绝**，
  而是收成相对路径——`IArchive.h` 第 3 条的既定语义。
- 解包侧：每个条目走 `store.resolveWithin(destRoot, name)`，返回空串则跳过
  （记 `skipped`），不整体失败。防线用**段比较**而非字符串前缀，所以
  `/data/foo2` 不会被判成在 `/data/foo` 之内。
- 符号链接条目一律跳过：Windows 上建链接要额外权限，而且链接目标本身
  也是逃逸点。

**三处刻意的取舍**：

1. **不支持 zip 的随机访问**，`list` 会完整解压一遍。传输场景里列目录是
   低频操作，而 zip 的中央目录一旦损坏整包报废；tar 能救回前面的条目。
2. **PAX 和 GNU `'L'` 两种长名字都认**。我们写包用 PAX，但 GNU tar 默认
   写 `'L'`——只认一种的话，用户拿系统 tar 打的包在我们这里长路径会被
   静默截断到 100 字节，两个深路径塌成同一个名字互相覆盖。
3. **流式落盘，不写临时 tar**。先打 tar 再压要把整包数据走两遍盘；
   而且这种实现在内存文件系统里测试时反而"能用"，到真机上才发现慢。

**与系统 tar 的双向互操作**已手工验证（GNU tar 1.35）：我们打的包能被
`tar tvJf` / `tar xJf` 完整识别（含 PAX 长路径与中文路径），系统打的包
我们也能完整列出与解出。

**zip**：miniz 已在 `third_party/miniz`，但传输场景用不上随机访问，
优先级放在 tar.xz 之后。需要时按同一套 `IArchive` 实现即可，路径校验
复用 `sanitizeEntryName` 与 `resolveWithin`。

---

### 3.2 core/protocol：协议

#### 3.2.1 帧格式

沿用 v1 的 RFM1（已被三批改造与 14 项测试验证），**不改线格式**，只做规范化：

```
offset size  field
0      4     magic      'R','F','M','1'
4      2     version    2
6      2     flags
8      2     type
10     2     reserved
12     8     sessionId
20     8     index
28     4     frameLen
32     4     headerCrc
36     ...   payload
```

v2 新增帧类型（认证、信令），不改动既有帧头的任何字节。

#### 3.2.2 文档化

`docs/PROTOCOL.md` 要能脱离代码读懂：帧布局、字段语义、状态机、时序图、错误处理。
这是开源项目的基本要求——协议不能只存在于头文件注释里。

**状态：已完成**（11 章：设计边界 / 帧布局 / 标志位 / 帧类型全表 / 解包算法 /
加密载荷 + AAD / 认证握手 / 传输状态机 / 载荷编码约定 / 错误处理 / 与 v1 差异）。

文档与代码的一致性由 `tools/check_protocol_doc.py` 在 CTest 里强制（测试名
`protocol_doc`）：帧类型表逐项比对枚举值与数值，固定头大小、分块段起始号也一并校验。
当前 54 个帧类型与文档完全对应。

#### 3.2.3 帧编解码实现

**状态：已完成**（26 用例 / 443 断言）。

接口是显式的字节指针 + 长度，与本项目其余部分的风格一致：

```cpp
std::vector<char> packFrame(const Frame &f);            // 失败返回空
Status packFrameChecked(const Frame &f, std::vector<char> &out);   // 失败带原因
UnPackResult unpackFrame(const char *data, std::size_t len,
                        Frame &out, std::size_t &consumed);
```

三态 `UnPackResult { kNeedMoreData, kOk, kProtocolError }` 与 v1 一致，
但 `consumed` 的语义在三种情况下不同（分别表示"可丢弃的垃圾字节数"、
"整帧长度"、"出错的帧长度"），这一点在头文件与 `PROTOCOL.md` §5.1 都写死了。

测试覆盖：往返保真（含负数 id、UTF-8 路由字段、0 到 200KB 的 body 长度）、
头部偏移钉死、body CRC 自动置位的四个边界、打包拒绝超长字段与超长帧、
**任意前缀长度都不解出帧**、**逐字节喂入**、粘包、前导垃圾、半个 magic 保留、
body 里的假 magic、虚假 magic + 非法 frameLen 的跳 1 字节重同步、
头 CRC 篡改、frameLen 篡改、版本不符、body CRC 损坏、类型分段、
CRC32 标准向量、2 万字节确定性随机数据的 fuzz（不崩且不误判）。

#### 3.2.4 状态机

传输状态机用显式枚举 + 转移表，而不是散落在 `if/else` 里：

```
Idle → Handshaking → Transferring → Finalizing → Done
                   ↘ Interrupted → (Resume) → Transferring
```

转移表写成数据（`struct Transition { State from; Event e; State to; }`），
用测试遍历所有非法转移，确保都不会静默通过。

**状态：未开始**（属阶段 C 剩余部分）。

---

### 3.3 core/crypto + auth：认证

#### 3.3.0 归属调整：新增 `common/` 基础层

原计划把 `Sha256` 放在 `filestore/detail/`。实际做阶段 C 时撞到一个结构问题：
`core` 需要 SHA-256（认证要用），但架构规则 R3 禁止 `filestore` 依赖 `core`，
R2 又只允许 `core` 看到 `IFileStore.h`。放进任何一侧都会造成反向依赖或两份副本。

于是抽出 `common/`：

```
common/include/relayer/common/Status.h         Status + ErrorCode（relayer 命名空间）
common/include/relayer/common/crypto/Sha256.h  Sha256 / HmacSha256 / Pbkdf2HmacSha256
common/include/relayer/common/crypto/Aes256Gcm.h
```

依赖方向变成 `filestore → common`、`core → common`，两侧都向下依赖同一个定义，
错误与密钥在模块边界上不需要任何转换。

为不破坏既有代码，`filestore` 里保留了 `using Status = ::relayer::Status;` 别名，
所以 `filestore::Status`、`filestore::ErrorCode`、`filestore::toString` 全部照旧可用；
`ErrorCode` 的枚举值也重新编排成了带段落的编号（通用 0-99 / 文件系统 100+ / 协议 200+），
**既有枚举名与语义不变**。

`tools/check_arch.py` 增加 R6：`common/` 不得包含 filestore / core / app / server / cli 的头文件。

#### 3.3.1 原语

- `Aes256Gcm`（从 v1 迁移，已自包含）
- `Sha256`（同上）
- **新增** `Pbkdf2HmacSha256`、`HmacSha256`

**状态：已完成**（16 用例 / 1111 断言，含 SHA-256 既有用例）。

| 原语 | 位置 | 测试来源 |
|------|------|---------|
| `Sha256` | `common/crypto/Sha256.h` | FIPS 180-4 官方向量 |
| `hmacSha256` | 同上 | RFC 4231 全部 7 条向量 |
| `constantTimeEquals` | 同上 | 语义 + 空指针边界 |
| `pbkdf2HmacSha256` | 同上 | 迭代 1/2/4096/20000、空 salt、超块长口令 |
| `aes256GcmEncrypt/Decrypt` | `common/crypto/Aes256Gcm.h` | McGrew-Viega 附录 B 的 AES-256 用例 |

比 v1 多做的三件事：

1. **GCM 支持 AAD**。v1 的实现只做纯加密，AAD 长度写死 0。v2 的协议要把固定头
   前 28 字节作为 AAD（`PROTOCOL.md` §6.1），否则攻击者改完 `type` 重算 headerCrc
   就能把"删除文件"伪装成"列目录"而不被发现。
2. **tag 比较改成常量时间**。v1 用逐字节 `if (...) ok = false` 循环，虽然也没提前
   返回，但写法容易被后来的人"优化"成 memcmp。现在走 `constantTimeEquals`，
   并在函数里用 `volatile` 累加器阻止编译器改写成早退。
3. **解封失败时清空输出**。v1 的 `Aes256GcmDecrypt` 返回 false 时 `out` 里可能已有
   部分解密的明文，调用方一个疏忽就会用上被篡改的数据。现在 `aes256GcmOpen`
   在失败分支里显式 `plain.clear()`。

**交叉验证**：除了单元测试里的公开向量，另用 Python `cryptography` 库（独立实现）
对 71 组输入做了逐字节比对（GCM 各种长度 × 带/不带 AAD、PBKDF2 各种迭代次数与
含中文/超长口令），结果零差异。这个比对程序是临时的，验证完即删，不进仓库。

**已知取舍**：AES 用 T-table 实现，查表下标依赖密钥，存在缓存计时侧信道。
威胁模型是"防窃听、防篡改、防重放"，不含防同机旁路攻击。这一点写在
`Aes256Gcm.h` 的文件头注释里，不用翻文档也能看到。

#### 3.3.2 握手

```
client                                 server
  │ ── TCP connect ──────────────────────▶│
  │◀── Challenge(salt16, nonceS32) ───────│
  │── Response(nonceC32, proof32) ───────▶│  key   = PBKDF2(pw, salt, 20000)
  │                                       │  proof = HMAC(key, tag||nonceS||nonceC)
  │◀── Result(ok, serverProof32) ─────────│  serverProof = HMAC(key, tag||nonceC||nonceS)
  │── Ask_OwnID ...                       │  此后才受理业务帧
```

- 双向验证，防假服务器。
- 双方 nonce 混入 HMAC 输入，防重放。
- 未认证连接发来的业务帧一律丢弃并断开。
- 禁用认证时（`authEnabled=false`）行为等价 v1，仅限内网自用，README 里写明风险。

**状态：已完成**（`core/include/relayer/core/auth/Handshake.h`）。
协议细节（`tag` 取值、`status` 码表、两个 proof 的 nonce 顺序为何相反、nonce 重放防护策略）
原先在 `PROTOCOL.md` §7 写定，实现按文档来；实现过程中发现文档把 `tag` 长度误写为
24 字节（实际 15），已一并修正。

实现要点：

- `ClientHandshake` / `ServerHandshake` 是两个纯逻辑状态机，"吃一帧吐一帧"，不含 I/O。
  谁读 socket、超时怎么算、超时后谁断开都在 transport 层。
- 相位用显式枚举（`ClientPhase` / `ServerPhase`），非法顺序一律进 `kFailed` 终态，
  不存在"失败之后靠某些字节恢复"的路径。
- 降级只有一条路：服务端构造时 `ServerConfig::allowNoAuth = true`。**没有**任何
  "因为握手失败所以跳过"的分支；客户端要求认证时收到业务帧直接判失败。
- 服务端"要认证却口令为空"在启动阶段就失败，不静默变成开放服务器。
- 重放检查放在 PBKDF2 之前：让重放请求先过一道便宜的过滤，避免被刷爆 CPU。
- 服务端额外拒绝 `nonceC == nonceS`：两者相同时两个 proof 的输入就一样了，
  §7.4 的对称性被打破。
- `bindSenderIdentity()` 把"转发前覆写 from"变成一个有名字的函数，而不是依赖
  调用方记得写 `f.from = id`——这是本协议里少数几个"漏写就是安全漏洞"的点。

随机源：`core/src/auth/Random.cpp`，Windows 走 `BCryptGenRandom`，Linux 走
`getrandom(2)`（失败退 `/dev/urandom`），macOS 走 `arc4random_buf`。**不用**
`std::random_device`：标准把质量留给实现，某些 MinGW 版本它是 mt19937 加固定种子。

交叉验证：除单元测试外，用 Python `hashlib.pbkdf2_hmac` / `hmac` 独立算出 4 组
输入（含空口令、中文口令）的 `key` / `clientProof` / `serverProof`，与本实现逐字节
比对，12 项全部一致。这一步是为了排除"测试和实现共享同一个错误"。

测试：`tests/core/AuthTest.cpp`，39 用例 / 291 断言，覆盖正确口令双向通过、
错口令判 `kBadProof`、假服务器（伪造 proof 与抄回客户端 proof 两种）被识破、
nonce 重放被拒、两个 proof 恒不相等、乱序帧被拒、失败终态不可恢复、
`allowNoAuth` 降级、未认证业务帧被拒、认证帧过一遍 Pack/UnPack、CSPRNG 可用性。

#### 3.3.3 身份绑定

**服务器转发时强制覆写 `frame->from` 为认证身份**。这是 v1「from 可伪造」的根治手段。

**状态：已完成**（`bindSenderIdentity()`）。做成独立函数而不是靠调用方记得写
`f.from = id`：这条规则漏掉一处，就允许任何已认证客户端伪装成任意其他客户端
给对方发文件。真正在转发路径上调用它属于 transport 层（3.4），届时接入。

---

### 3.4 core/transport：连接

- asio 裸 socket（不用 asio 的 Qt 类比封装），每条连接一个 `strand`，保证回调串行化。
- 0 个全局 io_context 之外，不允许有隐式全局状态；`io_context` 由调用方持有并注入。
- 关闭流程显式：`shutdown(fd) → 等待在读回调返回 → 释放`，杜绝 v1 里的"对象析构时还在收帧"。
- 心跳与超时统一由 `transport` 层提供，不散落在业务层。

**状态：部分完成**（帧切分 + 连接 + 心跳 + 会话策略 + 会话组装；peer 表未做）。

#### 3.4.1 帧切分（`FrameReader`，已完成）

把"字节流 → 帧"从 socket 里剥出来，零 I/O、可纯字节测试（`tests/core/FrameReaderTest.cpp`，
18 用例 / 678 断言）。要处理的情况：半包、粘包、垃圾重同步、超长帧。

关键实现细节（写的时候踩到过，值得记）：

- **`next()` 必须内循环**。`unpackFrame` 丢掉前导垃圾时返回 `kNeedMoreData`
  且 `consumed` 非零——此时帧就在后面，只是还没被解析。若 `next()` 直接返回
  false，调用方会以为"还差字节"，而对端可能已经发完了最后一帧，那一帧就
  永远卡在缓冲里。循环终止条件是 `consumed == 0`。
- 缓冲用 `std::deque<char>`：粘包场景要从头部消费，`vector::erase(begin())`
  是 O(n) 搬移，会退化成二次复杂度。
- 每次解帧前拷到连续内存：`unpackFrame` 只接受 `(const char*, len)`，为它改
  成迭代器接口会让一个很简单的函数变复杂，不值得。
- 缓冲有硬上限 `kMaxBufferedBytes`（2× 最大帧）。恶意对端可以永远不发完整帧
  来撑爆内存，超限即判协议错误。

#### 3.4.2 连接（`Connection`，已完成）

`tests/core/ConnectionTest.cpp`，13 用例 / 152 断言，跑真实回环 socket。

- `io_context` 由调用方注入，本层无全局状态。
- 每连接一个 `asio::strand`，回调串行化——不用自己加锁，因为锁的粒度几乎必然错。
- **关闭流程**：`close()` 只 shutdown + close + 清队列，不等回调返回（在 strand
  上等待 strand 会死锁）。`onClosed` 通过 `asio::post` 投递，且先把
  `std::function` 拷出来再投——上层很可能在回调里丢掉最后一个
  `shared_ptr<Connection>`，若 lambda 还捕着 `this` 就是 use-after-free。
- **`onClosed` 只通知一次**：对端 RST 时读和写会各自报错，两条路径都会走到
  `doClose`，没有幂等保护就会通知两次，上层释放两次。
- **发送按帧排队**，一次 `async_write` 只发队首一帧。并发 `async_write` 会让
  字节流交错，对端看到畸形帧——这是 TCP 上最容易犯的错之一。
- 发送队列有上限，对端不读时断开而不是无限增长。
- `connect()` 返回 `shared_ptr`，且允许在 `kConnecting` 阶段 `send()` 入队：
  握手第一帧不该等"连上了"再发。

#### 3.4.3 依赖修正

`relayer_asio` 的 include 路径由 `third_party/asio` 改为 `third_party`，并标记
`SYSTEM`：

- vendored asio 的头文件用 `#include "asio/detail/config.hpp"` 互引，必须让
  `<asio/...>` 能解析，所以 include 根目录要比 asio 目录高一层。原配置下
  `<asio.hpp>` 和 `<asio/io_context.hpp>` 都编译不过。
- asio 是 header-only，实现会在我们的编译单元里展开；上游代码不满足本项目的
  `-Werror -Wsign-conversion -Wold-style-cast`，不加 `SYSTEM` 会有几十个来自
  三方代码的报错。`SYSTEM` 只抑制警告，不改语义。

#### 3.4.4 心跳与超时（已完成）

`core/include/relayer/core/transport/Heartbeat.h` + `tests/core/HeartbeatTest.cpp`
（14 用例 / 766 断言）。参数与规则见 `PROTOCOL.md` §8.5。

核心取舍：**把时间决策抽成纯函数**。`HeartbeatPolicy::check(now)` 输入时间点、
输出 `{kNone, kSendPing, kTimedOut}`，不碰 socket 也不碰定时器。测试因此可以
传任意虚构时间，把十分钟的连接行为在微秒内跑完——如果时间判断散落在定时器
回调里，测试只能真的 `sleep`，那是 CI 偶发失败的头号来源。

几个刻意的决定：

- **双向心跳而非只靠 TCP keepalive**：keepalive 默认 2 小时才报死，且检测
  不到"对端进程卡死但 socket 还开着"。
- **任何入站字节都算生命迹象**，业务帧也算。心跳要回答"对端还有没有反应"，
  不是"对端是否遵循心跳协议"——一个只在发数据的对端显然活着。
- **出站动作不重置空闲计时器**。发 ping 不能当作连接健康的证据，否则只要
  我们在发包就永远不超时。
- **检查顺序先判超时、再判是否发新 ping**。写反会让死连接一直发新心跳，
  把问题掩盖成"在重试"。
- 边界一律用 `>=`。用 `>` 会出现"周期恰好等于阈值时永远不触发"。

#### 3.4.5 会话策略（已完成，纯逻辑部分）

`core/include/relayer/core/session/SessionPolicy.h` + `tests/core/SessionPolicyTest.cpp`
（23 用例 / 776 断言）。规则见 `PROTOCOL.md` §11。

这一层回答四个问题，每个都是一个有名字的函数而不是塞进一个 if-else：
`checkInbound()` / `identityToBind()` / `checkOutbound()` / `planRoute()`。
理由和握手状态机一样——转发路径是"漏一个分支就是安全漏洞"的地方，混在
socket 回调里写就没法把每种组合都测到。

实现时踩到并修掉的两个真问题：

- **`planRoute` 的 `selfId` 一度误传成 `f.from`**。`from` 是对端**自称**的
  身份，拿它判断"是不是发给自己"，攻击者只要把 `from` 写成自己的名字，
  就能让本该转发的帧被本地受理。现在 `selfId` 只能来自认证阶段分配的身份，
  并有一条专门的测试钉住（`route self-check uses authenticated id, not
  claimed from`）。
- **"该不该绑"和"绑成谁"必须分开**。合成一个函数之后就再也测不了"未认证
  时有没有偷偷绑"这类问题。现在 `identityToBind()` 只做判定，
  `applyIdentityBinding()` 负责落笔，两者各有测试。

另外补了 `Connection::onEstablished`。这不是可选项：客户端不能靠"send 第一帧"
来启动会话——建连前 `send()` 只是入队，而握手的第一帧由**服务端**发起
（§7.1），客户端手上根本没有帧可发。没有这条回调，客户端就只能在"我发了
一帧"这个假事件上启动状态机，而那个假事件在网络失败时永远不会到来。

#### 3.4.6 会话组装（`Session`，已完成）

`core/include/relayer/core/session/Session.h` + `tests/core/SessionTest.cpp`
（39 用例 / 858 断言）。

本层**不写新规则**：所有"该不该"的判断都已在前三层做完，这里只按顺序调用
它们并把结论翻译成动作。这条边界是刻意的——组装层最容易变成"顺手加个 if"
的地方，而一旦规则有了两处实现，就再也说不清哪处是权威。

几个实现时才发现、必须做对的地方：

- **回调必须用 `weak_ptr` 绑 Session**。Session 持有 `conn_`，`conn_` 的
  回调若强引用 Session 就形成环，两者的引用计数永远不归零。反向用
  `weak_ptr` 还有个好处：回调里 lock 失败说明 Session 已销毁，直接返回
  就是正确行为，不需要额外的"是否已析构"标志。
- **握手超时与心跳超时必须是两个值**。卡在握手半路时对端可能活得好好的
  （正在算 20000 次 PBKDF2），心跳一直在正常应答，永远不会判失活。没有
  独立的握手超时，一个半死的状态机能永久占着连接。
- **心跳基线在 `onEstablished` 建立，不在构造函数**。构造到连上之间可能
  隔几秒（DNS + TCP），用构造时刻做基线会让连接一建立就"已经空闲很久"。
- **`finish()` 里投递而非同步通知上层**。它在很多地方被调用（包括
  Connection 的帧回调里），同步通知会让上层在"会话还在处理这一帧"的栈上
  就把会话拆掉。
- **`finish()` 幂等且保留首个原因**。认证失败后底层连接会紧接着报 EOF，
  若被那次关闭覆盖成"成功"，上层就不知道口令是错的。

##### 转发与发送是两个入口（`forward()` / `send()`）

实现时撞上一个真冲突：`forward()` 先把 `from` 改写成发送者的认证身份，
而 `send()` 里的出站断言只允许"空或服务端自己的 id"——转发帧的 `from`
恰恰是别人的 id，于是**正确的转发被自己的断言拒掉了**。

根因是 `PROTOCOL.md` §11.4 原来只写了"不许出现第三方身份"，把转发帧
这个正当例外漏了。修法不是放宽断言（那等于关掉兜底），而是承认出站帧
有三类、`from` 的合法集合各不相同：

| 帧的来源 | 合法 `from` |
|----------|-------------|
| 服务端主动发起 | 空，或服务端自己的 id |
| 本地受理后回复 | 空，或服务端自己的 id |
| 转发他人的帧 | 恰好是发送者的认证身份 |

两个集合互不相交，所以判定时必须显式区分"这是转发帧"还是"我自己的帧"。
现在 `forward()` 走自己的断言（`bound == authenticatedId` 且 `to` 非空），
`send()` 走 `checkOutbound()`，两者最终都调同一个私有的 `deliver()` 落盘
计数。§11.4 已同步改写。

有一条测试专门钉住"两个入口的合法集合互不相交"——用同一个
`from="client-1"` 分别调 send（拒）和 forward（收），防止后人"顺手合并
两个入口"。

#### 3.4.7 peer 表、id 分配与心跳定时器（已完成）

**状态：已完成（peer → 会话的编排层除外）**。要点：

- `core/include/relayer/core/session/PeerTable.h`：`id → 在线连接信息` 的
  表，纯数据、不认识 `Session` 也不认识 asio，所以边界情况（重名、注销
  顺序、容量上限、列表顺序）全都能用纯数据测——`PeerTableTest.cpp` 24 个
  用例，不用起端口。
- 三条刻意的取舍：重名**不顶掉**先到者（顶掉等于允许后来者踢人）；注销
  **不做静默幂等**（调用方要知道这次是不是真的删了，否则会广播重复的
  下线通知）；底层用 `std::map` 而不是 `unordered_map`（迭代顺序稳定，
  客户端列表不会因重哈希乱跳）。
- `allocateId()` 递增跳号，而不是 `size()+1`：后者在"有客户端下线"之后
  会发出一个仍在使用的 id。
- `kAnswerId` / `kAnswerClientList` 载荷编解码，与路由字段统一用 `u8`
  长度前缀。
- 服务端在认证成功后**主动**发 `kAnswerId`，不等客户端 `kAskId`。请求-
  应答会让"客户端能不能干活"多依赖一次往返，而服务端那边其实已经登记
  完了。
- id 来源优先级：`allocatePeerId` 回调 > `initialPeerId` 配置 > 空。
  allocator 优先，因为这时代码里"配了却永不生效"是最难排查的一类问题。
  `Session::setPeerIdAllocator()` 在认证完成后调用会返回 `false`——id 已经
  announce 出去了，再改就是"注销后重新注册"，必须由上层显式做。
- 心跳 `steady_timer` 接线：固定 `kTickInterval` 轮询而不是"按到期时间设
  一个长定时器"。后者要在每次收到数据时重置，漏掉任何一处就让空闲判定
  失效；轮询的代价是每秒一次空转，相对 20 秒阈值可忽略。

**已知缺口**：`PeerTable` 还没有和 `Session` 装到一起——`planRoute()` 判定
`kForward` 之后，"从哪条连接发出去"目前仍由调用方自己持有引用完成。这层
编排（`RelayServer`）留到 app 层做，因为"一个 id 对应哪条连接"的生命周期
天然属于进程级，放进 core 只会让 core 反过来依赖"谁在管理这些会话"。

---

### 3.5 core/transfer：传输引擎

#### 3.5.0 状态机（已完成）

`core/include/relayer/core/transfer/StateMachine.h`。把 `PROTOCOL.md` §8 的状态与
转移写成一张 `constexpr` 表，`StateMachine::fire(event)` 查表并返回 `Status`。

**状态：已完成**。要点：

- 状态与事件都是显式枚举，转移表 `kTransitions[]` 是纯数据（`{from, event, to}`）。
  没有 switch-cascade——那种写法漏一个 case 编译器不会说话。
- 非法转移返回 `kProtocol` 错误、**状态保持不变**。不改成一个 `Failed` 状态，
  是因为诊断时最需要知道的是"当前停在哪一步"。
- `kDone` 是唯一终态，任何事件都被拒——包括迟到的 `Resume`：一次已作废的传输
  不该因为一条晚到的恢复请求又活过来。
- 测试遍历 **6 状态 × 10 事件 = 60 个组合**，逐个断言"表里写的成功、没写的失败
  且状态不变"，另有一条独立断言合法 10 条 / 非法 50 条。
- `tools/check_transition_doc.py` 接进 CTest：直接解析 `PROTOCOL.md` §8.1/§8.2
  的表格，与代码比对（T1-T5）。已用"篡改目标状态""删掉一条转移"两种方式验证
  它真的会失败，不是一个恒真的检查。
- 单元测试里另有一份**手抄**的文档表用于比对。刻意不从 `kTransitions` 生成：
  那样测试就退化成"表和自己相等"，永远不可能失败。

#### 3.5.1 线格式与校验（已完成）

**状态：已完成**。要点：

- **先补线格式，再写引擎**。原来 §8.3 只有语义描述，没有任何字节布局——
  照那样写编解码只能靠临时约定，而"两端的约定不一致但各自都能通过自测"
  是最难排查的一类问题。现在 `PROTOCOL.md` 补了 §8.3.1（Merkle 树构造）
  与 §9.4–§9.8（会话描述 / 应答 / 分块数据 / 分块确认 / 完成握手 / 取消）。
- `core/transfer/MerkleTree.{h,cpp}`（17 用例 / 203 断言）：
  `SHA256(0x00 || 补齐块)` 作叶子、`SHA256(0x01 || L || R)` 作节点，
  奇数个时**最后一个单独上提**（不复制）。
- `core/transfer/TransferPayload.{h,cpp}`（35 用例 / 556 断言）：
  六种载荷的编解码，含"未知状态码必须拒绝"和"位图长度与 body 不符必须拒绝"。
- `common/Payload.h`：`PayloadWriter` / `PayloadReader`。这套工具在 session
  已经有一份私有实现，transfer 是第二处，第三处（控制类应答解析）在排队，
  所以提到 common 共用——复制三份之后长度前缀该用 u8 还是 u16 就会开始分叉。

实现时撞上的两个真错误（都写进 .cpp 注释了）：

1. **增量 Merkle 合并与规范形状不等价**。最初的 `MerkleBuilder` 边收边合并，
   2 个叶子对、3 个叶子也对，**4 个开始分叉**。改成维护多层"待配对"节点后
   3 个对了、7 个又错。根因是"奇数上提"在多层结构里没有唯一答案。最终
   退回"存叶子、root() 时调 computeRoot"——用一点内存换"只有一份形状实现"。
2. **`root()` 的扫描方向**。从低层往高层扫会把落单节点当成左操作数，
   `[a,b,c]` 会算成 `node(node(c, node(a,b)))` 而不是 `node(node(a,b), c)`。
   层级越高覆盖的数据越靠左，所以必须**从高往低**扫。

验证方式：Python `hashlib` 按文档文字独立实现一遍，算出 0..9 块的树根，
与 C++ 逐条比对。这 10 个值已作为金标准向量写进测试——两份独立实现
（C++ 参考 + Python）都对上，才说明"文档 → 实现"这一步没走偏。

#### 3.5.2 引擎设计（未开始）

复用 v1 已验证的做法：

- 先落盘后 ACK
- `.relaypart` + 原子替换
- 字节窗口（4MB）+ 累积 ACK + SACK
- 断点续传（resumeOffset + 指纹）
- 完成握手（Request_Complete / Answer_Complete）

相对 v1 的改进：

- **依赖 `IFileStore`**，不再直接 `QFile`。
- **多任务并行**：每任务独立会话、独立 `.relaypart`、独立超时，无单槽状态。
- **校验和升级**：v1 只校验长度，v2 增加**分块 Merkle 树**，传完可证明内容一致，
  而不是只信长度（长度一致但内容错位的场景 v1 无法发现）。**已完成**。
- **背压**：窗口上限与内存占用挂钩，避免大窗口下内存翻倍。

几个已经定下来的设计选择（写在这里免得实现时重新纠结）：

- **块大小由发送方定，不做双向协商**（§9.4.1）。协商要一轮往返，而"多大块
  合适"几乎完全由发送方读盘速度决定。接收方只能在应答里给一个建议上界，
  采不采纳由发送方定。
- **`ackThrough` 是最长连续前缀，不是"最后收到的 + 1"**（§9.7）。写成后者
  在乱序时会直接丢数据且不报错——长度对得上、内容错位，只有哈希能发现。
- **`kFileAnswerComplete` 的 `status == 0` 表示"已经原子替换完成"**，不是
  "打算替换"（§9.8）。提前应答会让发送方删掉源侧状态，而此时目标文件还不存在。


#### 3.5.3 测试

三进程集成测试（server + sender + receiver），覆盖：

| 场景 | 断言 |
| --- | --- |
| 正常传输（2MB/200MB/1GB） | 内容哈希一致 |
| 中途 kill 发送端 | 目标文件不被破坏，残片只在 `.relaypart` |
| 预置正确残片 | 从断点续传，不重传已完成部分 |
| 预置伪造残片 | 丢弃残片，全量重传 |
| 乱序/丢包 | SACK 只重传空洞 |
| 空文件 / 1 字节 | 正常完成 |
| 目标被占用 | 明确报错，不挂死 |

---

### 3.6 core/discovery：目录、信令、P2P

- 客户端登录时上报本机监听地址列表。
- `Ask_PeerInfo` / `Answer_PeerInfo` 交换地址。
- 直连优先，失败回退中继；回退决策记录到日志，便于排查。
- **P2P 的信任模型**：直连同样要跑认证握手，不因为是局域网就放行。

---

### 3.7 app：Qt 前端

#### 3.7.1 复用清单

| 复用 | 方式 |
| --- | --- |
| 9 个 `.ui` | 原样复制，布局零改动 |
| `MenuIcons.h` | 原样复制 |
| 控件类（`OwnTableWidget` 等） | 复制骨架，**逻辑重写** |
| 表格列定义、菜单文案 | 保留 |

#### 3.7.2 控件逻辑重写清单（v1 的混乱点）

这是本次重写要集中治理的部分。逐条列出 v1 的具体问题与 v2 的做法：

| # | v1 问题 | 位置 | v2 做法 |
| --- | --- | --- | --- |
| 1 | `setItemData` 兼任「建 item / 设文本 / 设只读」三件事，参数是两个布尔，调用处难读，且建 item 时设只读、已有 item 时不改，导致拖拽后「远端」列不可编辑 | `OwnTableWidget.cpp` | 拆成 `ensureItem(row,col)` / `setText` / `setReadOnly`，列的可编辑性由**表头配置表**统一定义，不再逐处传参 |
| 2 | 拖拽落点判定写死列号 `if (startCol != 4 && startCol != 5)` | `OwnTableWidget.cpp:65` | 列语义用 `enum class Column`，判定用 `isPathColumn(col)` |
| 3 | 拖拽补列时只补 6 列以后，前面缺列不管 | 同上 | 统一 `ensureRowComplete(row)`，一次补齐 |
| 4 | 批量填充用 `datas[i * headers_.size()]` 取行，依赖"选中项必然是整行且列数对齐"的隐含假设 | `ComparisonControl.cpp:370` | 用 `selectedRows()` 取行集合，不依赖列数 |
| 5 | 右键菜单 9 个 `if (selectAction == xxx)` 平铺，且靠 `menu.exec` 返回值比较，顺序敏感 | `ComparisonControl.cpp:390-441` | 改成动作表 `{action, 条件, 回调}`，注册式 |
| 6 | 删除行时倒序删且手动收集 `delIds_`，删除与状态维护耦合 | 同上 | `deleteRows(rows)` 封装，内部处理 id 收集 |
| 7 | 多选时 `datas.size() / headers_.size()` 判行数，选中不完整时算错 | 多处 | 用 `selectionModel()->selectedRows()` |
| 8 | 保存配置时逐行读取 `item(i, 0)->text()`，任一 item 为空即崩 | `ComparisonControl.cpp:171-179` | 统一 `readRow(row) -> RowData`，缺列返回空值不崩 |
| 9 | `showEvent` 里用 `static bool inited` 做一次性初始化，最小化恢复也会触发，靠 static 兜住 | `ComparisonControl.cpp:281` | 用构造函数 + 显式 `initialize()`，去掉 static |
| 10 | 传输任务状态与表格展示混在一起，`rowDisplay_[id]` 与表格行号靠刷新函数同步 | `RelayTask.cpp` | 引入 `TaskRowModel`，状态变化走模型更新，表格只做渲染 |
| 11 | 进度/速度计算散落，且有空文件除零（已修但结构没变） | `RelayTask.cpp` | 抽出 `ProgressTracker`，除零/时间边界在类内闭环 |
| 12 | 批量传输循环里混着压缩分支、归档分支、普通分支，`return` 散落 | `RelayTask.cpp:200-437` | 拆成 `runDirect()` / `runArchive()` / `runSingle()` 三个方法 |

原则：**控件类不再直接持有业务逻辑**，只做「显示 + 转发事件」；业务逻辑下沉到
`app/model` 或 `core`。

#### 3.7.3 bridge 适配层

唯一的 Qt 边界。四件事：

| 适配项 | 做法 |
| --- | --- |
| 事件投递 | core 的 `Signal<>` → `QMetaObject::invokeMethod` 到 GUI 线程 |
| 字符串 | `std::string`（UTF-8）↔ `QString`，边界处转换 |
| 数据类型 | core 的 POD → Qt 侧只读视图 |
| 生命周期 | core 对象由 bridge 持有，析构顺序显式 |

---

## 4. 实施阶段

每阶段结束必须满足：**能编、能测、能提交**。不追求速度，追求可回退。

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **A 地基** | 仓库骨架、CMake、vendored 三方库、CI、`.clang-format`、LICENSE、CONTRIBUTING | 空仓库 CI 绿；`cmake && cmake --build` 通过 |
| **B filestore** | `IFileStore` + `StdFileStore` + `MemFileStore` + `PathPolicy` + `tar.xz` 归档 | ✅ 已完成：117 用例 / 1718 断言，Release `-Werror` 下 ctest 全绿；与 GNU tar 双向互操作已验证。zip 未做（见 3.1.4） |
| **C protocol + crypto** | 帧编解码、消息、PBKDF2/HMAC、握手状态机，`PROTOCOL.md` | ✅ 已完成：认证握手（39 用例 / 291 断言）、帧编解码（26 / 443）、传输状态机（60 组合）、心跳策略（14 / 766）、会话策略（23 / 776）、`Session` 组装（39 / 858）、`PeerTable`（24 / 106）；`PROTOCOL.md` 竣工 |
| **D transport + transfer** | asio 连接、传输引擎接入 `IFileStore` | ✅ 已完成：`server/` + `cli/` + 三进程 e2e 10 场景全过（含 1GB），缺陷与验证记录见 `docs/HANDOFF.md` |
| **E app** | 搬 `.ui`、控件逻辑按 3.7.2 重写、bridge 接线 | 🚧 **进行中**：最小闭环已通（连接/自动接收/发送/对照表/任务表单）——进度与剩余项见下方 |
| **F v2 功能** | 认证上线、多任务并行、P2P 直连、远端浏览/归档 | 并行 3 任务互不干扰；直连成功率与回退日志可查 |
| **G 发布** | 打包、安装包、版本号单一来源、CHANGELOG | 干净机器解压即用 |

当前 CTest 共 7 项：`common`（16 用例 / 1111 断言）、`filestore`（117 / 1718）、
`core`（341 / 5482）、`arch`（架构规则 R1-R6）、`protocol_doc` 与 `transition_doc`
（文档与代码一致性）、`e2e_quick`（三进程 13 场景，约 50s）。
合计 474 用例 / 8311 断言，全量 52 秒。

### 4.1 阶段 E 进度（2026-10-23 更新）

已落地（按提交顺序）：

| 子项 | 内容 | 状态 |
| --- | --- | --- |
| E1 | `app/` 骨架、9 个 `.ui` 原样搬迁、Qt6 自动发现与缺 Qt 时的可照做报错 | ✅ |
| E2 | `bridge/CoreService`：io_context 线程 + `asio::post` 单线程契约 + 跨线程 queued 信号 | ✅ |
| E3.1 | `Base/InfoDrop`、`Control/CheckableComboBox` | ✅ |
| E3.2 | `OwnTableWidget` 重写（清单 #1/#2/#3：`ComColumn`/`ExpColumn` 枚举、列可编辑性收敛、`ensureRowComplete`） | ✅ |
| E3.3 | `ExplorerControl`（清单外的本地浏览；远端面板排除，v2 无 list-dir 帧） | ✅ |
| E3.4 | `ComparisonControl`（清单 #4-#9：`selectedRows`、`readRow`、动作表、JSON 持久化、去掉 `static` 一次性初始化）+ 相对目录落盘 | ✅ |
| E3.5 | `RelayTaskControl` 任务表单（清单 #10/#12：批次推进、停止、重传、`connectionLost` 终止规则） | ✅ |
| — | 身份稳定：连接=常驻接收会话、`askListOnStart`、`sessionId` 解复用（见 `PROTOCOL.md` §9.4.1） | ✅ |
| — | 性能三杠杆：默认 256 KiB 块、控制帧插队、单帧写超时 60s；接收端写流复用 | ✅ |
| E3.6 | `Setting` 对话框（`btnLock` 解禁）：自动接收重试策略（重试开关/间隔/次数，次数 0=一直）+ 接收覆盖开关；QSettings 键收拢到 `ui/SettingsKeys.h`。v1 的压缩/加密两组**不迁移**（v2 无对应物，不放假开关，理由在 `Setting.h` 注释） | ✅ |
| E3.7 | `ReplaceArea` 作用域对话框：作用列对齐 v2 四字段（名称/本地目录/对端/相对目录）；v1 的 useCase/useRegex 是**假选项**（结果传入 lambda 但从未使用），v2 做成真的（`Qt::CaseSensitivity` + `QRegularExpression`，pattern 先验证） | ✅ |
| E3.8 | `model/ProgressTracker`（清单 #11）：除零/时间边界/速度择优类内闭环（bytesTotal=0 占位、无瞬时速度退回自算均值、开头 200ms 不显示均值）；任务表进度列升级为「百分比 · 速度」；`humanBytes` 三处拷贝收拢 | ✅ |

阶段 F 的第一块（2026-10-23）：

| 子项 | 内容 | 状态 |
| --- | --- | --- |
| F1 | 远端目录列举：`kAskFileList`/`kAnswerFileList`（`PROTOCOL.md` §11.8）、`core/session/FileList` 编解码、客户端侧服务（共享根 + `resolveWithin`）、CLI `ls`、GUI：`ExplorerControl` 的本地/远端双数据源（v1 形状，同一个控件切数据源）+ 共享目录设置 + 对照表右键"检查远端"、e2e `remote-list` | ✅ 完成 |

剩余（下一次从这里接）：

1. **E4 验收**——GUI 已在本机中继 + loopback 上完成收发，还缺一次**真实链路**
   （两台机器经中继）的完整记录。e2e 的 no-auth 缺口已补（见 §4.1 上方场景表）。
2. 阶段 F 才做的：远端浏览/下载、对端打包归档、P2P 直连、多任务并行
   （前三项需要协议扩展，`cli`/`core` 侧都还没有对应帧）。

上次的收尾三项（Setting / ReplaceArea / ProgressTracker）已全部完成，
提交 `beb6d63` / `c118c06` / `32500f6`。

---

## 5. 工程标准

### 5.1 命名与风格

- 类型 `PascalCase`，函数 `camelCase`，成员 `trailing_`，常量 `kPascalCase`。
- 头文件 `#pragma once`。
- 统一 `.clang-format`（基于 LLVM，120 列）。
- 不用 `using namespace`；头文件不引入 `<bits/stdc++.h>` 之类。

### 5.2 错误处理

- **跨模块边界不用异常**：文件、网络、协议错误全部走 `Status` / 错误码。
- 异常只允许出现在「解析外部不可信数据」的内层，且必须就地捕获。
- 每个 `catch` 都要有日志，不允许静默吞掉。

### 5.3 日志

- `spdlog`，分级，格式统一。
- **禁止在库代码里输出到 stdout**；日志由调用方配置。
- 关键路径（握手、任务开始/结束、重传、回退）必须有日志，且包含 uuid。

### 5.4 测试

- 框架 Catch2，vendored。
- 单测：纯函数、状态机、路径、编解码。
- 集成：三进程端到端。
- CI：Windows + Linux，Debug + Release 双跑（**仅 tag 触发**，见 7 节决策记录）。

### 5.5 提交规范

- Conventional Commits：`feat:` `fix:` `refactor:` `docs:` `test:` `build:` `chore:`。
- 一个提交一件事，能编译。

---

## 6. 需要拍板的决策

| # | 问题 | 结论 |
| --- | --- | --- |
| 1 | 依赖：vendored 还是 FetchContent | **已定：vendored**，版本与许可证记在 `third_party/README.md` |
| 2 | Qt 版本 | **已定：只支持 Qt 6**，砍掉 Qt5 条件编译 |
| 3 | 仓库历史 | **已定**：`D:\Code\relayer` 全新 `git init`，v1 留在原仓库当参考 |
| 4 | 测试框架 | **已定：Catch2**（vendored） |
| 5 | 服务端是否需要文件能力 | **已定：不需要**，见下方 6.1 |

### 6.1 服务端的职责边界（重要）

**`relayerd` 只做转发。** 它不理解文件，不读写磁盘上的用户数据，不打包、
不校验内容、不缓存分片。它知道的只有「谁在线」「谁想连谁」「这条连接该往哪条
通道倒字节」。

这个边界带来三条硬性约束，写进架构检查：

1. **`server/` 不链接 `filestore`**（已有的 R4 规则）。不是"暂时不用"，而是
   永远不用——一旦允许，转发层迟早会被塞进"顺手校验一下"的逻辑。
2. **服务端进程的工作目录里不产生任何用户数据文件**。日志除外，且日志路径
   可配置、可关闭。
3. **端到端校验由两端自己做**。服务端无法伪造校验通过，也无法因为自己的
   磁盘问题导致传输失败。

这样做换来的是三件事：

- **安全面小**：服务端被攻破时不持有用户数据，也没有可被路径穿越利用的
  文件操作入口（v1 的 zip-slip 这类问题在服务端根本不存在，因为服务端
  不碰归档）。
- **部署简单**：无状态、无磁盘依赖，可以跑在只读文件系统的容器里，
  也可以多实例水平扩展（会话粘滞即可）。
- **可审计**：想确认"服务端真的没偷看我的文件"，读一遍 `server/` 就够了，
  代码量会控制在一两千行。

代价也很明确，必须写在文档里避免以后被当成 bug：

- 服务端**无法**在两端都在线之前暂存文件，所以**不支持离线投递**。
  要发文件，双方必须同时在线的窗口内完成握手。
- 服务端**无法**校验数据完整性，损坏只能由接收端发现并请求重传。
- 如果两端都无法直连且中继带宽不足，传输会变慢——这是物理限制，
  不是实现问题。

---

### 6.2 客户端标识（id）必须是稳定身份

> 用户 2026-10-23 拍板：**沿用 v1 语义**——客户端自己声明标识，服务端只校验
> 占用，撞名**拒绝连接并提示**。本节是这条决策的完整设计，防止重写时丢失。
> 当前状态：**已实现**（同日完成，见 §6.2.5 落地记录）。

#### 6.2.1 动机（不是洁癖，是防事故）

对照表（`ComparisonControl`）的每一行都存着"发给哪个对端 id"。现实用法是
"这个文件固定发 A、那个固定发 C"，并且需要**事后核对**发对了人。id 一旦漂移：

- 表里的 id 指向别人 → 文件发错人（v1 时代真实发生过）；
- 或者指向不存在的连接 → 服务端静默丢帧，发送方等满应答超时才报错，
  用户只看到"卡了很久然后失败"，看不出是 id 的问题。

所以 **id 代表"运行该软件的人"，不是"这一次连接"**。它必须满足三条：

1. 由用户决定（至少能被固定下来），跨重启、跨重连都不变；
2. 全局唯一——同一个 id 同时只能有一条在线连接；
3. 冲突时**明确失败**，绝不静默改名、绝不顶掉先到者。

第 3 条是关键：自动改名会让对照表在用户不知情的情况下指向另一个人，
正是要防的那类事故。宁可连不上，也不能连错。

#### 6.2.2 v1 与 v2 现状的差异

| | v1 | v2（实现前） | v2（现在） |
| --- | --- | --- | --- |
| id 来源 | 用户配置，随便设 | 服务端 `allocateId()` 生成 `client-<n>` | 客户端 `kClaimId` 声明；不声明才分配 |
| 是否可固定 | 是 | 否 | 是（跨重连、跨重启） |
| 撞名处理 | 拒绝连接 + 提示 | 不存在（分配器天然不重名） | 拒绝（status=2），不顶替、不改名 |
| 跨重连稳定 | 是 | 只在常驻会话存活期间稳定 | 是 |
| `--id` | — | 只是**断言**（对不上就退出） | **声明**（被占用则连接失败） |

`kDirectRequestId`(550) 看着像这个用途，其实不是——它属于 P2P 直连段
（"直连时查询对端 id"，见 `PROTOCOL.md` §4.4），未实现且语义无关。别复用它。

#### 6.2.3 设计（实现时按这个来）

**线格式**

| 项 | 定义 |
| --- | --- |
| 新帧 | `kClaimId = 134`（控制段 100..199，C→S） |
| 路由 | `to` 为空 = 本地受理（与 `kAskId` 同一约定） |
| body | `u8 idLen \|\| id[idLen]`（与路由字段同用 u8 长度，见 §9.1） |
| 应答 | **复用** `kAnswerId`(101)：`u8 status \|\| u8 idLen \|\| id` |
| status | 0 = 接受（回显最终 id）；新增"已被占用"、"id 非法"两个码 |

**时序**：认证完成后、客户端 `kAskId` 之前。服务端收到 `kClaimId` 时：

1. `isValidPeerId()` 校验（非空、≤255 字节），不合法 → 回 status + 断开；
2. `peers_` 里已存在该 id（且不是自己）→ 回 status + **断开连接**
   （v1 语义：拒绝连接）；
3. 合法且空闲 → 把会话身份改成新 id（`Session::renamePeerId`），同步
   `peers_` / `sessions_` 两张表，回 `kAnswerId(0, id)`。

**为什么不让认证帧携带 id**：认证帧的布局与 HMAC 覆盖范围是协议核心，
塞变长字段会牵动所有测试向量；独立帧 + 改名代价小一个量级，而且
`kAnswerId` 的 `status + id` 结构本来就是现成的，客户端已有"改名后
再通知一次"的处理路径（`ClientConfig::onIdAssigned` 允许多次回调）。

**为什么保留服务端分配**：没声明 id 的客户端（临时用一次 CLI、e2e、
第三方接入）不该被强制先起名。默认仍是 `client-<n>`。

**影响面**

| 位置 | 改动 |
| --- | --- |
| `docs/PROTOCOL.md` | §4 帧表加 `kClaimId`；新增"身份声明"小节；status 表加两个码 |
| `core/protocol` | 帧类型 + body 编解码（`protocol_doc` 测试会逐项比对） |
| `core/session` | `Session::renamePeerId()`；`Server` 的 `kAcceptLocal` 分支 |
| `server` | 占用检查 → 改名 → 应答 → 冲突断开；统计计数 |
| `cli` | `--id` 语义从"断言"改为"声明"（对不上不再失败，冲突才失败） |
| `app` | 连接面板加"本机标识"输入框（QSettings 持久化）→ `ConnInfo.requestedId` |
| 测试 | 单测：帧编解码、改名、占用冲突；e2e：两个客户端抢同一 id，第二个被拒 |

#### 6.2.4 边界与待定
- **字符集**：`isValidPeerId` 只要求非空 + ≤255 字节，不设白名单
  （中文 id 应当能用）。是否禁止前导/尾随空白、控制字符——实现时定，
  倾向"拒绝控制字符与首尾空白"，避免"看起来一样但注册不冲突"的坑。
- **同一人开 GUI + CLI 用同一 id**：第二个被拒（这是期望行为，用户已确认）。
  需要多开就起两个不同的 id。
- **发给自己的 id（自投）**：**已实现**（2026-10-23）。`PROTOCOL.md`
  §11.3.1 现在规定 `to == 发送者自己的 id` 走正常转发、绕回同一条连接
  （`planRoute()` 返回 `kForward`，不是 `kReject`，也不是本地受理——本地
  受理会让帧进服务端的"无文件业务"黑洞）。客户端侧不需要特判：它本来就
  接受任意 `from` 的入站帧，同连接上的收发靠 `sessionId` 解复用。
  CLI 用 `recv --self-send <file>` 一个进程验完整链路；e2e 场景
  `self-send` 钉住它。安全性未变：`selfId` 来自认证而非 `from`，
  自投只能绕回自己，`from` 仍被绑定成真实身份。

#### 6.2.5 落地记录（2026-10-23）

按 §6.2.3 实现完毕，实现中修正了三处设计细节：

1. **拒绝时服务端不主动断开**。原计划是"应答后断开"，实际不可行：
   `Connection::close` 会清空写队列，先发后关会把 status 吃掉，客户端只能
   看到"连接断了"，分不清撞名还是网络问题。改为只回 status，由客户端
   收到后自己关闭。
2. **"临时号"应答可能晚于声明到达**。认证时服务端先发 `client-N`，那条
   `kAnswerId(status=0)` 可能排在声明之后；若仅凭"收到 status=0 就算声明
   结束"，会把待办状态清掉，随后的拒绝被当成无关应答忽略——症状是撞名的
   客户端一直挂着不报错。修法：`Session` 记住 `claimTarget_`，只有拿到
   **声明的那个名字**才算结束。
3. **客户端在声明完成前不对外宣布身份**。否则 `ASSIGNED_ID=` 会先打
   `client-N`，e2e 的 `head -1` 拿到临时号，GUI 也会闪一下错误身份。

落地清单：

| 位置 | 内容 |
| --- | --- |
| `PROTOCOL.md` | §4.2 帧表加 134；新增 §11.7（含 status 2/3 与两条"为什么不"） |
| `core/protocol` | `kClaimId` + `toString` |
| `core/session` | `PeerTable` 的 claim 编解码、`IdAnswerStatus`；`Session::claimPeerId` / `assignPeerId`；客户端按 `claimTarget_` 判定结果 |
| `server` | `handleClaimId`：校验 → 占用检查 → 迁表 → 改名；`idConflicts` 计数 |
| `cli` | `--id` 改为声明；拒绝后给出可照做的结论 |
| `app` | 连接面板「本机标识」输入框（QSettings 持久化）→ `ConnInfo::requestedId` |
| 测试 | `PeerTableTest` 6 组编解码用例；e2e `fixed-id`（采纳 / 拒撞名 / 先到者不受影响 / 断开后名字可重用） |

**自投仍未实现**（上面 6.2.4 最后一条），是这条决策之外的另一件事。

---

## 7. 决策记录

按时间追加，格式：`日期 | 决策 | 原因`。

| 日期 | 决策 | 原因 |
| --- | --- | --- |
| 2026-10-22 | Core 改为零 Qt，拆分 core（通信）与 filestore（文件） | 编译简单、服务端可脱 Qt、P2P 更自然 |
| 2026-10-22 | filestore 用接口 + 多实现 | `std::filesystem` 有坑时可切 Qt 实现；可注入测试实现 |
| 2026-10-22 | 控件逻辑允许重写，布局不动 | v1 控件层逻辑与展示耦合，问题集中（见 3.7.2） |
| 2026-10-22 | 帧格式沿用 RFM1 | 已验证，避免重蹈协议升级风险 |
| 2026-10-22 | 新增分块 Merkle 校验 | v1 只校验长度，内容错位无法发现 |
| 2026-10-22 | **服务端纯转发，零文件能力**（见 6.1） | 缩小攻击面、无状态部署、可审计；代价是不支持离线投递 |
| 2026-10-22 | CI 只在 `v*` tag 与手动触发时运行 | 日常 push 跑全量 Windows + Qt 构建耗额度，收益低于本地 ctest |
| 2026-10-23 | **id 改为客户端声明、服务端只校验占用，撞名拒绝连接**（见 6.2） | 对照表按 id 固定投递，id 漂移会发错人（真实发生过）；自动改名/顶替同样会让表悄悄指向别人 |
