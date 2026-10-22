# filestore：文件子系统
#
# 零 Qt。只依赖标准库 + miniz + liblzma + common（Status 与 crypto 原语）。
# core/ 只允许 include 这里的 IFileStore.h，不允许 include 具体实现
# （这条规则由 tools/check_arch.py 在 CI 上强制）。
#
# 目录约定：
#   include/relayer/filestore/         公开 API：IFileStore.h + 各实现的头
#   include/relayer/filestore/detail/  公开但"不承诺稳定"的编解码工具（tar/xz）
#   src/                               纯实现细节，外部不可见

add_library(relayer_filestore STATIC
    src/PathPolicy.cpp
    src/PlatformPath.cpp
    src/StdStream.cpp
    src/StdFileStore.cpp
    src/MemFileStore.cpp
    src/XzCodec.cpp
    src/TarCodec.cpp
    src/Archive.cpp
    src/TarXzArchiver.cpp
)

target_include_directories(relayer_filestore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# relayer_common 是 PUBLIC：filestore 的公开头文件里 IFileStore.h 会 include
# relayer/common/Status.h，因此链接 filestore 的目标也必须能看到 common 的
# include 目录。
target_link_libraries(relayer_filestore PUBLIC relayer_common PRIVATE relayer_platform liblzma)

relayer_set_warnings(relayer_filestore)
