# 测试。
#
# 命名规则：<模块>_test，一个可执行文件对应一个模块的单元测试。
# 集成测试（需要起进程/端口）单独放 integration/，标签设为 [integration]，
# 默认不在 CI 的快速通道里跑。

add_executable(filestore_test
    filestore/StdFileStoreTest.cpp
    filestore/PathPolicyTest.cpp
    filestore/MemFileStoreTest.cpp
    filestore/XzCodecTest.cpp
    filestore/TarCodecTest.cpp
    filestore/ArchiveTest.cpp
)
target_link_libraries(filestore_test PRIVATE relayer_filestore Catch2::Catch2WithMain)
target_include_directories(filestore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
relayer_set_warnings(filestore_test)
add_test(NAME filestore COMMAND filestore_test)

add_executable(sha256_test
    filestore/Sha256Test.cpp
)
target_link_libraries(sha256_test PRIVATE relayer_filestore Catch2::Catch2WithMain)
relayer_set_warnings(sha256_test)
add_test(NAME sha256 COMMAND sha256_test)

# 架构约束检查：把 docs/REWRITE-PLAN.md 2.2 节的依赖方向变成可执行断言。
# 放在 CTest 里是因为"人忘了跑脚本"比"人忘了写规则"更常见。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME arch
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_arch.py)
    set_tests_properties(arch PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                         LABELS "arch")
else()
    message(WARNING "未找到 Python3，跳过架构约束检查测试（tools/check_arch.py）")
endif()
