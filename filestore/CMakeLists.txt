# filestore：文件子系统
#
# 零 Qt。只依赖标准库 + miniz + liblzma。
# core/ 只允许 include 这里的 IFileStore.h，不允许 include 具体实现
# （这条规则由 tools/check_arch.py 在 CI 上强制）。
#
# 目录约定：
#   include/relayer/filestore/         公开 API：IFileStore.h + 各实现的头
#   include/relayer/filestore/detail/  公开但"不承诺稳定"的工具（如 Sha256）
#   src/                               纯实现细节，外部不可见

add_library(relayer_filestore STATIC
    src/Status.cpp
    src/Sha256.cpp
    src/PlatformPath.cpp
    src/StdStream.cpp
    src/StdFileStore.cpp
)

target_include_directories(relayer_filestore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(relayer_filestore PRIVATE relayer_platform)

relayer_set_warnings(relayer_filestore)
