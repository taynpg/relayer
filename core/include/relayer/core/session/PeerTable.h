#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/common/Status.h"

// 在线对端表（PROTOCOL.md §11.5）。
//
// 服务端用它把"目标 id"翻译成"哪条连接"。本文件只有映射逻辑，不认识
// Session、不认识 asio——注册与投递都由上层做。这样切是因为这张表的
// 正确性全在"边界情况"上（重名、注销顺序、并发迭代、容量上限），
// 而那些情况用纯数据就能测，不需要起端口。
//
// 为什么不用 `std::unordered_map`：连接数在几十到几千量级，`std::map`
// 的 O(log n) 完全够用，而它有一个 unordered_map 没有的性质——
// **迭代顺序稳定**（按 id 字典序）。客户端列表要发给 UI，顺序乱跳会
// 让"谁上线了"看起来像在闪烁。为此多付一点查找开销是值得的。

namespace relayer::core::session {

// 一条在线连接的公开信息。刻意只有 id 与地址两样：
//
// - 不放 Session 指针：这张表要能被 UI 与日志安全读取，暴露裸指针会
//   诱使调用方绕过 Session 的相位检查直接发帧。
// - 不放连接建立时间之类的诊断字段：需要的话按 id 去查会话，而不是把
//   会话状态复制一份到这里——复制品会与真身不同步。
struct PeerInfo {
    std::string id;
    std::string address;   // "ip:port"，仅用于展示与日志
};

// 注册结果。
//
// 单独一个枚举而不是 bool，是因为"重名"与"表满"的处置完全不同：
// 前者说明 id 生成逻辑有 bug（或有人在试探），后者是容量问题。
// 合成一个 false 会让调用方只能笼统地拒绝连接。
enum class RegisterResult {
    kOk,
    kDuplicateId,   // 该 id 已被占用
    kFull,          // 达到容量上限
};

[[nodiscard]] std::string_view toString(RegisterResult r) noexcept;

class PeerTable
{
public:
    // 容量上限。
    //
    // 有上限的理由与帧缓冲上限一样：id 由客户端请求、服务端分配，如果
    // 不做限制，一个循环重连的 bug 客户端就能把表撑到吃光内存。1024 条
    // 对个人文件传输场景远超需求，同时小到不会成为内存增长点。
    static constexpr std::size_t kDefaultCapacity = 1024;

    explicit PeerTable(std::size_t capacity = kDefaultCapacity) noexcept;

    // 注册一条在线连接。
    //
    // id 为空一律拒绝（返回 kDuplicateId 不合适，但也不该新增一个枚举
    // 只为表达"你没填"——空 id 在语义上就是"这个位置已经被占用了但没有
    // 名字"，调用方应当先分配 id 再注册）。
    //
    // 重名**不会**顶掉原来那条。理由：顶掉等于允许后来者把先到者踢下线，
    // 而"谁是真的"这个判断不该由 id 冲突来隐式决定。让调用方决定要不要
    // 先显式注销旧连接。
    [[nodiscard]] RegisterResult add(const PeerInfo& info);

    // 注销。返回 false 表示该 id 本来就不在表里（重复注销、或从未注册）。
    //
    // 幂等性刻意不在这里提供：调用方需要知道"这次注销是不是真的发生了"，
    // 因为那关系到要不要通知其他客户端"有人下线了"。把幂等做成静默成功
    // 会让重复通知变成常态。
    bool remove(std::string_view id);

    // 查找。找不到返回 nullptr。
    //
    // 返回的指针在下一次 add/remove 之前有效。调用方不得跨异步边界保存它
    // ——一次 await 之后表可能已经变了。
    [[nodiscard]] const PeerInfo* find(std::string_view id) const;

    [[nodiscard]] bool contains(std::string_view id) const;

    // 列出全部在线对端，按 id 字典序（std::map 的天然顺序）。
    //
    // 返回副本而不是迭代器：调用方拿到列表后往往要在遍历中做异步操作
    // （逐个发通知），而迭代器在表变化时会失效。
    [[nodiscard]] std::vector<PeerInfo> list() const;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return peers_.size();
    }
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return peers_.empty();
    }

    void clear() noexcept;

private:
    std::size_t capacity_;
    std::map<std::string, PeerInfo, std::less<>> peers_;
};

// ---------------------------------------------------------------------------
// id 分配
// ---------------------------------------------------------------------------

// 分配一个新 id。
//
// 规则：`client-<n>`，n 从 1 开始递增，跳过表中已存在的。
//
// 为什么用"递增 + 跳过已占用"而不是随机 id：
//   - 递增的 id 在日志里天然可排序，排查时能看出先后
//   - 但**必须跳过已占用**：单调递增的计数器在一个长跑进程里会绕回
//     （虽然要 2^64 次），而"跳过已占用"让它在任何取值空间里都成立
//   - 不要用"取 size()+1"：客户端 A 下线后 size 变小，新客户端会拿到
//     一个刚被释放的 id，此时旧客户端如果正在重连就会撞上
//
// nextHint 是调用方持有的计数器（传引用，本函数会更新它）。放在外面是
// 因为表本身不该有"下一个号"这种状态——表被清空后号段应当继续，而不是
// 从头再来（从头再来会让日志里的 id 重名）。
[[nodiscard]] std::string allocateId(const PeerTable& table, std::uint64_t& nextHint);

// 校验 id 是否可用作路由字段。
//
// 路由字段是 `u8 长度前缀`（上限 255）且**不允许空**。这条校验存在的
// 理由是：分配 id 的地方与使用 id 的地方相距很远，中间任何一处放宽都会
// 在序列化时才发现，那时已经晚了（帧打包失败，连接被迫断开）。
[[nodiscard]] bool isValidPeerId(std::string_view id) noexcept;

// ---------------------------------------------------------------------------
// 载荷编解码（PROTOCOL.md §9.4 / §9.5）
// ---------------------------------------------------------------------------

// `kAskId` 的 body 为空。
[[nodiscard]] std::vector<char> encodeAskId();

// `kAnswerId` 的 status 字节（PROTOCOL.md §11.6 / §11.7）。
//
// 0 与 1 是既有语义（1 = 服务端没能分配）。2/3 是 kClaimId 的两种拒绝：
// 标识被占用、标识非法。**只往后加**，既有值不能改——它们已经进了
// 线上流量与测试向量。
enum class IdAnswerStatus : std::uint8_t {
    kOk = 0,
    kNotAssigned = 1,   // 服务端没能分配任何 id
    kTaken = 2,         // kClaimId：该标识已被别人占用
    kInvalid = 3,       // kClaimId：标识不合法（空 / 超长 / 含 NUL）
};

// `kAnswerId`：`u8 status || u8 idLen || id[idLen]`
//
// status != 0 时不带 id。用 u8 长度而不是通用约定的 u16：这个字段要
// 塞进路由字段同级的短串，而 §9.1 已经声明路由字段用 u8。保持一致，
// 避免"同一个 id 有两套长度编码"。
[[nodiscard]] Status encodeAnswerId(std::string_view assignedId, std::vector<char>& out);

// 带显式 status 的版本。kOk 且 id 为空时仍按既有约定发 status=1
// （"没分配"），而不是"成功但身份空白"。
[[nodiscard]] Status encodeAnswerId(IdAnswerStatus status, std::string_view assignedId, std::vector<char>& out);

// 解 `kAnswerId`。失败时 err 说明原因（截断 / status 未知 / 长度不符）。
[[nodiscard]] bool decodeAnswerId(const std::vector<char>& body, std::uint8_t& status, std::string& assignedId, std::string& err);

// `kClaimId`：`u8 idLen || id[idLen]`（PROTOCOL.md §11.7）。
//
// 校验放在编码侧：非法 id 不该被发出去，服务端无论如何还会再校验一次
// ——两端的校验是"早失败"与"不信对端"的分工，不是重复。
[[nodiscard]] Status encodeClaimId(std::string_view id, std::vector<char>& out);

// 解 `kClaimId`。不校验语义合法性（那是服务端策略），只保证结构正确。
[[nodiscard]] bool decodeClaimId(const std::vector<char>& body, std::string& id, std::string& err);

// `kAnswerClientList`：`u32 count || (u8 idLen || id || u16 addrLen || addr)*`
[[nodiscard]] Status encodeClientList(const std::vector<PeerInfo>& peers, std::vector<char>& out);

[[nodiscard]] bool decodeClientList(const std::vector<char>& body, std::vector<PeerInfo>& peers, std::string& err);

}   // namespace relayer::core::session
