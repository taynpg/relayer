#include "relayer/common/crypto/Random.h"

#include <cstdint>

#include "relayer/core/auth/Handshake.h"

// salt / nonce 的取法。
//
// 真正的平台 CSPRNG 在 common/crypto/Random.cpp——它不认协议、不认认证，
// 任何层都能用（载荷加密取 GCM nonce 也走那里）。这里只保留"这两样东西
// 各要多少字节"这一层业务语义。

namespace relayer::core::auth {

bool randomSalt(Salt& out) noexcept
{
    return crypto::fillRandom(out.data(), out.size());
}

bool randomNonce(Nonce& out) noexcept
{
    return crypto::fillRandom(out.data(), out.size());
}

}   // namespace relayer::core::auth
