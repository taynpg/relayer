#include "relayer/filestore/TarXzArchiver.h"

#include <array>
#include <cstring>
#include <vector>

#include "relayer/filestore/PathPolicy.h"
#include "relayer/filestore/detail/TarCodec.h"
#include "relayer/filestore/detail/XzCodec.h"

namespace relayer::filestore
{

namespace
{

constexpr std::size_t kIoChunk = 64 * 1024;

// 一次"喂数据"的产出速率下限。
// 解压时可能出现"输入吃了很多、输出一个字节都没有"的情况（PAX 头里的
// 长路径、全零块），循环不能因为某一次没产出就认为"卡住了"。
constexpr int kMaxIdleRounds = 64;

// 打一个 tar.xz 包。
//
// 为什么不先写临时 tar 再压：
//   临时文件要把整包数据落两遍盘（写 tar、读回来压），大包时 IO 翻倍；
//   而且在内存文件系统（MemFileStore）里测试时反而"能用"，
//   到真机上才发现慢。这里全程流式，内存占用只有一个块。
Status packToXz(const IFileStore &store, std::string_view archivePath, const std::vector<ArchiveItem> &items,
                const ArchiveOptions &options, std::vector<std::string> &skipped)
{
    Status st;
    // 先建父目录再打开文件：顺序反了会在"目录不存在"时报一个
    // 与真实原因无关的错误（Windows 上是 ERROR_PATH_NOT_FOUND）。
    {
        const std::string archive = store.normalize(archivePath);
        const std::size_t slash = archive.find_last_of('/');
        if (slash != std::string::npos && slash > 0)
        {
            Status ms = store.mkdirs(archive.substr(0, slash));
            if (!ms)
            {
                return ms;
            }
        }
    }
    std::unique_ptr<IWriteStream> out = store.openWrite(archivePath, true, st);
    if (!out)
    {
        return st.ok() ? Status::Fail(ErrorCode::kIoError, "cannot open archive for writing") : st;
    }

    XzEncoder enc(options.compressionLevel);
    TarWriter tw;
    std::vector<char> buf(kIoChunk);

    // 把 tar 攒的字节取出、压一遍、写进归档文件。
    //
    // feed=true  从 tar 取字节喂给编码器（编码器必须还没 finish）
    // feed=false 只把编码器里剩的压缩数据挤出来（finish 之后用）
    auto pump = [&](bool feed) -> Status
    {
        if (feed)
        {
            for (;;)
            {
                const std::size_t n = tw.read(buf.data(), buf.size());
                if (n == 0)
                {
                    break;
                }
                Status ws = enc.write(buf.data(), n);
                if (!ws)
                {
                    return ws;
                }
                // 每喂一块就把编码器能吐的吐掉，避免内部缓冲无限增长
                for (;;)
                {
                    Status rs;
                    const std::size_t got = enc.read(buf.data(), buf.size(), rs);
                    if (!rs)
                    {
                        return rs;
                    }
                    if (got == 0)
                    {
                        break;
                    }
                    Status ws2 = out->write(buf.data(), got);
                    if (!ws2)
                    {
                        return ws2;
                    }
                }
            }
        }
        // 把编码器当前的输出全部取走
        for (;;)
        {
            Status rs;
            const std::size_t got = enc.read(buf.data(), buf.size(), rs);
            if (!rs)
            {
                return rs;
            }
            if (got == 0)
            {
                break;
            }
            Status ws = out->write(buf.data(), got);
            if (!ws)
            {
                return ws;
            }
        }
        return Status::Ok();
    };

    for (const auto &item : items)
    {
        const std::string entry = sanitizeEntryName(item.entryName);
        if (entry.empty())
        {
            skipped.push_back(item.entryName);
            continue;
        }
        const FileStat fs = store.stat(item.sourcePath);
        if (!fs.exists)
        {
            skipped.push_back(item.entryName);
            continue;
        }
        if (fs.isDir)
        {
            Status as = tw.addEntry(entry + "/", '5', static_cast<std::uint16_t>(fs.mode & 07777U), 0, fs.mtime);
            if (!as)
            {
                return as;
            }
            Status ps = tw.finishData();
            if (!ps)
            {
                return ps;
            }
            Status ps2 = pump(false);
            if (!ps2)
            {
                return ps2;
            }
            continue;
        }

        std::unique_ptr<IReadStream> in = store.openRead(item.sourcePath, st);
        if (!in)
        {
            skipped.push_back(item.entryName);
            continue;
        }
        Status as = tw.addEntry(entry, '0', static_cast<std::uint16_t>(fs.mode & 07777U), fs.size, fs.mtime);
        if (!as)
        {
            return as;
        }
        std::uint64_t left = fs.size;
        while (left > 0)
        {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
            Status rs;
            const std::size_t got = in->read(buf.data(), want, rs);
            if (!rs)
            {
                return rs;
            }
            if (got == 0)
            {
                // 源文件在读取过程中被截短。已经写了头却少数据，
                // 归档必然错位——直接失败比生成一个解不开的包好。
                return Status::Fail(ErrorCode::kIoError,
                                    "tar.xz: source file shrank while packing: " + item.sourcePath);
            }
            Status ws = tw.writeData(buf.data(), got);
            if (!ws)
            {
                return ws;
            }
            left -= got;
        }
        Status fps = tw.finishData();
        if (!fps)
        {
            return fps;
        }
        Status ps = pump(false);
        if (!ps)
        {
            return ps;
        }
    }

    Status cs = tw.close();
    if (!cs)
    {
        return cs;
    }
    // 顺序不能反：tar.close() 写的两个结束块还没喂给编码器，必须先喂
    // （pump(true)），再声明 finish，最后纯 drain（pump(false)）。
    // 反过来会在 finish 之后再 write，报 "write after finish"。
    Status ps = pump(true);
    if (!ps)
    {
        return ps;
    }
    Status fs2 = enc.finish();
    if (!fs2)
    {
        return fs2;
    }
    Status ds = pump(false);
    if (!ds)
    {
        return ds;
    }
    Status os = out->flush();
    if (!os)
    {
        return os;
    }
    return out->sync();
}

// 解压器 → tar 读取器之间的缓冲泵。
// tar 每次要多少给多少，不够就多压一点出来。
class DecoderPump
{
public:
    DecoderPump(const IFileStore &store, std::string_view archivePath)
        : store_(store)
        , archivePath_(archivePath)
        , buf_(kIoChunk)
    {
    }

    // 打开归档并填入第一批数据
    Status open()
    {
        in_ = store_.openRead(archivePath_, st_);
        if (!in_)
        {
            return st_.ok() ? Status::Fail(ErrorCode::kIoError, "cannot open archive") : st_;
        }
        return feedOnce();
    }

    // 把归档里的下一块读出来、解压、喂给 tar。
    // 返回 false 表示归档已经喂完（tar 应当看到结束标记）。
    Status pump(TarReader &tr)
    {
        for (int i = 0; i < kMaxIdleRounds; ++i)
        {
            // 先把解压器里攒着的东西全挤出来
            for (;;)
            {
                Status rs;
                const std::size_t got = dec_.read(buf_.data(), buf_.size(), rs);
                if (!rs)
                {
                    return rs;
                }
                if (got == 0)
                {
                    break;
                }
                Status ws = tr.write(buf_.data(), got);
                if (!ws)
                {
                    return ws;
                }
            }
            if (dec_.finished())
            {
                tr.finishInput();
                return Status::Ok();
            }
            Status fs = feedOnce();
            if (!fs)
            {
                return fs;
            }
            if (!hasMore_ && dec_.finished())
            {
                tr.finishInput();
                return Status::Ok();
            }
        }
        return Status::Fail(ErrorCode::kIoError, "tar.xz: decoding stalled");
    }

private:
    Status feedOnce()
    {
        Status rs;
        const std::size_t got = in_->read(buf_.data(), buf_.size(), rs);
        if (!rs)
        {
            return rs;
        }
        if (got == 0)
        {
            hasMore_ = false;
            return dec_.finish();
        }
        return dec_.write(buf_.data(), got);
    }

    const IFileStore &store_;
    std::string archivePath_;
    std::unique_ptr<IReadStream> in_;
    Status st_;
    XzDecoder dec_;
    std::vector<char> buf_;
    bool hasMore_ = true;
};

}  // namespace

TarXzArchiver::TarXzArchiver(const IFileStore &store)
    : store_(store)
{
}

TarXzArchiver::~TarXzArchiver() = default;

ArchiveFormat TarXzArchiver::format() const noexcept
{
    return ArchiveFormat::kTarXz;
}

Status TarXzArchiver::create(std::string_view archivePath, const std::vector<ArchiveItem> &items,
                             const ArchiveOptions &options, std::vector<std::string> &skipped) const
{
    skipped.clear();
    return packToXz(store_, archivePath, items, options, skipped);
}

Status TarXzArchiver::list(std::string_view archivePath, std::vector<ArchiveEntry> &out) const
{
    out.clear();
    DecoderPump pump(store_, archivePath);
    Status st = pump.open();
    if (!st)
    {
        return st;
    }
    TarReader tr;

    for (int guard = 0; guard < 1000000; ++guard)
    {
        TarHeader h;
        st = tr.readHeader(h);
        while (st.code == ErrorCode::kUnsupported)
        {
            Status ps = pump.pump(tr);
            if (!ps)
            {
                return ps;
            }
            st = tr.readHeader(h);
        }
        if (!st)
        {
            return st;
        }
        if (h.isPax)
        {
            std::vector<std::uint8_t> payload;
            Status ps = tr.readAllData(payload);
            while (ps.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ps = tr.readAllData(payload);
            }
            if (!ps)
            {
                return ps;
            }
            tr.applyPaxPayload(h.typeflag,
                               std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size()));
            continue;
        }
        if (h.name.empty())
        {
            return Status::Ok();
        }
        ArchiveEntry e;
        e.name = h.name;
        e.isDir = (h.typeflag == '5');
        e.size = e.isDir ? 0 : h.size;
        e.crc32 = 0;  // tar 不带 crc
        out.push_back(std::move(e));

        Status ss = tr.skipData();
        while (ss.code == ErrorCode::kUnsupported)
        {
            Status qs = pump.pump(tr);
            if (!qs)
            {
                return qs;
            }
            ss = tr.skipData();
        }
        if (!ss)
        {
            return ss;
        }
    }
    return Status::Fail(ErrorCode::kInvalidPath, "tar.xz: too many entries");
}

Status TarXzArchiver::extract(std::string_view archivePath, std::string_view destRoot, const ArchiveOptions &options,
                              std::vector<std::string> &extracted, std::vector<std::string> &skipped) const
{
    extracted.clear();
    skipped.clear();

    DecoderPump pump(store_, archivePath);
    Status st = pump.open();
    if (!st)
    {
        return st;
    }
    TarReader tr;
    std::vector<char> buf(kIoChunk);

    for (int guard = 0; guard < 1000000; ++guard)
    {
        TarHeader h;
        st = tr.readHeader(h);
        while (st.code == ErrorCode::kUnsupported)
        {
            Status ps = pump.pump(tr);
            if (!ps)
            {
                return ps;
            }
            st = tr.readHeader(h);
        }
        if (!st)
        {
            return st;
        }
        if (h.isPax)
        {
            std::vector<std::uint8_t> payload;
            Status ps = tr.readAllData(payload);
            while (ps.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ps = tr.readAllData(payload);
            }
            if (!ps)
            {
                return ps;
            }
            tr.applyPaxPayload(h.typeflag,
                               std::string_view(reinterpret_cast<const char *>(payload.data()), payload.size()));
            continue;
        }
        if (h.name.empty())
        {
            return Status::Ok();
        }

        // zip-slip 防线：唯一入口。返回空串的条目跳过，但不整体失败——
        // 一个恶意条目不该让用户拿不到其余文件。
        const std::string target = store_.resolveWithin(destRoot, h.name);
        if (target.empty())
        {
            skipped.push_back(h.name);
            Status ss = tr.skipData();
            while (ss.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ss = tr.skipData();
            }
            if (!ss)
            {
                return ss;
            }
            continue;
        }

        const bool isDir = (h.typeflag == '5');
        const bool isLink = h.isSymlink;

        if (isDir)
        {
            Status ms = store_.mkdirs(target);
            if (!ms)
            {
                skipped.push_back(h.name);
            }
            else
            {
                extracted.push_back(target);
            }
            Status ss = tr.skipData();
            while (ss.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ss = tr.skipData();
            }
            if (!ss)
            {
                return ss;
            }
            continue;
        }

        if (isLink)
        {
            // 符号链接在 Windows 上需要额外权限，而且目标路径本身
            // 也可能是逃逸点。直接跳过——传输场景里链接本就少见，
            // 而"解出一个指向 /etc/passwd 的链接"不值得为它冒险。
            skipped.push_back(h.name);
            Status ss = tr.skipData();
            while (ss.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ss = tr.skipData();
            }
            if (!ss)
            {
                return ss;
            }
            continue;
        }

        if (!options.overwrite)
        {
            const FileStat exist = store_.stat(target);
            if (exist.exists)
            {
                skipped.push_back(h.name);
                Status ss = tr.skipData();
                while (ss.code == ErrorCode::kUnsupported)
                {
                    Status qs = pump.pump(tr);
                    if (!qs)
                    {
                        return qs;
                    }
                    ss = tr.skipData();
                }
                if (!ss)
                {
                    return ss;
                }
                continue;
            }
        }

        // 建父目录
        const std::size_t slash = target.find_last_of('/');
        if (slash != std::string::npos)
        {
            const std::string parent = target.substr(0, slash);
            if (!parent.empty())
            {
                Status ms = store_.mkdirs(parent);
                if (!ms)
                {
                    return ms;
                }
            }
        }

        Status os;
        std::unique_ptr<IWriteStream> out = store_.openWrite(target, true, os);
        if (!out)
        {
            // 单个文件写不了（被占用、无权限）不该让整个解包失败：
            // 把剩下的文件解出来，把这个记进 skipped，用户至少能用。
            skipped.push_back(h.name);
            Status ss = tr.skipData();
            while (ss.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ss = tr.skipData();
            }
            if (!ss)
            {
                return ss;
            }
            continue;
        }

        std::uint64_t left = h.size;
        bool ioFailed = false;
        while (left > 0 && !ioFailed)
        {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
            Status rs;
            const std::size_t got = tr.readData(buf.data(), want, rs);
            if (rs.code == ErrorCode::kUnsupported)
            {
                Status ps = pump.pump(tr);
                if (!ps)
                {
                    return ps;
                }
                continue;
            }
            if (!rs)
            {
                return rs;
            }
            if (got == 0)
            {
                return Status::Fail(ErrorCode::kInvalidPath, "tar.xz: entry data ended early: " + h.name);
            }
            Status ws = out->write(buf.data(), got);
            if (!ws)
            {
                ioFailed = true;
                break;
            }
            left -= got;
        }
        if (ioFailed)
        {
            skipped.push_back(h.name);
            Status ss = tr.skipData();
            while (ss.code == ErrorCode::kUnsupported)
            {
                Status qs = pump.pump(tr);
                if (!qs)
                {
                    return qs;
                }
                ss = tr.skipData();
            }
            if (!ss)
            {
                return ss;
            }
            continue;
        }
        Status cs = out->flush();
        if (!cs)
        {
            skipped.push_back(h.name);
            continue;
        }
        extracted.push_back(target);
    }
    return Status::Fail(ErrorCode::kInvalidPath, "tar.xz: too many entries");
}

}  // namespace relayer::filestore
