#pragma once

#include <QString>

#include "relayer/core/session/PathText.h"

// 远端路径在 Qt 侧的薄包装。
//
// 真正的规则只有一份，在 `core/session/PathText`（Qt 无关、有单测）。这里
// 只做 QString ↔ std::string 的转换，避免每个控件各写一遍
// `replace('\\', '/')` 再加一堆边角处理——那种写法迟早出现
// `D:\/Download`（盘符根 + 斜杠 + 名字）。
namespace relayer::app {

// 规范成 `/` 分隔。空串保持空（协议里它表示"根列表"，不是当前目录）。
inline QString normalizeRemotePath(const QString& path)
{
    return QString::fromStdString(relayer::core::session::normalizePathText(path.toStdString()));
}

inline QString joinRemotePath(const QString& base, const QString& name)
{
    return QString::fromStdString(relayer::core::session::joinPathText(base.toStdString(), name.toStdString()));
}

// 空返回值 = 已在根列表之上，调用方据此回到根列表。
inline QString parentRemotePath(const QString& path)
{
    return QString::fromStdString(relayer::core::session::parentPathText(path.toStdString()));
}

}   // namespace relayer::app
