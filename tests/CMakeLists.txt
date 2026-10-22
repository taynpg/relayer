# 测试。
#
# 命名规则：<模块>_test，一个可执行文件对应一个模块的单元测试。
# 目录按模块分，跟源码树一一对应：common/ filestore/ core/ ...
# 集成测试（需要起进程/端口）单独放 integration/，标签设为 [integration]，
# 默认不在 CI 的快速通道里跑。

add_executable(common_test
    common/Sha256Test.cpp
    common/CryptoTest.cpp
)
target_link_libraries(common_test PRIVATE relayer_common Catch2::Catch2WithMain)
target_include_directories(common_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
relayer_set_warnings(common_test)
add_test(NAME common COMMAND common_test)

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

add_executable(core_test
    core/FrameTest.cpp
    core/AuthTest.cpp
    core/StateMachineTest.cpp
    core/MerkleTreeTest.cpp
    core/TransferPayloadTest.cpp
    core/WorkerPoolTest.cpp
    core/SendWindowTest.cpp
    core/TransferSenderTest.cpp
    core/TransferReceiverTest.cpp
    core/TransferPeerTest.cpp
    core/FrameReaderTest.cpp
    core/ConnectionTest.cpp
    core/HeartbeatTest.cpp
    core/SessionPolicyTest.cpp
    core/SessionTest.cpp
    core/PeerTableTest.cpp
)
# relayer_asio 是 relayer_core 的 PRIVATE 依赖（本模块内部用），所以测试
# 需要自己显式链接——ConnectionTest 直接 include <asio/...> 并用 io_context。
# relayer_platform 提供 ws2_32 / mswsock 等 socket 库。
target_link_libraries(core_test PRIVATE relayer_core relayer_asio relayer_platform Catch2::Catch2WithMain)
target_include_directories(core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
relayer_set_warnings(core_test)
add_test(NAME core COMMAND core_test)

# 集成测试（需要真实 socket、耗时较长）单独登记，可以用 -L integration 选跑。
set_tests_properties(core PROPERTIES LABELS "unit")

# 文档与代码的一致性检查。
#
# 这类"文档声称是权威、代码是另一份真相"的东西，靠人盯必然漂移。
# 把它变成 CTest 里的断言，忘了同步就会红。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    # 架构约束：把 docs/REWRITE-PLAN.md 2.2 节的依赖方向变成可执行断言。
    # 放在 CTest 里是因为"人忘了跑脚本"比"人忘了写规则"更常见。
    add_test(NAME arch
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_arch.py)
    set_tests_properties(arch PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                         LABELS "arch")

    # 协议文档：docs/PROTOCOL.md 的帧类型表必须与 Frame.h 的枚举逐项对应。
    add_test(NAME protocol_doc
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_protocol_doc.py)
    set_tests_properties(protocol_doc PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                         LABELS "docs")

    # 传输状态机：PROTOCOL.md §8 的状态表/转移表必须与 StateMachine.h 一致。
    add_test(NAME transition_doc
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_transition_doc.py)
    set_tests_properties(transition_doc PROPERTIES
                         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                         LABELS "docs")
else()
    message(WARNING "未找到 Python3，跳过架构与协议文档检查")
endif()
