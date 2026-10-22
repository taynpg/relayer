# common：最底层的共用件
#
# 这里的代码**不允许**依赖 filestore / core / app / server 中的任何一个。
# 依赖方向只能是 filestore → common、core → common。
# 这条规则由 tools/check_arch.py 的 R6 强制。
#
# 放什么：
#   Status / ErrorCode     错误类型，跨模块共用同一套码
#   crypto/                SHA-256、HMAC、PBKDF2、AES-256-GCM
#
# 不放什么：
#   任何与业务语义有关的东西。这里只应该有"纯函数 + 纯数据结构"，
#   不出现文件、网络、线程、日志。这样它才能被任何层安全地依赖。

add_library(relayer_common STATIC
    src/Status.cpp
    src/crypto/Sha256.cpp
    src/crypto/Aes256Gcm.cpp
)

target_include_directories(relayer_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

relayer_set_warnings(relayer_common)
