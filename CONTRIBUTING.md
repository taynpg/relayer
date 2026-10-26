# 参与开发

本文写给两类人：想给 relayer 提 PR 的人，和三个月后回来改自己代码的人。

工程的整体设计见 [`docs/DESIGN.md`](docs/DESIGN.md)，线协议见
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)。改动涉及分层、协议、依赖方向时，
先读这两份文档的对应章节；要推翻某个既有决策，先在 `DESIGN.md` 的决策记录
里补一条再动实现。

---

## 1. 环境

| 项 | 要求 | 说明 |
| --- | --- | --- |
| 编译器 | MSVC 2022 / GCC 13+ / Clang 16+ | 需要完整的 C++17 支持 |
| CMake | 3.20+ | |
| 构建器 | Ninja（推荐）或任意 | |
| Qt | **6.5+**（仅构建 GUI 时需要） | 只支持 Qt 6 |
| Python | 3.9+（仅跑架构检查时需要） | 可选，但强烈建议装上 |

三方库全部 vendored 在 `third_party/`，版本与许可证见
[`third_party/README.md`](third_party/README.md)。**不要**改用 FetchContent 或
系统包管理器获取依赖，理由同样写在那份文档里。

### Windows（MSYS2 UCRT64）

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
                    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-clang-tools
```

GUI 还需要 Qt6：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools
```

### 通用构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELAYER_BUILD_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

只改核心库时可以关掉不需要的部分，编译速度快一个数量级：

```bash
cmake -S . -B build -G Ninja -DRELAYER_BUILD_GUI=OFF -DRELAYER_BUILD_SERVER=OFF -DRELAYER_BUILD_CLI=OFF
```

---

## 2. 提交前必须做的三件事

```bash
# 1. 格式：不一致的文件会被 CI 打回
clang-format -i $(git diff --name-only --diff-filter=ACM | grep -E '\.(cpp|h)$')

# 2. 构建零告警（-Werror 打开时告警即错误）
cmake -S . -B build -DRELAYER_WERROR=ON && cmake --build build

# 3. 测试 + 架构检查
cd build && ctest --output-on-failure
```

`ctest` 里的 `arch` 用例等价于 `python3 tools/check_arch.py`。它会检查依赖方向：

- `core/` 不得包含 Qt 头文件
- `core/` 只能包含 `filestore/IFileStore.h`，不能碰具体实现
- `filestore/` 不得反向依赖 `core/`
- `server/` 不得包含 Qt，也不得碰 `filestore/`
- `app/` 之外不得包含 Qt

违反时脚本会打印 `规则名: 文件:行号`，按提示改回去即可，不要修改脚本放过自己。

---

## 3. 代码约定

### 3.1 能自动化的不靠自觉

缩进、换行、大括号位置、include 分组全部由 `.clang-format` 决定。**不要**
手工调整这些细节——评审时如果发现纯格式差异，说明提交前忘了格式化。

### 3.2 命名

| 种类 | 规则 | 例子 |
| --- | --- | --- |
| 类型、类、结构体、枚举 | PascalCase | `StdFileStore`、`DirEntry` |
| 函数、方法、局部变量 | camelCase | `openRead`、`quoteBytes` |
| 成员变量 | camelCase + 尾部下划线 | `fp_`、`bufferLen_` |
| 常量（`constexpr` / `static const`） | `k` + PascalCase | `kBlockSize`、`kProbe` |
| 枚举值 | `k` + PascalCase | `ErrorCode::kNotFound` |
| 宏 | 全大写 + 前缀 | `RELAYER_BUILD_GUI` |
| 文件名 | 与主类型同名 | `StdFileStore.h` / `StdFileStore.cpp` |

### 3.3 错误处理

**接口边界不用异常。** 可能失败的操作返回 `Status`（或其它显式结果类型）；
实现内部可以抛（例如 `std::filesystem` 抛 `filesystem_error`），但必须在
实现内就地捕获并转成 `Status`。

```cpp
// 好
Status st = fs.remove(path);
if (!st) {
    log.warn("删除失败 {}: {}", path, st.message);
    return st;
}

// 坏：异常穿过接口，调用方无从判断
fs.remove(path);   // throws
```

为什么：传输引擎在 IO 线程上跑，异常一路飘到线程入口就只剩
`std::terminate`，出错现场的信息全丢了。

### 3.4 路径一律 UTF-8

接口上的路径参数与环境变量保持一致：`std::string`，内容是 UTF-8。
Windows 上的宽字符转换由 `filestore` 的实现负责，**不要**在调用方各自
`MultiByteToWideChar`。各处转换方式不一时，症状是"文件名看起来一样但打不开"，
定位成本极高——转换只允许出现在 `filestore` 的实现里。

### 3.5 注释写"为什么"，不写"做了什么"

```cpp
// 坏：读完代码就知道
// 把 size 写进哈希
h.update(lenLe, sizeof(lenLe));

// 好：解释取舍，避免后人"优化"掉
// 长度必须参与哈希：两个前缀相同但长度不同的文件不应被判为同一份。
h.update(lenLe, sizeof(lenLe));
```

公共头文件的每个非平凡方法都要有一段注释说明契约（失败语义、线程安全、
副作用）。私有实现允许少写，但凡是绕开的坑要标注，否则会被后人改回去。

### 3.6 不要引入全局状态

没有单例、没有全局可变变量。需要的对象由上层创建并通过构造参数注入。
这条规则的直接受益者是测试：能注入就能替换成 `MemFileStore` 或裸 `std::FILE*`。

---

## 4. 提交信息

格式：`<类型>: <一句话>`，可选一行正文。

类型取值：`feat` / `fix` / `refactor` / `test` / `docs` / `build` / `chore`。

```
feat: 支持断点续传时校验残片归属

quickFingerprint 现在把源文件总长与已存在长度都混入哈希，
残片补全后指纹才能与源文件一致。
```

要求：

- 一行不超过 72 列
- 说明**为什么**改，而不是重复 diff
- 一个提交做一件事。格式化、重命名、改逻辑混在一起时请拆开

---

## 5. 新增依赖的流程

1. 把源码放进 `third_party/<名字>/`（只保留构建所需的文件）
2. 在 `cmake/Dependencies.cmake` 加一个 INTERFACE 或 STATIC 目标
3. 更新 `third_party/README.md` 的版本表：名字、版本、许可证、来源 URL
4. 如果它需要 `#include <Q...>`，说明它不该出现在 `core/` 或 `filestore/`

**优先选择**：header-only、无外部依赖、许可证为 MIT/BSD/Apache-2.0。
需要自己维护补丁的库要慎重——补丁会随上游升级不断失配。

---

## 6. 目录速查

```
core/          通信库，零 Qt。协议、加密、认证、传输、发现
filestore/     文件子系统，零 Qt。IFileStore 及实现、归档格式
app/           Qt GUI，唯一允许用 Qt 的模块
server/        无界面服务端（relayerd）
cli/           命令行客户端
tests/         单元测试 + 架构检查
tools/         开发脚本（架构检查等）
docs/          设计文档
third_party/   三方库源码
```
