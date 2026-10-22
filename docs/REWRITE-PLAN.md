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

#### 3.2.3 状态机

传输状态机用显式枚举 + 转移表，而不是散落在 `if/else` 里：

```
Idle → Handshaking → Transferring → Finalizing → Done
                   ↘ Interrupted → (Resume) → Transferring
```

转移表写成数据（`struct Transition { State from; Event e; State to; }`），
用测试遍历所有非法转移，确保都不会静默通过。

---

### 3.3 core/crypto + auth：认证

#### 3.3.1 原语

- `Aes256Gcm`（从 v1 迁移，已自包含）
- `Sha256`（同上）
- **新增** `Pbkdf2HmacSha256`、`HmacSha256`

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

#### 3.3.3 身份绑定

**服务器转发时强制覆写 `frame->from` 为认证身份**。这是 v1「from 可伪造」的根治手段。

---

### 3.4 core/transport：连接

- asio 裸 socket（不用 asio 的 Qt 类比封装），每条连接一个 `strand`，保证回调串行化。
- 0 个全局 io_context 之外，不允许有隐式全局状态；`io_context` 由调用方持有并注入。
- 关闭流程显式：`shutdown(fd) → 等待在读回调返回 → 释放`，杜绝 v1 里的"对象析构时还在收帧"。
- 心跳与超时统一由 `transport` 层提供，不散落在业务层。

---

### 3.5 core/transfer：传输引擎

#### 3.5.1 复用 v1 已验证的设计

- 先落盘后 ACK
- `.relaypart` + 原子替换
- 字节窗口（4MB）+ 累积 ACK + SACK
- 块大小协商
- 断点续传（resumeOffset + 指纹）
- 完成握手（Request_Complete / Answer_Complete）

#### 3.5.2 改进

- **依赖 `IFileStore`**，不再直接 `QFile`。
- **多任务并行**：每任务独立会话、独立 `.relaypart`、独立超时，无单槽状态。
- **校验和升级**：v1 只校验长度，v2 增加**分块 Merkle 树**，传完可证明内容一致，
  而不是只信长度（长度一致但内容错位的场景 v1 无法发现）。
- **背压**：窗口上限与内存占用挂钩，避免大窗口下内存翻倍。

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
| **C protocol + crypto** | 帧编解码、消息、PBKDF2/HMAC、握手状态机，`PROTOCOL.md` | 半包/粘包/重同步/CRC/重放测试全过 |
| **D transport + transfer** | asio 连接、传输引擎接入 `IFileStore` | 三进程集成测试覆盖第 3.5.3 表全部场景 |
| **E app** | 搬 `.ui`、控件逻辑按 3.7.2 重写、bridge 接线 | GUI 可完成一次完整传输；控件逻辑问题清单逐条关闭 |
| **F v2 功能** | 认证上线、多任务并行、P2P 直连 | 并行 3 任务互不干扰；直连成功率与回退日志可查 |
| **G 发布** | 打包、安装包、版本号单一来源、CHANGELOG | 干净机器解压即用 |

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
