#include "relayer/core/transfer/TransferPeer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <asio/post.hpp>

#include "relayer/core/transfer/MerkleTree.h"

namespace relayer::core::transfer
{

namespace
{

using protocol::Frame;
using protocol::FrameType;

constexpr auto kAckInterval = std::chrono::milliseconds(20);

}  // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

TransferPeer::TransferPeer(asio::io_context &ctx, std::shared_ptr<session::Session> session,
                           filestore::IFileStore &store, WorkerPool &pool, Role role, std::int64_t sessionId,
                           Config config, std::function<void(const TransferResult &)> onDone)
    : ctx_(ctx)
    , session_(std::move(session))
    , store_(store)
    , pool_(pool)
    , role_(role)
    , sessionId_(sessionId)
    , config_(std::move(config))
    , onDone_(std::move(onDone))
{
}

TransferPeer::~TransferPeer() = default;

std::shared_ptr<TransferPeer> TransferPeer::createSender(asio::io_context &ctx,
                                                         std::shared_ptr<session::Session> session,
                                                         filestore::IFileStore &store, WorkerPool &pool,
                                                         std::string sourcePath, std::string remoteName,
                                                         std::string toPeerId, std::int64_t sessionId, Config config,
                                                         std::function<void(const TransferResult &)> onDone)
{
    // make_shared 不能用：构造函数是私有的，拿不到 make_shared 的访问权
    // （它需要公开构造）。这里显式 new 后用 shared_ptr 接管，效果一致。
    std::shared_ptr<TransferPeer> peer(new TransferPeer(ctx, std::move(session), store, pool, Role::kSender, sessionId,
                                                        std::move(config), std::move(onDone)));
    peer->sourcePath_ = std::move(sourcePath);
    peer->remoteName_ = std::move(remoteName);
    peer->toPeerId_ = std::move(toPeerId);

    peer->sender_ = std::make_unique<TransferSender>(store, pool, peer->config_.sender);

    // open 失败（文件不存在、块大小非法）是本地问题，不该发请求出去再等
    // 对端拒绝——那要多一个往返才能知道"其实是我的源文件没了"。
    const auto st = peer->sender_->open(peer->sourcePath_, peer->config_.chunkSize);
    if (!st.ok())
    {
        peer->finish(TransferOutcome::kFailed, st);
        return peer;
    }

    peer->startSending();
    return peer;
}

std::shared_ptr<TransferPeer>
TransferPeer::createReceiver(asio::io_context &ctx, std::shared_ptr<session::Session> session,
                             filestore::IFileStore &store, WorkerPool &pool, std::string targetPath,
                             std::int64_t sessionId, Config config, std::function<void(const TransferResult &)> onDone)
{
    std::shared_ptr<TransferPeer> peer(new TransferPeer(ctx, std::move(session), store, pool, Role::kReceiver,
                                                        sessionId, std::move(config), std::move(onDone)));
    peer->targetPath_ = std::move(targetPath);
    peer->receiver_ = std::make_unique<TransferReceiver>(store, pool, peer->config_.receiver);

    // 接收方不等 open——它还不知道块大小与总长，那些在 kFileRequestSend 里。
    return peer;
}

// ---------------------------------------------------------------------------
// 发送方
// ---------------------------------------------------------------------------

void TransferPeer::startSending()
{
    if (requestSent_ || finished_)
    {
        return;
    }
    // 立刻置位，防止异步摘要还没回来时被重复调用一次（比如上层连着两次
    // pump）。这不是"可以发块"的信号——那要等对端应答，见 started_。
    requestSent_ = true;

    // 摘要必须异步算。它要**整读一遍文件**：1GB 在机械盘上是几十秒，
    // 在 strand 上同步做等于把这条连接的心跳、收帧、超时判定全部冻住
    // （见 Session.h 的"不能做阻塞操作"）。TransferSender 专门提供了
    // computeDigest 把这件事丢到线程池，这里没有理由绕开它。
    //
    // 代价是"发请求"这个动作被推迟到摘要算完，期间对端什么都不收到。
    // 这在语义上是对的——请求体里的 sha256 本来就得等摘要出来才能填。
    const auto st = sender_->computeDigest(
        [self = shared_from_this()](TransferSender::DigestResult digest)
        {
            // 回调在 WorkerPool 线程上。所有状态归 strand 管，必须投回去。
            asio::post(self->ctx_, [self, digest = std::move(digest)] { self->onDigestReady(std::move(digest)); });
        });

    if (!st.ok())
    {
        finish(TransferOutcome::kFailed, st);
    }
}

void TransferPeer::onDigestReady(TransferSender::DigestResult digest)
{
    if (finished_)
    {
        return;
    }

    if (!digest.status.ok())
    {
        finish(TransferOutcome::kFailed, digest.status);
        return;
    }

    // 存下来给收尾握手用。重算要再整读一遍文件，而文件在传输过程中完全
    // 可能被改动——那时算出来的根和描述对不上，接收方会报 kRootMismatch，
    // 而真正的错误（源文件被改了）谁也看不出来。
    merkleRootHex_ = digest.merkleRootHex;

    TransferDesc desc;
    desc.displayName = remoteName_.empty() ? sourcePath_ : remoteName_;
    desc.totalSize = sender_->totalSize();
    desc.chunkSize = sender_->chunkSize();
    desc.sha256Hex = digest.sha256Hex;
    desc.fingerprintHex = store_.quickFingerprint(sourcePath_);
    desc.root = desc.displayName;  // 接收方以它为准决定落点名

    std::vector<char> body;
    if (auto enc = encodeDesc(desc, body); !enc.ok())
    {
        finish(TransferOutcome::kFailed, enc);
        return;
    }

    Frame f;
    f.type = FrameType::kFileRequestSend;
    f.sessionId = sessionId_;
    f.to = toPeerId_;
    f.fuuid = remoteName_;
    f.body = std::move(body);
    requestSentAt_ = std::chrono::steady_clock::now();
    sendFrame(std::move(f));
}

void TransferPeer::handleAnswerSend(const Frame &frame)
{
    TransferAnswer answer;
    std::string err;
    if (!decodeAnswer(frame.body, answer, err))
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "bad answer frame: " + err));
        return;
    }

    // 对端拒绝（目标已存在、磁盘满、描述非法……）。把它的原因原样带上去，
    // 比本地编一句"传输失败"有用得多。
    if (answer.status != AnswerStatus::kOk)
    {
        finish(TransferOutcome::kFailed,
               Status::Fail(ErrorCode::kProtocol,
                            "peer refused: " + std::string(toString(answer.status)) + " " + answer.reason));
        return;
    }

    // 续传点：对端说"前 N 块我有了"。发送方据此把这 N 块标成已确认——
    // 它们是**对端已经落盘**的，不该再发一遍。
    if (answer.resumeOffset > 0)
    {
        const auto chunkSize = sender_->chunkSize();
        if (chunkSize == 0 || answer.resumeOffset % chunkSize != 0)
        {
            ++stats_.protocolErrors;
            finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "resumeOffset is not chunk aligned"));
            return;
        }

        const auto resumeFrom = static_cast<std::uint32_t>(answer.resumeOffset / chunkSize);

        // 用一条 ACK 把 [0, resumeFrom) 标记为已确认。复用 applyAck 而不是
        // 加一个新接口：语义上它确实是"对端告诉我这些块它有了"。
        ChunkAck ack;
        ack.ackThrough = std::min(resumeFrom, sender_->totalChunks());
        sender_->applyAck(ack);
    }

    started_ = true;

    // 续传点等于总长：对端**整份都有了**，没有块要发。直接进收尾，
    // 让它跑一遍校验把残片转正——不这么做的话，sendReadyChunks 什么都不发，
    // 而 done() 为真时没人再来调 sendCompleteRequest，两端就一起干等。
    if (sender_->done())
    {
        sendCompleteRequest();
        return;
    }

    sendReadyChunks();
}

void TransferPeer::handleChunkAck(const Frame &frame)
{
    ChunkAck ack;
    std::string err;
    if (!decodeAck(frame.body, ack, err))
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "bad ack frame: " + err));
        return;
    }

    ++stats_.acksReceived;
    sender_->applyAck(ack);

    if (sender_->done())
    {
        sendCompleteRequest();
        return;
    }

    // ACK 之后窗口腾出来了，立刻再发一批。不等到下一次 pump——
    // 那最多白等一个 pump 周期（测试里是手工调，等不到就卡住了）。
    sendReadyChunks();
}

void TransferPeer::handleAnswerComplete(const Frame &frame)
{
    CompleteStatus status = CompleteStatus::kWriteFailed;
    std::string rootHex;
    std::string err;
    if (!decodeCompleteAnswer(frame.body, status, rootHex, err))
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "bad complete answer: " + err));
        return;
    }

    if (status == CompleteStatus::kOk)
    {
        finish(TransferOutcome::kCompleted, Status::Ok());
        return;
    }

    // 校验失败。kRootMismatch 说明数据传坏了，kShapeMismatch 说明两端对
    // 块结构的理解不一致——后者更严重，因为它意味着协议层面出了问题。
    Status st = Status::Fail(ErrorCode::kProtocol, "peer verify failed: " + std::string(toString(status)));
    if (!err.empty() && status == CompleteStatus::kWriteFailed)
    {
        // 写盘失败是对端的本地问题，不该归成协议错误。
        st = Status::Fail(ErrorCode::kIoError, "peer write failed: " + err);
    }
    finish(TransferOutcome::kFailed, st);
}

void TransferPeer::sendReadyChunks()
{
    if (finished_)
    {
        return;
    }

    // PostFn：把读盘完成回调投回事件循环。
    //
    // 不能直接执行：读盘在线程池上，回调里要动 sender_ 的成员（取块、
    // 改状态），而 sender_ 同时被 strand 上的 pump 碰。投回 io_context
    // 让它与 pump 落在同一线程上——本层没有额外加锁，全靠这个串行化。
    const PostFn post = [&ctx = ctx_](std::function<void()> fn) { asio::post(ctx, std::move(fn)); };

    (void)sender_->pump(post);

    for (auto &chunk : sender_->takeReadyChunks())
    {
        std::vector<char> body;
        if (auto st = encodeChunk(chunk.chunkIndex, chunk.data.data(), chunk.data.size(), body); !st.ok())
        {
            finish(TransferOutcome::kFailed, st);
            return;
        }

        Frame f;
        f.type = FrameType::kChunkRequestData;
        f.sessionId = sessionId_;
        f.index = static_cast<std::int64_t>(chunk.chunkIndex);
        f.to = toPeerId_;
        f.body = std::move(body);

        sendFrame(std::move(f));
        ++stats_.chunksSent;
        chunkSentAt_[chunk.chunkIndex] = std::chrono::steady_clock::now();
    }

    // 读盘出错要立刻终止。继续发只会让接收方收到一堆空洞然后校验失败，
    // 而失败原因被掩盖成"哈希不对"。
    if (auto re = sender_->readError(); !re.ok())
    {
        finish(TransferOutcome::kFailed, re);
    }
}

void TransferPeer::checkRetransmit()
{
    if (finished_ || !sender_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    // 找出"发了但超时没被确认"的块。
    //
    // 只扫 outstanding()（在飞的那部分）：pending / needRetransmit 没发过，
    // 谈不上超时。
    std::vector<std::uint32_t> timedOut;
    for (auto idx : sender_->outstandingChunks())
    {
        const auto it = chunkSentAt_.find(idx);
        if (it == chunkSentAt_.end())
        {
            continue;
        }
        if (now - it->second >= config_.retransmitTimeout)
        {
            timedOut.push_back(idx);
        }
    }

    if (timedOut.empty())
    {
        return;
    }

    sender_->markRetransmit(timedOut);
    sendReadyChunks();
}

void TransferPeer::sendCompleteRequest()
{
    if (finished_ || completeRequested_)
    {
        return;
    }
    completeRequested_ = true;
    completeRequestedAt_ = std::chrono::steady_clock::now();

    // 根在这里才算：前面 computeDigest 已经算过一遍，但要再读一次文件
    // 太浪费。改为在 startSending 时存下来——见 merkleRootHex_。
    std::vector<char> body;
    if (auto st = encodeCompleteRequest(merkleRootHex_, sender_->totalSize(), body); !st.ok())
    {
        finish(TransferOutcome::kFailed, st);
        return;
    }

    Frame f;
    f.type = FrameType::kFileRequestComplete;
    f.sessionId = sessionId_;
    f.to = toPeerId_;
    f.body = std::move(body);
    sendFrame(std::move(f));
}

// ---------------------------------------------------------------------------
// 接收方
// ---------------------------------------------------------------------------

void TransferPeer::handleRequestSend(const Frame &frame)
{
    TransferDesc desc;
    std::string err;
    if (!decodeDesc(frame.body, desc, err))
    {
        sendAnswerSend(AnswerStatus::kBadDesc, 0, err);
        ++stats_.protocolErrors;
        return;
    }

    // 落点名以描述里的 root 为准，但要经过路径校验——这是唯一能挡
    // "对端让我写到 ../../etc/passwd"的地方。
    //
    // 两种调用形态要区分开：
    //   targetPath_ 是已存在的目录 → 拼上 desc.root，落在目录里
    //   targetPath_ 是文件路径（或还不存在）→ 就是它，desc.root 只作展示名
    // 后者是"用户指定了完整落点"的场景，此时再拼一次 root 会变成
    // `target/foo.txt/foo.txt`。
    std::string target = targetPath_;
    const auto targetStat = store_.stat(targetPath_);
    const bool treatAsDir = targetStat.exists && targetStat.isDir;

    if (!desc.root.empty())
    {
        // 无论最终用不用，都要先验一遍 root：即使落到用户指定的文件名，
        // 一个含 `..` 的 root 也说明对端的行为不可信，该在握手期就拒掉。
        const auto resolved = store_.resolveWithin(store_.normalize(targetPath_), desc.root);
        if (resolved.empty())
        {
            sendAnswerSend(AnswerStatus::kBadDesc, 0, "path escapes the target directory");
            ++stats_.protocolErrors;
            return;
        }
        if (treatAsDir)
        {
            target = resolved;
        }
    }

    std::uint32_t resumeFrom = 0;
    const auto openSt = receiver_->open(desc, target, resumeFrom);

    if (openSt.code == ErrorCode::kAlreadyExists)
    {
        // 目标已存在且长度一致。这不是错误，是"本端已经有了"。
        sendAnswerSend(AnswerStatus::kTargetExists, 0, "target already exists");
        finish(TransferOutcome::kFailed, openSt, target);
        return;
    }

    if (!openSt.ok())
    {
        sendAnswerSend(openSt.code == ErrorCode::kNoSpace ? AnswerStatus::kNoSpace : AnswerStatus::kNotWritable, 0,
                       openSt.message);
        finish(TransferOutcome::kFailed, openSt, target);
        return;
    }

    targetPath_ = target;
    started_ = true;

    sendAnswerSend(AnswerStatus::kOk, resumeFrom * desc.chunkSize, {});
}

void TransferPeer::handleChunkData(const Frame &frame)
{
    if (!started_ || finished_)
    {
        return;
    }

    ChunkData chunk;
    std::string err;
    if (!decodeChunk(frame.body, chunk, err))
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "bad chunk frame: " + err));
        return;
    }

    // 写盘完成/失败都要回到 strand 上触发一次 ACK。
    //
    // 这是整层最容易漏的一环：acceptChunk 只是"受理"，块还在 WorkerPool
    // 队列里没落盘；而 buildCurrentAck() 依据的是 written_（已落盘）。
    // 若此处只把回调原样传给 receiver_，写盘完成之后没有任何东西会让
    // ackPending_ 重新变真，ACK 就永远停在受理那一刻的位图上——发送方
    // 收不到新确认，超时后重传同一批块，接收方判重返回 kAlreadyExists，
    // 于是两头都"有事在做"却永远推进不下去。这表现为 100% CPU 的活锁，
    // 而不是报错。
    //
    // 捕 weak_from_this 而不是 shared_from_this：这个回调会被 receiver_
    // 存住，而 receiver_ 是本对象的成员——强引用会绕成自我持有的环，
    // 传输结束后谁都释放不掉。
    const PostFn post = [&ctx = ctx_, weak = weak_from_this()](std::function<void()> fn)
    {
        asio::post(ctx,
                   [weak, fn = std::move(fn)]()
                   {
                       fn();

                       const auto self = weak.lock();
                       if (!self || self->finished_ || !self->receiver_)
                       {
                           return;
                       }

                       // 落盘这件事本身改变了"能确认到哪"，必须让 ACK 重发。
                       self->ackPending_ = true;
                       if (self->receiver_->allChunksReceived())
                       {
                           // 收齐了就不等 pump 的 20ms 节流，立刻回一次，
                           // 否则对端要多等一个周期才进收尾。
                           self->sendAck();
                       }
                   });
    };

    const auto st = receiver_->acceptChunk(chunk.chunkIndex, chunk.payload, chunk.payloadLen, post);

    if (st.code == ErrorCode::kAlreadyExists)
    {
        // 重传的重复块。正常路径：说明我们上一轮的 ACK 丢了。
        // 回应仍然是"再 ACK 一次"，由下面的 ackPending_ 保证。
        ++stats_.duplicateChunks;
        ackPending_ = true;
        return;
    }

    if (st.code == ErrorCode::kBusy)
    {
        // 背压。此刻**不能**回 ACK 说收到了——这一块没被受理。让它停在
        // 对端的窗口里，等我们消化完它自然会重传。
        //
        // 但也不能什么都不做：如果一直 busy，对端会一直等。这里的选择是
        // "先不回，等 pump 之后再试"——对端的重传超时会推动它。
        return;
    }

    if (!st.ok())
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, st);
        return;
    }

    ++stats_.chunksReceived;
    ackPending_ = true;

    if (receiver_->allChunksReceived())
    {
        // 收齐了，立刻回一个完整的 ACK，让对端尽快进收尾。
        sendAck();
    }
}

void TransferPeer::handleCompleteRequest(const Frame &frame)
{
    std::string rootHex;
    std::uint64_t finalSize = 0;
    std::string err;
    if (!decodeCompleteRequest(frame.body, rootHex, finalSize, err))
    {
        ++stats_.protocolErrors;
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "bad complete request: " + err));
        return;
    }

    // 收尾前必须等所有块真正落盘。
    //
    // 对端发 complete 请求只说明它"把块都发出去了"，不说明我们这边写完
    // 了：最后一个块的写盘任务可能还在 WorkerPool 队列里。此时去算根，
    // 读到的是残片末尾的一截空洞，得到的根必然不匹配——而这个不匹配会
    // 被报成"数据传坏了"，真实的进度却只是"还差几毫秒"。
    //
    // 直接返回不发应答：对端有重传/超时机制，等写完会随下一次 ACK 触发
    // 它重发 complete 请求。这不优雅，但比在原地阻塞事件循环好得多。
    if (receiver_->hasPendingWrites())
    {
        completeDeferred_ = true;
        deferredComplete_ = frame;
        return;
    }
    completeDeferred_ = false;

    // 总长不符说明两端对文件形状的理解不一致，重传也没用——直接报
    // kShapeMismatch，让上层丢弃残片重来。
    if (finalSize != receiver_->totalSize())
    {
        std::vector<char> body;
        if (encodeCompleteAnswer(CompleteStatus::kShapeMismatch, rootHex, body).ok())
        {
            Frame f;
            f.type = FrameType::kFileAnswerComplete;
            f.sessionId = sessionId_;
            f.body = std::move(body);
            sendFrame(std::move(f));
        }
        finish(TransferOutcome::kFailed, Status::Fail(ErrorCode::kProtocol, "final size mismatch"));
        return;
    }

    const auto res = receiver_->finalize(rootHex);

    std::vector<char> body;
    if (auto st = encodeCompleteAnswer(res.status, res.actualRootHex, body); !st.ok())
    {
        finish(TransferOutcome::kFailed, st);
        return;
    }

    Frame f;
    f.type = FrameType::kFileAnswerComplete;
    f.sessionId = sessionId_;
    f.body = std::move(body);
    sendFrame(std::move(f));

    if (res.status == CompleteStatus::kOk)
    {
        finish(TransferOutcome::kCompleted, Status::Ok(), receiver_->targetPath());
    }
    else
    {
        finish(TransferOutcome::kFailed, res.error, receiver_->partPath());
    }
}

void TransferPeer::sendAck()
{
    if (!receiver_ || finished_)
    {
        return;
    }

    const auto ack = receiver_->buildCurrentAck();
    std::vector<char> body;
    if (auto st = encodeAck(ack, body); !st.ok())
    {
        finish(TransferOutcome::kFailed, st);
        return;
    }

    Frame f;
    f.type = FrameType::kChunkRequestAck;
    f.sessionId = sessionId_;
    f.body = std::move(body);
    sendFrame(std::move(f));

    ++stats_.acksSent;
    ackPending_ = false;
    lastAckSent_ = std::chrono::steady_clock::now();
}

void TransferPeer::sendAnswerSend(AnswerStatus status, std::uint32_t resumeFrom, std::string_view reason)
{
    TransferAnswer answer;
    answer.status = status;
    answer.resumeOffset = resumeFrom;
    answer.reason = std::string(reason);

    std::vector<char> body;
    if (auto st = encodeAnswer(answer, body); !st.ok())
    {
        finish(TransferOutcome::kFailed, st);
        return;
    }

    Frame f;
    f.type = FrameType::kFileAnswerSend;
    f.sessionId = sessionId_;
    f.body = std::move(body);
    sendFrame(std::move(f));
}

// ---------------------------------------------------------------------------
// 共用
// ---------------------------------------------------------------------------

bool TransferPeer::onFrame(const Frame &frame)
{
    if (finished_)
    {
        return false;
    }
    if (frame.sessionId != sessionId_)
    {
        return false;
    }

    switch (frame.type)
    {
    case FrameType::kFileAnswerSend:
        if (role_ == Role::kSender)
        {
            handleAnswerSend(frame);
            return true;
        }
        break;

    case FrameType::kChunkRequestData:
        if (role_ == Role::kReceiver)
        {
            handleChunkData(frame);
            return true;
        }
        break;

    case FrameType::kChunkRequestAck:
        if (role_ == Role::kSender)
        {
            handleChunkAck(frame);
            return true;
        }
        break;

    case FrameType::kFileRequestSend:
        if (role_ == Role::kReceiver)
        {
            handleRequestSend(frame);
            return true;
        }
        break;

    case FrameType::kFileRequestComplete:
        if (role_ == Role::kReceiver)
        {
            handleCompleteRequest(frame);
            return true;
        }
        break;

    case FrameType::kFileAnswerComplete:
        if (role_ == Role::kSender)
        {
            handleAnswerComplete(frame);
            return true;
        }
        break;

    case FrameType::kFileRequestCancel:
    case FrameType::kFileAnswerCancel:
    {
        CancelReason reason = CancelReason::kUserCancelled;
        std::string msg;
        std::string err;
        (void)decodeCancel(frame.body, reason, msg, err);
        if (msg.empty())
        {
            msg = std::string(toString(reason));
        }
        finish(TransferOutcome::kPeerCancelled, Status::Fail(ErrorCode::kCancelled, msg));
        return true;
    }

    default:
        break;
    }

    return false;
}

void TransferPeer::pump()
{
    if (finished_)
    {
        return;
    }

    if (role_ == Role::kSender)
    {
        // 只有对端**答应了**才发块。
        //
        // 应答里才带续传点（resumeOffset）——不等它就发，会把对端已有的块
        // 重发一遍。而且对端在应答之前还没 open，块数据到了也会被丢弃，
        // 之后就永远补不回来了（它不在对端的 received_ 里）。
        if (!started_)
        {
            // 等应答也要有上限。此时一块都没发过，重传超时完全看不到这条
            // 传输，对端若不理（进程没起、帧丢了、sessionId 不对），
            // 就会永远停在这里。
            if (requestSent_ && std::chrono::steady_clock::now() - requestSentAt_ >= config_.answerTimeout)
            {
                finish(TransferOutcome::kPeerGone,
                       Status::Fail(ErrorCode::kTimeout, "peer did not answer the send request"));
            }
            return;
        }
        sendReadyChunks();
        checkRetransmit();

        // 收尾阶段没有块在飞，重传超时管不到这里。对端不回应答（被杀、
        // 卡在算根、或者干脆不认这个 sessionId）时必须自己有个出口，
        // 否则这条传输永远停在"等一个不会来的应答"上。
        if (completeRequested_ && std::chrono::steady_clock::now() - completeRequestedAt_ >= config_.finalizeTimeout)
        {
            finish(TransferOutcome::kPeerGone,
                   Status::Fail(ErrorCode::kTimeout, "peer did not answer the complete request"));
        }
        return;
    }

    // 压着的收尾请求：等写盘清空后再处理。
    if (completeDeferred_ && receiver_ && !receiver_->hasPendingWrites())
    {
        completeDeferred_ = false;
        handleCompleteRequest(deferredComplete_);
        return;
    }

    // 接收方：攒一小会儿再回 ACK。
    //
    // 每收一块就回一次 ACK 会在 4 MiB 块下变成"每个块多一个往返"，
    // 而 ACK 的信息量极小。攒 kAckInterval 能把多个块的确认合并成一个
    // 位图，代价是最多延迟 20ms 让对端腾窗口。
    if (ackPending_ && std::chrono::steady_clock::now() - lastAckSent_ >= kAckInterval)
    {
        sendAck();
    }
}

void TransferPeer::cancel(std::string_view reason)
{
    if (finished_)
    {
        return;
    }

    std::vector<char> body;
    if (encodeCancel(CancelReason::kUserCancelled, reason, body).ok())
    {
        Frame f;
        f.type = FrameType::kFileRequestCancel;
        f.sessionId = sessionId_;
        f.body = std::move(body);
        sendFrame(std::move(f));
    }

    if (sender_)
    {
        sender_->cancelAll();
    }
    if (receiver_)
    {
        receiver_->abort();
    }

    finish(TransferOutcome::kLocalCancelled, Status::Fail(ErrorCode::kCancelled, std::string(reason)));
}

TransferProgress TransferPeer::progress() const
{
    TransferProgress p;

    if (sender_)
    {
        p.chunksTotal = sender_->totalChunks();
        p.chunksDone = sender_->ackedChunks();
        p.bytesTotal = sender_->totalSize();
        p.bytesDone = static_cast<std::uint64_t>(p.chunksDone) * sender_->chunkSize();
        if (p.bytesDone > p.bytesTotal)
        {
            p.bytesDone = p.bytesTotal;
        }
        return p;
    }

    if (receiver_)
    {
        p.chunksTotal = receiver_->totalChunks();
        p.chunksDone = receiver_->writtenChunks();
        p.bytesTotal = receiver_->totalSize();
        p.bytesDone = static_cast<std::uint64_t>(p.chunksDone) * receiver_->chunkSize();
        if (p.bytesDone > p.bytesTotal)
        {
            p.bytesDone = p.bytesTotal;
        }
    }

    return p;
}

void TransferPeer::sendFrame(protocol::Frame frame)
{
    if (finished_)
    {
        return;
    }

    // from 一律留空。
    //
    // 这里踩过一次坑：原先各调用点写的是 f.from = session_->assignedId()，
    // 在**客户端**会话上它是对端（即自己）的 id，恰好合法；但在**服务端**
    // 会话上 assignedId() 返回的是"我给对端分配的 id"（比如 client-1），
    // 于是服务端发出的帧自称 client-1 —— checkOutbound 正确地把这当成
    // "跳过身份绑定"的迹象拒掉了，整条传输在第一个应答帧就死了。
    //
    // 空 from 的语义是"就是我发的"，接收方按连接身份认人。这是唯一
    // 在两个角色下都成立的写法，所以放在这个统一出口，调用点不必再想。
    frame.from.clear();

    // 一律用 send()，不用 forward()：本层发出的帧**始终代表本端身份**。
    // forward() 的语义是"替别人转发"，那是服务端编排层（把 client-1 的
    // 帧送给 client-9）才该用的入口，传输层从不转发。
    //
    // 服务端用 send() 发 to=<对端 id> 的帧是合法的：checkOutbound 只约束
    // from（必须是空或服务端自己的 id），to 想填谁都可以。
    if (const auto st = session_->send(frame); !st.ok())
    {
        finish(TransferOutcome::kFailed, st);
    }
}

void TransferPeer::finish(TransferOutcome outcome, Status status, std::string targetPath)
{
    if (finished_)
    {
        return;
    }
    finished_ = true;

    if (onDone_)
    {
        TransferResult result;
        result.outcome = outcome;
        result.status = std::move(status);
        result.targetPath = std::move(targetPath);
        onDone_(result);
    }
}

}  // namespace relayer::core::transfer
