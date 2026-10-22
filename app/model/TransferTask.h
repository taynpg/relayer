#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

// 传输任务的 Qt 侧数据模型。
//
// 放在 model/ 而不是某个控件里：ComparisonControl（配置表）是生产者，
// MainWindow / RelayTask（E3.5）是消费者，两侧都只依赖这个头。
//
// 与 v1 RelayTaskData/FileItemData 的对应关系：
//   name      <- FileItemData::name
//   isDir     <- RFileType::mTypeDir
//   localDir  <- FileItemData::localRoot（文件所在目录或目录本身）
//   peerId    <- v1 没有（v1 固定"当前通信对象"），v2 每行可指定对端
//   relDir    <- FileItemData::remoteRoot 的 v2 语义化：不再是对方的
//                绝对路径，而是"对方接收根目录之下的相对目录"（协议层
//                由 kFileRequestSend 的 root 字段承载，见 PROTOCOL.md §9.4）
namespace relayer::app
{

struct RelayTaskItem
{
    QString name;  // 文件/目录名（不含路径）
    bool isDir = false;
    QString localDir;  // 本地根目录（文件所在目录，或目录本身）
    QString peerId;    // 目标对端 id（空 = 未指定，发送前必须补上）
    QString relDir;    // 对方接收根目录之下的相对目录（可为空 = 直接落根）
};

inline bool operator==(const RelayTaskItem &a, const RelayTaskItem &b)
{
    return a.name == b.name && a.isDir == b.isDir && a.localDir == b.localDir && a.peerId == b.peerId &&
           a.relDir == b.relDir;
}

using RelayTaskItems = QList<RelayTaskItem>;

}  // namespace relayer::app

Q_DECLARE_METATYPE(relayer::app::RelayTaskItem)
Q_DECLARE_METATYPE(relayer::app::RelayTaskItems)
