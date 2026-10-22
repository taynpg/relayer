#include "relayer/core/transport/Connection.h"

#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/connect.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>

namespace relayer::core::transport
{

std::string_view toString(ConnectionState s) noexcept
{
    switch (s)
    {
    case ConnectionState::kConnecting:
        return "connecting";
    case ConnectionState::kOpen:
        return "open";
    case ConnectionState::kClosing:
        return "closing";
    case ConnectionState::kClosed:
        return "closed";
    }
    return "unknown";
}

namespace
{

std::string formatPeer(const asio::ip::tcp::socket &s)
{
    asio::error_code ec;
    const auto ep = s.remote_endpoint(ec);
    if (ec)
    {
        return "<unknown>";
    }
    std::string out = ep.address().to_string();
    out.push_back(':');
    out.append(std::to_string(ep.port()));
    return out;
}

}  // namespace

Connection::Connection(asio::io_context &ctx, asio::ip::tcp::socket socket, ConnectionHandler handler,
                       ConnectionOptions options)
    : strand_(asio::make_strand(ctx))
    , socket_(std::move(socket))
    , handler_(std::move(handler))
    , options_(options)
    , readBuffer_(options.readChunk)
{
}

Connection::~Connection() = default;

// ---------------------------------------------------------------------------
// 建连
// ---------------------------------------------------------------------------

std::shared_ptr<Connection> Connection::connect(asio::io_context &ctx, const std::string &host, std::uint16_t port,
                                                ConnectionHandler handler, ConnectionOptions options)
{
    // 先建一个"未连接"的 socket 包进 Connection，这样连接过程中的失败
    // 也能走同一条 doClose 路径。否则就要为"建连失败"单独写一套通知逻辑。
    asio::ip::tcp::socket sock(ctx);
    auto conn = std::shared_ptr<Connection>(new Connection(ctx, std::move(sock), std::move(handler), options));
    conn->peer_ = host + ":" + std::to_string(port);

    auto resolver = std::make_shared<asio::ip::tcp::resolver>(ctx);

    // 捕 conn 让它保持存活；resolver 也要捕，否则 async_resolve 期间
    // resolver 被销毁会导致回调访问已释放对象。
    resolver->async_resolve(
        host, std::to_string(port),
        asio::bind_executor(conn->strand_,
                            [conn, resolver](const asio::error_code &ec, asio::ip::tcp::resolver::results_type results)
                            {
                                if (ec)
                                {
                                    conn->doClose(Status::Fail(ErrorCode::kUnknown, "resolve failed: " + ec.message()));
                                    return;
                                }

                                // async_connect 会依次尝试解析出的每个地址（IPv4 / IPv6）
                                asio::async_connect(
                                    conn->socket_, results,
                                    asio::bind_executor(conn->strand_,
                                                        [conn](const asio::error_code &cec, const auto &)
                                                        {
                                                            if (cec)
                                                            {
                                                                conn->doClose(
                                                                    Status::Fail(ErrorCode::kUnknown,
                                                                                 "connect failed: " + cec.message()));
                                                                return;
                                                            }
                                                            conn->peer_ = formatPeer(conn->socket_);
                                                            conn->state_ = ConnectionState::kOpen;
                                                            conn->startRead();
                                                            // 连接建立前 send() 排进队列的帧，
                                                            // 这时候才能真的写出去
                                                            conn->startWrite();
                                                        }));
                            }));

    return conn;
}

std::shared_ptr<Connection> Connection::adopt(asio::io_context &ctx, asio::ip::tcp::socket socket,
                                              ConnectionHandler handler, ConnectionOptions options)
{
    auto conn = std::shared_ptr<Connection>(new Connection(ctx, std::move(socket), std::move(handler), options));
    conn->peer_ = formatPeer(conn->socket_);
    conn->state_ = ConnectionState::kOpen;
    conn->startRead();
    return conn;
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------

void Connection::startRead()
{
    if (!isActive())
    {
        return;
    }

    socket_.async_read_some(asio::buffer(readBuffer_),
                            asio::bind_executor(strand_,
                                                [self = shared_from_this()](const asio::error_code &ec, std::size_t n)
                                                {
                                                    if (ec)
                                                    {
                                                        // 对端正常关闭（EOF）不是错误，用空 Status 表示
                                                        if (ec == asio::error::eof ||
                                                            ec == asio::error::connection_reset)
                                                        {
                                                            self->doClose(Status::Ok());
                                                        }
                                                        else
                                                        {
                                                            self->doClose(Status::Fail(ErrorCode::kIoError,
                                                                                       "read failed: " + ec.message()));
                                                        }
                                                        return;
                                                    }

                                                    self->bytesReceived_ += n;

                                                    const auto st = self->reader_.append(self->readBuffer_.data(), n);
                                                    if (!st.ok())
                                                    {
                                                        self->doClose(st);
                                                        return;
                                                    }

                                                    // 把能解的帧全部解完，不能只解一个就回去等：一次 read 可能收到
                                                    // 好几帧，剩下的如果留在缓冲里，要等下一次 read 才会被处理，
                                                    // 而对端可能已经不再发数据了。
                                                    protocol::Frame frame;
                                                    Status frameStatus;
                                                    while (self->reader_.next(frame, frameStatus))
                                                    {
                                                        if (self->handler_.onFrame)
                                                        {
                                                            self->handler_.onFrame(frame);
                                                        }
                                                        // 回调里可能已经调用了 close()，那就不要再继续派发
                                                        if (!self->isActive())
                                                        {
                                                            return;
                                                        }
                                                    }

                                                    if (!frameStatus.ok())
                                                    {
                                                        self->doClose(frameStatus);
                                                        return;
                                                    }

                                                    self->startRead();
                                                }));
}

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------

Status Connection::send(const protocol::Frame &frame)
{
    // 允许在 kConnecting 阶段入队：客户端连上之后要立刻发握手的第一帧，
    // 让它等"连上了"再发没有意义，而且会让调用方多写一层状态判断。
    // 其余状态（Closing / Closed）一律拒。
    if (state_ != ConnectionState::kOpen && state_ != ConnectionState::kConnecting)
    {
        return Status::Fail(ErrorCode::kProtocol, "connection is not open");
    }

    std::vector<char> wire;
    const auto st = protocol::packFrameChecked(frame, wire);
    if (!st.ok())
    {
        // 帧本身非法（字段超长等）。这是调用方的 bug，不是连接的错，
        // 所以不关闭连接——但要报上去。
        return st;
    }

    if (pendingWriteBytes_ + wire.size() > options_.maxPendingWrites)
    {
        // 对端不读，我们的队列在涨。继续排下去会吃光内存。
        // 断开是唯一正确的选择：这已经不是"慢"，是"对面不收"。
        doClose(Status::Fail(ErrorCode::kNoSpace, "send queue overflow: peer is not reading"));
        return Status::Fail(ErrorCode::kNoSpace, "send queue overflow");
    }

    ++framesSent_;
    const std::size_t size = wire.size();
    pendingWriteBytes_ += size;
    writeQueue_.push_back(std::move(wire));

    if (!writing_)
    {
        startWrite();
    }
    return Status::Ok();
}

void Connection::startWrite()
{
    if (writeQueue_.empty() || !isActive())
    {
        writing_ = false;
        return;
    }

    writing_ = true;

    // 一次只发队首那一帧。发完再从回调里发下一帧。
    //
    // 为什么不在一个 async_write 里发所有待发数据：那样会让多个帧挤在
    // 一次系统调用里，虽然 TCP 层面没问题，但无法逐帧统计，而且一旦
    // 中途出错，很难知道是哪一帧的边界断的。逐帧发让错误定位简单得多。
    const auto &front = writeQueue_.front();
    asio::async_write(
        socket_, asio::buffer(front),
        asio::bind_executor(strand_,
                            [self = shared_from_this()](const asio::error_code &ec, std::size_t n)
                            {
                                if (ec)
                                {
                                    self->doClose(Status::Fail(ErrorCode::kIoError, "write failed: " + ec.message()));
                                    return;
                                }

                                self->bytesSent_ += n;
                                if (!self->writeQueue_.empty())
                                {
                                    self->pendingWriteBytes_ -= self->writeQueue_.front().size();
                                    self->writeQueue_.pop_front();
                                }

                                self->startWrite();
                            }));
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------

void Connection::close()
{
    doClose(Status::Ok());
}

void Connection::doClose(Status reason)
{
    // 幂等保护：onClosed 只允许通知一次。
    //
    // 没有这个保护的话，读和写可能各自报错（对端 RST 时很常见：读报
    // connection_reset、写报 broken_pipe），两条路径都会走到这里，
    // 上层就会收到两次 onClosed 并对同一份资源释放两次。
    if (state_ == ConnectionState::kClosed || state_ == ConnectionState::kClosing)
    {
        return;
    }
    state_ = ConnectionState::kClosing;

    // 只取消，不等待。等待回调返回在 strand 上会死锁（见头文件说明）。
    asio::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    writeQueue_.clear();
    pendingWriteBytes_ = 0;
    writing_ = false;
    reader_.clear();

    state_ = ConnectionState::kClosed;

    if (closedNotified_)
    {
        return;
    }
    closedNotified_ = true;

    // 把 handler 取出来再投递，而不是在投递时读 this->handler_。
    //
    // 原因：上层很可能在 onClosed 里丢掉最后一个 shared_ptr<Connection>，
    // 那一刻 this 就被销毁了。如果投递的 lambda 还捕着 this 去访问
    // handler_，就是 use-after-free。先把 std::function 拷一份出来，
    // 之后 lambda 只持有这份拷贝，与 this 的生死无关。
    auto notify = handler_.onClosed;
    if (!notify)
    {
        return;
    }

    asio::post(strand_, [n = std::move(notify), r = std::move(reason)]() mutable { n(std::move(r)); });
}

}  // namespace relayer::core::transport
