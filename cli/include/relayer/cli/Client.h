#pragma once

#include <asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "relayer/common/Status.h"
#include "relayer/core/session/FileList.h"
#include "relayer/core/session/PathMeta.h"
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

namespace relayer::cli {

// 传输层的诊断计数，只挑验收用得着的那几个。
//
// 刻意**不**直接暴露 core::transfer::TransferPeer::Stats：那会把
// TransferPeer.h（连带 SendWindow / TransferPayload）拖进这个头文件，
// 而 CLI 的上层只需要几个整数。转换在 Client.cpp 里一次做完。
struct TransferStats {
    std::uint64_t chunksSent = 0;
    std::uint64_t chunksReceived = 0;
    std::uint64_t chunksRetransmitted = 0;   // 丢包后补空洞的次数（不是块总数）
    std::uint64_t duplicateChunks = 0;
    std::uint64_t acksSent = 0;
    std::uint64_t acksReceived = 0;
};

// 在线对端（kAnswerClientList 的一项）。
//
// 之前只存在于 Impl 里、以拼好的文本出现在 summary 中。GUI 需要结构化的
// 列表来填表格，文本拼回去再拆开是往显示格式上挂逻辑，所以提到这里。
struct PeerEntry {
    std::string id;
    std::string address;
};

// 一次操作的最终结论。
//
// 不直接用 Status 是有原因的：Status 只有"成功/失败 + 原因"，而这里的
// 失败有好几类需要调用方区别对待（对端不在线可以重试，校验不符重试也没用，
// 目标被占用得先解决占用）。把它们压成一个 kUnknown 之后，上层再想区分
// 就只能去解析 message 字符串——那是往日志文案上挂逻辑。
struct Outcome {
    bool ok = false;
    // 给用户看的结论（中文，一句话）。不用于程序判断。
    std::string summary;
    // 出错时的技术细节（路径、系统错误）。可能为空。
    Status status;
    // 连接级失败：中继断开、对端失联——重试剩余工作没有意义，调用方
    // （任务批次）应当就此打住。文件级失败（目标已存在、写盘失败、
    // 校验不过）不置位，批次继续下一个。v1 的 RelayTask 有同款判断
    // （core->isConnected()），v2 没有常驻连接可查，靠结论携带。
    bool connectionLost = false;
    // 传输层的计数。list 这类没有传输的操作全为 0。
    TransferStats stats;
    // list 操作的在线对端。其他操作为空。
    std::vector<PeerEntry> peerList;

    // browse（远端目录列举）的结果。其他操作为空。
    // status 为 kRejected / kNotFound 时 entries 为空，summary 说明原因。
    std::vector<core::session::RemoteEntry> remoteEntries;
    core::session::FileListStatus listStatus = core::session::FileListStatus::kOk;
    // 本次列举是不是"根列表"（条目名是完整路径，不是单段名）。
    bool rootListing = false;

    // askPathMeta 的结果：与请求里的路径**一一对应、顺序相同**。
    // 其他操作为空。
    std::vector<core::session::RemoteMeta> remoteMetas;
};

// 进度回调。bytesPerSecond 由 Client 自己算——传原始计数让每个调用方
// 各算一遍速度，必然出现三个略有出入的实现。
struct ProgressReport {
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    double bytesPerSecond = 0.0;
};

struct ClientConfig {
    // 中继地址。
    std::string host = "127.0.0.1";
    std::uint16_t port = 9527;
    std::string password;

    // 显式声明"中继已关闭认证"（对应服务端的 --allow-no-auth）。
    //
    // 为什么不能从 password 为空自动推断：服务端侧的 ServerHandshake 明确
    // 禁止"口令为空就算不认证"（漏配口令的部署会静默变成开放中继），
    // 客户端是同一条原则的另一半——用户漏填口令时，正确行为是认证失败
    // 并提示，而不是替他决定"那就别认证了"。所以这个开关必须显式给出。
    bool serverAllowNoAuth = false;

    // 要声明的身份（v1 语义：自己的标识自己定）。
    //
    // 非空时，客户端在认证完成后立刻发 kClaimId（PROTOCOL.md §11.7）：
    //   - 服务端接受 → 本机 id 变成这个名字，跨重连保持不变；
    //   - 已被别人占用 / 不合法 → 连接以 kIdTaken / kIdInvalid 结束。
    //
    // **不自动改名、不顶替先到者**：用户依赖 id 做"文件固定发给谁"的
    // 判断，悄悄改名比连不上危险得多。留空则沿用服务端分配（client-N）。
    //
    // 与旧字段 expectId 的区别：那个只是"拿到后核对，不符就失败"，
    // 无法改变结果；这个是主动声明。
    std::string requestedId;

    // 认证与握手的时限。中继不可达时不能无限等——命令行工具挂住比报错
    // 更糟，用户不知道是卡住了还是慢。
    std::chrono::seconds connectTimeout{15};

    // 服务端分配身份的回调（可选）。
    //
    // CLI 不用它（CLI 的契约输出是 ASSIGNED_ID= 那行 stdout），GUI 没有
    // stdout 可抓，靠它把自己的 id 显示出来。**可能被多次调用**：认证刚过
    // 时 id 为空，收到 kAnswerId 后是真实 id，服务端改名再通知也会再来。
    // 与 onDone 不同，回调之后操作还会继续。
    std::function<void(const std::string&)> onIdAssigned;

    // 常驻接收：接收完成一个文件后**不断开**，原地重建 receiver 继续等
    // 下一个请求。整个会话期间服务端分配的 id 保持不变。
    //
    // 为什么必须有：v2 每操作一条连接、id 由服务端递增分配，GUI 的自动
    // 接收如果"收完就结束再重挂"，每收一个文件 id 就变一次——发送方
    // 表格里存的对端 id 立刻过期，再发就发进虚空（服务端对不存在的
    // 目标静默丢帧，发送方要等满 answerTimeout 才知道失败）。核心场景
    // "频繁把固定文件发给同一个对端"要求接收方 id 稳定，这是正解。
    //
    // 只对 kReceive 有意义。CLI 的 recv 保持默认（false）：一次一收的
    // 语义是它的契约，e2e 靠它判定进程退出。
    bool keepReceiving = false;

    // 常驻接收模式下每收完一个文件的回调（kCompleted 时触发）。操作本身
    // 不结束，onDone 不会来；GUI 靠它把"接收完成：路径"写进日志。
    std::function<void(const std::string& targetPath)> onFileReceived;

    // 常驻接收模式下某次接收**失败**的回调。与 onFileReceived 对称：
    // 失败同样不会结束操作（连接还活着就原地重建 receiver 继续等），
    // 上层只能靠它把失败写进日志。连接已死（kPeerGone）或本端主动取消
    // 时走 onDone，不走这里。
    std::function<void(const std::string& message)> onReceiveFailed;

    // 常驻接收会话上刷新在线列表的结果（refreshPeers 的回程）。列表是
    // **结构化**的：GUI 要填表格，文本拼回去再拆开是往显示格式上挂逻辑。
    std::function<void(const std::vector<PeerEntry>& peers)> onPeerList;

    // 认证拿到真实 id 后自动问一次在线列表（结果走 onPeerList）。
    //
    // 为什么 GUI 需要它：连接/刷新若先开一条临时列表连接再挂接收会话，
    // 一次"连接"就要消耗两个 id（临时连接一个、接收会话一个），第一条
    // 还立刻作废。合并进常驻会话后，从连上那一刻起只有一条连接、一个 id。
    // CLI 的 recv/list 不用它（list 是独立操作；recv 没注册 onPeerList）。
    bool askListOnStart = false;

    // 接收方覆盖策略：目标已存在时删掉旧文件再收。透传给
    // TransferPeer::Config::overwriteExisting（见那里的注释——默认拒绝
    // 覆盖是 core 层的刻意设计，打开它等于调用方显式授权"先删目标"）。
    // "反复把固定文件发给对方同一目录"的场景需要它，否则第二次发送起
    // 永远失败。
    bool overwriteExisting = false;

    // 自检：接收会话建立后，把这一个文件发给**自己**（PROTOCOL.md §11.3.1）。
    //
    // 空 = 关闭。非空只在 kReceive 下有意义：一个进程同时当收发两端，
    // 一条命令就能验证"中继 + 认证 + 传输 + 落盘"整条链路，不必找第二台
    // 机器。走的是服务端的自投转发（帧绕回本连接），客户端靠 sessionId
    // 把接收端与发送端分开——与 GUI"自动接收 + 搭车发送"同一条路径。
    //
    // 退出条件：文件收到并校验通过（与普通 recv 一致）。自检发送失败会
    // 提前结束并报错。
    std::string selfSendPath;

    // 自检列举（recv --self-browse）：在**常驻接收会话**上向自己发一次
    // kAskFileList，列完即退出。
    //
    // 存在的理由：GUI 的远端浏览走 `Client::browseOnSession`——请求搭在
    // kReceive 会话上；而 CLI 的 `ls` 走 `browsePeer`（一次性连接）。两条
    // 路不是同一份代码，只测 `ls` 覆盖不到 GUI 那条。这个开关让"接收会话
    // 上发列举"能被 e2e 跑到（自投把请求与应答都绕回本连接，同时覆盖了
    // 服务端应答侧与客户端请求侧）。
    //
    // 与 selfSendPath 互斥：两个自检都想在列/传完后结束进程，同时给会让
    // 退出条件含糊。
    bool selfBrowse = false;

    // selfBrowse 要列的绝对路径。空 = 根列表。
    std::string selfBrowsePath;

    // 远端取回（recv --pull-from / --pull-path，PROTOCOL.md §11.9）。
    //
    // 做成"recv + 搭车请求"而不是独立子命令，理由和 --self-send 一样：请求
    // 要与常驻接收会话共用一条连接（新开连接会顶掉接收会话、换掉 id），而
    // 回程的文件正好由这个接收会话收下。
    //
    // pullFrom 为空表示不做取回。
    std::string pullFrom;
    std::string pullPath;   // 对端的绝对路径

    // true 表示 pullPath 指向**目录**，请对端打包成 tar.xz 再发（PROTOCOL.md
    // §11.10）。收到的是包本身，不会自动解包。
    bool pullAsArchive = false;

    // 收到 `.tar.xz` 时自动解包并删除包（PROTOCOL.md §11.10.6 的"下一步"）。
    //
    // 只对**目录取回**有意义，所以默认关：别人主动发给你的 .tar.xz 可能是你
    // 就想要的那个包，擅自解开会让"东西呢"变成一个问题。
    bool unpackArchives = false;

    // 远端目录列举/取回**不再有共享根**（DESIGN.md §3.8）。
    //
    // 单用户自用工具，v1 就是全路径访问。少了这一层，远端浏览可以直接
    // 从盘符/家目录开始导航，不必先配置"我要共享哪个目录"再浏览。
    // 代价是"能连上中继并知道本机 id 的人可以列举本机路径"——这是有意的
    // 取舍，别再偷偷把它当安全边界（PROTOCOL.md §11.8.1）。

    // 传输参数。chunkSize 在命令行可调，主要用于把"小文件测不出多块"
    // 这条路堵上（见 TransferPeer::Config 的注释）。
    std::uint32_t chunkSize = 0;   // 0 = 用协议默认值（256 KiB）

    // 接收方在收到描述后，最多等多久让发送方把数据发完。
    //
    // 这个值必须存在：接收方是被动的，发送方进程被 kill 之后它不会收到
    // 任何帧，会永远停在"等下一块"。没有上限时，kill 测试里的接收进程
    // 要等到 CI 的全局超时才退出，而那时已经分不清"卡住"与"慢"了。
    //
    // 语义是**空闲超时**（多久没有任何进展），不是总时长：1GB 在慢盘上
    // 可能跑很久，用总时长会误杀正常传输。
    //
    // **只在传输开始后生效**（TransferPeer::started()）。传输开始前的沉默
    // 是正常的：发送方在算摘要，接收方在等第一个请求帧。想让 recv 等待
    // 多久就放弃，用 hardTimeout——那是"总共多久"，语义不同。
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
enum class Action {
    kSend,      // 把本地文件推给对端
    kReceive,   // 把收到的文件落到本地
    kList,      // 只连上去打印在线列表，然后退出
    kBrowse,    // 问对端要一次目录列举（远端浏览 / 对照表粗校验的前置）
};

class Client
{
public:
    // callback 在事件循环上调用，可能被调用多次（进度）或一次（结束）。
    // 结束之后不会再调用任何回调。
    Client(asio::io_context& ctx, ClientConfig config, std::function<void(const ProgressReport&)> onProgress,
           std::function<void(const Outcome&)> onDone);

    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // 发文件。remoteName 为空时用 sourcePath 的文件名。
    void sendFile(std::string sourcePath, std::string remoteName, std::string toPeerId);

    // 收文件。targetPath 是**本地绝对/相对路径**，直接交给 StdFileStore，
    // 不经过别的 root——命令行工具的落点就是用户写下来的那个路径。
    void receiveFile(std::string targetPath);

    // 连上去打印在线列表。
    void listPeers();

    // 问对端要一次目录列举（PROTOCOL.md §11.8）。
    //
    // absPath 是**对端视角的绝对路径**，空表示根列表（盘符 + 家目录）。
    // 或路径越界时结果为失败（kRejected），不是崩溃。
    void browsePeer(std::string peerId, std::string relPath);

    // 在**当前这条连接**上问一次在线列表（只对常驻接收会话有意义）。
    //
    // 为什么必须有：每条新连接都会消耗一个服务端分配的新 id，还会把
    // 常驻接收顶掉重挂——GUI 的"刷新"如果每次都走 listPeers（临时
    // 连接），用户每点一次，本机 id 就涨一号（实测如此）。列表就该在
    // 已经在线的那条连接上问，这是 v1 常驻连接的天然行为，v2 靠本方法
    // 补回来。结果经 ClientConfig::onPeerList 回报，操作不结束。
    void refreshPeers();

    // 在**现有常驻接收会话**上直接发一个文件：不新建连接，服务端不会
    // 重新分配本机 id，接收也照常进行（两者靠 sessionId 区分各自的帧）。
    //
    // 返回 false = 当前没有可用的常驻会话（没连上/已结束），调用方应回落
    // 到 sendFile 的一次性连接。done 在**这一次发送**结束时调用，会话本身
    // 继续存活（成功、失败、取消都不会结束 Client）。
    //
    // 为什么必须这样做：sendFile 每次新建连接，服务端给新连接分配新 id，
    // GUI 里"点一次发送，自己的 id 就涨一号"，发送方自己也因此从对端表里
    // 消失。v1 是常驻连接天然没这问题，v2 用这个方法补回来。
    bool sendOnSession(std::string sourcePath, std::string remoteName, std::string toPeerId,
                       std::function<void(const Outcome&)> done);

    // 在**现有常驻会话**上问一次远端目录（PROTOCOL.md §11.8）。
    //
    // GUI 专用：新开连接会顶掉常驻接收会话、把自己的 id 换掉（文档里反复
    // 警告的那类回归）。done 在这一次列举结束时调用，会话继续存活。
    // 返回 false = 当前没有可用的常驻会话（没连上 / 正在跑别的操作），
    // 调用方应提示"先连接"而不是偷偷降级成新连接。
    bool browseOnSession(std::string peerId, std::string relPath, std::function<void(const Outcome&)> done);

    // 在**现有常驻会话**上批量查询一批绝对路径的元信息（PROTOCOL.md §11.11）。
    //
    // 用途：图形界面的"发送前检查"——先问对方这些目标路径是否已存在、
    // 多大、内容指纹是否一致，再在一个普通对话框里逐项问用户要不要覆盖。
    // 决策被整体挪到传输开始**之前**，因此不存在"传输中阻塞 io 线程等
    // 用户点按钮"的问题（那条路会饿死心跳，只能加超时兜底）。
    //
    // 结果按请求顺序落在 Outcome::remoteMetas；对端返回的条数不匹配时
    // 整批判失败（按下标对不上就是错位，错位的覆盖提示会让用户点错文件）。
    bool askPathMetaOnSession(std::string peerId, std::vector<std::string> absPaths, std::function<void(const Outcome&)> done);

    // 在**现有常驻会话**上请对端把某个文件发过来（PROTOCOL.md §11.9）。
    //
    // done 在收到应答时就调用，**不等文件传完**：ok 表示"对方接受了"，
    // 文件随后落到接收目录，那一步的结论走正常的接收回调。这样上层能区分
    // "对方不给我"（立刻可知）和"给了但传得慢"（接收进度会报）。
    //
    // 返回 false = 没有可用的常驻会话，或已经有一个取回请求在等应答。
    // archive=true 表示 relPath 指向目录，请对端打包（PROTOCOL.md §11.10）。
    // 归档请求的应答超时是 120 秒：对端要先压缩，应答在打包完成后才发。
    bool pullOnSession(std::string peerId, std::string relPath, bool archive, std::function<void(const Outcome&)> done);

    // 改**现有接收会话**的落点（GUI 的对照表行级下载用）。
    //
    // 协议里落点由接收方决定、发送方无权指定（§11.9）；这条接口是接收方
    // 自己改主意——"把这一行要的东西放到这一行的本地目录"，而不是笼统扔进
    // 全局接收目录。它只改内存里的 targetPath，**不重连**（重连会换 id，
    // 那正是整套常驻会话要避免的事）。
    //
    // 返回 false = 当前不是"接收会话"状态（没连上 / 正在跑别的操作）。
    // 调用方负责在这次接收结束后改回去——会话是长期的，落点不该被一次
    // 下载永久改掉。
    bool setReceiveDirOnSession(std::string targetPath);

    // 把 `<dir>/x.tar.xz` 解到 `<dir>/x/`，成功后删除包，返回解包目录。
    // 失败返回空串并**保留包**——用户可以自己解，总比包被删了又没解开好。
    //
    // 为什么不直接解到 `<dir>/`：包内路径是"相对被请求的那个目录"的
    // （§11.10.3），没有顶层目录。解到 `<dir>/` 会让 a.txt、nested/ 散在
    // 接收目录里，与对方看到的层次不一致。
    // intoSubdir=true 时解到 `<父目录>/<包名去 .tar.xz>/`。
    //
    // 两种包的形状不同，所以要有这个开关：
    //   - 取回目录（§11.10）的包**无顶层目录**（entryName 相对被请求的目录），
    //     必须解到子目录里，否则内容散在接收目录；
    //   - 手动压缩的包**有顶层目录**（entryName 含源名），解到父目录即可；
    //     再套一层会变成 a/a/。
    static std::string unpackArchive(const std::string& archivePath, bool intoSubdir = true);

    // 把若干本地路径打成 tar.xz（手动压缩，GUI 右键用）。
    //
    // 包内路径相对**各源的父目录**，所以包里有顶层目录——与取回目录的包相反，
    // 解压时要配 unpackArchive(path, false)。
    static bool createArchive(const std::vector<std::string>& sources, const std::string& archivePath, std::string& err);

    // 取消正在进行的"会话内发送"（对端的收尾会看到 kLocalCancelled）。
    // 会话本身不受影响——接收继续等下一个文件。
    void cancelSend();

    // 主动停下。不发 cancel 帧：取消一个正在跑的传输时对端的收尾逻辑
    // 依赖它，但本工具在 Ctrl-C 场景下更应该**硬停**——那才是要测的
    // "发送方突然消失"。正常退出走 finishIdle()。
    void abort();

    [[nodiscard]] const std::string& assignedId() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace relayer::cli
