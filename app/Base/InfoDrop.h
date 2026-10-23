#pragma once

#include <QBuffer>
#include <QDataStream>
#include <QString>
#include <QVector>

// 拖拽载荷的 MIME 类型。ExplorerControl 与 ComparisonControl 之间互拖
// （以及自拖）都靠它识别"这是我们的拖拽"，与系统文件拖拽区分开。
#define MY_MIME_DROP_TYPE "application/x-info-drop"

// 附加标记：这次拖拽来自**远端**浏览面板，载荷里的 from 是相对共享根而不是
// 本地绝对路径。对照表的"本地目录"列据此拒收（落进去会写成一个谁都不认的
// 路径）。单独走一个 MIME 而不是往 InfoDrop 里加字段：那个结构是与 v1 的
// 序列化契约，加字段会让旧数据读不动。
#define MY_MIME_DROP_REMOTE_MARK "application/x-info-drop-remote"

// v2 与 v1 的差异只有命名空间式的整理；序列化格式不变——v1 的持久化
// 数据（如果有以此格式落盘的）必须还能读。
//
// infoPack/infoUnpack：T 要提供 serialize/deserialize(QDataStream&)。
// 打包后 seek(0) 是因为 QDataStream 不自动回卷，调用方拿到的必须是从头
// 开始的完整字节流。
template <typename T> QByteArray infoPack(const T &obj)
{
    QByteArray byteArray;
    QDataStream stream(&byteArray, QIODevice::ReadWrite);
    obj.serialize(stream);
    stream.device()->seek(0);
    return byteArray;
}

template <typename T> T infoUnpack(const QByteArray &byteArray)
{
    T obj;
    QDataStream stream(byteArray);
    obj.deserialize(stream);
    return obj;
}

struct InfoDropItem
{
    QString fileName;
    quint32 type;
    void serialize(QDataStream &stream) const;
    void deserialize(QDataStream &stream);
};

struct InfoDrop
{
    QString from;
    QString to;
    QVector<InfoDropItem> items;
    void serialize(QDataStream &stream) const;
    void deserialize(QDataStream &stream);
};

QDataStream &operator<<(QDataStream &stream, const InfoDropItem &item);
QDataStream &operator>>(QDataStream &stream, InfoDropItem &item);
QDataStream &operator<<(QDataStream &stream, const InfoDrop &infoDrop);
QDataStream &operator>>(QDataStream &stream, InfoDrop &infoDrop);
