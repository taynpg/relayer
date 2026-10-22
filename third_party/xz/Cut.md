# xz (liblzma) 裁剪记录

## 版本信息

- **上游项目**: XZ Utils (https://tukaani.org/xz / https://github.com/tukaani-project/xz)
- **版本号**: 5.8.4 (stable)
- **引入方式**: `git clone --depth 1 --branch v5.8.4` 后裁剪
- **许可证**: 0BSD (Public Domain) — 见 `COPYING` / `COPYING.0BSD`
- **裁剪日期**: 2026-09-24

## 裁剪目的

relayFile 仅需要 liblzma 的压缩/解压能力（tar.xz 容器），不需要 xz 命令行工具、文档、测试、翻译等。
为了让仓库保持「仅依赖 QtBase 即可构建」的特性，将 xz 源码裁剪到只剩构建 liblzma 静态库所需的最小集合。

## 构建方式

在顶层 `CMakeLists.txt` 中通过 `add_subdirectory(Third/xz)` 引入，并强制关闭所有工具/测试/文档/NLS：

```cmake
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(XZ_DOXYGEN OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
```

然后 `target_link_libraries(relayFileCore PUBLIC liblzma)`，并将
`Third/xz/src/liblzma/api` 加入 include 路径。

## 保留的目录/文件

```
Third/xz/
├── CMakeLists.txt          # 上游 CMake 构建脚本（有 2 处裁剪修改，见下）
├── COPYING                 # 许可证
├── COPYING.0BSD            # 许可证
├── cmake/                  # tuklib CMake 辅助模块（构建 liblzma 必需）
│   ├── remove-ordinals.cmake
│   ├── tuklib_common.cmake
│   ├── tuklib_cpucores.cmake
│   ├── tuklib_integer.cmake
│   ├── tuklib_large_file_support.cmake
│   ├── tuklib_mbstr.cmake       # 仅工具用，include 但未调用
│   ├── tuklib_physmem.cmake
│   └── tuklib_progname.cmake    # 仅工具用，include 但未调用
├── src/common/             # liblzma 依赖的公共头/源
│   ├── mythread.h
│   ├── sysdefs.h
│   ├── tuklib_common.h
│   ├── tuklib_config.h
│   ├── tuklib_cpucores.c / .h
│   ├── tuklib_integer.h
│   └── tuklib_physmem.c / .h
└── src/liblzma/            # liblzma 完整源码（所有编码器/解码器/校验/滤波器）
    ├── api/lzma*.h         # 对外 API 头
    ├── check/              # CRC32/CRC64/SHA256（含 arm64/loongarch/x86 各架构实现）
    ├── common/             # stream/block/index/filter 编解码
    ├── delta/              # delta 滤波器
    ├── lz/                 # LZ 匹配查找器
    ├── lzma/               # LZMA1/LZMA2 编解码
    ├── rangecoder/         # 区间编码器
    └── simple/             # BCJ 滤波器（arm/arm64/armthumb/powerpc/ia64/sparc/riscv/x86）
```

### 跨平台架构保留

为保证在 x86-64 / ARM64（树莓派、M1 Mac）/ LoongArch 等平台上都能编译，**保留了所有架构相关的 CRC 实现和 BCJ 滤波器**：

- `check/crc32_arm64.h`、`crc32_loongarch.h` — ARM/LoongArch 专用 CRC（条件编译）
- `check/crc_x86_clmul.h` — x86 CLMUL CRC（运行时检测）
- `check/crc32_x86.S`、`crc64_x86.S` — 32 位 x86 汇编（仅 `XZ_ASM_I386=ON` 时启用）
- `simple/` 下全部 BCJ 滤波器源文件

## 删除的内容

### 整个目录
- `.git/` — 版本历史
- `.github/` — CI 工作流
- `build-aux/` — Autotools 辅助脚本
- `debug/` — 调试工具
- `doc/` — 文档、示例、格式说明
- `dos/` — DOS 构建
- `doxygen/` — Doxygen 配置
- `extra/` — 7z2lzma、scanlzma 等附加工具
- `lib/` — Gnulib getopt 替换（仅命令行工具用）
- `m4/` — Autotools 宏
- `po/`、`po4a/` — 翻译文件
- `tests/` — 测试用例及测试数据
- `windows/` — Windows 构建说明/脚本
- `src/lzmainfo/` — lzmainfo 工具
- `src/scripts/` — xzdiff/xzgrep 等脚本
- `src/xz/` — xz 命令行工具
- `src/xzdec/` — xzdec/lzmadec 工具

### 顶层文件
- `.codespellrc`、`.gitattributes`、`.gitignore`
- `AUTHORS`、`ChangeLog`、`NEWS`、`README`、`THANKS`、`TODO`、`PACKAGERS`
- `INSTALL`、`INSTALL.generic`
- `autogen.sh`、`configure.ac`、`Makefile.am`
- `COPYING.GPLv2`、`COPYING.GPLv3`、`COPYING.LGPLv2.1`（仅工具/脚本涉及，liblzma 为 0BSD）

### liblzma 内部死代码
- 表生成器（独立程序，不编译进库）：
  `check/crc32_tablegen.c`、`check/crc64_tablegen.c`、`check/crc_clmul_consts_gen.c`、
  `lzma/fastpos_tablegen.c`、`rangecoder/price_tablegen.c`
- ELF 符号版本脚本（仅共享库 + Linux 用）：
  `liblzma_generic.map`、`liblzma_linux.map`、`validate_map.sh`
- Windows DLL 资源（仅共享库用）：`liblzma_w32res.rc`
- pkg-config 模板（仅安装时用）：`liblzma.pc.in`
- Autotools 构建文件：各子目录下的 `Makefile.inc`、`Makefile.am`

### src/common 中工具专用文件
`common_w32res.rc`、`tuklib_exit.*`、`tuklib_gettext.h`、`tuklib_mbstr*`、
`tuklib_open_stdxxx.*`、`tuklib_progname.*`、`w32_application.manifest*`、`my_landlock.h`

## 对 CMakeLists.txt 的修改

裁剪后对上游 `CMakeLists.txt` 做了两处最小修改，均不影响 liblzma 本身的构建：

1. **libgnu (getopt) 段加守卫**（约第 1800 行）：
   ```cmake
   if(XZ_TOOL_XZ OR XZ_TOOL_XZDEC OR XZ_TOOL_LZMADEC OR XZ_TOOL_LZMAINFO)
       ... 原有 libgnu/getopt 逻辑 ...
   endif()
   ```
   原因：getopt 仅命令行工具需要。删除 `lib/` 目录后，若不加守卫，
   在没有系统 getopt_long 的平台（如 MSVC）上 `configure_file(lib/getopt.in.h)` 会失败。

2. **liblzma.pc.in 的 configure_file 加存在性判断**（约第 1690 行）：
   ```cmake
   if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/liblzma/liblzma.pc.in")
       configure_file(src/liblzma/liblzma.pc.in liblzma.pc @ONLY)
   endif()
   ```
   原因：vendored 构建不安装、不需要 pkg-config 文件，删除了 `liblzma.pc.in`。

## 升级/排障指南

### 升级 xz 版本
1. 重新克隆对应版本到临时目录：
   `git clone --depth 1 --branch vX.Y.Z https://github.com/tukaani-project/xz.git xz_new`
2. 用本文件「删除的内容」清单作为参考，从 `xz_new` 中删掉对应目录/文件
3. 保留 `src/liblzma/` 全部内容、`src/common/` 中 liblzma 用到的部分、`cmake/` 全部
4. 对比新旧 `CMakeLists.txt`，重新应用上述两处修改（或检查是否仍需要）
5. 替换 `Third/xz` 目录并构建验证

### 跨平台编译失败排查
- **找不到 getopt.h / getopt_long**：确认顶层 CMake 已将所有 `XZ_TOOL_*` 设为 OFF，
  且 libgnu 段的守卫条件正确。
- **架构相关 CRC 编译错误**：检查 `check/` 下对应架构头文件是否存在
  （`crc32_arm64.h`、`crc32_loongarch.h` 等），勿误删。
- **BCJ 滤波器链接错误**：确认 `simple/` 下对应架构 `.c` 文件保留。
- **`liblzma.pc.in` 缺失**：确认 CMakeLists 中的 `configure_file` 已加 `if(EXISTS ...)` 守卫。
- **符号版本脚本缺失**：静态构建不需要 `.map` 文件；若改为共享库构建，需从上游恢复。
