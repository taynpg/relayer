cmake_minimum_required(VERSION 3.20)

project(relayer
    VERSION 2.0.0
    DESCRIPTION "Peer-to-peer file transfer over a relay"
    HOMEPAGE_URL "https://github.com/taynpg/relayer"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# 源码树里不生成任何中间产物
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# 选项：少而明确。新增选项必须同步更新 README 与本文件的注释。
# ---------------------------------------------------------------------------
option(RELAYER_BUILD_GUI    "Build the Qt GUI client (app/)"        ON)
option(RELAYER_BUILD_SERVER "Build the headless server (relayerd)"  ON)
option(RELAYER_BUILD_CLI    "Build the command line client"         ON)
option(RELAYER_BUILD_TESTS  "Build unit and integration tests"      OFF)
option(RELAYER_WERROR       "Treat warnings as errors"              OFF)

# ---------------------------------------------------------------------------
# 子模块
# ---------------------------------------------------------------------------
include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(filestore)
add_subdirectory(core)

if(RELAYER_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(RELAYER_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(RELAYER_BUILD_GUI)
    add_subdirectory(app)
endif()

if(RELAYER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 汇总信息：构建完一眼能看出编了什么、去哪找
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "relayer ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  gui      : ${RELAYER_BUILD_GUI}")
message(STATUS "  server   : ${RELAYER_BUILD_SERVER}")
message(STATUS "  cli      : ${RELAYER_BUILD_CLI}")
message(STATUS "  tests    : ${RELAYER_BUILD_TESTS}")
message(STATUS "  output   : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "")
