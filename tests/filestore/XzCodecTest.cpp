// xz 编解码的测试。
//
// 这一层是 archive 的地基：tar.xz 的读写都建立在"能正确压缩/解压一个字节流"
// 之上。如果这里有一个边界处理错（比如最后一块没 flush、或者 LZMA_STREAM_END
// 之后还继续读），表现为"大文件解压后尾部缺几个字节"，而归档层完全看不出
// 问题出在底层。
//
// 因此本文件把各种长度都过一遍：0、1、正好一个缓冲、跨缓冲、明显大于缓冲。
//
// 另外这里钉死了两条语义，后面的 archive 层依赖它们：
//   1. 解压侧的"截断"只在 finish() 之后才判定——流式喂数据时输入本来就会
//      时断时续，不能一没数据就报错；
//   2. 因此读取循环必须一直跑到 read() 返回 0 为止，中途因为
//      finished() 就 break 会漏掉截断错误。

#include "relayer/filestore/detail/XzCodec.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "relayer/filestore/IFileStore.h"

using relayer::filestore::ErrorCode;
using relayer::filestore::Status;
using relayer::filestore::XzDecoder;
using relayer::filestore::XzEncoder;

namespace {

// 把一个 string 压缩成字节串。
// 注意要先一直 write 再 finish，然后反复 read 直到 drained——
// 这是本 API 的既定用法，测试里也照这个顺序写，顺带验证用法本身。
std::string compress(const std::string& input, int level = 6)
{
    XzEncoder enc(level);
    Status st = enc.write(input.data(), input.size());
    REQUIRE(st);
    st = enc.finish();
    REQUIRE(st);

    std::string out;
    std::vector<char> buf(4096);
    for (;;) {
        const std::size_t n = enc.read(buf.data(), buf.size(), st);
        REQUIRE(st);
        out.append(buf.data(), n);
        if (enc.drained()) {
            break;
        }
        if (n == 0) {
            // 没产出也没结束：说明编码器卡住了，属于实现 bug
            FAIL("encoder stalled before draining");
        }
    }
    return out;
}

// 解压一个完整的 xz 流。输入不完整时用 REQUIRE 直接判失败。
std::string decompress(const std::string& input)
{
    XzDecoder dec;
    Status st = dec.write(input.data(), input.size());
    REQUIRE(st);
    st = dec.finish();
    REQUIRE(st);

    std::string out;
    std::vector<char> buf(4096);
    for (;;) {
        const std::size_t n = dec.read(buf.data(), buf.size(), st);
        REQUIRE(st);   // 完整输入不该出错
        out.append(buf.data(), n);
        if (n == 0) {
            REQUIRE(dec.finished());
            break;
        }
    }
    return out;
}

// 反复 read 直到结束，返回是否报过错。
// 这是"截断检测"的标准姿势：必须跑到 read 返回 0，不能提前 break。
bool drainReportsError(XzDecoder& dec)
{
    std::vector<char> buf(4096);
    for (int guard = 0; guard < 100000; ++guard) {
        Status st;
        const std::size_t n = dec.read(buf.data(), buf.size(), st);
        if (!st) {
            return true;
        }
        if (n == 0) {
            return false;
        }
    }
    FAIL("decoder never reached a terminal state");
    return false;
}

}   // namespace

TEST_CASE("xz 往返：各种长度", "[xz][roundtrip]")
{
    // 长度选择有讲究：
    //   0      → 空输入，压缩后是纯头部，最容易漏
    //   1      → 最小有效载荷
    //   4095/4096/4097 → 正好跨内部缓冲边界
    //   100000 → 明显大于缓冲
    const std::vector<std::size_t> sizes{0, 1, 2, 100, 4095, 4096, 4097, 65535, 65536, 65537, 100000};
    for (const auto size : sizes) {
        std::string input;
        input.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            // 用可压缩但非常规的序列，避免"全 0 恰好压缩成 2 字节"这种特例
            input.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
        }

        const std::string packed = compress(input);
        const std::string back = decompress(packed);
        INFO("size = " << size);
        REQUIRE(back.size() == input.size());
        REQUIRE(back == input);
    }
}

TEST_CASE("xz 往返：全零与高重复内容", "[xz][roundtrip]")
{
    // 高重复内容是压缩器最容易走"特殊路径"的地方（长匹配、字典溢出）
    const std::string zeros(200000, '\0');
    REQUIRE(decompress(compress(zeros)) == zeros);

    const std::string repeated = [] {
        std::string s;
        for (int i = 0; i < 5000; ++i) {
            s += "relayer ";
        }
        return s;
    }();
    REQUIRE(decompress(compress(repeated)) == repeated);
}

TEST_CASE("xz 往返：随机二进制", "[xz][roundtrip]")
{
    // 不可压缩的随机数据：压缩后体积略增。验证不会因为"压缩没效果"出错。
    std::vector<char> raw(50000);
    std::uint32_t state = 0x12345678;
    for (auto& c : raw) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>((state >> 16) & 0xFF);
    }
    const std::string input(raw.data(), raw.size());
    const std::string back = decompress(compress(input));
    REQUIRE(back == input);
}

TEST_CASE("xz 各级别都能往返", "[xz][roundtrip][level]")
{
    const std::string input = "the quick brown fox jumps over the lazy dog";
    for (int level = 0; level <= 9; ++level) {
        INFO("level = " << level);
        REQUIRE(decompress(compress(input, level)) == input);
    }
}

TEST_CASE("xz 压缩确实缩小了可压缩内容", "[xz][ratio]")
{
    // 不只是"能往返"：如果实现把原始数据原样透传，往返也会成功。
    // 这条用例确保压缩真的在工作。
    const std::string input(100000, 'A');
    const std::string packed = compress(input);
    REQUIRE(packed.size() < input.size() / 10);
}

TEST_CASE("xz 解压被截断的流会失败", "[xz][error]")
{
    const std::string input(50000, 'B');
    const std::string packed = compress(input);
    REQUIRE(packed.size() > 20);

    // 砍掉尾部一半（顺带丢掉 xz 的结束标记）
    const std::string truncated = packed.substr(0, packed.size() / 2);

    XzDecoder dec;
    REQUIRE(dec.write(truncated.data(), truncated.size()));
    REQUIRE(dec.finish());

    // 截断必须被识别为错误，而不是"安静地解出一半"
    REQUIRE(drainReportsError(dec));
}

TEST_CASE("xz 解压非 xz 数据会失败", "[xz][error]")
{
    const std::string garbage(1000, 'x');
    XzDecoder dec;
    REQUIRE(dec.write(garbage.data(), garbage.size()));
    REQUIRE(dec.finish());
    REQUIRE(drainReportsError(dec));
}

TEST_CASE("xz 尾部缺一个字节也算截断", "[xz][error]")
{
    // 只砍掉最后 1 个字节：CRC 和结束标记都不完整。
    // 这条比"砍一半"更苛刻——很多实现能察觉大段缺失，却对差一字节沉默。
    const std::string input(20000, 'C');
    const std::string packed = compress(input);
    REQUIRE(packed.size() > 32);
    const std::string truncated = packed.substr(0, packed.size() - 1);

    XzDecoder dec;
    REQUIRE(dec.write(truncated.data(), truncated.size()));
    REQUIRE(dec.finish());
    REQUIRE(drainReportsError(dec));
}

TEST_CASE("xz 没调 finish 就不该报截断", "[xz][streaming]")
{
    // 流式场景：数据还没喂完，read 拿不到东西是正常的，
    // 不能据此判定失败。这条钉死"截断只在 finish 之后才判定"。
    const std::string input(10000, 'D');
    const std::string packed = compress(input);

    XzDecoder dec;
    REQUIRE(dec.write(packed.data(), packed.size() / 2));

    std::vector<char> buf(64);
    for (int i = 0; i < 50; ++i) {
        Status st;
        dec.read(buf.data(), buf.size(), st);
        REQUIRE(st);   // 一半输入 → 读到某处就没数据了，但不是错误
    }
    REQUIRE_FALSE(dec.finished());
}

TEST_CASE("xz 分小块喂入能解出完整内容", "[xz][streaming]")
{
    // 调用方可能按网络包大小（远小于 64KB）分块喂数据。
    // 如果实现只处理"一次喂完"的情形，分块时就会出错。
    std::string input;
    for (int i = 0; i < 5000; ++i) {
        input.push_back(static_cast<char>('a' + (i % 26)));
    }
    const std::string packed = compress(input);

    XzDecoder dec;
    std::string out;
    std::vector<char> buf(16);
    // 每次只喂 7 个字节，制造大量"输入耗尽但要继续读"的情形
    for (std::size_t off = 0; off < packed.size(); off += 7) {
        const std::size_t n = std::min<std::size_t>(7, packed.size() - off);
        REQUIRE(dec.write(packed.data() + off, n));
        for (;;) {
            Status st;
            const std::size_t got = dec.read(buf.data(), buf.size(), st);
            REQUIRE(st);
            out.append(buf.data(), got);
            if (got == 0) {
                break;   // 当前输入吃完了，回去再喂
            }
        }
    }
    REQUIRE(dec.finish());
    // finish 之后把最后的尾巴取完
    for (;;) {
        Status st;
        const std::size_t got = dec.read(buf.data(), buf.size(), st);
        REQUIRE(st);
        out.append(buf.data(), got);
        if (got == 0) {
            break;
        }
    }
    REQUIRE(dec.finished());
    REQUIRE(out == input);
}

TEST_CASE("xz 空输出缓冲区不消耗输入", "[xz][edge]")
{
    XzEncoder enc;
    REQUIRE(enc.write("abc", 3));
    REQUIRE(enc.finish());
    Status st;
    // 传 0 长度缓冲区：应当返回 0 且不破坏内部状态
    REQUIRE(enc.read(nullptr, 0, st) == 0);
    REQUIRE(st);

    // 之后仍然能正常吐出压缩数据
    std::vector<char> buf(1024);
    const std::size_t n = enc.read(buf.data(), buf.size(), st);
    REQUIRE(st);
    REQUIRE(n > 0);
}
