#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <asio/io_context.hpp>

#include "relayer/common/Status.h"
#include "relayer/core/session/Session.h"
#include "relayer/core/transfer/WorkerPool.h"
#include "relayer/filestore/StdFileStore.h"

// 客户端门面：一条到中继的会话 + 在其上跑的一次传输。
//
// 为什么 C++ 侧的 CLI 不是"main 直接拼 Session + TransferPeer"：
//
//   拼起来要处理的琐事（回调指向谁、什么时候挂 peer、超时怎么收尾、
//   退出码从哪来）与参数解析混在一个文件里之后，两边都读不清。而这三件事
//   在 app/ 的 Qt 前端里还得再做一遍——GUI 要做的事与命令行**完全相同**，
//   只是"显示进度"从 printf 变成信号。
//
//   所以把这条链路抽成 Client，main 只负责参数与退出码。app/ 将来可以照着
//   这个类写它的 worker，或者直接复用它（本类不含任何 Qt）。
//
// 与 TransferPeer 的分工：TransferPeer 管"一次传输"，Client 管"这条连接上
// 发生的一切"——握手、等对端上线、发起或等待传输、以及结束时把结论汇总成
// 一个退出码。

namespace relayer::cli
{

// 一次操作的最终结论。
//
// 不直接用 Status 是有原因的：Status 只有"成功/失败 + 原因"，而这里的
// 失败有好几类需要调用方区别对待（对端不在线可以重试，校验不符重试也没用，
// 目标被占用得先解决占用）。把它们压成一个 kUnknown 之后，上层再想区分
// 就只能去解析 message 字符串——那是往日志文案上挂逻辑。
struct Outcome
{
    bool ok = false;
    // 给用户看的结论（中文，一句话）。不用于程序判断。
    std::string summary;
    // 出错时的技术细节（路径、系统错误）。可能为空。
    Status status;
};

// 进度回调。bytesPerSecond 由 Client 自己算——传原始计数让每个调用方
// 各算一遍速度，必然出现三个略有出入的实现。
struct ProgressReport
{
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    double bytesPerSecond = 0.0;
};

struct ClientConfig
{
    // 中继地址。
    std::string host = "127.0.0.1";
    std::uint16_t port = 9527;
    std::string password;

    // 服务端分配的身份。留空表示"接受服务端给的"。
    //
    // 这个值只在**本机自测**时有意义：两个客户端连同一个中继时靠它区分
    // 谁是谁，而给谁 id 是服务端说了算。留空更接近真实用法。
    std::string expectId;

    // 认证与握手的时限。中继不可达时不能无限等——命令行工具挂住比报错
    // 更糟，用户不知道是卡住了还是慢。
    std::chrono::seconds connectTimeout{15};

    // 传输参数。chunkSize 在命令行可调，主要用于把"小文件测不出多块"
    // 这条路堵上（见 TransferPeer::Config 的注释）。
    std::uint32_t chunkSize = 0;  // 0 = 用协议默认值（4 MiB）

    // 接收方在收到描述后，最多等多久让发送方把数据发完。
    //
    // 这个值必须存在：接收方是被动的，发送方进程被 kill 之后它不会收到
    // 任何帧，会永远停在"等下一块"。没有上限时，kill 测试里的接收进程
    // 要等到 CI 的全局超时才退出，而那时已经分不清"卡住"与"慢"了。
    //
    // 语义是**空闲超时**（多久没有任何进展），不是总时长：1GB 在慢盘上
    // 可能跑很久，用总时长会误杀正常传输。
    std::chrono::seconds idleTimeout{60};

    // 整个操作的硬上限。0 表示不限。
    //
    // 与 idleTimeout 的区别：后者看"有没有进展"，本项看"总共花了多久"。
    // 测试用它兜底，避免一个逻辑 bug（比如永远不会完成的收尾握手）把
    // CI 拖到全局超时才有结论。
    std::chrono::seconds hardTimeout{0};
};

// 命令行用法之外的两种操作。放在一个枚举里而不是两个类：
// 两者共享 90% 的骨架（连、等、收尾），拆开就是复制粘贴。
enum class Action
{
    kSend,    // 把本地文件推给对端
    kReceive, // 把收到的文件落到本地
    kList,    // 只连上去打印在线列表，然后退出
};

class Client
{
public:
    // callback 在事件循环上调用，可能被调用多次（进度）或一次（结束）。
    // 结束之后不会再调用任何回调。
    Client(asio::io_context &ctx, ClientConfig config, std::function<void(const ProgressReport &)> onProgress,
           std::function<void(const Outcome &)> onDone);

    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // 发文件。remoteName 为空时用 sourcePath 的文件名。
    void sendFile(std::string sourcePath, std::string remoteName, std::string toPeerId);

    // 收文件。targetPath 是**本地绝对/相对路径**，直接交给 StdFileStore，
    // 不经过别的 root——命令行工具的落点就是用户写下来的那个路径。
    void receiveFile(std::string targetPath);

    // 连上去打印在线列表。
    void listPeers();

    // 主动停下。不发 cancel 帧：取消一个正在跑的传输时对端的收尾逻辑
    // 依赖它，但本工具在 Ctrl-C 场景下更应该**硬停**——那才是要测的
    // "发送方突然消失"。正常退出走 finishIdle()。
    void abort();

    [[nodiscard]] const std::string &assignedId() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace relayer::cli
