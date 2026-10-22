#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include <asio/io_context.hpp>

#include "relayer/cli/Client.h"

// relayer-cli 的入口。
//
// 三个子命令，刻意做成"动词在前"（send / recv / list）而不是 flag 组合：
// 本工具的用法只有三种，让用户记住 -s / -r 的语义差不如让他读完那三个词。
//
// 为什么它与 relayerd 一样不引第三方参数库：参数依然很少，而本工具要在
// 干净机器上解压即用；更重要的是手写解析能给出中文的、指出具体位置
// 的报错。
//
// 退出码约定（与 relayerd 对齐，测试依赖它）：
//   0  操作成功
//   1  参数错误
//   2  连接或传输失败
//   3  超时（对端无响应、总时长超限）—— 单独一个码，因为 kill 测试要
//      区分"对端死了导致超时"与"真的失败了"。混在一起时，一个失败的
//      测试无法自证是预期路径还是 bug。

namespace
{

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;
constexpr int kExitTimeout = 3;

void printUsage()
{
    std::printf(
        "用法: relayer-cli <子命令> [选项]\n"
        "\n"
        "子命令:\n"
        "  send <本地文件> --to <对端id>   把文件发给在线的对端\n"
        "  recv --to-dir <目录>            等待别人发来文件，落到目录里\n"
        "  list                            只连上去列出在线对端\n"
        "\n"
        "通用选项:\n"
        "  -H, --host <addr>        中继地址（默认 127.0.0.1）\n"
        "  -p, --port <port>        中继端口（默认 9527）\n"
        "      --password <pw>      认证口令\n"
        "      --id <name>          要求服务端分配的身份（一般不用给）\n"
        "      --chunk-size <bytes> 分块大小（默认 4 MiB，测试时可调小）\n"
        "      --idle-timeout <sec> 多久没有进展就判定对端失联（默认 60）\n"
        "      --hard-timeout <sec> 整个操作的硬上限，0 表示不限（默认 0）\n"
        "  -q, --quiet              不打印进度，只在结束时输出一行结论\n"
        "  -h, --help               显示本帮助\n"
        "\n"
        "示例:\n"
        "  relayer-cli send ./a.bin --to client-2 -H 10.0.0.5 -p 9527 --password pw\n"
        "  relayer-cli recv --to-dir ./downloads -H 10.0.0.5 -p 9527 --password pw\n");
}

// 人类可读的字节数。进度行里 "1023.9 MiB / 2.0 GiB" 比裸字节好读，
// 但**只**用于显示，判断逻辑一律用原始计数。
std::string humanBytes(std::uint64_t n)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[i]);
    return buf;
}

std::string humanSpeed(double bps)
{
    if (bps <= 0.0)
    {
        return "-";
    }
    return humanBytes(static_cast<std::uint64_t>(bps)) + "/s";
}

struct Args
{
    std::string command;
    std::string file;
    std::string to;
    std::string toDir;
    relayer::cli::ClientConfig config;
    bool quiet = false;
};

struct ParseResult
{
    bool ok = false;
    bool showHelp = false;
    std::string error;
    Args args;
};

bool parseU64(const std::string &s, std::uint64_t &out)
{
    try
    {
        std::size_t pos = 0;
        const auto v = std::stoull(s, &pos);
        if (pos != s.size())
        {
            return false;
        }
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

ParseResult parseArgs(int argc, char **argv)
{
    ParseResult r;

    if (argc < 2)
    {
        r.error = "缺少子命令";
        return r;
    }

    r.args.command = argv[1];
    if (r.args.command == "-h" || r.args.command == "--help")
    {
        r.showHelp = true;
        r.ok = true;
        return r;
    }
    if (r.args.command != "send" && r.args.command != "recv" && r.args.command != "list")
    {
        r.error = "未知子命令: " + r.args.command;
        return r;
    }

    // send 的第一个位置参数是文件。刻意只支持"文件紧跟在子命令后"这一种
    // 写法：让位置参数出现在选项之后（`send --to x file`）需要判断"这个
    // 裸词是值还是位置参数"，而 --to 的值也是裸词，二者无法可靠区分。
    int i = 2;
    if (r.args.command == "send")
    {
        if (i >= argc || argv[i][0] == '-')
        {
            r.error = "send 需要指定要发送的文件";
            return r;
        }
        r.args.file = argv[i++];
    }

    for (; i < argc; ++i)
    {
        const std::string_view a = argv[i];

        const auto takeValue = [&](std::string_view opt, std::string &out) -> bool
        {
            if (i + 1 >= argc)
            {
                r.error = std::string(opt) + " 需要一个值";
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (a == "-h" || a == "--help")
        {
            r.showHelp = true;
            r.ok = true;
            return r;
        }
        else if (a == "-H" || a == "--host")
        {
            if (!takeValue(a, r.args.config.host))
            {
                return r;
            }
        }
        else if (a == "-p" || a == "--port")
        {
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0 || n > 65535)
            {
                r.error = "端口非法: " + v;
                return r;
            }
            r.args.config.port = static_cast<std::uint16_t>(n);
        }
        else if (a == "--password")
        {
            if (!takeValue(a, r.args.config.password))
            {
                return r;
            }
        }
        else if (a == "--id")
        {
            if (!takeValue(a, r.args.config.expectId))
            {
                return r;
            }
        }
        else if (a == "--chunk-size")
        {
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0 || n > (1u << 20) * 16)
            {
                r.error = "分块大小非法: " + v;
                return r;
            }
            r.args.config.chunkSize = static_cast<std::uint32_t>(n);
        }
        else if (a == "--idle-timeout")
        {
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0)
            {
                r.error = "空闲超时非法: " + v;
                return r;
            }
            r.args.config.idleTimeout = std::chrono::seconds(n);
        }
        else if (a == "--hard-timeout")
        {
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n))
            {
                r.error = "总时长上限非法: " + v;
                return r;
            }
            r.args.config.hardTimeout = std::chrono::seconds(n);
        }
        else if (a == "-q" || a == "--quiet")
        {
            r.args.quiet = true;
        }
        else if (a == "--to")
        {
            if (!takeValue(a, r.args.to))
            {
                return r;
            }
        }
        else if (a == "--to-dir")
        {
            if (!takeValue(a, r.args.toDir))
            {
                return r;
            }
        }
        else
        {
            r.error = "未知选项: " + std::string(a);
            return r;
        }
    }

    // 子命令的必备参数。放在解析之后统一检查，这样 "send 缺 --to" 与
    // "recv 缺 --to-dir" 都能得到明确的说法，而不是靠解析顺序碰运气。
    if (r.args.command == "send" && r.args.to.empty())
    {
        r.error = "send 需要 --to 指定目标对端";
        return r;
    }
    if (r.args.command == "recv" && r.args.toDir.empty())
    {
        r.error = "recv 需要 --to-dir 指定落点目录";
        return r;
    }

    r.ok = true;
    return r;
}

}  // namespace

int main(int argc, char **argv)
{
    const auto parsed = parseArgs(argc, argv);
    if (!parsed.ok)
    {
        std::fprintf(stderr, "错误: %s\n\n", parsed.error.c_str());
        printUsage();
        return kExitUsage;
    }
    if (parsed.showHelp)
    {
        printUsage();
        return kExitOk;
    }

    const auto &a = parsed.args;

    asio::io_context ctx;

    bool done = false;
    relayer::cli::Outcome outcome;

    // 进度行用 \r 覆盖同一行：一个大文件会触发几百次上报，逐行打印会
    // 把终端刷满，而用户真正要看的只有最后那行结论。quiet 时连这行都省。
    bool progressDirty = false;

    auto client = std::make_shared<relayer::cli::Client>(
        ctx, a.config,
        [quiet = a.quiet, &progressDirty](const relayer::cli::ProgressReport &p)
        {
            if (quiet)
            {
                return;
            }
            const double pct = p.bytesTotal > 0
                                   ? 100.0 * static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal)
                                   : 0.0;
            std::fprintf(stderr, "\r%5.1f%%  %s / %s  %s   ", pct, humanBytes(p.bytesDone).c_str(),
                         humanBytes(p.bytesTotal).c_str(), humanSpeed(p.bytesPerSecond).c_str());
            std::fflush(stderr);
            progressDirty = true;
        },
        [&done, &outcome](const relayer::cli::Outcome &o)
        {
            outcome = o;
            done = true;
        });

    if (a.command == "send")
    {
        client->sendFile(a.file, {}, a.to);
    }
    else if (a.command == "recv")
    {
        client->receiveFile(a.toDir);
    }
    else
    {
        client->listPeers();
    }

    // run() 在"没有待处理事件"时才会返回。Client 结束时会 cancel 掉自己
    // 的定时器，但仍可能有排队的 completed handler，所以这里靠 done 标志
    // 而不是靠 run() 返回——stop() 之后再 run() 一次会立即返回，这样做
    // 既能跑完收尾，也不会卡住。
    while (!done)
    {
        if (ctx.stopped())
        {
            ctx.restart();
        }
        if (ctx.run_for(std::chrono::milliseconds(50)) == 0 && !done)
        {
            // 没有任何事件在等，说明内部状态机已经彻底停下（既不完成也不
            // 报错）。这是逻辑 bug，不该让进程挂住。
            std::fprintf(stderr, "\n错误: 内部状态机停止响应\n");
            return kExitFailed;
        }
    }

    // 进度行之后要换行，否则结论会接在进度条后面。
    if (progressDirty)
    {
        std::fprintf(stderr, "\n");
    }

    if (outcome.ok)
    {
        std::printf("%s\n", outcome.summary.c_str());
        return kExitOk;
    }

    std::fprintf(stderr, "%s\n", outcome.summary.c_str());
    if (!outcome.status.ok() && !outcome.status.message.empty())
    {
        std::fprintf(stderr, "  原因: %s\n", outcome.status.message.c_str());
    }

    // 超时单独一个退出码，见文件头的说明。
    if (outcome.status.code == relayer::ErrorCode::kTimeout)
    {
        return kExitTimeout;
    }
    return kExitFailed;
}
