#pragma once

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <cmath>

// 右键菜单自绘图标集
// 风格：扁平、16x16 逻辑画布、统一线条粗细、配色协调
// 不依赖 Qt SVG 模块，仅使用 QPainter 绘制，保证跨 Qt5/Qt6 与跨平台可用。
namespace MenuIcons {

// 统一调色板
inline QColor cTransfer() { return QColor("#2563EB"); }   // 蓝
inline QColor cUpload()   { return QColor("#16A34A"); }   // 绿
inline QColor cDownload() { return QColor("#2563EB"); }   // 蓝
inline QColor cDelete()   { return QColor("#DC2626"); }   // 红
inline QColor cRename()   { return QColor("#EA580C"); }   // 橙
inline QColor cInfo()     { return QColor("#475569"); }   // 石板灰
inline QColor cHash()     { return QColor("#7C3AED"); }   // 紫
inline QColor cCompress() { return QColor("#0D9488"); }   // 青绿
inline QColor cFolder()   { return QColor("#D97706"); }   // 琥珀
inline QColor cCopy()     { return QColor("#92400E"); }   // 棕
inline QColor cLocal()    { return QColor("#2563EB"); }   // 蓝
inline QColor cRemote()   { return QColor("#7C3AED"); }   // 紫
inline QColor cNew()      { return QColor("#4F46E5"); }   // 靛
inline QColor cCheck()    { return QColor("#16A34A"); }   // 绿
inline QColor cUncheck()  { return QColor("#64748B"); }   // 灰
inline QColor cFill()     { return QColor("#DB2777"); }   // 玫红
inline QColor cTime()     { return QColor("#0891B2"); }   // 青

// 创建设备像素比感知的透明画布（逻辑 px x px）
inline QPixmap makePixmap(int px = 16)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

// 画三角箭头：尖端在 tip，方向 dir 指向 tip
inline void drawArrowHead(QPainter& p, const QPointF& tip, const QPointF& dir, qreal size, const QColor& c)
{
    double len = std::hypot(dir.x(), dir.y());
    if (len < 1e-9) return;
    QPointF u = dir / len;
    QPointF n(-u.y(), u.x());
    QPointF base = tip - u * size;
    QPolygonF poly;
    poly << tip << (base + n * size * 0.55) << (base - n * size * 0.55);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPolygon(poly);
}

// 画带箭头的线段
inline void drawArrow(QPainter& p, const QPointF& from, const QPointF& to, qreal width, qreal headSize, const QColor& c)
{
    QPointF d = to - from;
    double len = std::hypot(d.x(), d.y());
    if (len < 1e-9) return;
    QPointF u = d / len;
    QPointF lineEnd = to - u * headSize * 0.6;
    p.setPen(QPen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(from, lineEnd);
    drawArrowHead(p, to, u, headSize, c);
}

// 填充文件夹（带 tab）
inline void drawFolder(QPainter& p, const QRectF& r, const QColor& c)
{
    QPainterPath path;
    path.moveTo(r.left(), r.top() + 1.5);
    path.lineTo(r.left() + 1.2, r.top());
    path.lineTo(r.left() + r.width() * 0.45, r.top());
    path.lineTo(r.left() + r.width() * 0.57, r.top() + 1.5);
    path.lineTo(r.right(), r.top() + 1.5);
    path.lineTo(r.right(), r.bottom());
    path.lineTo(r.left(), r.bottom());
    path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(path);
}

// ---------------- 各菜单项图标 ----------------

// 传输：双向水平箭头
inline QIcon transfer()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cTransfer();
    drawArrow(p, QPointF(2, 5), QPointF(14, 5), 1.6, 3.2, c);
    drawArrow(p, QPointF(14, 11), QPointF(2, 11), 1.6, 3.2, c);
    p.end();
    return QIcon(pm);
}

// 在资源管理器中打开：文件夹 + 放大镜
inline QIcon explorer()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawFolder(p, QRectF(1.5, 4.5, 9, 9), cFolder());
    // 放大镜
    auto glass = QColor("#0EA5E9");
    p.setPen(QPen(glass, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(11, 11), 3.0, 3.0);
    p.drawLine(QPointF(13.2, 13.2), QPointF(15, 15));
    p.end();
    return QIcon(pm);
}

// SHA256：井号 #
inline QIcon sha256()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cHash();
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    // 两条竖线
    p.drawLine(QPointF(6, 2.5), QPointF(6, 13.5));
    p.drawLine(QPointF(10, 2.5), QPointF(10, 13.5));
    // 两条横线
    p.drawLine(QPointF(3, 6), QPointF(13, 6));
    p.drawLine(QPointF(3, 10), QPointF(13, 10));
    p.end();
    return QIcon(pm);
}

// 解压缩：开口盒子 + 向上箭头抽出
inline QIcon extract()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cCompress();
    // 盒身
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    p.setBrush(QColor("#0D948833"));
    p.drawRoundedRect(QRectF(2.5, 7.5, 11, 6), 1.2, 1.2);
    // 向上箭头
    drawArrow(p, QPointF(8, 13), QPointF(8, 2.5), 1.6, 3.0, c);
    p.end();
    return QIcon(pm);
}

// 压缩：盒子 + 两侧向内箭头
inline QIcon compress()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cCompress();
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    p.setBrush(QColor("#0D948833"));
    p.drawRoundedRect(QRectF(2.5, 4.5, 11, 9), 1.2, 1.2);
    // 左→右
    drawArrow(p, QPointF(1, 8), QPointF(5.5, 8), 1.4, 2.6, c);
    // 右→左
    drawArrow(p, QPointF(15, 8), QPointF(10.5, 8), 1.4, 2.6, c);
    p.end();
    return QIcon(pm);
}

// 复制全路径：剪贴板 + 文本行
inline QIcon copyPath()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cCopy();
    // 夹子
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(5.5, 1.5, 5, 2.2), 0.8, 0.8);
    // 板
    p.setBrush(QColor("#F5F5F5"));
    p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawRoundedRect(QRectF(3, 3, 10, 11.5), 1.2, 1.2);
    // 文本行
    p.setPen(QPen(c, 1.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(4.5, 6), QPointF(11.5, 6));
    p.drawLine(QPointF(4.5, 8.5), QPointF(11.5, 8.5));
    p.drawLine(QPointF(4.5, 11), QPointF(9.5, 11));
    p.end();
    return QIcon(pm);
}

// 重命名：文本行 + 铅笔
inline QIcon rename()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cRename();
    p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(2, 4), QPointF(8, 4));
    p.drawLine(QPointF(2, 7), QPointF(7, 7));
    // 铅笔（斜）
    QPainterPath pen;
    pen.moveTo(QPointF(8.5, 14.5));
    pen.lineTo(QPointF(13.5, 9.5));
    pen.lineTo(QPointF(15, 11));
    pen.lineTo(QPointF(10, 16));
    // 反向延伸点
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(pen);
    // 笔尖（黑色三角）
    p.setBrush(QColor("#1F2937"));
    QPolygonF tip;
    tip << QPointF(8.5, 14.5) << QPointF(10, 16) << QPointF(9, 15);
    p.drawPolygon(tip);
    // 橡皮端（浅色）
    p.setBrush(QColor("#FCA5A5"));
    QPolygonF er;
    er << QPointF(13.5, 9.5) << QPointF(15, 11) << QPointF(14.2, 11.8) << QPointF(12.7, 10.3);
    p.drawPolygon(er);
    p.end();
    return QIcon(pm);
}

// 详细信息：信息圆圈 i
inline QIcon detail()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cInfo();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawEllipse(QPointF(8, 8), 6, 6);
    // 点
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QPointF(8, 4.6), 0.9, 0.9);
    // 竖
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(8, 6.6), QPointF(8, 11.4));
    p.end();
    return QIcon(pm);
}

// 删除：垃圾桶
inline QIcon del()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cDelete();
    // 把手
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(6, 1.5, 4, 1.6), 0.6, 0.6);
    // 盖
    p.drawRoundedRect(QRectF(3, 3.2, 10, 1.8), 0.6, 0.6);
    // 桶身
    QPainterPath body;
    body.moveTo(QPointF(4.2, 5));
    body.lineTo(QPointF(5, 14.5));
    body.lineTo(QPointF(11, 14.5));
    body.lineTo(QPointF(11.8, 5));
    body.closeSubpath();
    p.drawPath(body);
    // 竖纹
    p.setPen(QPen(QColor("#FCA5A5"), 1.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(6.5, 6.5), QPointF(6.8, 13));
    p.drawLine(QPointF(8, 6.5), QPointF(8, 13));
    p.drawLine(QPointF(9.5, 6.5), QPointF(9.2, 13));
    p.end();
    return QIcon(pm);
}

// 新建文件夹：文件夹 + 加号
inline QIcon newFolder()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawFolder(p, QRectF(1.5, 3.5, 11, 9), cFolder());
    // 加号徽章
    auto g = cCheck();
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(12, 12), 3.2, 3.2);
    p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(12, 10.2), QPointF(12, 13.8));
    p.drawLine(QPointF(10.2, 12), QPointF(13.8, 12));
    p.end();
    return QIcon(pm);
}

// 上传：向上箭头 + 底座
inline QIcon upload()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cUpload();
    drawArrow(p, QPointF(8, 12), QPointF(8, 3), 1.8, 3.4, c);
    // 底座
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(3, 14), QPointF(13, 14));
    p.end();
    return QIcon(pm);
}

// 下载：向下箭头 + 底座
inline QIcon download()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cDownload();
    drawArrow(p, QPointF(8, 3), QPointF(8, 12), 1.8, 3.4, c);
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(3, 14), QPointF(13, 14));
    p.end();
    return QIcon(pm);
}

// 新行：两行 + 加号
inline QIcon newRow()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cNew();
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(2, 4.5), QPointF(9, 4.5));
    p.drawLine(QPointF(2, 8.5), QPointF(9, 8.5));
    // 加号
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(12, 11.5), 3.2, 3.2);
    p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(12, 9.7), QPointF(12, 13.3));
    p.drawLine(QPointF(10.2, 11.5), QPointF(13.8, 11.5));
    p.end();
    return QIcon(pm);
}

// 访问本地目录：显示器 + 文件夹
inline QIcon accessLocal()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cLocal();
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor("#2563EB22"));
    p.drawRoundedRect(QRectF(1.5, 2, 13, 9), 1.2, 1.2);
    // 底座
    p.drawLine(QPointF(6, 11), QPointF(6, 13.5));
    p.drawLine(QPointF(10, 11), QPointF(10, 13.5));
    p.drawLine(QPointF(4, 14), QPointF(12, 14));
    // 内部文件夹
    drawFolder(p, QRectF(4.5, 4.5, 7, 5), cFolder());
    p.end();
    return QIcon(pm);
}

// 访问远程目录：云 + 文件夹
inline QIcon accessRemote()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cRemote();
    // 云
    QPainterPath cloud;
    cloud.addEllipse(QPointF(5.5, 7), 3.0, 3.0);
    cloud.addEllipse(QPointF(9, 5.5), 3.5, 3.5);
    cloud.addEllipse(QPointF(11.5, 7.5), 2.6, 2.6);
    cloud.addRect(QRectF(5.5, 7, 7, 3.5));
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawPath(cloud);
    // 云内文件夹（白色）
    drawFolder(p, QRectF(6, 7.5, 5, 3.5), Qt::white);
    p.end();
    return QIcon(pm);
}

// 打开本地所在目录：打开的文件夹
inline QIcon openDir()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cFolder();
    // 后片
    QPainterPath back;
    back.moveTo(QPointF(1.5, 4.5));
    back.lineTo(QPointF(3, 3));
    back.lineTo(QPointF(7, 3));
    back.lineTo(QPointF(8, 4.5));
    back.lineTo(QPointF(14.5, 5.5));
    back.lineTo(QPointF(14.5, 7));
    back.lineTo(QPointF(1.5, 7));
    back.closeSubpath();
    p.setBrush(c.darker(110));
    p.setPen(Qt::NoPen);
    p.drawPath(back);
    // 前片（更亮，呈打开状）
    QPainterPath front;
    front.moveTo(QPointF(1.5, 6.5));
    front.lineTo(QPointF(3.2, 14.5));
    front.lineTo(QPointF(13, 13));
    front.lineTo(QPointF(14.5, 6.5));
    front.closeSubpath();
    p.setBrush(c.lighter(115));
    p.drawPath(front);
    p.end();
    return QIcon(pm);
}

// 全选：勾选的复选框
inline QIcon selectAll()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cCheck();
    p.setBrush(QColor("#16A34A22"));
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 1.6, 1.6);
    // 勾
    QPainterPath tick;
    tick.moveTo(QPointF(5, 8.5));
    tick.lineTo(QPointF(7.3, 11));
    tick.lineTo(QPointF(11.5, 5.5));
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(tick);
    p.end();
    return QIcon(pm);
}

// 取消全选：空复选框
inline QIcon unselectAll()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cUncheck();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 1.6, 1.6);
    // 中间横线表示取消
    p.drawLine(QPointF(5, 8), QPointF(11, 8));
    p.end();
    return QIcon(pm);
}

// 粗校验：文档 + 放大镜 + 勾
inline QIcon check()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cCheck();
    // 文档（带折角）
    p.setPen(QPen(cInfo(), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(QColor("#F8FAFC"));
    QPainterPath doc;
    doc.moveTo(3.5, 2.5);
    doc.lineTo(11, 2.5);
    doc.lineTo(13, 4.5);
    doc.lineTo(13, 13.5);
    doc.lineTo(3.5, 13.5);
    doc.closeSubpath();
    p.drawPath(doc);
    // 折角
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#E2E8F0"));
    QPainterPath fold;
    fold.moveTo(11, 2.5);
    fold.lineTo(13, 4.5);
    fold.lineTo(11, 4.5);
    fold.closeSubpath();
    p.drawPath(fold);
    // 放大镜
    auto glass = cCheck();
    p.setPen(QPen(glass, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(7.5, 8), 3.6, 3.6);
    p.drawLine(QPointF(10.1, 10.6), QPointF(12, 12.5));
    // 镜内勾
    p.setPen(QPen(glass, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath tick;
    tick.moveTo(QPointF(5.8, 8));
    tick.lineTo(QPointF(7, 9.2));
    tick.lineTo(QPointF(9.2, 6.8));
    p.drawPath(tick);
    p.end();
    return QIcon(pm);
}

// 时间戳校验：时钟表盘 + 上下双向箭头（比较两端文件谁更新）
inline QIcon timeCheck()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cTime();
    // 时钟表盘
    p.setBrush(QColor("#F8FAFC"));
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(QPointF(6.6, 8.2), 4.6, 4.6);
    // 中心轴点
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(6.6, 8.2), 0.7, 0.7);
    // 时针、分针
    p.setPen(QPen(c, 1.3, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(6.6, 8.2), QPointF(6.6, 5.4));
    p.drawLine(QPointF(6.6, 8.2), QPointF(8.6, 9.4));
    // 右侧上下双向箭头
    p.drawLine(QPointF(13.4, 4.8), QPointF(13.4, 11.6));
    drawArrowHead(p, QPointF(13.4, 3.4), QPointF(0, -1), 2.2, c);  // 向上=更新
    drawArrowHead(p, QPointF(13.4, 13.0), QPointF(0, 1), 2.2, c);   // 向下=更旧
    p.end();
    return QIcon(pm);
}

// 批量填充列：多行横线 + 沿列向下的长箭头
inline QIcon fillColumn()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cFill();
    // 三条横线代表多行（长短不一，表示原有内容各不相同）
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(2, 4), QPointF(8, 4));
    p.drawLine(QPointF(2, 8), QPointF(6.5, 8));
    p.drawLine(QPointF(2, 12), QPointF(7.5, 12));
    // 沿列向下的长箭头（统一填充）
    drawArrow(p, QPointF(12, 2.5), QPointF(12, 13.5), 1.8, 3.2, c);
    p.end();
    return QIcon(pm);
}

// 与该客户端通信：两端节点 + 双向链路
inline QIcon communicate()
{
    auto pm = makePixmap();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto c = cTransfer();
    // 两端节点
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(3, 8), 2.2, 2.2);
    p.drawEllipse(QPointF(13, 8), 2.2, 2.2);
    // 中间双向链路：线 + 两端箭头
    p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(5.6, 8), QPointF(10.4, 8));
    drawArrowHead(p, QPointF(10.4, 8), QPointF(1, 0), 2.4, c);  // 指向右
    drawArrowHead(p, QPointF(5.6, 8), QPointF(-1, 0), 2.4, c); // 指向左
    p.end();
    return QIcon(pm);
}

} // namespace MenuIcons
