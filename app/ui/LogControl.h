#pragma once

#include <QDate>
#include <QDialog>
#include <QFile>

QT_BEGIN_NAMESPACE
namespace Ui {
class LogControl;
}
QT_END_NAMESPACE

// 日志面板。v1 的 LogControl 去掉 Logger 依赖的版本：
//   - v1 通过 Logger 单例的五个信号喂日志，并同时写 log/relayFileGUI.log；
//   - v2 的 app 没有日志门面，两个入口就够：appendLog()（程序内显式调用）
//     与 Qt 消息处理（qWarning 及以上经 installMessageHandler 转进来）。
//
// 落盘：每条进界面的日志同时追加到
//   <AppDataLocation>/logs/gui-YYYY-MM-DD.log
// 按天分文件而不是单文件轮转：事后找"昨天那次失败"时不用在一坨混合日志里
// 翻。单日上限 16 MiB，超了就停写——日志是用来事后取证的，不是磁盘填充物。
//
// 右键菜单提供"设置接收目录"、"发送前检查"与"清空"（接收目录是自动接收的
// 落点，Setting 对话框全量迁移之前先借这里的右键菜单安家）。
class LogControl : public QDialog
{
    Q_OBJECT

public:
    explicit LogControl(QWidget* parent = nullptr);
    ~LogControl() override;

    LogControl(const LogControl&) = delete;
    LogControl& operator=(const LogControl&) = delete;

    // 把 Qt 的 warning/critical/fatal 转发进本控件（以及落盘文件）。
    //
    // qInstallMessageHandler 是进程级的，只装一次；本控件创建时调用，并把
    // 自己登记为转发目标。**只转 warning 及以上**：qDebug/qInfo 的噪声会
    // 淹掉结论行，v1 把所有级别都灌进日志的那份文件事后基本没法读。
    //
    // handler 可能在任意线程被调用，所以它只做一件事：排队到 GUI 线程。
    void installMessageHandler();

public slots:
    void appendLog(const QString& msg);

signals:
    // 用户在右键菜单里改了接收目录。
    void receiveDirChanged(const QString& dir);

private:
    static void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);

    void initMenu();

    // 按天打开（跨零点自动切）；失败时静默——界面日志不该因为磁盘满就停。
    void openLogFile(const QDate& day);
    void writeToFile(const QString& text);

    Ui::LogControl* ui_;
    QFile logFile_;
    QDate logDay_;
};
