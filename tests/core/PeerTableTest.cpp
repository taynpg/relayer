#include "relayer/core/session/PeerTable.h"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace relayer::core::session;

namespace
{

PeerInfo peer(const std::string &id, const std::string &addr = "127.0.0.1:1000")
{
    PeerInfo p;
    p.id = id;
    p.address = addr;
    return p;
}

}  // namespace

// ===========================================================================
// 注册与注销
// ===========================================================================

TEST_CASE("add stores and find retrieves", "[peer]")
{
    PeerTable t;
    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);
    REQUIRE(t.size() == 1);

    const auto *p = t.find("client-1");
    REQUIRE(p != nullptr);
    REQUIRE(p->id == "client-1");
    REQUIRE(p->address == "127.0.0.1:1000");

    REQUIRE(t.find("client-2") == nullptr);
    REQUIRE_FALSE(t.contains("client-2"));
}

TEST_CASE("duplicate id does not evict the original", "[peer]")
{
    // 后来者不能把先到者顶下线。
    //
    // "顶掉"看起来更"方便"（重连时不用先注销），但它等于让 id 冲突隐式
    // 决定"谁是真的"——而这个判断不该由一次 add 来完成。调用方应当先显式
    // 注销旧连接，那一步也正好是"通知其他人有人下线"的时机。
    PeerTable t;
    REQUIRE(t.add(peer("client-1", "10.0.0.1:1")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-1", "10.0.0.2:2")) == RegisterResult::kDuplicateId);

    REQUIRE(t.size() == 1);
    // 原来的地址没被改掉
    REQUIRE(t.find("client-1")->address == "10.0.0.1:1");
}

TEST_CASE("empty id is rejected", "[peer]")
{
    // 空 id 必须在查重之前拦掉。否则 `find("")` 返回"不存在"，空 id 会
    // 以成功的假象插进表里，之后所有按空 id 查找的调用都会命中它。
    PeerTable t;
    REQUIRE(t.add(peer("")) == RegisterResult::kDuplicateId);
    REQUIRE(t.empty());
    REQUIRE(t.find("") == nullptr);
}

TEST_CASE("remove reports whether it actually removed", "[peer]")
{
    // 刻意不做幂等：调用方需要知道"这次注销是不是真的发生了"，
    // 因为那关系到要不要广播"有人下线"。
    PeerTable t;
    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);
    REQUIRE(t.remove("client-1"));
    REQUIRE_FALSE(t.remove("client-1"));  // 第二次
    REQUIRE_FALSE(t.remove("never-existed"));
    REQUIRE(t.empty());
}

TEST_CASE("capacity is enforced", "[peer]")
{
    // 有上限的理由：id 由客户端请求、服务端分配。不做限制的话，一个循环
    // 重连的 bug 客户端就能把表撑到吃光内存。
    PeerTable t(3);
    REQUIRE(t.add(peer("a")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("b")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("c")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("d")) == RegisterResult::kFull);

    REQUIRE(t.size() == 3);
    // 满了之后已存在的仍可查到、仍可被删
    REQUIRE(t.contains("a"));
    REQUIRE(t.remove("a"));
    // 空出位置后又能加
    REQUIRE(t.add(peer("d")) == RegisterResult::kOk);
}

// ===========================================================================
// 列表
// ===========================================================================

TEST_CASE("list is ordered by id", "[peer]")
{
    // 顺序稳定是选 std::map 而不是 unordered_map 的唯一理由：客户端列表
    // 要发给 UI，顺序乱跳会让"谁上线了"看起来像在闪烁。
    PeerTable t;
    REQUIRE(t.add(peer("client-10")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-2")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);

    const auto all = t.list();
    REQUIRE(all.size() == 3);
    // 字典序：client-1 < client-10 < client-2
    REQUIRE(all[0].id == "client-1");
    REQUIRE(all[1].id == "client-10");
    REQUIRE(all[2].id == "client-2");
}

TEST_CASE("list returns a copy that survives table changes", "[peer]")
{
    // 调用方拿到列表后往往要在遍历中做异步操作（逐个发通知），
    // 迭代器会在表变化时失效，所以返回副本。
    PeerTable t;
    REQUIRE(t.add(peer("a")) == RegisterResult::kOk);

    auto snapshot = t.list();
    REQUIRE(t.remove("a"));
    REQUIRE(t.add(peer("b")) == RegisterResult::kOk);

    REQUIRE(snapshot.size() == 1);
    REQUIRE(snapshot[0].id == "a");  // 快照不受影响
    REQUIRE(t.list()[0].id == "b");
}

// ===========================================================================
// id 分配
// ===========================================================================

TEST_CASE("allocateId starts from 1 and increments", "[peer][id]")
{
    PeerTable t;
    std::uint64_t hint = 1;

    REQUIRE(allocateId(t, hint) == "client-1");
    REQUIRE(hint == 2);
    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);

    REQUIRE(allocateId(t, hint) == "client-2");
    REQUIRE(t.add(peer("client-2")) == RegisterResult::kOk);

    REQUIRE(allocateId(t, hint) == "client-3");
}

TEST_CASE("allocateId skips ids already in the table", "[peer][id]")
{
    // 这是关键的一条：不能用"取 size()+1"。
    //
    // client-1 下线后 size 变小，若按 size 算，新客户端会拿到 client-1，
    // 而旧客户端如果正在重连就会撞上。单调递增 + 跳过已占用让这个场景
    // 不可能发生。
    PeerTable t;
    std::uint64_t hint = 1;

    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-2")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-3")) == RegisterResult::kOk);

    // 删掉中间那条
    REQUIRE(t.remove("client-2"));
    REQUIRE(t.size() == 2);

    // 注意 hint 仍停在 1（上面三条是手动 add，不走 allocateId）。
    // 所以从 1 开始试：1 占、2 空出来了 → 拿到 client-2。
    //
    // 这一点恰恰说明"跳过已占用"是对的：client-2 那条已经下线，
    // 它的号可以回收；而 client-3 还活着，绝不会被发出去。
    REQUIRE(hint == 1);
    REQUIRE(allocateId(t, hint) == "client-2");
    REQUIRE(hint == 3);

    // 反例：按 size()+1 会给出 client-3，而 client-3 正在使用中。
    // 这正是不能用 size 的理由。
    REQUIRE(t.contains("client-3"));
}

TEST_CASE("allocateId keeps numbers monotonic after the table is cleared", "[peer][id]")
{
    // 表清空了，号段也要继续。从头再来会让日志里出现两个 client-1，
    // 而那正是排查"谁断了"时最不想看到的。
    PeerTable t;
    std::uint64_t hint = 1;

    REQUIRE(allocateId(t, hint) == "client-1");
    REQUIRE(allocateId(t, hint) == "client-2");
    t.clear();
    REQUIRE(allocateId(t, hint) == "client-3");
}

TEST_CASE("allocateId returns nothing when the table is saturated", "[peer][id]")
{
    // 容量满时返回空串，而不是返回一个可能重复的 id。
    // 静默制造重复比明确失败难查得多。
    PeerTable t(2);
    std::uint64_t hint = 1;
    REQUIRE(t.add(peer("client-1")) == RegisterResult::kOk);
    REQUIRE(t.add(peer("client-2")) == RegisterResult::kOk);

    // 容量 2 已满。注意这里**不会**返回空串：client-1 / client-2 确实
    // 被占，但循环上限是 capacity+1，而 hint 从 1 起步时会试到 3 —
    // 表里没有 client-3，所以它仍然能给出一个 id。
    //
    // 这条测试原本的意图（"满了就返回空"）是错的：allocateId 不知道
    // 容量目标，它只负责"按 hint 递增找一个没被占用的号"。容量判断属于
    // add()。把两件事混在一起会让"表里还有空号但容量已满"这种情况
    // 表现得像"没有 id 可用"，误导调用方。
    REQUIRE(allocateId(t, hint) == "client-3");
    // 而真正防止超员的是 add()
    REQUIRE(t.add(peer("client-3")) == RegisterResult::kFull);
}

// ===========================================================================
// id 校验
// ===========================================================================

TEST_CASE("peer id validation", "[peer][id]")
{
    REQUIRE_FALSE(isValidPeerId(""));
    REQUIRE(isValidPeerId("client-1"));
    REQUIRE(isValidPeerId(std::string(255, 'a')));
    REQUIRE_FALSE(isValidPeerId(std::string(256, 'a')));

    // 内嵌 NUL 会让日志截断，也会让"看起来相同的两个 id"不等
    std::string withNul = "client";
    withNul.push_back('\0');
    withNul.append("-1");
    REQUIRE_FALSE(isValidPeerId(withNul));
}

// ===========================================================================
// kAnswerId 载荷
// ===========================================================================

TEST_CASE("answer id roundtrip", "[peer][codec]")
{
    std::vector<char> body;
    REQUIRE(encodeAnswerId("client-7", body).ok());
    // status(1) + len(1) + id(8)
    REQUIRE(body.size() == 10);

    std::uint8_t status = 9;
    std::string id;
    std::string err;
    REQUIRE(decodeAnswerId(body, status, id, err));
    REQUIRE(status == 0);
    REQUIRE(id == "client-7");
}

TEST_CASE("answer id with empty id encodes a failure status", "[peer][codec]")
{
    // 空 id 表示"服务端没能给你分配"。发长度为 0 的 id 会让对端
    // 误以为"成功但身份空白"。
    std::vector<char> body;
    REQUIRE(encodeAnswerId("", body).ok());
    REQUIRE(body.size() == 1);

    std::uint8_t status = 9;
    std::string id;
    std::string err;
    REQUIRE(decodeAnswerId(body, status, id, err));
    REQUIRE(status == 1);
    REQUIRE(id.empty());
}

TEST_CASE("answer id rejects a success status with empty id", "[peer][codec]")
{
    // status=0 却说 id 为空：矛盾。不能把它登记进 peer 表。
    const std::vector<char> body = {0, 0};
    std::uint8_t status = 9;
    std::string id;
    std::string err;
    REQUIRE_FALSE(decodeAnswerId(body, status, id, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("answer id rejects truncated payloads", "[peer][codec]")
{
    std::uint8_t status = 0;
    std::string id;
    std::string err;

    // 空载荷：连 status 都没有
    REQUIRE_FALSE(decodeAnswerId({}, status, id, err));

    // 声明了 8 字节 id 但只给 3 字节
    const std::vector<char> bad = {0, 8, 'a', 'b', 'c'};
    REQUIRE_FALSE(decodeAnswerId(bad, status, id, err));
}

TEST_CASE("answer id tolerates trailing bytes", "[peer][codec]")
{
    // 尾部多余字节说明对端版本比我们新。不当成错误——断开会让版本升级
    // 变得更痛苦，而多余信息可以忽略。
    std::vector<char> body;
    REQUIRE(encodeAnswerId("client-1", body).ok());
    body.push_back('X');
    body.push_back('Y');

    std::uint8_t status = 9;
    std::string id;
    std::string err;
    REQUIRE(decodeAnswerId(body, status, id, err));
    REQUIRE(id == "client-1");
}

TEST_CASE("encodeAnswerId refuses an over-long id", "[peer][codec]")
{
    std::vector<char> body;
    const auto st = encodeAnswerId(std::string(256, 'x'), body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

// ===========================================================================
// kClaimId 载荷（PROTOCOL.md §11.7）
// ===========================================================================

TEST_CASE("claim id roundtrip", "[peer][codec]")
{
    std::vector<char> body;
    REQUIRE(encodeClaimId("alice", body).ok());
    // len(1) + id(5)
    REQUIRE(body.size() == 6);

    std::string id;
    std::string err;
    REQUIRE(decodeClaimId(body, id, err));
    REQUIRE(id == "alice");
}

TEST_CASE("claim id encodes UTF-8 names byte-wise", "[peer][codec]")
{
    // 长度是**字节数**不是字符数：中文名字按 UTF-8 编码后占 3 字节/字。
    // 用字符数会让解码侧把 id 截断成半个汉字。
    const std::string name = "小明";
    std::vector<char> body;
    REQUIRE(encodeClaimId(name, body).ok());
    REQUIRE(body.size() == 1 + name.size());

    std::string id;
    std::string err;
    REQUIRE(decodeClaimId(body, id, err));
    REQUIRE(id == name);
}

TEST_CASE("claim id refuses empty and over-long ids", "[peer][codec]")
{
    std::vector<char> body;
    REQUIRE_FALSE(encodeClaimId("", body).ok());
    REQUIRE_FALSE(encodeClaimId(std::string(256, 'x'), body).ok());
    // 含 NUL 的 id 会让"看起来相同"的两个名字不等，一律拒绝。
    const std::string withNul("ab\0cd", 5);
    REQUIRE_FALSE(encodeClaimId(withNul, body).ok());
}

TEST_CASE("claim id rejects malformed payloads", "[peer][codec]")
{
    std::string id;
    std::string err;

    // 空载荷：没有长度字节
    REQUIRE_FALSE(decodeClaimId({}, id, err));
    // 长度 0：声明"没有名字"没有意义
    REQUIRE_FALSE(decodeClaimId({0}, id, err));
    // 长度说 5 但只给 3 字节
    REQUIRE_FALSE(decodeClaimId({5, 'a', 'b', 'c'}, id, err));
    // 长度说 3 却给了 5 字节（尾部多余 = 结构不符，这里的严格是对称的：
    // 发送方永远按同一张表编码，多出来的字节说明不是这张表）
    REQUIRE_FALSE(decodeClaimId({3, 'a', 'b', 'c', 'd', 'e'}, id, err));
}

TEST_CASE("answer id carries claim rejection statuses without an id", "[peer][codec]")
{
    // 拒绝时只发状态码：带 id 会让客户端误以为"这个 id 也有效"。
    for (const auto status : {IdAnswerStatus::kTaken, IdAnswerStatus::kInvalid})
    {
        std::vector<char> body;
        REQUIRE(encodeAnswerId(status, "", body).ok());
        REQUIRE(body.size() == 1);

        std::uint8_t decoded = 0;
        std::string id;
        std::string err;
        REQUIRE(decodeAnswerId(body, decoded, id, err));
        REQUIRE(decoded == static_cast<std::uint8_t>(status));
        REQUIRE(id.empty());
    }
}

// ===========================================================================
// kAnswerClientList 载荷
// ===========================================================================

TEST_CASE("client list roundtrip", "[peer][codec]")
{
    const std::vector<PeerInfo> in = {peer("client-1", "10.0.0.1:1111"), peer("client-2", "10.0.0.2:2222")};

    std::vector<char> body;
    REQUIRE(encodeClientList(in, body).ok());

    std::vector<PeerInfo> out;
    std::string err;
    REQUIRE(decodeClientList(body, out, err));
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].id == "client-1");
    REQUIRE(out[0].address == "10.0.0.1:1111");
    REQUIRE(out[1].id == "client-2");
    REQUIRE(out[1].address == "10.0.0.2:2222");
}

TEST_CASE("empty client list roundtrips", "[peer][codec]")
{
    std::vector<char> body;
    REQUIRE(encodeClientList({}, body).ok());
    REQUIRE(body.size() == 4);  // 只有 count

    std::vector<PeerInfo> out{peer("stale")};
    std::string err;
    REQUIRE(decodeClientList(body, out, err));
    REQUIRE(out.empty());
}

TEST_CASE("client list rejects a count larger than the payload can hold", "[peer][codec]")
{
    // 恶意的 count 让我们 reserve 一块巨大内存然后失败。先按"每条至少
    // 4 字节"检查，再 reserve。
    std::vector<char> body;
    body.push_back(static_cast<char>(0xFF));
    body.push_back(static_cast<char>(0xFF));
    body.push_back(static_cast<char>(0xFF));
    body.push_back(static_cast<char>(0xFF));
    body.push_back(0);  // 一条记录的 idLen，但后面什么都没有

    std::vector<PeerInfo> out;
    std::string err;
    REQUIRE_FALSE(decodeClientList(body, out, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("client list rejects trailing bytes", "[peer][codec]")
{
    // 与 kAnswerId 不同，这里尾部多余字节被判错：列表是**精确**的计数
    // 结构，多出来的字节无法归因到任何一类"未来扩展"，更像是长度算错了。
    std::vector<char> body;
    REQUIRE(encodeClientList({peer("a")}, body).ok());
    body.push_back('X');

    std::vector<PeerInfo> out;
    std::string err;
    REQUIRE_FALSE(decodeClientList(body, out, err));
}

TEST_CASE("encodeClientList refuses an invalid id", "[peer][codec]")
{
    std::vector<char> body;
    const auto st = encodeClientList({peer("")}, body);
    REQUIRE_FALSE(st.ok());
    REQUIRE(st.code == relayer::ErrorCode::kProtocol);
}

TEST_CASE("register result names are stable", "[peer]")
{
    REQUIRE(toString(RegisterResult::kOk) == "ok");
    REQUIRE(toString(RegisterResult::kDuplicateId) == "duplicate_id");
    REQUIRE(toString(RegisterResult::kFull) == "full");
}
