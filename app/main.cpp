#include <QApplication>

#include "MainWindow.h"
#include "SingleInstance.h"
#include "relayer/common/TimerResolution.h"

int main(int argc, char* argv[])
{
    // GUI 自己跑事件循环，同样受 Windows 默认 15.6ms 定时器分辨率影响；
    // 而传输引擎的推进完全按 tick 走（见 TimerResolution.h）。
    const relayer::common::TimerResolutionGuard timerGuard;
    QApplication app(argc, argv);
    QApplication::setApplicationName("relayer");
    QApplication::setApplicationVersion(RELAYER_VERSION);
    QApplication::setOrganizationName("relayer");

    // --version 必须在单实例判定之前：命令行探测不该被"已经有实例在跑"挡住。
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("relayer v%s\n", RELAYER_VERSION);
        return 0;
    }

    // 单实例（v1 的 Third/SingleApplication）：第二个实例只负责把已有窗口
    // 提到前台，然后退出。
    relayer::app::SingleInstance guard(QStringLiteral("relayer-gui"));
    if (!guard.isPrimary()) {
        guard.notifyPrimary();
        return 0;
    }

    MainWindow w;
    QObject::connect(&guard, &relayer::app::SingleInstance::activateRequested, &w, [&w]() {
        // showNormal 先做：窗口可能已被最小化，raise/activateWindow 对它无效。
        w.showNormal();
        w.raise();
        w.activateWindow();
    });
    w.show();
    return QApplication::exec();
}
