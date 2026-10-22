# relayerd：无界面服务端。
#
# **不链接 Qt，也不链接 filestore**（REWRITE-PLAN 2.3）。前者是为了能在
# 没有图形环境的机器上跑；后者是结构性的：服务端看不见文件系统，就没法
# 把用户数据落盘——"只做转发"这条承诺靠类型系统保证，而不是靠纪律。
#
# relayer_core 的 PUBLIC 依赖里有 relayer_filestore，所以这里必须显式
# 说明"我确实要用它"，见下面的 INTERFACE 讨论。

add_executable(relayerd
    src/Server.cpp
    src/main.cpp
)

target_include_directories(relayerd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# relayer_asio 要显式链接：Server.h 暴露了 asio::io_context 与 acceptor，
# 那是本模块的公开接口，不是实现细节。
target_link_libraries(relayerd PRIVATE relayer_core relayer_asio relayer_platform)

relayer_set_warnings(relayerd)
