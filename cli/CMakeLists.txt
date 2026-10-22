# relayer-cli：不依赖 Qt 的命令行客户端。
#
# 它的存在意义不只是"给不想开 GUI 的人一个选择"：
#
#   1. 三进程集成测试需要两个**独立进程**分别扮演发送方与接收方，
#      而 app/ 的 Qt 前端启动成本高、还难以在 CI 里驱动；
#   2. "发送端被 kill"这个场景要求发送方是一个能被 SIGKILL 的进程，
#      用 Qt 前端做这件事等于把一个 GUI 拖进测试；
#   3. 它同时是 app/ 的复用实现——两者走同一条 Client → Session →
#      TransferPeer 链路，CLI 能跑通说明这条链路本身是通的。
#
# Client 因此提成独立的库目标（relayer_client）：app/ 的 bridge 直接链接
# 它，而不是把 Client.cpp 再编一遍。main.cpp 才是 CLI 特有的部分
# （参数解析、退出码、进度行格式）。

add_library(relayer_client STATIC
    src/Client.cpp
)

target_include_directories(relayer_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# relayer_core 是 PUBLIC 链到 filestore 的，Client.h 里用到的 StdFileStore
# 也来自那里，所以不必再列。
#
# relayer_asio 必须显式链接：Client.h 暴露了 asio::io_context，那是本模块
# 的公开接口而非实现细节（理由同 server/CMakeLists.txt）。
target_link_libraries(relayer_client PUBLIC relayer_core relayer_asio relayer_platform)

relayer_set_warnings(relayer_client)

add_executable(relayer-cli
    src/main.cpp
)

target_link_libraries(relayer-cli PRIVATE relayer_client)

relayer_set_warnings(relayer-cli)
