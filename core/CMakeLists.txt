# core：通信库（零 Qt）
#
# 零 Qt 是硬约束：服务端 relayerd 不链接 Qt，所以 core 里一行 Qt 都不能有。
# 这条规则由 tools/check_arch.py 的 R1 强制。
#
# 目录约定：
#   include/relayer/core/protocol/   帧协议：Frame 结构 + Pack/UnPack
#   include/relayer/core/auth/       握手状态机（纯逻辑）
#   include/relayer/core/transport/  asio 连接 + 帧切分 + 心跳（纯逻辑部分）
#   include/relayer/core/session/    会话策略：转发判定、身份绑定、路由（纯逻辑）
#   include/relayer/core/transfer/   传输状态机 + Merkle 树 + 传输引擎
#
# 依赖：只能依赖 common（Status / crypto 原语），以及 filestore 的 IFileStore.h。

add_library(relayer_core STATIC
    src/protocol/Frame.cpp
    src/protocol/Pairing.cpp
    src/auth/Handshake.cpp
    src/auth/HandshakeClient.cpp
    src/auth/HandshakeServer.cpp
    src/auth/Random.cpp
    src/transfer/StateMachine.cpp
    src/transfer/MerkleTree.cpp
    src/transfer/TransferPayload.cpp
    src/transfer/WorkerPool.cpp
    src/transfer/SendWindow.cpp
    src/transfer/TransferSender.cpp
    src/transfer/TransferReceiver.cpp
    src/transfer/TransferPeer.cpp
    src/transport/FrameReader.cpp
    src/transport/Connection.cpp
    src/transport/Heartbeat.cpp
    src/session/SessionPolicy.cpp
    src/session/Session.cpp
    src/session/PeerTable.cpp
    src/net/ControlLink.cpp
    src/session/DataChannel.cpp
    src/session/FileList.cpp
    src/session/FileOp.cpp
    src/session/PathMeta.cpp
    src/session/PathText.cpp
)

target_include_directories(relayer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# relayer_filestore 是 PUBLIC：core 的后续模块会用到 IFileStore 接口，
# 而它由 filestore 提供。relayer_common 由 filestore 传递过来（PUBLIC）。
#
# relayer_platform 是 PRIVATE 但必须显式列出：Random.cpp 用到 BCryptGenRandom
# （Windows）和 getrandom（Linux），这些系统库通过它链接。
#
# relayer_asio 也是 PRIVATE：Connection.h 会 include <asio/...>，但那是本模块
# 的实现细节。上层（server/cli/app）若需要 asio 类型（比如自己持有 io_context），
# 应当显式链接 relayer_asio，而不是依赖传递——传递来的 include 路径会掩盖
# "这个模块其实没打算暴露 asio"这个事实。
target_link_libraries(relayer_core
    PUBLIC relayer_filestore
    PRIVATE relayer_platform relayer_asio)

relayer_set_warnings(relayer_core)
