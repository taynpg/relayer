#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "relayer/filestore/IFileStore.h"

// ustar tar 的读写，含 PAX 长路径扩展。
//
// 为什么自己做而不引第三方 tar 库：
//   需求只有"写一串文件、再按顺序读回来"，不需要符号链接、设备号、
//   sparse 文件、增量归档这些边角。第三方库（libarchive 等）为了这些
//   能力把 API 做得很大，绑定进来编译时间与依赖都变重。这里 300 行
//   覆盖了实际用得到的全部格式细节，而且不依赖任何全局状态。
//
// 格式边界（**刻意不支持**，不是遗漏）：
//   - 符号链接 / 硬链接：一律按普通文件读，链接指向的内容会被忽略；
//   - 设备号、sparse 文件：devmajor/devminor 写 0，读取时忽略；
//   - GNU 长文件名（'L' 类型）：只认 PAX（'x'）。GNU 格式的包由
//     tar 自己解，我们打出来的包一律走 PAX，所以只在读别人的包时
//     会遇到——那时按"名字被截断"处理，不报错。
//
// 与 v1 的差别：
//   v1 的 TarWriter/TarReader 用 std::function 回调套 sink/source，
//   和 XzCodec 又是回调，压栈四层。v2 改成"显式的缓冲区接口"：
//   写侧返回字节给人搬，读侧由人喂字节。谁在什么时候搬多少一目了然。
//
// 所有方法返回 Status，不抛异常。

namespace relayer::filestore
{

// tar 的块大小。所有头、数据、填充都以它为单位。
inline constexpr std::size_t kTarBlock = 512;

// 一个 tar 头的解析结果。
struct TarHeader
{
    std::string name;      // 已把 prefix/name 拼好，也应用过 PAX 覆盖
    char typeflag{};       // '0' 普通文件，'5' 目录，'x' PAX 扩展头
    std::uint16_t mode{};  // 权限位（低 12 位）
    std::uint64_t size{};  // 数据区长度（不含填充）
    std::int64_t mtime{};  // Unix epoch 秒
    bool isPax{};
    bool isSymlink{};  // '1'/'2'：调用方应当忽略其内容
};

class TarWriter
{
public:
    TarWriter() = default;
    ~TarWriter() = default;

    TarWriter(const TarWriter &) = delete;
    TarWriter &operator=(const TarWriter &) = delete;

    // 追加一个条目的头。名字超过 100 字节时自动插入 PAX 扩展头。
    // size 是数据区长度，调用方随后用 writeData 写满这么多字节，
    // 或者用 skipData 代替（目录条目不需要数据）。
    Status addEntry(std::string_view name, char typeflag, std::uint16_t mode, std::uint64_t size, std::int64_t mtime);

    // 写数据区。会自动在末尾补齐到 512 的整数倍。
    // 调用方必须保证 total 与 addEntry 里声明的 size 一致，
    // 否则后续条目全部错位——这里的检查只能挡住"多写"，挡不住"少写"。
    Status writeData(const void *data, std::size_t len);

    // 补齐当前条目的数据区到块边界。写目录或跳过内容时用。
    Status finishData();

    // 写归档结束标记（两个全零块）。必须在所有条目之后调用一次。
    Status close();

    // 取走已生成的字节。返回写入 out 的字节数。
    // 与 XzEncoder 一样：循环读到返回 0 为止。
    std::size_t read(void *out, std::size_t outLen);

    // 是否还有待取的字节。close() 之后才可能为 false。
    [[nodiscard]] bool drained() const noexcept;

private:
    Status emit(const void *data, std::size_t len);
    Status padToBlock();

    std::vector<std::uint8_t> buf_;  // 待取走的字节
    std::size_t readPos_ = 0;
    // declaredSize_ / written_ 只描述**当前条目**的数据区。
    // 填充必须按 written_ 算（当前条目里的偏移），不能按"整条归档的
    // 累计偏移"算——那样第二个条目起就会多补一块，整个归档错位 512 字节，
    // 而且读侧看到的第一个条目仍然是对的，很难定位。
    std::uint64_t declaredSize_ = 0;
    std::uint64_t written_ = 0;
    bool padded_ = false;  // 当前条目已补过 512 对齐
    bool closed_ = false;
};

class TarReader
{
public:
    TarReader() = default;
    ~TarReader() = default;

    TarReader(const TarReader &) = delete;
    TarReader &operator=(const TarReader &) = delete;

    // 喂入归档字节。
    Status write(const void *data, std::size_t len);

    // 通知"不会再喂数据了"。之后若还有半个头，readHeader 会报失败。
    void finishInput() noexcept;

    // 读下一个条目的头。
    //
    // 返回值约定（三种情况必须能区分开，否则流式解压会把"还没喂完"
    // 当成"归档结束"，解出一半文件却报成功）：
    //   - Status 为 kOk 且 out.name 非空  → 拿到一个条目；
    //   - Status 为 kOk 且 out.name 为空  → 遇到结束标记，归档读完；
    //   - Status 为 kUnsupported          → 输入不够，先 write() 更多数据再重试；
    //   - 其它 Status                     → 归档损坏。
    Status readHeader(TarHeader &out);

    // 读当前条目的数据区。dataLeft 是还剩多少字节没读。
    // 返回 0 且 Status 为 Ok 表示输入不够，应继续 write() 后重试。
    std::size_t readData(void *out, std::size_t outLen, Status &st);

    // 读当前条目的完整数据区到 out。
    // 输入不够时返回 kUnsupported（"need more input"），调用方
    // 补数据后**重新调用本方法**——内部不推进读位置，可以安全重试。
    // 这是 PAX 负载唯一的读法：负载不大（最多几百字节），值得为
    // "可重试"多一次拷贝，换掉"读到一半发现没数据"的麻烦。
    Status readAllData(std::vector<std::uint8_t> &out);

    // 跳过当前条目剩余的数据与填充。
    Status skipData();

    // 设置"下一个条目的真实名字"。由调用方在读完 PAX（'x'）条目后调用，
    // 用来覆盖 ustar 头里那个被截断到 100 字节的名字。
    //
    // 一般不用直接调：applyPaxPayload 已经做了这件事。留着它是为了
    // 应对将来可能的其它扩展头格式（GNU 'L'）。
    void overrideNextName(std::string name);

    // 解析一段扩展头负载，并记为"下一个条目的名字"。
    // 返回解析出的路径；负载里没有路径信息时返回空串（此时不改动状态）。
    //
    // typeflag 决定格式：
    //   'x' / 'X' → PAX 记录，形如 `LEN path=VALUE\n`
    //   'L'       → GNU 长文件名，整个负载就是路径（可能带结尾 NUL）
    //   其它       → 忽略
    //
    // 为什么两种都认：我们写包用 PAX，但 GNU tar 默认写 'L'。
    // 只认一种的话，用户拿系统 tar 打的包在我们这里长路径会被
    // 静默截断到 100 字节——两个深路径塌成同一个名字、互相覆盖，
    // 而且任何一个环节都不报错。
    std::string applyPaxPayload(char typeflag, std::string_view payload);

    // 当前条目还剩多少数据没读
    [[nodiscard]] std::uint64_t dataLeft() const noexcept;

private:
    Status fill(std::size_t need);

    std::vector<std::uint8_t> buf_;
    std::size_t readPos_ = 0;
    std::uint64_t dataLeft_ = 0;  // 当前条目剩余数据字节
    std::uint64_t padLeft_ = 0;   // 当前条目剩余填充字节
    bool eofInput_ = false;
    bool sawEndMarker_ = false;
    // 正在解析的上一个 PAX 覆盖项
    std::string paxPath_;
};

}  // namespace relayer::filestore
