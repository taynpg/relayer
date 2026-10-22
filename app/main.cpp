#include <QApplication>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("relayer");
    QApplication::setApplicationVersion(RELAYER_VERSION);

    if (argc == 2 && std::strcmp(argv[1], "--version") == 0)
    {
        std::printf("relayer v%s\n", RELAYER_VERSION);
        return 0;
    }

    MainWindow w;
    w.show();
    return QApplication::exec();
}
