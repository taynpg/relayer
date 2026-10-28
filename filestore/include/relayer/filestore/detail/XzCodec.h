#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "relayer/filestore/IFileStore.h"

// xz (LZMA2) 流式编解码，包装 liblzma。
//
// 为什么不直接暴露 lzma_stream：
//   liblzma 的 API 是"喂一块、取一块"的循环，容易写成三种典型错误——
//   忘记处理 LZMA_STREAM_END、把 avail_out 用完后没重试、close 时没 flush
//   尾巴。这里把循环封死，上层只需要 push/pull，出错时拿到 Status。
//
// 为什么不做成 push/pull 回调：
//   v1 用 std::function 回调（XzEncoder::open(sink)），调用链是
//   文件 → xz → tar → 文件，四层回调套在一起，栈深且难以单步调试。
//   v2 改成显式的对象 + 缓冲区接口：谁在什么时候读多少字节一目了然。
//
// 所有方法都可能在任意时刻返回失败（压缩器内部错误、输入损坏），
// 因此都返回 Status，不抛异常。

namespace relayer::filestore {

class XzEncoder
{
public:
    // level: 0-9，默认 6。
    //   0 = 快速、低压缩率（传输大文件且 CPU 吃紧时用）
    //   6 = 默认，压缩率与速度平衡
    //   9 = 最慢最高压缩率（体积敏感、CPU 空闲时用）
    explicit XzEncoder(int level = 6);
    ~XzEncoder();

    XzEncoder(const XzEncoder&) = delete;
    XzEncoder& operator=(const XzEncoder&) = delete;

    // 喂入原始数据。内部可能还留有未输出的压缩数据，
    // 用 read() 取走。
    Status write(const void* data, std::size_t len);

    // 取出已压缩的字节。返回写入 out 的字节数（0 表示暂时没有）。
    // 必须循环调用直到 drained() 为 true。
    std::size_t read(void* out, std::size_t outLen, Status& st);

    // 通知压缩器"输入结束"，之后 read() 会吐出剩余的尾巴。
    Status finish();

    // 是否已把全部压缩数据吐完。为 true 之后 read() 只会返回 0。
    // 注意：drained() 可能和"本次 read 返回了数据"同时为 true，
    // 所以循环写法必须是"先 append 再判断 drained"，不能反过来。
    [[nodiscard]] bool drained() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

class XzDecoder
{
public:
    XzDecoder();
    ~XzDecoder();

    XzDecoder(const XzDecoder&) = delete;
    XzDecoder& operator=(const XzDecoder&) = delete;

    // 喂入压缩数据
    Status write(const void* data, std::size_t len);

    // 取出解压后的字节。
    //
    // 返回值与 finished() 的组合含义：
    //   n > 0                有数据，继续调（循环里用）
    //   n == 0, !finished()  → 输入不够，等调用方 write() 更多数据
    //   n == 0, finished()   流正常结束
    //   st 非 Ok             ← 数据损坏或流被截断，此时直接放弃
    //
    // **截断的判定点**：故意不在"输入耗尽"时立刻报错——流式场景下输入本就会
    // 时断时续。只有调过 finish()（声明"不会再喂数据了"）之后仍然解不到结束
    // 标记，才算截断。这个判断发生在 read() 里，所以调用方必须把 read 循环
    // 一直跑到 n == 0 为止，中途 break 会错过截断错误。
    std::size_t read(void* out, std::size_t outLen, Status& st);

    // 通知解压器"输入结束"。若流不完整，之后调用 read() 会返回失败。
    Status finish();

    // 是否已解出全部数据（遇到 xz 流结束标记）
    [[nodiscard]] bool finished() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}   // namespace relayer::filestore
