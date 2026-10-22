# core：通信库（零 Qt）
#
# 零 Qt 是硬约束：服务端 relayerd 不链接 Qt，所以 core 里一行 Qt 都不能有。
# 这条规则由 tools/check_arch.py 的 R1 强制。
#
# 目录约定：
#   include/relayer/core/protocol/   帧协议：Frame 结构 + Pack/UnPack
#   include/relayer/core/crypto/     （后续）认证用到的原语封装
#   include/relayer/core/auth/       （后续）握手状态机
#   include/relayer/core/transport/  （后续）asio 连接
#   include/relayer/core/transfer/   （后续）传输引擎
#
# 依赖：只能依赖 common（Status / crypto 原语），以及 filestore 的 IFileStore.h。

add_library(relayer_core STATIC
    src/protocol/Frame.cpp
    src/auth/Handshake.cpp
    src/auth/HandshakeClient.cpp
    src/auth/HandshakeServer.cpp
    src/auth/Random.cpp
)

target_include_directories(relayer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# relayer_filestore 是 PUBLIC：core 的后续模块会用到 IFileStore 接口，
# 而它由 filestore 提供。relayer_common 由 filestore 传递过来（PUBLIC）。
#
# relayer_platform 是 PRIVATE 但必须显式列出：Random.cpp 用到 BCryptGenRandom
# （Windows）和 getrandom（Linux），这些系统库通过它链接。
target_link_libraries(relayer_core PUBLIC relayer_filestore PRIVATE relayer_platform)

relayer_set_warnings(relayer_core)
