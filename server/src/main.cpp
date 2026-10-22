#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include "relayer/server/Server.h"

// relayerd 的入口。
//
// 刻意不引第三方参数库：参数只有五六个，而"多一个依赖"对一个要被
// 静态打包、要在干净机器上解压即用的服务端来说不值得。手写的解析也
// 更容易给出准确的中文报错。
//
// 退出码约定：
//   0  正常（收到 SIGINT/SIGTERM）
//   1  参数错误
//   2  启动失败（端口占用、口令缺失等）

namespace
{

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitStartup = 2;

void printUsage()
{
    std::printf("用法: relayerd [选项]\n"
                "\n"
                "选项:\n"
                "  -l, --listen <addr>    监听地址（默认 0.0.0.0）\n"
                "  -p, --port <port>      监听端口，0 表示由系统分配（默认 9527）\n"
                "      --id <name>        服务端在协议里的身份（默认 relayerd）\n"
                "      --password <pw>    认证口令。不提供时必须显式 --allow-no-auth\n"
                "      --allow-no-auth    关闭认证（危险：任何能连上的人都能接入中继）\n"
                "      --max-peers <n>    在线对端上限（默认 1024）\n"
                "      --drop-data-frames <n[,n...]>\n"
                "                         测试专用：丢弃第 n 个转发的传输数据帧（从 1 起）\n"
                "  -h, --help             显示本帮助\n"
                "\n"
                "安全说明：不传 --password 时默认**拒绝启动**，而不是悄悄变成开放\n"
                "服务端。一个忘记配口令就上线的中继等于任何人都能借它传文件。\n");
}

struct ParseResult
{
    bool ok = false;
    bool showHelp = false;
    relayer::server::ServerConfig config;
};

ParseResult parseArgs(int argc, char **argv)
{
    ParseResult r;
    r.config.port = 9527;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];

        // 需要跟一个值的选项。抽成 lambda 是为了统一处理"忘了给值"：
        // 少一个参数值是最常见的命令行错误，而默认行为（把它当成空串）
        // 会让服务端以一个空口令启动。
        const auto takeValue = [&](std::string_view opt, std::string &out) -> bool
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "错误: %.*s 需要一个值\n", static_cast<int>(opt.size()), opt.data());
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

        if (a == "-l" || a == "--listen")
        {
            if (!takeValue(a, r.config.listenAddress))
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
            try
            {
                const auto n = std::stoul(v);
                if (n > 65535)
                {
                    std::fprintf(stderr, "错误: 端口超出范围: %s\n", v.c_str());
                    return r;
                }
                r.config.port = static_cast<std::uint16_t>(n);
            }
            catch (const std::exception &)
            {
                std::fprintf(stderr, "错误: 端口不是数字: %s\n", v.c_str());
                return r;
            }
        }
        else if (a == "--id")
        {
            if (!takeValue(a, r.config.serverId))
            {
                return r;
            }
        }
        else if (a == "--password")
        {
            if (!takeValue(a, r.config.password))
            {
                return r;
            }
        }
        else if (a == "--allow-no-auth")
        {
            r.config.allowNoAuth = true;
        }
        else if (a == "--max-peers")
        {
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            try
            {
                const auto n = std::stoul(v);
                if (n == 0)
                {
                    std::fprintf(stderr, "错误: --max-peers 必须大于 0\n");
                    return r;
                }
                r.config.maxPeers = n;
            }
            catch (const std::exception &)
            {
                std::fprintf(stderr, "错误: --max-peers 不是数字: %s\n", v.c_str());
                return r;
            }
        }
        else if (a == "--drop-data-frames")
        {
            // **测试专用**。格式："3,7" → 丢掉第 3 和第 7 个转发的数据帧。
            //
            // 生产上永远不该出现这个参数，所以启动日志里会明确打一条
            // warning：一个悄悄丢帧的中继比一个明显崩溃的中继更难查。
            std::string v;
            if (!takeValue(a, v))
            {
                return r;
            }
            std::string_view rest(v);
            while (!rest.empty())
            {
                const auto comma = rest.find(',');
                const auto piece = rest.substr(0, comma);
                try
                {
                    const auto n = std::stoull(std::string(piece));
                    if (n == 0)
                    {
                        std::fprintf(stderr, "错误: --drop-data-frames 的序号从 1 开始\n");
                        return r;
                    }
                    r.config.dropDataFrames.push_back(n);
                }
                catch (const std::exception &)
                {
                    std::fprintf(stderr, "错误: --drop-data-frames 里有非数字项: %s\n", std::string(piece).c_str());
                    return r;
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                rest.remove_prefix(comma + 1);
            }
        }
        else
        {
            std::fprintf(stderr, "错误: 未知参数: %s\n", argv[i]);
            return r;
        }
    }

    // 口令与 allow-no-auth 必须恰好给一个。
    //
    // 都不给时**拒绝启动**（而不是默认关闭认证）：一个忘记配口令的中继
    // 是所有人都能借道传文件的开放节点，而"忘了配"比"故意开"常见得多。
    if (r.config.password.empty() && !r.config.allowNoAuth)
    {
        std::fprintf(stderr, "错误: 必须提供 --password，或用 --allow-no-auth 显式关闭认证\n");
        return r;
    }
    if (!r.config.password.empty() && r.config.allowNoAuth)
    {
        std::fprintf(stderr, "错误: --password 与 --allow-no-auth 不能同时给出\n");
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
        printUsage();
        return (argc == 1) ? kExitOk : kExitUsage;
    }
    if (parsed.showHelp)
    {
        printUsage();
        return kExitOk;
    }

    try
    {
        asio::io_context ctx;

        relayer::server::Server srv(ctx, parsed.config);
        if (const auto st = srv.start(); !st.ok())
        {
            std::fprintf(stderr, "启动失败: %s\n", st.message.c_str());
            return kExitStartup;
        }

        std::printf("relayerd 已启动: %s:%u  id=%s  认证=%s\n", parsed.config.listenAddress.c_str(),
                    static_cast<unsigned>(srv.port()), parsed.config.serverId.c_str(),
                    parsed.config.allowNoAuth ? "关闭" : "开启");
        std::fflush(stdout);

        // 丢帧是测试注入的故障，必须自曝。一个悄悄丢帧的中继比一个直接
        // 崩溃的中继难查得多——排障的人会先怀疑网络。
        if (!parsed.config.dropDataFrames.empty())
        {
            std::printf("警告: 测试模式，将丢弃指定的传输数据帧（共 %zu 个序号）\n",
                        parsed.config.dropDataFrames.size());
            std::fflush(stdout);
        }

        // 测试要能拿到真实端口：端口为 0 时打印出来，由测试解析。
        // 这不是"调试输出"，而是与本进程的契约——见 tests/e2e。
        if (parsed.config.port == 0)
        {
            std::printf("LISTEN_PORT=%u\n", static_cast<unsigned>(srv.port()));
            std::fflush(stdout);
        }

        asio::signal_set signals(ctx, SIGINT, SIGTERM);
        signals.async_wait(
            [&](const std::error_code &, int sig)
            {
                std::printf("\n收到信号 %d，正在关闭……\n", sig);
                std::fflush(stdout);
                srv.stop();
                ctx.stop();
            });

        ctx.run();
        return kExitOk;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "异常: %s\n", e.what());
        return kExitStartup;
    }
}
