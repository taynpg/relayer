#include "InfoDrop.h"

// 序列化格式即契约：字段顺序与 v1 完全一致（fileName、type / from、to、
// itemCount、items）。改顺序或加字段前先确认没有落盘数据依赖旧格式。

void InfoDropItem::serialize(QDataStream& stream) const
{
    stream << fileName << type;
}

void InfoDropItem::deserialize(QDataStream& stream)
{
    stream >> fileName >> type;
}

QDataStream& operator<<(QDataStream& stream, const InfoDropItem& item)
{
    item.serialize(stream);
    return stream;
}

QDataStream& operator>>(QDataStream& stream, InfoDropItem& item)
{
    item.deserialize(stream);
    return stream;
}

void InfoDrop::serialize(QDataStream& stream) const
{
    stream << from << to;
    // 显式定长类型：quint32 跨版本/跨平台宽度稳定，items.size() 的
    // int 不是（虽然目前所有目标平台都是 32 位 int，不赌）。
    const quint32 itemCount = static_cast<quint32>(items.size());
    stream << itemCount;
    for (const auto& item : items) {
        stream << item;
    }
}

void InfoDrop::deserialize(QDataStream& stream)
{
    stream >> from >> to;
    quint32 itemCount = 0;
    stream >> itemCount;
    // 恶意/损坏数据可能给出超大计数：QVector 越界或OOM都是崩溃路径。
    // 上限取一个远超真实需求的值（单次拖拽几百万项没有意义），超过即视为
    // 数据损坏，让 stream 置错并返回空。
    constexpr quint32 kMaxItems = 10u * 1000u * 1000u;
    if (itemCount > kMaxItems) {
        stream.setStatus(QDataStream::ReadCorruptData);
        items.clear();
        return;
    }
    items.resize(static_cast<int>(itemCount));
    for (quint32 i = 0; i < itemCount; ++i) {
        stream >> items[static_cast<int>(i)];
    }
}

QDataStream& operator<<(QDataStream& stream, const InfoDrop& infoDrop)
{
    infoDrop.serialize(stream);
    return stream;
}

QDataStream& operator>>(QDataStream& stream, InfoDrop& infoDrop)
{
    infoDrop.deserialize(stream);
    return stream;
}
