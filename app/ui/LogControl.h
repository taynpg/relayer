#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui
{
class LogControl;
}
QT_END_NAMESPACE

// 日志面板。v1 的 LogControl 去掉 Logger/Setting 依赖的版本：
//   - v1 通过 Logger 单例的五个信号喂日志；v2 的 app 还没有日志门面，
//     E3a 用两个入口：appendLog()（程序内显式调用）与 Qt 消息处理
//     （qWarning/qInfo 被 MainWindow 转发进来）。
//   - 右键菜单提供"设置接收目录"与"清空"：接收目录是自动接收的落点，
//     Setting 对话框全量迁移之前先借这里的右键菜单安家，不新增
//     .ui 控件（布局零改动约束）。
class LogControl : public QDialog
{
    Q_OBJECT

public:
    explicit LogControl(QWidget *parent = nullptr);
    ~LogControl() override;

    LogControl(const LogControl &) = delete;
    LogControl &operator=(const LogControl &) = delete;

public slots:
    void appendLog(const QString &msg);

signals:
    // 用户在右键菜单里改了接收目录。
    void receiveDirChanged(const QString &dir);

private:
    void initMenu();

    Ui::LogControl *ui_;
};
