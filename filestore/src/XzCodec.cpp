#include "relayer/filestore/detail/XzCodec.h"

#include <cstring>
#include <lzma.h>
#include <vector>

namespace relayer::filestore
{

namespace
{

// 输入缓冲区上限。liblzma 不复制输入，只借用指针。
// 这里留一份拷贝是因为调用方的 buffer 生命周期往往比 write() 调用短，
// 而 lzma_code 可能只消费了一部分。
constexpr std::size_t kInChunk = 64 * 1024;

// liblzma 的版本间 API 有差异（lzma_strerror 是 5.4 之后才有的），
// 自己维护一张映射表，既避免版本依赖，也让日志文案可控。
const char *lzmaRetName(lzma_ret ret)
{
    switch (ret)
    {
    case LZMA_OK:
        return "ok";
    case LZMA_STREAM_END:
        return "stream end";
    case LZMA_NO_CHECK:
        return "no integrity check";
    case LZMA_UNSUPPORTED_CHECK:
        return "unsupported integrity check";
    case LZMA_GET_CHECK:
        return "get integrity check";
    case LZMA_MEM_ERROR:
        return "out of memory";
    case LZMA_MEMLIMIT_ERROR:
        return "memory limit exceeded";
    case LZMA_FORMAT_ERROR:
        return "not an xz stream";
    case LZMA_OPTIONS_ERROR:
        return "unsupported options";
    case LZMA_DATA_ERROR:
        return "corrupted data";
    case LZMA_BUF_ERROR:
        return "truncated stream";
    case LZMA_PROG_ERROR:
        return "internal error";
    default:
        return "unknown error";
    }
}

Status lzmaError(lzma_ret ret, const char *op)
{
    const std::string msg = std::string("xz ") + op + " failed: " + lzmaRetName(ret);
    if (ret == LZMA_MEM_ERROR || ret == LZMA_MEMLIMIT_ERROR)
    {
        return Status::Fail(ErrorCode::kNoSpace, msg);
    }
    if (ret == LZMA_FORMAT_ERROR || ret == LZMA_DATA_ERROR || ret == LZMA_OPTIONS_ERROR)
    {
        // 内容损坏或不是 xz 流：归为"输入无效"，调用方据此提示
        // "归档已损坏"而不是"磁盘有问题"
        return Status::Fail(ErrorCode::kInvalidPath, msg);
    }
    return Status::Fail(ErrorCode::kIoError, msg);
}

}  // namespace

// ---------------------------------------------------------------------------
// XzEncoder
// ---------------------------------------------------------------------------

struct XzEncoder::Impl
{
    lzma_stream strm = LZMA_STREAM_INIT;
    std::vector<std::uint8_t> pending;  // 尚未被压缩器消费的输入
    bool finished = false;              // 已调用 finish
    bool drained = false;               // 输出已取完
    bool failed = false;                // 已经报过错
};

XzEncoder::XzEncoder(int level)
    : impl_(new Impl)
{
    const int clamped = level < 0 ? 0 : (level > 9 ? 9 : level);
    const lzma_ret ret = lzma_easy_encoder(&impl_->strm, static_cast<std::uint32_t>(clamped), LZMA_CHECK_CRC64);
    // 构造不能返回 Status，初始化失败后续 read/finish 会报错。
    // 这种情况只可能在 OOM 时发生，不值得为它引入异常。
    if (ret != LZMA_OK)
    {
        impl_->drained = true;
        impl_->failed = true;
    }
    impl_->pending.reserve(kInChunk);
}

XzEncoder::~XzEncoder()
{
    lzma_end(&impl_->strm);
    delete impl_;
}

Status XzEncoder::write(const void *data, std::size_t len)
{
    if (impl_->failed)
    {
        return Status::Fail(ErrorCode::kIoError, "xz: encoder is in failed state");
    }
    if (impl_->finished)
    {
        return Status::Fail(ErrorCode::kIoError, "xz: write after finish");
    }
    if (len == 0)
    {
        return Status::Ok();
    }
    const auto *p = static_cast<const std::uint8_t *>(data);
    impl_->pending.insert(impl_->pending.end(), p, p + len);
    return Status::Ok();
}

std::size_t XzEncoder::read(void *out, std::size_t outLen, Status &st)
{
    st = Status::Ok();
    if (impl_->drained)
    {
        return 0;
    }
    if (outLen == 0)
    {
        return 0;
    }
    impl_->strm.next_out = static_cast<std::uint8_t *>(out);
    impl_->strm.avail_out = outLen;
    impl_->strm.next_in = impl_->pending.data();
    impl_->strm.avail_in = impl_->pending.size();

    const lzma_action action = impl_->finished ? LZMA_FINISH : LZMA_RUN;
    const lzma_ret ret = lzma_code(&impl_->strm, action);

    // 消费掉的输入要移出 pending
    const std::size_t consumed = impl_->pending.size() - impl_->strm.avail_in;
    if (consumed > 0)
    {
        impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    }

    if (ret == LZMA_STREAM_END)
    {
        impl_->drained = true;
    }
    else if (ret == LZMA_BUF_ERROR && !impl_->finished)
    {
        // LZMA_RUN 下的 BUF_ERROR 只是"这一轮没有进展"——输入被吃完了、
        // 压缩器在等更多数据。编码是流式的，调用方随时会再 write(),
        // 所以这不是错误。只有 FINISH 之后还 BUF_ERROR 才是真出错。
    }
    else if (ret != LZMA_OK)
    {
        st = lzmaError(ret, "encode");
        impl_->failed = true;
        impl_->drained = true;
        return 0;
    }
    return outLen - impl_->strm.avail_out;
}

Status XzEncoder::finish()
{
    if (impl_->failed)
    {
        return Status::Fail(ErrorCode::kIoError, "xz: encoder is in failed state");
    }
    impl_->finished = true;
    return Status::Ok();
}

bool XzEncoder::drained() const noexcept
{
    return impl_->drained;
}

// ---------------------------------------------------------------------------
// XzDecoder
// ---------------------------------------------------------------------------

struct XzDecoder::Impl
{
    lzma_stream strm = LZMA_STREAM_INIT;
    std::vector<std::uint8_t> pending;
    bool eofSignalled = false;
    bool finished = false;
    bool failed = false;  // 已经报过错，后续 read 一律返回 0 Ok
};

XzDecoder::XzDecoder()
    : impl_(new Impl)
{
    // 用 256MB 而不是 UINT64_MAX 作为内存上限：xz 的头部可以声明极大的
    // 字典，恶意构造的小文件能让解压器申请几个 GB。超过上限则
    // LZMA_MEMLIMIT_ERROR——对文件传输场景足够。
    const lzma_ret ret = lzma_stream_decoder(&impl_->strm, 256ull * 1024 * 1024, LZMA_CONCATENATED);
    // 构造不能返回 Status，只能把失败延后到 read()：
    // 这里把 finished 置位，read() 会因为 finished 直接返回 0，
    // 调用方看到"没数据也没错"就结束了——所以必须同时记下构造失败。
    if (ret != LZMA_OK)
    {
        impl_->finished = true;
        impl_->failed = true;
    }
    impl_->pending.reserve(kInChunk);
}

XzDecoder::~XzDecoder()
{
    lzma_end(&impl_->strm);
    delete impl_;
}

Status XzDecoder::write(const void *data, std::size_t len)
{
    if (impl_->eofSignalled)
    {
        return Status::Fail(ErrorCode::kIoError, "xz: write after finish");
    }
    if (len == 0)
    {
        return Status::Ok();
    }
    const auto *p = static_cast<const std::uint8_t *>(data);
    impl_->pending.insert(impl_->pending.end(), p, p + len);
    return Status::Ok();
}

std::size_t XzDecoder::read(void *out, std::size_t outLen, Status &st)
{
    st = Status::Ok();
    // 出错后不再改动 strm 状态：liblzma 在错误之后的行为未定义，
    // 重复调用只会把错误码变得不可预测。直接短路返回。
    if (impl_->failed || impl_->finished)
    {
        return 0;
    }
    if (outLen == 0)
    {
        return 0;
    }

    std::uint8_t *const base = static_cast<std::uint8_t *>(out);
    std::size_t total = 0;
    for (;;)
    {
        impl_->strm.next_out = base + total;
        impl_->strm.avail_out = outLen - total;
        impl_->strm.next_in = impl_->pending.data();
        impl_->strm.avail_in = impl_->pending.size();

        const lzma_action action = impl_->eofSignalled ? LZMA_FINISH : LZMA_RUN;
        const lzma_ret ret = lzma_code(&impl_->strm, action);

        const std::size_t consumed = impl_->pending.size() - impl_->strm.avail_in;
        if (consumed > 0)
        {
            impl_->pending.erase(impl_->pending.begin(),
                                 impl_->pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
        total = outLen - impl_->strm.avail_out;

        if (ret == LZMA_STREAM_END)
        {
            impl_->finished = true;
            return total;
        }
        if (ret == LZMA_BUF_ERROR)
        {
            // 这个码本身的含义只是"这一轮没有进展"，liblzma 在 LZMA_RUN 下
            // 也可能返回它。只有声明过"不会再有输入"之后还没见结束标记，
            // 才真正等于截断。
            if (impl_->eofSignalled)
            {
                st = Status::Fail(ErrorCode::kInvalidPath, "xz: truncated stream (input ended before end marker)");
                impl_->failed = true;
                impl_->finished = true;
            }
            return total;
        }
        if (ret != LZMA_OK)
        {
            st = lzmaError(ret, "decode");
            impl_->failed = true;
            impl_->finished = true;
            return total;
        }

        // 到这里 ret == LZMA_OK。三种情况：
        //   a) 输出缓冲填满 → 交给调用方取走，下次接着读；
        //   b) 输入耗尽、还没 finish → 正常，等调用方 write 更多数据；
        //   c) 输入耗尽、已 finish、但本轮毫无进展。
        //
        // c) 必须再转一圈才拿得到错误：liblzma 会把第一次"零进展"吞成
        // LZMA_OK（见 liblzma/common/common.c 里 allow_buf_error 的处理），
        // 第二次才返回 LZMA_BUF_ERROR。少转这一圈，截断的归档就会被
        // 当成"正常读完"——正是本模块最怕的那种静默错误。
        const bool outFull = impl_->strm.avail_out == 0;
        const bool noProgress = (total == 0 && consumed == 0);
        if (outFull || !(impl_->eofSignalled && noProgress))
        {
            return total;
        }
    }
}

Status XzDecoder::finish()
{
    impl_->eofSignalled = true;
    return Status::Ok();
}

bool XzDecoder::finished() const noexcept
{
    return impl_->finished;
}

}  // namespace relayer::filestore
