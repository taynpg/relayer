#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace relayer::app {

// 单实例守卫——v1 里 Third/SingleApplication 在 v2 的对应物。
//
// v1 用的是第三方库（共享内存 + 本地套接字，`instanceStarted` 信号里把已有
// 窗口 showNormal/raise/activateWindow）。v2 不引第三方：QLocalServer /
// QLocalSocket 就是这同一件事的最小实现，Qt6 自带，行为可控。
//
// 为什么值得有：这个工具的常见用法是"点一下图标，以为没开，又点一下"。
// 两个实例同时挂着会自动接收，第二个实例还会占掉一个中继 id，且它收到的
// 文件落点与第一个互相不知道——v1 已经踩过，别再退回去。
//
// 用法（main.cpp）：
//   SingleInstance guard(QStringLiteral("relayer-gui"));
//   if (!guard.isPrimary()) {
//       guard.notifyPrimary();
//       return 0;
//   }
//   connect(&guard, &SingleInstance::activateRequested, &w, [&w]{ ... });
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(QString key, QObject* parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // 尝试成为主实例并开始监听。
    // true  = 本进程是唯一实例，可以继续启动；
    // false = 已经有实例在跑（或监听失败），调用方应发一次激活请求后退出。
    [[nodiscard]] bool isPrimary();

    // 请已在运行的那个实例把窗口提到前台。
    //
    // 返回 false 表示没能通知到（对方正好在退出）。调用方**仍然应该退出**：
    // 监听端已被占用，再开一个窗口只会出现两份互不知情的实例。
    bool notifyPrimary(int timeoutMs = 1000);

signals:
    // 另一个实例请求激活（在 GUI 线程发出）。
    void activateRequested();

private:
    void onNewConnection();

    QString key_;
    QLocalServer* server_ = nullptr;
};

}   // namespace relayer::app
