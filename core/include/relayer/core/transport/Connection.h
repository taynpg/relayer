#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>

#include "relayer/common/Status.h"
#include "relayer/core/protocol/Frame.h"
#include "relayer/core/transport/FrameReader.h"

// TCP 连接（asio 裸 socket）。
//
// 设计要点，每条都是为了避开 v1 踩过的坑：
//
// 1. **io_context 由调用方注入**，本层不持有任何全局状态。这样测试可以用
//    自己的 io_context 跑单线程，服务端可以用一个多线程池跑——同一份代码。
//
// 2. **每条连接一个 strand**。asio 的回调可能来自不同线程（多线程 run 时），
//    把每个回调都绑到 strand 上，就让"这条连接上的所有事件"串行执行。
//    没有 strand 的话就得自己加锁，而锁的粒度几乎必然错——要么太粗（把
//    socket 操作也锁进去，阻塞 io 线程），要么太细（漏掉某些状态）。
//
// 3. **关闭流程显式**。v1 的"对象析构时还在收帧"是崩溃的主要来源：
//    析构开始后 socket 回调仍可能被投递，此时对象已经半销毁。
//    v2 的做法是 close() 只 shutdown + 取消，真正的释放靠 shared_from_this()
//    的最后一个引用结束。析构函数里不做任何 I/O。
//
// 4. **本层不认识认证，也不认识业务**。它只负责"收发帧"和"连接生死"。
//    握手与转发都在上层——这样 transport 可以被单独测试，不需要密码学。

namespace relayer::core::transport
{

// 连接的生命周期状态。与 transfer 的 State 不是一回事：
// 那个是"传输会话"的状态，这个是"TCP 连接"的状态。
enum class ConnectionState
{
    kConnecting,  // 正在建连
    kOpen,        // 已连接，可收发
    kClosing,     // 已请求关闭，等回调返回
    kClosed,      // 完全关闭
};

[[nodiscard]] std::string_view toString(ConnectionState s) noexcept;

// 连接回调。
//
// 所有回调都在该连接的 strand 上执行，因此实现里**不需要加锁**。
// 但也因此**不要在回调里做阻塞操作**（读文件、等锁、同步 DNS）——
// 那会卡住整个 strand，让这条连接上的所有事件全部停摆。
struct ConnectionHandler
{
    // 连接可用（已连上 / 已接受）。在 strand 上调用，**先于任何 onFrame**。
    //
    // 为什么必须有这条回调：客户端不能靠"send 第一帧"来启动会话。
    // 建连前 send() 只是入队，而会话层要启动的是握手状态机——握手的第一帧
    // 由服务端发起（PROTOCOL.md §7.1），客户端手上根本没有"第一帧"可发。
    // 没有 onEstablished，客户端就只能在"我发了一帧"这个假事件上启动状态机，
    // 而那个假事件在网络失败时永远不会到来。
    //
    // 顺带也让 counters 与 peer() 在此时才可信（peer_ 在 connect 成功前
    // 只是调用方给的 host:port，之后才是真实对端地址）。
    std::function<void()> onEstablished;

    // 收到一帧。frame 在下一次回调之前一直有效。
    std::function<void(const protocol::Frame &)> onFrame;

    // 连接关闭。error 为空表示正常关闭（对端先关或本地主动关）。
    //
    // 这条回调**保证只调用一次**，且之后不会再有 onFrame。这是本层对
    // 上层唯一的强承诺，因为上层的资源释放依赖它。
    std::function<void(Status)> onClosed;
};

// 每连接的配置
struct ConnectionOptions
{
    // 单次读的字节数。4KB 是经验值：小于 MTU×N 会导致系统调用过多，
    // 太大会让"半帧到达"的延迟变大。协议帧本身可以是 32MB，靠 FrameReader
    // 累积，不依赖这个值。
    std::size_t readChunk = 4096;

    // 发送队列上限。对端不读而本地持续 write 时，队列会无限增长并吃光内存。
    // 超过上限就判该连接不可用——宁可断开，也不能让一个不读数据的对端
    // 把发送方拖垮。
    //
    // 高/低两个优先级队列共用这一个闸门，不分别设限：控制帧都很小（几十
    // 字节），真正会把内存吃掉的只有数据帧，分设上限只是多一处可能写错的
    // 状态。总闸在，疯跑的控制帧流也吃不到 32 MiB 以上。
    std::size_t maxPendingWrites = 32u * 1024 * 1024;

    // 单帧写入的超时。
    //
    // 解决的是"对端彻底不读"这一种故障：TCP 发送窗口填满之后 async_write
    // 永远不完成，写队列原地不动——**连控制帧都发不出去**，上层只能干等
    // 自己的空闲超时。此时连接其实已经死了，早断早报错。
    //
    // 60 秒的来历：默认块 256 KiB 要写满 60 秒，等价于链路速率低于
    // 4.4 KB/s。那不是"慢"，是不可用。而真正慢的链路（10 KB/s）26 秒就
    // 能写完一帧，不会被误杀。
    //
    // 0 表示关闭这项检查（测试里偶尔需要一个永不超时的连接）。
    std::chrono::milliseconds writeTimeout{60000};
};

// 唯一所有权：由 Connection 自己管理生命周期，外部拿 shared_ptr。
class Connection : public std::enable_shared_from_this<Connection>
{
public:
    // 建连（客户端用）。
    //
    // 这是异步的：返回时还没连上。返回的指针立即可用于 send()——帧会先
    // 排进队列，等连接建立后再发出去。
    //
    // 连接成功会走 handler.onEstablished，失败会走 handler.onClosed（带错误）。
    // 上层只需要处理一种"结束"路径，不用同时维护"连接失败"和"连接断开"
    // 两套逻辑。
    [[nodiscard]] static std::shared_ptr<Connection> connect(asio::io_context &ctx, const std::string &host,
                                                             std::uint16_t port, ConnectionHandler handler,
                                                             ConnectionOptions options = {});

    // 接受一个已建立的 socket（服务端用，由 acceptor 交出）。
    //
    // 显式传 io_context 而不是从 socket 的 executor 里取：从 executor
    // 反查 context 需要 static_cast，一旦 executor 被包装过（比如加了
    // strand）就会取到错误的类型。调用方本来就知道用的是哪个 context。
    //
    // onEstablished 与连接实际可用的时机对齐：adopt 不做 I/O，返回前就该算
    // "已连上"，所以这条回调是投递到 strand 上执行，而不是在 adopt 里同步调。
    // 同步调会让调用方在自己的构造函数里就被回调，此时对象还没构造完。
    static std::shared_ptr<Connection> adopt(asio::io_context &ctx, asio::ip::tcp::socket socket,
                                             ConnectionHandler handler, ConnectionOptions options = {});

    // 析构里**不做 I/O**，只保证不再有任何回调访问 this。
    // 实际关闭由 close() 完成。
    ~Connection();

    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    // 发送一帧。
    //
    // 内部会把帧序列化并追加到发送队列；队列非空时立即发起一次 async_write，
    // 后续帧在写完成的回调里续发（保证帧边界不被交错——这是 TCP 上最容易
    // 出的错：两个 async_write 并发会让字节流交错，对端看到一个拼接了
    // 两帧前缀的畸形帧）。
    //
    // 返回失败表示连接已不可用（或帧本身非法）。
    [[nodiscard]] Status send(const protocol::Frame &frame);

    // 请求关闭。
    //
    // 幂等：重复调用无副作用。只做 shutdown + cancel，不等回调返回——
    // 等回调返回是"优雅关闭"，由上层决定要不要等（用 onClosed 回调即可）。
    //
    // 为什么不在 close() 里同步等待：本层是单线程模型驱动（strand），
    // 在 strand 上阻塞等待 strand 上的回调返回必然死锁。
    void close();

    [[nodiscard]] ConnectionState state() const noexcept
    {
        return state_;
    }

    // 对端地址，用于日志。"host:port" 形式。
    [[nodiscard]] const std::string &peer() const noexcept
    {
        return peer_;
    }

    // 统计
    [[nodiscard]] std::uint64_t framesSent() const noexcept
    {
        return framesSent_;
    }
    [[nodiscard]] std::uint64_t bytesSent() const noexcept
    {
        return bytesSent_;
    }
    [[nodiscard]] std::uint64_t bytesReceived() const noexcept
    {
        return bytesReceived_;
    }

private:
    Connection(asio::io_context &ctx, asio::ip::tcp::socket socket, ConnectionHandler handler,
               ConnectionOptions options);

    void startRead();
    void startWrite();
    void notifyEstablished();
    void doClose(Status reason);

    [[nodiscard]] bool isActive() const noexcept
    {
        return state_ == ConnectionState::kOpen;
    }

    asio::strand<asio::io_context::executor_type> strand_;
    asio::ip::tcp::socket socket_;

    // 单帧写超时的计时器。每帧写开始时重置，写完成时取消。
    //
    // 放在 strand 上等待：这样超时回调与写完成回调、close 互斥，
    // 不会出现"超时判死的同时写完成回调正在 pop 队列"的竞态。
    asio::steady_timer writeTimer_;

    ConnectionHandler handler_;
    ConnectionOptions options_;

    ConnectionState state_ = ConnectionState::kConnecting;
    std::string peer_;

    FrameReader reader_;
    std::vector<char> readBuffer_;

    // 发送队列：按优先级分成两条。
    //
    // 为什么分：TCP 有序字节流 + 一次只写一帧，FIFO 队列意味着一帧 256 KiB
    // 的数据排在前面时，后面的 Cancel 必须等它写完。而"取消要及时生效"恰恰
    // 是用户最在意的交互（也是 v1 用两条 TCP 想解决的问题）。
    //
    // 分队列是单连接能做到的最好程度：控制帧能在**当前这一帧**写完之后立刻
    // 出队，不用等整个积压。已经在 async_write 中的帧无法撤回——那要靠
    // 减小块大小，不是靠调度。
    //
    // 判据来自 protocol::isBulkFrame()，不要在这里另写一份帧类型判断。
    std::deque<std::vector<char>> controlQueue_;
    std::deque<std::vector<char>> bulkQueue_;
    std::size_t pendingControlBytes_ = 0;
    std::size_t pendingBulkBytes_ = 0;
    bool writing_ = false;

    std::uint64_t framesSent_ = 0;
    std::uint64_t bytesSent_ = 0;
    std::uint64_t bytesReceived_ = 0;

    bool closedNotified_ = false;
};

}  // namespace relayer::core::transport
