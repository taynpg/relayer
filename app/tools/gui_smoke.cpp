// CoreService 冒烟测试：无窗口，真连一次中继。
//
// 为什么需要它：GUI 之前一条自动化用例都没有——核心那一层（协议、传输、
// 双连接）有 400 多个单元用例和 21 个三进程场景，而"GUI 到底连不连得上"
// 只能靠手点。手点的代价是"改了 CoreService 之后不知道有没有坏"，而它恰好
// 是最容易坏的一层（线程边界、Qt 字符串转换、事件投递）。
//
// 这一条覆盖的链路是：
//
//   startReceive → 控制连接认证 → 数据连接建立并 claim 名字
//                → 控制连接上的并发请求（列举） → 应答按请求号配对
//
// 用的是**自投**（to == 自己）把请求绕回来，所以不需要第二个进程：
// 中继把帧转回本连接，本进程同时当问的人和答的人（PROTOCOL.md §11.3.1）。
//
// 契约输出（供脚本断言，格式固定）：
//   SMOKE_ASSIGNED=<id>
//   SMOKE_BROWSE ok=<0|1> root=<0|1> count=<n> summary=<text>
//   SMOKE_FAIL <reason>
//
// 退出码：0 成功 / 2 请求没发出去 / 3 列举失败 / 4 会话提前结束 / 5 超时。

#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <string>

#include "bridge/CoreService.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    relayer::app::ConnInfo conn;
    conn.host = QStringLiteral("127.0.0.1");
    conn.port = 9527;
    conn.noAuth = true;
    QString dir = QStringLiteral(".");

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](QString& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = QString::fromUtf8(argv[++i]);
            return true;
        };
        QString v;
        if (a == "--host" && next(v)) {
            conn.host = v;
        } else if (a == "--port" && next(v)) {
            conn.port = static_cast<quint16>(v.toUShort());
        } else if (a == "--dir" && next(v)) {
            dir = v;
        } else if (a == "--id" && next(v)) {
            conn.requestedId = v;
        } else if (a == "--password" && next(v)) {
            conn.password = v;
            conn.noAuth = false;
        }
    }

    relayer::app::CoreService core;
    bool browsed = false;

    // 会话提前结束（认证失败、中继断开）：直接报出来，别等到超时。
    // 四个参数（多一个 context object）不是风格问题：三个参数的
    // `connect(sender, signal, functor)` 是**直连**，lambda 会在 emit 信号的
    // 那个线程（这里是 io 线程）执行。那样一来 `app.exit()` 之后主线程立刻
    // 开始析构 CoreService，而 io 线程还在回调栈里访问 Client —— 段错误，
    // 而且 Debug 下因为时序不同看不出来，只在 Release 崩。
    QObject::connect(&core, &relayer::app::CoreService::operationFinished, &app,
                     [&](bool ok, const QString& summary, const QString& detail) {
                         std::printf("SMOKE_FAIL 会话结束 ok=%d summary=%s detail=%s\n", ok ? 1 : 0, summary.toUtf8().constData(),
                                     detail.toUtf8().constData());
                         std::fflush(stdout);
                         if (!browsed) {
                             app.exit(4);
                         }
                     });

    QObject::connect(&core, &relayer::app::CoreService::assignedIdChanged, &app, [&](const QString& id) {
        std::printf("SMOKE_ASSIGNED=%s\n", id.toUtf8().constData());
        std::fflush(stdout);
        // 自投列举：向**自己**要一次根目录列表。
        //
        // 这条请求与"接收会话"共用控制连接——正是 GUI 的浏览所走的路径
        // （browsePeerOnSession），也是双连接改造要保住的那条路。
        if (!core.browsePeerOnSession(id, QString())) {
            std::printf("SMOKE_FAIL 列举请求没发出去（没有常驻会话）\n");
            std::fflush(stdout);
            app.exit(2);
        }
    });

    QObject::connect(&core, &relayer::app::CoreService::remoteBrowseFinished, &app,
                     [&](bool ok, bool root, const QString& summary, const QList<relayer::app::RemoteRow>& rows) {
                         browsed = true;
                         std::printf("SMOKE_BROWSE ok=%d root=%d count=%lld summary=%s\n", ok ? 1 : 0, root ? 1 : 0,
                                     static_cast<long long>(rows.size()), summary.toUtf8().constData());
                         std::fflush(stdout);
                         app.exit(ok ? 0 : 3);
                     });

    core.startReceive(dir, conn);

    // 兜底：本机回环上 15 秒足够走完认证 + 建立数据连接 + 一次列举往返。
    QTimer::singleShot(15000, [&] {
        std::printf("SMOKE_FAIL 超时\n");
        std::fflush(stdout);
        app.exit(5);
    });

    return app.exec();
}
