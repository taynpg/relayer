#include <asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include "relayer/cli/Client.h"
#include "relayer/common/Console.h"

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

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;
constexpr int kExitTimeout = 3;

void printUsage()
{
    std::printf("用法: relayer-cli <子命令> [选项]\n"
                "\n"
                "子命令:\n"
                "  send <本地文件> --to <对端id>   把文件发给在线的对端\n"
                "        [--as <相对名>]           对端落点名，可带目录段（如 docs/a.bin）\n"
                "  recv --to-dir <目录>            等待别人发来文件，落到目录里\n"
                "        [--overwrite]            目标已存在时覆盖（默认拒绝，保护已有文件）\n"
                "  ls --to <对端id> [--path <相对路径>]  列出对端共享目录（远端浏览/粗校验）\n"
                "  op --to <对端id> --op <动作> --path <对端路径>\n"
                "                                  让对端做一次文件操作（PROTOCOL.md §11.12）：\n"
                "        --op sha256               算这个文件的 SHA-256\n"
                "        --op rename --new-name N  改名（拒绝覆盖已存在的目标）\n"
                "        --op mkdir                新建目录（含中间层）\n"
                "        --op unpack               把 .tar.xz 解到同目录的同名子目录\n"
                "        --op delete               删除多条 --path（目录递归）\n"
                "  pack <输出.tar.xz> <路径>...     本地压缩（不联网）\n"
                "  unpack <包.tar.xz>               本地解压（不联网；手动压缩的包）\n"
                "  list                            只连上去列出在线对端\n"
                "\n"
                "通用选项:\n"
                "  -H, --host <addr>        中继地址（默认 127.0.0.1）\n"
                "  -p, --port <port>        中继端口（默认 9527）\n"
                "      --password <pw>      认证口令\n"
                "      --no-auth            中继已用 --allow-no-auth 启动时必须加这个\n"
                "                           （与 --password 互斥；不自动推断，防止漏填口令静默降级）\n"
                "      --id <name>          声明本机标识（重连后保持不变；被别人占用则连接失败）\n"
                "      --self-send <file>   自检：recv 时把这个文件发给自己（验完整链路）\n"
                "      --self-browse        自检：recv 时在接收会话上列一次本机根目录\n"
                "      --pull-from <id>     取回：recv 时请这个对端把文件发过来\n"
                "      --pull-path <path>   取回的文件（对端的绝对路径）\n"
                "      --pull-dir <path>    取回的目录（对端打包成 tar.xz，收到后自动解开）\n"
                "      --unpack             收到 .tar.xz 就解开并删除包\n"
                "      --path <path>        ls 的绝对路径（不给 = 列出根：盘符 + 家目录）\n"
                "      --chunk-size <bytes> 分块大小（默认 256 KiB，上限 4 MiB）\n"
                "      --idle-timeout <sec> 传输开始后多久没有进展就判定对端失联（默认 60）\n"
                "                           传输开始前不计时：recv 会一直等，除非给 --hard-timeout\n"
                "      --hard-timeout <sec> 整个操作的硬上限，0 表示不限（默认 0）\n"
                "  -q, --quiet              不打印进度，只在结束时输出一行结论\n"
                "      --stats              结束时额外输出一行统计（STATS k=v ...），供脚本判断\n"
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
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[i]);
    return buf;
}

std::string humanSpeed(double bps)
{
    if (bps <= 0.0) {
        return "-";
    }
    return humanBytes(static_cast<std::uint64_t>(bps)) + "/s";
}

struct Args {
    std::string command;
    std::string file;
    std::string to;
    std::string toDir;

    // ls 的相对路径（相对对端共享根，空 = 根目录）。
    std::string path;

    // op 的操作种类与重命名目标名（见 parseArgs 里的 op 段）。
    std::string opKind;
    std::string newName;

    // pack / unpack 的位置参数（pack: 输出包 + 各源；unpack: 包）。
    std::vector<std::string> inputs;

    // send 的 --as：对端落点的相对名（可带目录段，如 "docs/a.bin"）。
    // 空则用本地文件名。安全性由接收方负责（路径校验 + 中间目录建齐，
    // 见 PROTOCOL.md §9.4 的 root 说明）。
    std::string asName;

    relayer::cli::ClientConfig config;
    bool quiet = false;

    // 结束时多打一行机器可读的统计（STATS k=v ...）。
    //
    // 为什么不是默认输出：正常用户的结论只有"发送完成"一句，计数只会
    // 让人以为出了问题。而集成测试需要一个**契约行**来断言"丢包只补了
    // 空洞、没有全量重传"——那件事没有别的可观测痕迹。
    bool stats = false;
};

struct ParseResult {
    bool ok = false;
    bool showHelp = false;
    std::string error;
    Args args;
};

bool parseU64(const std::string& s, std::uint64_t& out)
{
    try {
        std::size_t pos = 0;
        const auto v = std::stoull(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = v;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

ParseResult parseArgs(int argc, char** argv)
{
    ParseResult r;

    if (argc < 2) {
        r.error = "缺少子命令";
        return r;
    }

    r.args.command = argv[1];
    if (r.args.command == "-h" || r.args.command == "--help") {
        r.showHelp = true;
        r.ok = true;
        return r;
    }
    if (r.args.command != "send" && r.args.command != "recv" && r.args.command != "list" && r.args.command != "ls" &&
        r.args.command != "pack" && r.args.command != "unpack" && r.args.command != "op") {
        r.error = "未知子命令: " + r.args.command;
        return r;
    }

    // send 的第一个位置参数是文件。刻意只支持"文件紧跟在子命令后"这一种
    // 写法：让位置参数出现在选项之后（`send --to x file`）需要判断"这个
    // 裸词是值还是位置参数"，而 --to 的值也是裸词，二者无法可靠区分。
    int i = 2;
    if (r.args.command == "send") {
        if (i >= argc || argv[i][0] == '-') {
            r.error = "send 需要指定要发送的文件";
            return r;
        }
        r.args.file = argv[i++];
    }

    // pack / unpack 是**纯本地**操作，不连中继。所以在这里就解析完并返回，
    // 跳过后面所有的网络参数校验（认证方式、--to、--to-dir 等都不适用）。
    if (r.args.command == "pack" || r.args.command == "unpack") {
        for (; i < argc && argv[i][0] != '-'; ++i) {
            r.args.inputs.emplace_back(argv[i]);
        }
        if (i < argc) {
            r.error = r.args.command + " 不接受选项";
            return r;
        }
        if (r.args.command == "pack" && r.args.inputs.size() < 2) {
            r.error = "pack 需要：pack <输出.tar.xz> <路径>...";
            return r;
        }
        if (r.args.command == "unpack" && r.args.inputs.size() != 1) {
            r.error = "unpack 需要：unpack <包.tar.xz>";
            return r;
        }
        r.ok = true;
        return r;
    }

    for (; i < argc; ++i) {
        const std::string_view a = argv[i];

        const auto takeValue = [&](std::string_view opt, std::string& out) -> bool {
            if (i + 1 >= argc) {
                r.error = std::string(opt) + " 需要一个值";
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (a == "-h" || a == "--help") {
            r.showHelp = true;
            r.ok = true;
            return r;
        } else if (a == "-H" || a == "--host") {
            if (!takeValue(a, r.args.config.host)) {
                return r;
            }
        } else if (a == "-p" || a == "--port") {
            std::string v;
            if (!takeValue(a, v)) {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0 || n > 65535) {
                r.error = "端口非法: " + v;
                return r;
            }
            r.args.config.port = static_cast<std::uint16_t>(n);
        } else if (a == "--password") {
            if (!takeValue(a, r.args.config.password)) {
                return r;
            }
        } else if (a == "--no-auth") {
            // 显式声明"中继已关闭认证"。与 --password 互斥的校验放在
            // 解析后的统一检查里，和子命令必备参数的检查放在一起。
            r.args.config.serverAllowNoAuth = true;
        } else if (a == "--id") {
            if (!takeValue(a, r.args.config.requestedId)) {
                return r;
            }
        } else if (a == "--chunk-size") {
            std::string v;
            if (!takeValue(a, v)) {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0 || n > (1u << 20) * 16) {
                r.error = "分块大小非法: " + v;
                return r;
            }
            r.args.config.chunkSize = static_cast<std::uint32_t>(n);
        } else if (a == "--idle-timeout") {
            std::string v;
            if (!takeValue(a, v)) {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n) || n == 0) {
                r.error = "空闲超时非法: " + v;
                return r;
            }
            r.args.config.idleTimeout = std::chrono::seconds(n);
        } else if (a == "--hard-timeout") {
            std::string v;
            if (!takeValue(a, v)) {
                return r;
            }
            std::uint64_t n = 0;
            if (!parseU64(v, n)) {
                r.error = "总时长上限非法: " + v;
                return r;
            }
            r.args.config.hardTimeout = std::chrono::seconds(n);
        } else if (a == "-q" || a == "--quiet") {
            r.args.quiet = true;
        } else if (a == "--stats") {
            r.args.stats = true;
        } else if (a == "--to") {
            if (!takeValue(a, r.args.to)) {
                return r;
            }
        } else if (a == "--to-dir") {
            if (!takeValue(a, r.args.toDir)) {
                return r;
            }
        } else if (a == "--as") {
            if (!takeValue(a, r.args.asName)) {
                return r;
            }
        } else if (a == "--self-send") {
            // 自检：recv 时把自己的这个文件也发给"自己"（服务端绕回本连接）。
            // 一条命令验完中继 + 认证 + 传输 + 落盘，不需要第二台机器。
            if (!takeValue(a, r.args.config.selfSendPath)) {
                return r;
            }
        } else if (a == "--self-browse") {
            // 自检列举：recv 时在**接收会话上**向自己要一次目录。走的是 GUI
            // 远端浏览那条路（browseOnSession），CLI 的 ls 走的是另一条
            // （一次性连接），两条不能互相替代。
            r.args.config.selfBrowse = true;
        } else if (a == "--pull-from") {
            // 远端取回（PROTOCOL.md §11.9）：recv 时请这个对端把文件发过来。
            if (!takeValue(a, r.args.config.pullFrom)) {
                return r;
            }
        } else if (a == "--pull-path") {
            if (!takeValue(a, r.args.config.pullPath)) {
                return r;
            }
        } else if (a == "--unpack") {
            // 收到 .tar.xz 就解开（对别人主动发来的包也生效）。
            r.args.config.unpackArchives = true;
        } else if (a == "--pull-dir") {
            // 目录取回：请对端打包成 tar.xz 再发（PROTOCOL.md §11.10）。
            // 复用 pullPath 存路径，用 pullAsArchive 区分"文件"与"目录"——
            // 两者在请求侧只差一个帧类型。
            if (!takeValue(a, r.args.config.pullPath)) {
                return r;
            }
            r.args.config.pullAsArchive = true;
            // 目录取回默认解包：用户要的是"目录"，不是一个 .tar.xz。
            // 想保留包就自己解——但那是少数需求，多数人拿到包只会问"东西呢"。
            r.args.config.unpackArchives = true;
        } else if (a == "--path") {
            std::string value;
            if (!takeValue(a, value)) {
                return r;
            }
            if (r.args.command == "op") {
                // op 允许多次 --path：delete 要删的是一批路径。
                r.args.inputs.push_back(std::move(value));
            } else {
                r.args.path = std::move(value);
            }
        } else if (a == "--op") {
            if (!takeValue(a, r.args.opKind)) {
                return r;
            }
        } else if (a == "--new-name") {
            if (!takeValue(a, r.args.newName)) {
                return r;
            }
        } else if (a == "--overwrite") {
            // 接收目标已存在时删旧收新。默认拒绝覆盖（保护用户文件），
            // "反复发同一个文件到同一目录"的场景需要显式打开。
            r.args.config.overwriteExisting = true;
        } else {
            r.error = "未知选项: " + std::string(a);
            return r;
        }
    }

    // 子命令的必备参数。放在解析之后统一检查，这样 "send 缺 --to" 与
    // "recv 缺 --to-dir" 都能得到明确的说法，而不是靠解析顺序碰运气。
    if (r.args.command == "send" && r.args.to.empty()) {
        r.error = "send 需要 --to 指定目标对端";
        return r;
    }
    if (r.args.command == "recv" && r.args.toDir.empty()) {
        r.error = "recv 需要 --to-dir 指定落点目录";
        return r;
    }
    if (r.args.command == "ls" && r.args.to.empty()) {
        r.error = "ls 需要 --to 指定要列举哪个对端";
        return r;
    }

    if (r.args.command == "op") {
        // 子命令与它的必备参数，说法与其它子命令一致（缺什么直说）。
        if (r.args.to.empty()) {
            r.error = "op 需要 --to 指定对端";
            return r;
        }
        if (r.args.opKind.empty()) {
            r.error = "op 需要 --op <sha256|mkdir|rename|delete|unpack>";
            return r;
        }
        if (r.args.inputs.empty()) {
            r.error = "op 需要 --path 指定目标路径";
            return r;
        }
        if (r.args.opKind == "rename" && r.args.newName.empty()) {
            r.error = "op rename 需要 --new-name 指定新名字";
            return r;
        }

        auto& req = r.args.config.fileOp;
        req.peerId = r.args.to;

        if (r.args.opKind == "sha256") {
            req.kind = relayer::cli::FileOpKind::kSha256;
            req.path = r.args.inputs[0];
        } else if (r.args.opKind == "mkdir") {
            req.kind = relayer::cli::FileOpKind::kCreateDir;
            req.path = r.args.inputs[0];
        } else if (r.args.opKind == "unpack") {
            req.kind = relayer::cli::FileOpKind::kUnarchive;
            req.path = r.args.inputs[0];
        } else if (r.args.opKind == "rename") {
            req.kind = relayer::cli::FileOpKind::kRename;
            req.path = r.args.inputs[0];
            // 协议里的目标是**完整路径**而不是单段名（与其它帧一致），
            // 所以在这里拼同目录。两种分隔符都要认：用户从资源管理器
            // 粘过来的是反斜杠。
            const auto slash = req.path.find_last_of("/\\");
            req.newPath = (slash == std::string::npos) ? r.args.newName : req.path.substr(0, slash + 1) + r.args.newName;
        } else if (r.args.opKind == "delete") {
            req.kind = relayer::cli::FileOpKind::kDelete;
            req.paths = r.args.inputs;
        } else {
            r.error = "未知的 --op 取值: " + r.args.opKind;
            return r;
        }
    }

    if (r.args.config.selfBrowse && !r.args.path.empty()) {
        // --self-browse 默认列根列表；给了 --path 就列那个绝对路径。
        r.args.config.selfBrowsePath = r.args.path;
        r.args.path.clear();   // 免得 ls 的校验把它当命令参数
    }

    if (!r.args.config.selfSendPath.empty() && r.args.command != "recv") {
        // 自检发送是"接收会话上搭车发送"，只有 recv 有接收会话。
        r.error = "--self-send 只能与 recv 一起用";
        return r;
    }
    if (!r.args.config.pullFrom.empty() && r.args.command != "recv") {
        // 取回要搭在接收会话上：回程的文件需要它收下。
        r.error = "--pull-from 只能与 recv 一起用";
        return r;
    }
    if (!r.args.config.pullPath.empty() && r.args.config.pullFrom.empty()) {
        r.error = "--pull-path 需要配合 --pull-from（说明从哪个对端取）";
        return r;
    }
    if (r.args.config.pullAsArchive && r.args.command != "recv") {
        r.error = "--pull-dir 只能与 recv 一起用";
        return r;
    }
    if (!r.args.config.pullFrom.empty() && r.args.config.pullPath.empty()) {
        // 空路径指的是共享根（一个目录），取回它只会得到"不存在或目录"。
        // 与其让对方回一个含糊的 kNotFound，不如在参数校验处就说清楚。
        r.error = "--pull-from 需要配合 --pull-path 指定要取回哪个文件";
        return r;
    }
    if (r.args.config.selfBrowse && r.args.command != "recv") {
        // 同理：要搭在接收会话上。
        r.error = "--self-browse 只能与 recv 一起用";
        return r;
    }
    if (r.args.config.selfBrowse && !r.args.config.selfSendPath.empty()) {
        // 两者都靠"做完就退出"，同时给会让退出条件含糊（列完就退还是传完才退）。
        r.error = "--self-browse 与 --self-send 不能同时使用";
        return r;
    }

    // 认证方式必须恰好声明一种。两者都不给时客户端不知道服务端是什么
    // 配置：按认证走会被无认证中继拒绝，按无认证走会被认证中继拒绝。
    // 与 relayerd 的"口令与 allow-no-auth 必须恰好给一个"是同一条规则
    // 在两端的镜像。
    if (r.args.config.serverAllowNoAuth && !r.args.config.password.empty()) {
        r.error = "--no-auth 与 --password 不能同时给出";
        return r;
    }
    // 上面那句注释一直写着"必须恰好声明一种"，但这条分支此前是缺的：
    // 什么都不给时客户端会拿着空口令按认证模式去连，对无认证中继得到的
    // 是协议层的"unauthenticated peer sent non-auth frame"——话不算错，
    // 但要用户从协议错误里反推"我漏了个参数"，不如在这里就说清。
    // 由 e2e 的 no-auth 场景看门（parseArgs 在匿名命名空间里，单测够不着）。
    if (!r.args.config.serverAllowNoAuth && r.args.config.password.empty()) {
        r.error = "必须提供 --password，或用 --no-auth 声明中继已关闭认证";
        return r;
    }

    r.ok = true;
    return r;
}

}   // namespace

int main(int argc, char** argv)
{
    // 同 relayerd：先修终端编码再输出任何东西。本文件目前全用 printf 系
    // （用法、报错、进度行），关键是 use_utf8_locale()；init() 一并调用，
    // 这样以后加 std::cout 输出时不必记得回来补一句。
    // 重定向场景字节不变，e2e 的 ASSIGNED_ID= / STATS 契约行不受影响。
    relayer::console::init();
    relayer::console::use_utf8_locale();

    const auto parsed = parseArgs(argc, argv);
    if (!parsed.ok) {
        std::fprintf(stderr, "错误: %s\n\n", parsed.error.c_str());
        printUsage();
        return kExitUsage;
    }
    if (parsed.showHelp) {
        printUsage();
        return kExitOk;
    }

    const auto& a = parsed.args;

    // 本地压缩 / 解压：不联网，直接做完了返回。
    if (a.command == "pack") {
        std::vector<std::string> sources(a.inputs.begin() + 1, a.inputs.end());
        std::string err;
        if (!relayer::cli::Client::createArchive(sources, a.inputs.front(), err)) {
            std::fprintf(stderr, "压缩失败：%s\n", err.c_str());
            return 2;
        }
        std::printf("已压缩：%s\n", a.inputs.front().c_str());
        return kExitOk;
    }
    if (a.command == "unpack") {
        // false：手动压缩的包**带顶层目录**，解到包的父目录即可。取回目录的
        // 包形状相反（无顶层），那条路径由接收流程自己处理，不走这里。
        const auto dir = relayer::cli::Client::unpackArchive(a.inputs.front(), false);
        if (dir.empty()) {
            std::fprintf(stderr, "解压失败（不是 tar.xz，或包已损坏）\n");
            return 2;
        }
        std::printf("已解压到：%s\n", dir.c_str());
        return kExitOk;
    }

    asio::io_context ctx;

    bool done = false;
    relayer::cli::Outcome outcome;

    // 进度行用 \r 覆盖同一行：一个大文件会触发几百次上报，逐行打印会
    // 把终端刷满，而用户真正要看的只有最后那行结论。quiet 时连这行都省。
    bool progressDirty = false;

    auto client = std::make_shared<relayer::cli::Client>(
        ctx, a.config,
        [quiet = a.quiet, &progressDirty](const relayer::cli::ProgressReport& p) {
            if (quiet) {
                return;
            }
            const double pct =
                p.bytesTotal > 0 ? 100.0 * static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal) : 0.0;
            std::fprintf(stderr, "\r%5.1f%%  %s / %s  %s   ", pct, humanBytes(p.bytesDone).c_str(),
                         humanBytes(p.bytesTotal).c_str(), humanSpeed(p.bytesPerSecond).c_str());
            std::fflush(stderr);
            progressDirty = true;
        },
        [&done, &outcome](const relayer::cli::Outcome& o) {
            outcome = o;
            done = true;
        });

    if (a.command == "send") {
        client->sendFile(a.file, a.asName, a.to);
    } else if (a.command == "recv") {
        client->receiveFile(a.toDir);
    } else if (a.command == "ls") {
        client->browsePeer(a.to, a.path);
    } else if (a.command == "op") {
        client->runFileOp(a.config.fileOp);
    } else {
        client->listPeers();
    }

    // 等结论。
    //
    // 这里**不能**用"`run_for()` 返回 0 就等于内部状态机停了"来判断：返回 0
    // 只意味着这 50ms 里没有 handler 跑到，而"安静地等着对端发文件"完全
    // 合法——recv 的常态就是长时间无事发生。上一版之所以看起来能这么判断，
    // 是因为它有个 10ms 的 pump 定时器在不停唤醒事件循环，恰好掩盖了这条
    // 判据本身不成立。
    //
    // 结论由 Client 保证：连接阶段有连接看门狗（connectTimeout），每个请求
    // 有应答超时，整个操作可选 hardTimeout。三者覆盖了"该有结论"的所有情形。
    while (!done) {
        if (ctx.stopped()) {
            ctx.restart();
        }
        ctx.run_for(std::chrono::milliseconds(50));
    }

    // 进度行之后要换行，否则结论会接在进度条后面。
    if (progressDirty) {
        std::fprintf(stderr, "\n");
    }

    // 统计行在结论之前打：脚本通常按"先拿到 STATS，再判断退出码"的顺序
    // 处理，而结论那行在失败时走 stderr。同一路输出（stdout）便于抓。
    if (a.stats) {
        const auto& s = outcome.stats;
        std::printf("STATS chunksSent=%llu chunksReceived=%llu chunksRetransmitted=%llu "
                    "duplicateChunks=%llu acksSent=%llu acksReceived=%llu\n",
                    static_cast<unsigned long long>(s.chunksSent), static_cast<unsigned long long>(s.chunksReceived),
                    static_cast<unsigned long long>(s.chunksRetransmitted), static_cast<unsigned long long>(s.duplicateChunks),
                    static_cast<unsigned long long>(s.acksSent), static_cast<unsigned long long>(s.acksReceived));
        std::fflush(stdout);
    }

    // ls 的契约输出：先一行机器可读的汇总，再逐条。e2e 断言靠这些行，
    // 所以格式固定（不用本地化文案）。
    // --self-browse 用同一套格式：它验的是 GUI 那条路，但结论同样是"一个
    // 目录列举结果"，让 e2e 用同一组断言比对两种情况才有可比性。
    if ((a.command == "ls" || a.config.selfBrowse) && outcome.ok) {
        std::printf("REMOTE_LIST status=%d count=%zu\n", static_cast<int>(outcome.listStatus), outcome.remoteEntries.size());
        for (const auto& e : outcome.remoteEntries) {
            if (e.isDir) {
                std::printf("D  %s\n", e.name.c_str());
            } else {
                std::printf("F  %llu  %s\n", static_cast<unsigned long long>(e.size), e.name.c_str());
            }
        }
        std::fflush(stdout);
    }

    // op 的契约输出：机器可读、格式固定（e2e 断言靠它，不做本地化）。
    if (a.command == "op") {
        if (!outcome.sha256Hex.empty()) {
            std::printf("OP_SHA256=%s\n", outcome.sha256Hex.c_str());
        }
        if (!outcome.extractedDir.empty()) {
            std::printf("OP_DIR=%s\n", outcome.extractedDir.c_str());
        }
        for (const auto& p : outcome.failedPaths) {
            std::printf("OP_FAILED=%s\n", p.c_str());
        }
        std::fflush(stdout);
    }

    if (outcome.ok) {
        std::printf("%s\n", outcome.summary.c_str());
        return kExitOk;
    }
    std::fprintf(stderr, "%s\n", outcome.summary.c_str());
    if (!outcome.status.ok() && !outcome.status.message.empty()) {
        std::fprintf(stderr, "  原因: %s\n", outcome.status.message.c_str());
    }

    // 超时单独一个退出码，见文件头的说明。
    if (outcome.status.code == relayer::ErrorCode::kTimeout) {
        return kExitTimeout;
    }
    return kExitFailed;
}
