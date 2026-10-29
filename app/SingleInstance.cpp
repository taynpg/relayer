#include "SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace relayer::app {

SingleInstance::SingleInstance(QString key, QObject* parent) : QObject(parent), key_(std::move(key))
{
}

SingleInstance::~SingleInstance()
{
    if (server_ != nullptr) {
        server_->close();
    }
}

bool SingleInstance::isPrimary()
{
    // 先探一次：能连上就是"已经有实例在跑"。300ms 是本地套接字的合理上限
    // ——对方就在同一台机器上，连不上就是没有。
    QLocalSocket probe;
    probe.connectToServer(key_);
    if (probe.waitForConnected(300)) {
        probe.disconnectFromServer();
        return false;
    }

    // 连不上有两种可能：真的没有实例，或上次进程被 kill 留下了 socket 文件
    // （Unix）。removeServer 对这两种情况都安全；Windows 上是命名管道，
    // 调用是无害的空操作。
    QLocalServer::removeServer(key_);

    server_ = new QLocalServer(this);
    if (!server_->listen(key_)) {
        // 监听失败：极短的竞态（另一个实例刚 listen 完还没 accept）。
        // 按"我是第二个实例"处理，交给调用方去激活已有的那个——
        // 这比强开第二个窗口安全。
        delete server_;
        server_ = nullptr;
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    return true;
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket* sock = server_->nextPendingConnection()) {
        connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
        // 载荷不参与判断：连上就是"请把窗口提到前台"。读掉是为了让
        // 对端的 waitForBytesWritten 立刻返回，不必等连接超时。
        sock->readAll();
        emit activateRequested();
    }
}

bool SingleInstance::notifyPrimary(int timeoutMs)
{
    QLocalSocket sock;
    sock.connectToServer(key_);
    if (!sock.waitForConnected(timeoutMs)) {
        return false;
    }
    sock.write("activate");
    sock.flush();
    sock.waitForBytesWritten(timeoutMs);
    sock.disconnectFromServer();
    return true;
}

}   // namespace relayer::app
