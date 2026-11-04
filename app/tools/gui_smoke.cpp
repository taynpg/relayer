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
//   SMOKE_SECURITY encrypted=<0|1> compressed=<0|1>
//   SMOKE_RECONNECTED
//   SMOKE_BROWSE ... phase=<1|2>      （--browse-twice 专用）
//   SMOKE_FAIL <reason>
//
// 退出码：0 成功 / 2 请求没发出去 / 3 列举失败 / 4 会话提前结束 / 5 超时。

#include <QCoreApplication>
#include <QSettings>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <string>

#include "bridge/CoreService.h"
#include "ui/SettingsKeys.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    relayer::app::ConnInfo conn;
    conn.host = QStringLiteral("127.0.0.1");
    conn.port = 9527;
    conn.noAuth = true;
    QString dir = QStringLiteral(".");
    // 非空 = 连上之后把这个文件发给**自己**，验完整的收发链路。
    // 用户实跑时第一步就挂在这里（"传输失败：session is closed"），
    // 而在此之前所有断言都只验到"能列目录"为止。
    QString selfSend;

    // 传输选项走**真实路径**：写进 QSettings，让 CoreService::makeConfig
    // 自己去读。直接传进 ConnInfo 就测不到"设置 → 配置"这一段了，而
    // 那段恰恰是新增的、最容易接错的地方（键名写错就静默失效）。
    bool compress = false;
    QString encryptPass;

    // QSettings 的落点。**必须**指到临时目录：不指的话这个测试会往用户
    // 真实的设置里写"开启加密 + 测试口令"，跑一次测试就把人家的客户端
    // 改成加了密的——而对方并不知道这个口令。
    QString settingsDir;

    // 列举两次：第一次成功后等一会儿再列第二次。中间由测试脚本杀掉中继
    // 再拉起来，用来验证"界面这一层也真的会重连"——重连逻辑在 CLI 那边
    // 有 e2e，但"CoreService 到底有没有把 ReconnectPolicy 打开"只有这里能证。
    bool browseTwice = false;

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
        } else if (a == "--self-send" && next(v)) {
            selfSend = v;
        } else if (a == "--compress") {
            compress = true;
        } else if (a == "--encrypt" && next(v)) {
            encryptPass = v;
        } else if (a == "--settings-dir" && next(v)) {
            settingsDir = v;
        } else if (a == "--browse-twice") {
            browseTwice = true;
        }
    }

    // QSettings 按 (组织, 应用) 分文件。与 app/main.cpp 保持一致，否则
    // 这里写的设置和真实客户端读的不是同一份——那种"测试过了、用户没生效"
    // 的假绿灯比没有测试更糟。
    QCoreApplication::setOrganizationName(QStringLiteral("relayer"));
    QCoreApplication::setApplicationName(QStringLiteral("relayer"));

    // 指到临时目录之后，CoreService::makeConfig 里的 `QSettings settings;`
    // 读的就是同一份 ini（默认格式已经改成 IniFormat）。
    if (!settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }

    if (compress || !encryptPass.isEmpty()) {
        QSettings settings;
        settings.setValue(relayer::app::settings::kSenderCompress, compress);
        settings.setValue(relayer::app::settings::kSecurityEncryptPass, encryptPass);
        settings.setValue(relayer::app::settings::kSecurityEncrypt, !encryptPass.isEmpty());
        settings.sync();
    }

    relayer::app::CoreService core;
    bool browsed = false;
    bool announced = false;
    int browseCount = 0;
    QString sessionId;

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

    // 实际生效的传输选项。这里断言的是"设置真的走到了 ClientConfig"——
    // 若键名写错，这一行会打印 0/0 而传输照样成功（加密只是没开），
    // 光看传输结果是发现不了的。
    QObject::connect(&core, &relayer::app::CoreService::securityChanged, &app, [&](bool encrypted, bool compressed) {
        std::printf("SMOKE_SECURITY encrypted=%d compressed=%d\n", encrypted ? 1 : 0, compressed ? 1 : 0);
        std::fflush(stdout);
    });

    QObject::connect(&core, &relayer::app::CoreService::assignedIdChanged, &app, [&](const QString& id) {
        // 声明了 --id 时，认证刚过的那一次还是临时号：那时发出去会打错人。
        if (!conn.requestedId.isEmpty() && id != conn.requestedId) {
            return;
        }
        // 重连之后再列举要用它。id 稳定（--id 声明过）所以重连前后是同一个。
        sessionId = id;
        if (announced) {
            return;
        }
        announced = true;

        std::printf("SMOKE_ASSIGNED=%s\n", id.toUtf8().constData());
        std::fflush(stdout);

        if (!selfSend.isEmpty()) {
            // 发文件给自己：走的是 GUI 的路径（sendFileTask → 常驻数据连接），
            // 覆盖"数据连接建起来 → 协商 → 传输 → 落盘"整条链路。
            //
            // 延后 300ms：startReceive 的建 Client 是 post 到 io 线程的，
            // 立刻发会撞上"还没有常驻会话"。
            QTimer::singleShot(300, &app, [&, id] {
                if (!core.sendFileTask(1, selfSend, QStringLiteral("smoke-sent.bin"), id, conn)) {
                    std::printf("SMOKE_FAIL 发送请求没发起（没有常驻会话）\n");
                    std::fflush(stdout);
                    app.exit(6);
                }
            });
            return;
        }

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

    // 第二次列举：重连刚回来时会话可能还没就绪（canOperate 为假），
    // 所以不在定时器里一把梭，而是每 300ms 试一次，最多 20 次。
    auto secondBrowse = std::make_shared<std::function<void(int)>>();
    *secondBrowse = [&core, &app, &sessionId, secondBrowse](int attempt) {
        if (core.browsePeerOnSession(sessionId, QString())) {
            return;
        }
        if (attempt >= 20) {
            std::printf("SMOKE_FAIL 重连之后列举请求发不出去（常驻会话没恢复）\n");
            std::fflush(stdout);
            app.exit(9);
            return;
        }
        QTimer::singleShot(300, &app, [secondBrowse, attempt] { (*secondBrowse)(attempt + 1); });
    };

    QObject::connect(&core, &relayer::app::CoreService::remoteBrowseFinished, &app,
                     [&](bool ok, bool root, const QString& summary, const QList<relayer::app::RemoteRow>& rows) {
                         browsed = true;
                         ++browseCount;
                         const int phase = browseCount;
                         std::printf("SMOKE_BROWSE ok=%d root=%d count=%lld phase=%d summary=%s\n", ok ? 1 : 0, root ? 1 : 0,
                                     static_cast<long long>(rows.size()), phase, summary.toUtf8().constData());
                         std::fflush(stdout);

                         if (!ok) {
                             app.exit(3);
                             return;
                         }
                         if (browseTwice && phase == 1) {
                             // 给测试脚本 6 秒窗口去杀中继（它看到 phase=1 就动手）。
                             QTimer::singleShot(6000, &app, [secondBrowse] { (*secondBrowse)(0); });
                             return;
                         }
                         app.exit(0);
                     });

    // 自投的结果。这两条是"发文件给自己"模式专用的；列举模式不看它们。
    QObject::connect(&core, &relayer::app::CoreService::fileReceived, &app, [&](const QString& path) {
        std::printf("SMOKE_RECEIVED=%s\n", path.toUtf8().constData());
        std::fflush(stdout);
        app.exit(0);
    });
    QObject::connect(&core, &relayer::app::CoreService::receiveFailed, &app, [&](const QString& msg) {
        std::printf("SMOKE_RECEIVE_FAILED=%s\n", msg.toUtf8().constData());
        std::fflush(stdout);
        // 只有"发文件给自己"模式才把它当失败：那条路在等文件落地。
        // 列举模式里收不到文件是**常态**，而且断线重连时接收侧本来就会报
        // 一条"连接中断，等待重连后由对端重新发起"——那是重连在起作用，
        // 不是错误（真实界面里它只写一行日志）。
        if (!selfSend.isEmpty()) {
            app.exit(7);
        }
    });
    QObject::connect(&core, &relayer::app::CoreService::sendTaskFinished, &app,
                     [&](qint64, bool ok, const QString& summary, const QString& detail) {
                         std::printf("SMOKE_SEND ok=%d summary=%s detail=%s\n", ok ? 1 : 0, summary.toUtf8().constData(),
                                     detail.toUtf8().constData());
                         std::fflush(stdout);
                         if (!ok) {
                             app.exit(8);
                         }
                     });

    QObject::connect(&core, &relayer::app::CoreService::reconnecting, &app, [&](int attempt, int delayMs) {
        std::printf("SMOKE_RECONNECTING attempt=%d delay_ms=%d\n", attempt, delayMs);
        std::fflush(stdout);
    });
    QObject::connect(&core, &relayer::app::CoreService::reconnected, &app, [&] {
        std::printf("SMOKE_RECONNECTED\n");
        std::fflush(stdout);
    });

    core.startReceive(dir, conn);

    // 兜底：本机回环上 15 秒足够走完认证 + 建立数据连接 + 一次列举往返。
    // --browse-twice 要额外等一轮"杀中继 + 重启 + 重连"，放宽到 45 秒。
    const int fallbackMs = browseTwice ? 45000 : 15000;
    QTimer::singleShot(fallbackMs, [&] {
        std::printf("SMOKE_FAIL 超时\n");
        std::fflush(stdout);
        app.exit(5);
    });

    return app.exec();
}
