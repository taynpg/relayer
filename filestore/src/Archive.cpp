#include <algorithm>
#include <cctype>

#include "relayer/filestore/IArchive.h"
#include "relayer/filestore/PathPolicy.h"

namespace relayer::filestore
{

std::string_view toString(ArchiveFormat format)
{
    switch (format)
    {
    case ArchiveFormat::kZip:
        return "zip";
    case ArchiveFormat::kTarXz:
        return "tar.xz";
    }
    return "unknown";
}

namespace
{

std::string toLowerAscii(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool guessFormat(std::string_view fileName, ArchiveFormat &out)
{
    const std::string lower = toLowerAscii(fileName);
    if (endsWith(lower, ".tar.xz") || endsWith(lower, ".txz"))
    {
        out = ArchiveFormat::kTarXz;
        return true;
    }
    if (endsWith(lower, ".zip"))
    {
        out = ArchiveFormat::kZip;
        return true;
    }
    return false;
}

std::string sanitizeEntryName(std::string_view entryName)
{
    // 统一分隔符：Windows 上调用方可能传 `dir\file`，归档里必须是 `/`。
    std::string s(entryName);
    std::replace(s.begin(), s.end(), '\\', '/');

    // 逐段过滤：丢掉空段（重复斜杠）、`.`；`..` 一律拒绝。
    // 直接返回空串而不是尝试"消掉 .."——调用方给的路径本身有问题，
    // 静默改写会让"打出来的包"和"用户以为打进去的东西"不一致。
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == '/')
        {
            ++i;
            continue;
        }
        std::size_t j = s.find('/', i);
        if (j == std::string::npos)
        {
            j = s.size();
        }
        const std::string_view seg(s.data() + i, j - i);
        if (seg == "..")
        {
            return {};
        }
        if (seg != ".")
        {
            parts.emplace_back(seg);
        }
        i = j;
    }
    if (parts.empty())
    {
        return {};
    }

    // 盘符前缀一律拒绝。两种形式都要挡住：
    //   `C:/Windows/x.dll` → 第一段是 "C:"
    //   `C:x.dll`          → 第一段是 "C:x.dll"（没有斜杠，盘符内联）
    // 第二种容易漏：它看起来像个普通文件名，但在 Windows 上会被解释成
    // "C 盘当前目录下的 x.dll"，解包时落点完全不受 root 约束。
    if (parts.front().size() >= 2 && std::isalpha(static_cast<unsigned char>(parts.front()[0])) &&
        parts.front()[1] == ':')
    {
        return {};
    }

    std::string out;
    for (const auto &p : parts)
    {
        if (!out.empty())
        {
            out.push_back('/');
        }
        out += p;
    }
    return out;
}

}  // namespace relayer::filestore
