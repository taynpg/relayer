#include "relayer/core/session/DataChannel.h"

#include "ByteReader.h"

namespace relayer::core::session {

namespace {
using detail::putU8;
using detail::Reader;

bool acceptableId(std::string_view id) noexcept
{
    return id.size() <= kMaxDataChannelIdBytes;
}

void putId(std::vector<char>& out, std::string_view id)
{
    putU8(out, static_cast<std::uint8_t>(id.size()));
    out.insert(out.end(), id.begin(), id.end());
}

bool takeId(Reader& r, std::string& out)
{
    const auto len = r.u8();
    if (!r.ok()) {
        return false;
    }
    out = r.str(len);
    return r.ok();
}
}   // namespace

std::vector<char> encodeAskDataChannel(std::string_view myDataId)
{
    if (!acceptableId(myDataId)) {
        return {};   // 空体 → 解码侧判失败，调用方看到的是"请求不合法"
    }
    std::vector<char> out;
    putId(out, myDataId);
    return out;
}

bool decodeAskDataChannel(const std::vector<char>& body, std::string& myDataId, std::string& err)
{
    myDataId.clear();
    Reader r(body);
    if (!takeId(r, myDataId)) {
        err = "ask_data_channel truncated";
        return false;
    }
    // 空 id 合法：那是"我还没建好"的明确表达，不是结构错误。
    return true;
}

Status encodeAnswerDataChannel(DataChannelStatus status, std::string_view peerDataId, std::vector<char>& out)
{
    out.clear();
    // 成功必须带 id：没有 id 的"成功"会让请求方拿着空串去发帧，
    // 中继找不到目标后静默丢弃，症状是"发送方等满超时"。
    const std::string_view id = (status == DataChannelStatus::kOk) ? peerDataId : std::string_view{};
    if (status == DataChannelStatus::kOk && id.empty()) {
        return Status::Fail(ErrorCode::kProtocol, "data channel id is empty on success");
    }
    if (!acceptableId(id)) {
        return Status::Fail(ErrorCode::kProtocol, "data channel id too long");
    }
    putU8(out, static_cast<std::uint8_t>(status));
    putId(out, id);
    return Status::Ok();
}

bool decodeAnswerDataChannel(const std::vector<char>& body, DataChannelStatus& status, std::string& peerDataId, std::string& err)
{
    peerDataId.clear();
    Reader r(body);
    status = static_cast<DataChannelStatus>(r.u8());
    if (!r.ok()) {
        err = "answer_data_channel missing status";
        return false;
    }
    if (!takeId(r, peerDataId)) {
        err = "answer_data_channel truncated inside id";
        return false;
    }
    if (status == DataChannelStatus::kOk && peerDataId.empty()) {
        // 与编码侧对称：成功却没有 id 是坏包，不能当"对方还没准备好"放过。
        err = "answer_data_channel ok but id is empty";
        return false;
    }
    return true;
}

const char* dataChannelStatusText(DataChannelStatus status) noexcept
{
    switch (status) {
    case DataChannelStatus::kOk:
        return "成功";
    case DataChannelStatus::kNotReady:
        return "对端暂时建不起数据连接";
    case DataChannelStatus::kRejected:
        return "对端拒绝数据通道";
    }
    return "未知状态";
}

}   // namespace relayer::core::session
