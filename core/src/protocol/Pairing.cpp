#include "relayer/core/protocol/Pairing.h"

namespace relayer::protocol {

std::optional<FrameType> answerForAsk(FrameType ask) noexcept
{
    switch (ask) {
    // ---- 握手 / 身份 ----
    case FrameType::kAskId:
    case FrameType::kClaimId:   // 声明身份复用同一个应答类型（PROTOCOL.md §11.7）
        return FrameType::kAnswerId;

    // ---- 目录与元信息 ----
    case FrameType::kAskHome:
        return FrameType::kAnswerHome;
    case FrameType::kAskHomeAndDriver:
        return FrameType::kAnswerHomeAndDriver;
    case FrameType::kAskFileList:
        return FrameType::kAnswerFileList;
    case FrameType::kAskFileMeta:
        return FrameType::kAnswerFileMeta;
    case FrameType::kAskPathMeta:
        return FrameType::kAnswerPathMeta;

    // ---- 文件操作 ----
    case FrameType::kAskDelete:
        return FrameType::kAnswerDelete;
    case FrameType::kAskSha256:
        return FrameType::kAnswerSha256;
    case FrameType::kAskRename:
        return FrameType::kAnswerRename;
    case FrameType::kAskCreateDir:
        return FrameType::kAnswerCreateDir;
    case FrameType::kAskArchive:
        return FrameType::kAnswerArchive;
    case FrameType::kAskUnarchive:
        return FrameType::kAnswerUnarchive;
    case FrameType::kAskFileSamples:
        return FrameType::kAnswerFileSamples;

    // ---- 取回与打包 ----
    case FrameType::kAskFilePull:
    case FrameType::kAskArchivePull:   // 结论同类，共用应答
        return FrameType::kAnswerFilePull;
    case FrameType::kAskArchivePack:
        return FrameType::kAnswerArchivePack;
    case FrameType::kAskArchiveReady:
        return FrameType::kAnswerArchiveReady;

    // ---- 在线表 ----
    case FrameType::kAskClientList:
        return FrameType::kAnswerClientList;
    // `kNotifyClientList` / `kAnswerNotifyClientList` 刻意不在这张表里：
    // 它是服务端单向推的通知，客户端"应答"它没有意义。放进请求表会让
    // 上层被要求处理一个不存在的问答，所以它走通知路径（见 ControlLink）。

    // ---- 心跳 ----
    case FrameType::kAskHeart:
        return FrameType::kAnswerHeart;

    // ---- 数据通道协商（§11.13） ----
    case FrameType::kAskDataChannel:
        return FrameType::kAnswerDataChannel;

    // ---- 直连（P2P，保留未实现） ----
    case FrameType::kDirectRequestId:
        return FrameType::kDirectAnswerId;

    // 除上表以外的都不是请求：认证帧有自己的状态机、数据帧不算问答、
    // 传输会话管理帧（kFileRequestSend 等）的"应答"由 TransferPeer 按
    // sessionId 认领，不参与通用的请求表。
    default:
        return std::nullopt;
    }
}

bool isAskType(FrameType t) noexcept
{
    return answerForAsk(t).has_value();
}

bool isAnswerType(FrameType t) noexcept
{
    switch (t) {
    case FrameType::kAnswerId:
    case FrameType::kAnswerHome:
    case FrameType::kAnswerHomeAndDriver:
    case FrameType::kAnswerFileList:
    case FrameType::kAnswerFileMeta:
    case FrameType::kAnswerPathMeta:
    case FrameType::kAnswerDelete:
    case FrameType::kAnswerSha256:
    case FrameType::kAnswerRename:
    case FrameType::kAnswerCreateDir:
    case FrameType::kAnswerArchive:
    case FrameType::kAnswerUnarchive:
    case FrameType::kAnswerFileSamples:
    case FrameType::kAnswerFilePull:
    case FrameType::kAnswerArchivePack:
    case FrameType::kAnswerArchiveReady:
    case FrameType::kAnswerClientList:
    case FrameType::kAnswerNotifyClientList:
    case FrameType::kAnswerHeart:
    case FrameType::kAnswerDataChannel:
    case FrameType::kDirectAnswerId:
        return true;
    default:
        return false;
    }
}

}   // namespace relayer::protocol
