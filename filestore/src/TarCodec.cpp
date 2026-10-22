#include "relayer/filestore/detail/TarCodec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace relayer::filestore
{

namespace
{

// ustar 头的字段布局（POSIX 1003.1-1990，512 字节）。
// 用 struct + static_assert 表达比逐偏移量硬编码安全：
// 少写一个字段编译期就能发现，而不是等到读别人的包时才错位。
struct UstarHeader
{
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];    // "ustar\0"
    char version[2];  // "00"
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(UstarHeader) == kTarBlock, "ustar header must be 512 bytes");

constexpr std::size_t kNameField = sizeof(UstarHeader::name);      // 100
constexpr std::size_t kPrefixField = sizeof(UstarHeader::prefix);  // 155

// 把 value 写成定长八进制字段：零填充、右对齐、最后一位留 NUL。
//
// 为什么不用 snprintf("%llo")：
//   snprintf 遇到底层字段宽度不够时会截断但不报错，而且我要的
//   "右对齐后把剩余位置补 0" 它做不到。手写逻辑更短也更明确。
// 为什么字段末尾是 NUL 而不是空格：
//   POSIX 规定两种都合法（GNU tar 用空格，star 用 NUL）。读侧
//   两者都认，写侧选 NUL——这样 strnlen 能直接拿到长度。
void writeOctal(char *dst, std::size_t len, std::uint64_t value)
{
    std::memset(dst, '0', len);
    if (len == 0)
    {
        return;
    }
    std::size_t pos = len - 1;  // 给 NUL 留一位
    while (value > 0 && pos > 0)
    {
        dst[--pos] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    dst[len - 1] = '\0';
}

// 解析定长八进制字段。允许尾部是 NUL 或空格（GNU tar 写空格）。
// 也接受 base-256（最高位为 1）——那是 >8GB 文件的编码方式，
// 我们自己不写，但读别人的包时不该崩。
std::uint64_t readNumeric(const char *field, std::size_t len)
{
    if (len == 0)
    {
        return 0;
    }
    const auto first = static_cast<unsigned char>(field[0]);
    if ((first & 0x80U) != 0)
    {
        // base-256：去掉标志位，其余是大端整数
        std::uint64_t v = first & 0x7FU;
        for (std::size_t i = 1; i < len; ++i)
        {
            v = (v << 8) | static_cast<unsigned char>(field[i]);
        }
        return v;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < len; ++i)
    {
        const char c = field[i];
        if (c == '\0' || c == ' ')
        {
            // 前导空格可以跳过，遇到结束就停
            if (v == 0)
            {
                continue;
            }
            break;
        }
        if (c < '0' || c > '7')
        {
            break;  // 垃圾数据：容忍，取已解析的部分
        }
        v = (v << 3) | static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

// 计算头的校验和。规则：chksum 字段先填满空格，再把全部 512 字节
// 当作无符号字节求和。
std::uint32_t computeChecksum(const UstarHeader &h)
{
    UstarHeader copy = h;
    std::memset(copy.chksum, ' ', sizeof(copy.chksum));
    std::uint32_t sum = 0;
    const auto *p = reinterpret_cast<const unsigned char *>(&copy);
    for (std::size_t i = 0; i < sizeof(copy); ++i)
    {
        sum += p[i];
    }
    return sum;
}

// chksum 字段的特殊格式：6 位八进制 + NUL + 空格。
// 这一点和别的数字字段都不一样——别的字段末位是 NUL，这里是
// "NUL + 空格"。写错的话自家读侧可能放行（校验时字段被替换成空格），
// 只有外部 tar 会报 "checksum error"，所以必须用外部工具验证。
void writeChecksum(char *dst, std::size_t len, std::uint32_t sum)
{
    std::memset(dst, '0', len);
    // 6 位八进制，右对齐到 len-2 位置，最后两位是 NUL + 空格
    std::size_t pos = len - 2;
    std::uint32_t v = sum;
    while (v > 0 && pos > 0)
    {
        dst[--pos] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    }
    if (len >= 2)
    {
        dst[len - 2] = '\0';
        dst[len - 1] = ' ';
    }
}

// 构造一条 PAX 记录：`LEN key=value\n`，其中 LEN 是整条记录的长度
// （包含 LEN 自身的十进制数字）。
//
// 长度要自指，所以得迭代到不动点。看似能用公式算，但数字位数的
// 边界（99→100 时多一位）会让公式算出的值恰好跨位，迭代两轮最稳。
std::string makePaxRecord(std::string_view key, std::string_view value)
{
    const std::string body = std::string(key) + "=" + std::string(value) + "\n";
    std::size_t total = body.size() + 4;
    for (;;)
    {
        std::size_t digits = 1;
        for (std::size_t t = total; t >= 10; t /= 10)
        {
            ++digits;
        }
        const std::size_t next = digits + 1 + body.size();
        if (next == total)
        {
            break;
        }
        total = next;
    }
    return std::to_string(total) + " " + body;
}

// 从 PAX 负载里取出 path= 那一条。找不到返回 false。
// 其它记录（atime/mtime/uid/gid/comment/SCHILY.*…）一律忽略——我们只在乎名字。
//
// 记录格式：`<总长度> <key>=<value>\n`，总长度含自身的十进制数字。
// 这是 PAX 唯一的自描述机制，长度不对就意味着负载被篡改或截断，
// 此时放弃剩余部分（已经解析出来的项仍然有效）。
bool parsePaxPath(std::string_view payload, std::string &out)
{
    std::size_t pos = 0;
    while (pos < payload.size())
    {
        const std::size_t sp = payload.find(' ', pos);
        if (sp == std::string_view::npos)
        {
            return !out.empty();
        }
        std::size_t recLen = 0;
        for (std::size_t i = pos; i < sp; ++i)
        {
            const char c = payload[i];
            if (c < '0' || c > '9')
            {
                return !out.empty();
            }
            recLen = recLen * 10 + static_cast<std::size_t>(c - '0');
        }
        // recLen 必须覆盖到换行，且不能超出负载
        if (recLen <= sp - pos + 1 || pos + recLen > payload.size())
        {
            return !out.empty();
        }
        // 去掉末尾的 '\n'
        const std::string_view rec = payload.substr(sp + 1, pos + recLen - sp - 2);
        const std::size_t eq = rec.find('=');
        if (eq != std::string_view::npos && rec.substr(0, eq) == "path")
        {
            out.assign(rec.substr(eq + 1));
        }
        pos += recLen;
    }
    return !out.empty();
}

std::string paxPathOf(std::string_view payload)
{
    std::string out;
    parsePaxPath(payload, out);
    return out;
}

std::string fieldToString(const char *field, std::size_t len)
{
    std::size_t n = 0;
    while (n < len && field[n] != '\0')
    {
        ++n;
    }
    return std::string(field, n);
}

// 名字能不能只用一个 ustar 头装下。
// 两种装法：整段塞 name（≤100），或者拆成 prefix + "/" + name。
bool fitsUstar(std::string_view name, std::string &prefixOut, std::string &nameOut)
{
    if (name.size() <= kNameField)
    {
        prefixOut.clear();
        nameOut.assign(name);
        return true;
    }
    // 从右往左找斜杠，让 suffix 尽量长（这样 name 字段装得下）
    const std::size_t slash = name.rfind('/');
    if (slash == std::string_view::npos || slash > kPrefixField || slash == 0)
    {
        return false;
    }
    const std::string_view prefix = name.substr(0, slash);
    const std::string_view suffix = name.substr(slash + 1);
    if (prefix.size() > kPrefixField || suffix.size() > kNameField || suffix.empty())
    {
        return false;
    }
    prefixOut.assign(prefix);
    nameOut.assign(suffix);
    return true;
}

bool allZero(const std::uint8_t *p, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i)
    {
        if (p[i] != 0)
        {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// TarWriter
// ---------------------------------------------------------------------------

Status TarWriter::emit(const void *data, std::size_t len)
{
    const auto *p = static_cast<const std::uint8_t *>(data);
    buf_.insert(buf_.end(), p, p + len);
    return Status::Ok();
}

Status TarWriter::padToBlock()
{
    const std::uint64_t rem = (kTarBlock - (written_ % kTarBlock)) % kTarBlock;
    if (rem == 0)
    {
        return Status::Ok();
    }
    const std::array<std::uint8_t, kTarBlock> zero{};
    return emit(zero.data(), static_cast<std::size_t>(rem));
}

Status TarWriter::addEntry(std::string_view name, char typeflag, std::uint16_t mode, std::uint64_t size,
                           std::int64_t mtime)
{
    if (closed_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: addEntry after close");
    }
    // 上一个条目没写完就开新条目 → 必然错位，宁可在这里失败
    if (written_ != declaredSize_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: previous entry data incomplete");
    }
    if (name.empty())
    {
        return Status::Fail(ErrorCode::kInvalidPath, "tar: empty entry name");
    }

    std::string prefix;
    std::string shortName;
    if (!fitsUstar(name, prefix, shortName))
    {
        // 装不下 → 先写一条 PAX 'x' 记录带上完整路径，
        // 再写一个把名字截断到 100 字节的常规头。
        // 读侧会以 PAX 里的路径为准覆盖掉截断的名字。
        const std::string rec = makePaxRecord("path", name);
        UstarHeader pax{};
        const std::string_view paxName = "PaxHeaders/entry";
        std::memcpy(pax.name, paxName.data(), paxName.size());
        writeOctal(pax.mode, sizeof(pax.mode), mode);
        writeOctal(pax.size, sizeof(pax.size), rec.size());
        writeOctal(pax.mtime, sizeof(pax.mtime), static_cast<std::uint64_t>(mtime < 0 ? 0 : mtime));
        pax.typeflag = 'x';
        std::memcpy(pax.magic, "ustar", 6);
        pax.version[0] = '0';
        pax.version[1] = '0';
        writeChecksum(pax.chksum, sizeof(pax.chksum), computeChecksum(pax));
        Status st = emit(&pax, sizeof(pax));
        if (!st)
        {
            return st;
        }
        st = emit(rec.data(), rec.size());
        if (!st)
        {
            return st;
        }
        const std::size_t rem = (kTarBlock - (rec.size() % kTarBlock)) % kTarBlock;
        if (rem != 0)
        {
            const std::array<std::uint8_t, kTarBlock> zero{};
            st = emit(zero.data(), rem);
            if (!st)
            {
                return st;
            }
        }
        // 常规头里放截断名，只是为了兼容不认 PAX 的读取器
        shortName.assign(name.substr(0, kNameField));
        prefix.clear();
    }

    UstarHeader h{};
    std::memcpy(h.name, shortName.data(), shortName.size());
    if (!prefix.empty())
    {
        std::memcpy(h.prefix, prefix.data(), prefix.size());
    }
    writeOctal(h.mode, sizeof(h.mode), mode & 07777U);
    writeOctal(h.uid, sizeof(h.uid), 0);
    writeOctal(h.gid, sizeof(h.gid), 0);
    writeOctal(h.size, sizeof(h.size), size);
    writeOctal(h.mtime, sizeof(h.mtime), static_cast<std::uint64_t>(mtime < 0 ? 0 : mtime));
    h.typeflag = typeflag;
    std::memcpy(h.magic, "ustar", 6);
    h.version[0] = '0';
    h.version[1] = '0';
    std::memcpy(h.uname, "relayer", 7);
    std::memcpy(h.gname, "relayer", 7);
    writeChecksum(h.chksum, sizeof(h.chksum), computeChecksum(h));

    Status st = emit(&h, sizeof(h));
    if (!st)
    {
        return st;
    }
    declaredSize_ = size;
    written_ = 0;
    padded_ = false;
    return Status::Ok();
}

Status TarWriter::writeData(const void *data, std::size_t len)
{
    if (closed_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: writeData after close");
    }
    if (written_ + len > declaredSize_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: writeData exceeds declared size");
    }
    written_ += len;
    return emit(data, len);
}

Status TarWriter::finishData()
{
    if (closed_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: finishData after close");
    }
    if (written_ != declaredSize_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: data size mismatch");
    }
    Status st = padToBlock();
    if (st)
    {
        padded_ = true;
    }
    return st;
}

Status TarWriter::close()
{
    if (closed_)
    {
        return Status::Ok();
    }
    if (written_ != declaredSize_)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: closing with incomplete entry");
    }
    // 正常路径下 finishData() 已经补过填充了。没补的情况有两种：
    // 只写了头没写数据（目录条目），或者压根没写条目。
    // 用 padded_ 标记判断，避免重复补——多补一块会让整个归档错位。
    if (!padded_ && declaredSize_ > 0)
    {
        Status st = padToBlock();
        if (!st)
        {
            return st;
        }
        padded_ = true;
    }
    // 两个全零块表示归档结束。读侧只认第一个，第二个是为了让
    // 老式磁带工具能正确识别——沿用惯例，成本只有 512 字节。
    const std::array<std::uint8_t, kTarBlock * 2> zero{};
    Status st = emit(zero.data(), zero.size());
    if (!st)
    {
        return st;
    }
    closed_ = true;
    return Status::Ok();
}

std::size_t TarWriter::read(void *out, std::size_t outLen)
{
    const std::size_t avail = buf_.size() - readPos_;
    const std::size_t n = std::min(outLen, avail);
    if (n > 0)
    {
        std::memcpy(out, buf_.data() + readPos_, n);
        readPos_ += n;
    }
    // 全部取走后释放内存：大归档写下来是几百 MB，没必要一直攥着。
    if (readPos_ == buf_.size() && readPos_ > 0)
    {
        buf_.clear();
        readPos_ = 0;
    }
    return n;
}

bool TarWriter::drained() const noexcept
{
    return closed_ && readPos_ == buf_.size();
}

// ---------------------------------------------------------------------------
// TarReader
// ---------------------------------------------------------------------------

Status TarReader::write(const void *data, std::size_t len)
{
    if (len == 0)
    {
        return Status::Ok();
    }
    // 攒在缓冲区里，按需消费。缓冲区只在整体上"压缩"，
    // 不做环形队列——tar 的读法天然是单调向前的。
    const auto *p = static_cast<const std::uint8_t *>(data);
    buf_.insert(buf_.end(), p, p + len);
    return Status::Ok();
}

void TarReader::finishInput() noexcept
{
    eofInput_ = true;
}

Status TarReader::fill(std::size_t need)
{
    // 把已消费的前缀丢掉，腾出空间
    if (readPos_ > 0)
    {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(readPos_));
        readPos_ = 0;
    }
    if (buf_.size() >= need)
    {
        return Status::Ok();
    }
    // 输入已结束仍然不够 → 归档被截断
    if (eofInput_)
    {
        return Status::Fail(ErrorCode::kInvalidPath, "tar: truncated archive");
    }
    return Status::Fail(ErrorCode::kUnsupported, "tar: need more input");
}

Status TarReader::readHeader(TarHeader &out)
{
    out = TarHeader{};
    if (sawEndMarker_)
    {
        return Status::Ok();  // 已到结束标记，再调也返回"没有更多条目"
    }
    // 上一个条目的数据必须被读完（readData 或 skipData），
    // 否则下一个头会从数据区中间读起——这种错法在测试里表现为
    // "第一个文件对了，第二个文件名字是乱码"，很难反推原因。
    // 填充字节（padLeft_）则允许残留：readData 在流式场景下可能
    // 拿不到足够的填充，这里替它吃掉。
    if (dataLeft_ > 0)
    {
        return Status::Fail(ErrorCode::kIoError, "tar: previous entry data not consumed");
    }
    if (padLeft_ > 0)
    {
        if (readPos_ > 0)
        {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(readPos_));
            readPos_ = 0;
        }
        const auto pad = static_cast<std::size_t>(padLeft_);
        if (buf_.size() < pad)
        {
            if (eofInput_)
            {
                return Status::Fail(ErrorCode::kInvalidPath, "tar: truncated archive (padding incomplete)");
            }
            return Status::Fail(ErrorCode::kUnsupported, "tar: need more input");
        }
        readPos_ = pad;
        padLeft_ = 0;
    }

    Status st = fill(kTarBlock);
    if (!st)
    {
        return st;
    }
    const std::uint8_t *raw = buf_.data() + readPos_;
    if (allZero(raw, kTarBlock))
    {
        readPos_ += kTarBlock;
        sawEndMarker_ = true;
        return Status::Ok();
    }

    UstarHeader h{};
    std::memcpy(&h, raw, sizeof(h));
    readPos_ += kTarBlock;

    const std::uint32_t expected = static_cast<std::uint32_t>(readNumeric(h.chksum, sizeof(h.chksum)));
    const std::uint32_t actual = computeChecksum(h);
    if (expected != actual)
    {
        return Status::Fail(ErrorCode::kInvalidPath, "tar: header checksum mismatch");
    }

    const std::uint64_t size = readNumeric(h.size, sizeof(h.size));
    const auto mode = static_cast<std::uint16_t>(readNumeric(h.mode, sizeof(h.mode)) & 07777U);
    const auto mtime = static_cast<std::int64_t>(readNumeric(h.mtime, sizeof(h.mtime)));
    const char typeflag = h.typeflag == '\0' ? '0' : h.typeflag;

    if (typeflag == 'x' || typeflag == 'X')
    {
        // PAX 扩展头：内容是一串 "LEN key=value\n" 记录。
        // 负载可能还没喂进来（流式场景），所以把长度记下交给 readAllData，
        // 由调用方读完负载后调 applyPaxPayload 再读下一个头。
        out.typeflag = typeflag;
        out.isPax = true;
        out.size = size;
        dataLeft_ = size;
        padLeft_ = (kTarBlock - (size % kTarBlock)) % kTarBlock;
        return Status::Ok();
    }
    if (typeflag == 'L' || typeflag == 'K')
    {
        // GNU 长名字扩展（'L' 长文件名 / 'K' 长链接名）：负载是一段
        // 以 NUL 结尾的原始字符串，没有 PAX 那套 "LEN key=value" 结构。
        //
        // 为什么要认它：GNU tar 默认用它而不是 PAX。不认的话，别人的
        // tar 包里的长路径会被静默截断成 100 字节——两个不同的深路径
        // 会塌成同一个名字，解包时互相覆盖，而且完全不报错。
        out.typeflag = typeflag;
        out.isPax = true;  // 复用"扩展头"的语义
        out.size = size;
        dataLeft_ = size;
        padLeft_ = (kTarBlock - (size % kTarBlock)) % kTarBlock;
        return Status::Ok();
    }

    std::string name;
    if (h.prefix[0] != '\0')
    {
        name = fieldToString(h.prefix, sizeof(h.prefix));
        name.push_back('/');
    }
    name += fieldToString(h.name, sizeof(h.name));
    if (!paxPath_.empty())
    {
        name = paxPath_;  // PAX 记录的路径优先
        paxPath_.clear();
    }

    out.name = std::move(name);
    out.typeflag = typeflag;
    out.mode = mode;
    out.size = size;
    out.mtime = mtime;
    out.isPax = false;
    out.isSymlink = (typeflag == '1' || typeflag == '2');

    // 目录条目在部分实现里会带 size > 0，但那不是文件内容，
    // 一律当作 0 跳过，否则会把后面的条目吃掉。
    const std::uint64_t dataSize = (typeflag == '5') ? 0 : size;
    dataLeft_ = dataSize;
    padLeft_ = (kTarBlock - (size % kTarBlock)) % kTarBlock;
    return Status::Ok();
}

Status TarReader::readAllData(std::vector<std::uint8_t> &out)
{
    out.clear();
    if (dataLeft_ == 0)
    {
        return Status::Ok();
    }
    if (dataLeft_ > (1u << 20))
    {
        // PAX 头按规范只有几百字节，超过 1MB 一定是构造的恶意包。
        // 拒绝而不是尝试分配，避免一条 4GB 的"路径"把内存吃光。
        return Status::Fail(ErrorCode::kInvalidPath, "tar: pax payload too large");
    }
    Status st = fill(static_cast<std::size_t>(dataLeft_));
    if (!st)
    {
        return st;  // 可安全重试：读位置没动
    }
    out.assign(buf_.begin() + static_cast<std::ptrdiff_t>(readPos_),
               buf_.begin() + static_cast<std::ptrdiff_t>(readPos_ + dataLeft_));
    readPos_ += static_cast<std::size_t>(dataLeft_);
    dataLeft_ = 0;
    // 填充交给下一次 readHeader / readData 处理：这里如果强行要求
    // 填充到齐，流式喂入时就会误报"截断"。
    return Status::Ok();
}

std::size_t TarReader::readData(void *out, std::size_t outLen, Status &st)
{
    st = Status::Ok();
    if (dataLeft_ == 0 || outLen == 0)
    {
        return 0;
    }
    // 有多少给多少：流式场景下缓冲区里常常只有零头，
    // 硬要凑满 outLen 会让调用方以为"卡住了"。
    // 先把已消费的前缀丢掉，再取 available 与 outLen 的较小值。
    if (readPos_ > 0)
    {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(readPos_));
        readPos_ = 0;
    }
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(dataLeft_, outLen));
    if (buf_.size() < want)
    {
        if (eofInput_)
        {
            st = Status::Fail(ErrorCode::kInvalidPath, "tar: truncated archive (entry data incomplete)");
            return 0;
        }
        const std::size_t avail = std::min(buf_.size(), want);
        if (avail == 0)
        {
            // 一点数据都没有：告诉调用方"去喂更多"，不是错误
            st = Status::Fail(ErrorCode::kUnsupported, "tar: need more input");
            return 0;
        }
        std::memcpy(out, buf_.data(), avail);
        readPos_ = avail;
        dataLeft_ -= avail;
        return avail;
    }
    std::memcpy(out, buf_.data(), want);
    readPos_ = want;
    dataLeft_ -= want;
    if (dataLeft_ == 0 && padLeft_ > 0)
    {
        // 数据读完时顺手把填充吃掉，调用方不必操心。
        // 填充还没喂齐就标记出来，下次 readData/skipData 或
        // readHeader 会继续尝试。
        const auto pad = static_cast<std::size_t>(padLeft_);
        if (readPos_ > 0)
        {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(readPos_));
            readPos_ = 0;
        }
        if (buf_.size() >= pad)
        {
            readPos_ = pad;
            padLeft_ = 0;
        }
        // 不够就先留着，padLeft_ 不变，下次接着吃
    }
    return want;
}

Status TarReader::skipData()
{
    if (padLeft_ > 0 || dataLeft_ > 0)
    {
        const std::uint64_t total = dataLeft_ + padLeft_;
        Status st = fill(static_cast<std::size_t>(total));
        if (!st)
        {
            return st;
        }
        readPos_ += static_cast<std::size_t>(total);
        dataLeft_ = 0;
        padLeft_ = 0;
    }
    // PAX 负载被跳过时，路径信息就丢了；清掉避免污染下一个条目
    paxPath_.clear();
    return Status::Ok();
}

std::uint64_t TarReader::dataLeft() const noexcept
{
    return dataLeft_;
}

void TarReader::overrideNextName(std::string name)
{
    paxPath_ = std::move(name);
}

std::string TarReader::applyPaxPayload(char typeflag, std::string_view payload)
{
    std::string path;
    if (typeflag == 'x' || typeflag == 'X')
    {
        path = paxPathOf(payload);
    }
    else if (typeflag == 'L' || typeflag == 'K')
    {
        // GNU 长名字：整个负载就是路径，规范说以 NUL 结尾但不保证
        std::size_t n = payload.find('\0');
        if (n == std::string_view::npos)
        {
            n = payload.size();
        }
        path.assign(payload.substr(0, n));
    }
    if (!path.empty())
    {
        paxPath_ = path;
    }
    return path;
}

}  // namespace relayer::filestore
