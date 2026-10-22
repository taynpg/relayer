# 三方依赖统一入口。
#
# 全部 vendored 在 third_party/ 下（见 third_party/README.md），不联网、不装包。
# 本文件只做三件事：
#   1. 建立 INTERFACE 目标，把 include 路径与编译宏集中收口；
#   2. 关闭 vendored 库自带的无用构建项（命令行工具、文档、测试、NLS）；
#   3. 供各子模块 target_link_libraries 引用。
#
# 新增依赖时：先放 third_party/，再在这里加一段，最后更新 third_party/README.md。

set(RELAYER_THIRD_PARTY_DIR ${PROJECT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# header-only 库：只加 include 路径
# ---------------------------------------------------------------------------
add_library(relayer_asio INTERFACE)
target_include_directories(relayer_asio INTERFACE ${RELAYER_THIRD_PARTY_DIR}/asio)

add_library(relayer_cereal INTERFACE)
target_include_directories(relayer_cereal INTERFACE ${RELAYER_THIRD_PARTY_DIR}/cereal)

add_library(relayer_fmt INTERFACE)
target_include_directories(relayer_fmt INTERFACE ${RELAYER_THIRD_PARTY_DIR}/fmt)
target_compile_definitions(relayer_fmt INTERFACE FMT_HEADER_ONLY)

add_library(relayer_json INTERFACE)
target_include_directories(relayer_json INTERFACE ${RELAYER_THIRD_PARTY_DIR}/nlohmann)

add_library(relayer_miniz INTERFACE)
target_include_directories(relayer_miniz INTERFACE ${RELAYER_THIRD_PARTY_DIR}/miniz)

# spdlog：按 header-only 使用（vendored 版本只含头文件）。
# 与 {fmt} 共用同一份 fmt，避免编译出两份格式化实现（否则会报符号冲突）。
add_library(relayer_spdlog INTERFACE)
target_include_directories(relayer_spdlog INTERFACE ${RELAYER_THIRD_PARTY_DIR}/spdlog)
target_compile_definitions(relayer_spdlog INTERFACE
    SPDLOG_HEADER_ONLY
    SPDLOG_FMT_EXTERNAL
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
)
target_link_libraries(relayer_spdlog INTERFACE relayer_fmt)

# ---------------------------------------------------------------------------
# 有源码的库
# ---------------------------------------------------------------------------
# liblzma：只编库本体，命令行工具/文档/测试/国际化全部关闭
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(XZ_DOXYGEN OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable vendored xz tests" FORCE)
add_subdirectory(${RELAYER_THIRD_PARTY_DIR}/xz ${CMAKE_BINARY_DIR}/third_party/xz EXCLUDE_FROM_ALL)
set_target_properties(liblzma PROPERTIES POSITION_INDEPENDENT_CODE ON)

# miniz：单文件 C 实现
add_library(relayer_miniz_impl STATIC ${RELAYER_THIRD_PARTY_DIR}/miniz/miniz.c)
target_link_libraries(relayer_miniz_impl PUBLIC relayer_miniz)

# ---------------------------------------------------------------------------
# 测试框架
# ---------------------------------------------------------------------------
if(RELAYER_BUILD_TESTS)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RELAYER_THIRD_PARTY_DIR}/catch2 ${CMAKE_BINARY_DIR}/third_party/catch2 EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_MODULE_PATH ${RELAYER_THIRD_PARTY_DIR}/catch2/CMake)
    include(Catch)
endif()

# ---------------------------------------------------------------------------
# 平台系统库
# ---------------------------------------------------------------------------
add_library(relayer_platform INTERFACE)
if(WIN32)
    target_link_libraries(relayer_platform INTERFACE ws2_32 mswsock)
    target_compile_definitions(relayer_platform INTERFACE
        _WIN32_WINNT=0x0A00        # Windows 10+：需要 UTF-8 代码页 API
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(relayer_platform INTERFACE pthread)
endif()
