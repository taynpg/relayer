#pragma once

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui
{
class FileMetaInfo;
}
QT_END_NAMESPACE

// 详细信息对话框（v1 `Gui/Form/FileMetaInfo` 的 v2 版本）。
//
// 数据来源跟 v1 不同：v1 拿协议对象 FileMeta；v2 的浏览面板本来就有完整的
// FileEntry（名字 / 类型 / 大小 / 修改时间），不必再问一遍文件系统——远端
// 条目也没有别的字段可问（§11.8 只回名字、类型、大小、时间）。
//
// 本地与远端共用这一份 UI（面板本身就是一个类、两个数据源）。区别只在
// "目录"一栏：远端填 `peer:/相对路径`，并在类型后标"（远端）"——否则那段
// 字符串会被当成可以粘到资源管理器的本地路径。
class FileMetaInfo : public QDialog
{
    Q_OBJECT

public:
    explicit FileMetaInfo(QWidget *parent = nullptr);
    ~FileMetaInfo() override;

    FileMetaInfo(const FileMetaInfo &) = delete;
    FileMetaInfo &operator=(const FileMetaInfo &) = delete;

    // dirText：本地绝对路径，或远端的 `peer:/rel`（空 = 未知）。
    // size / modifiedMs 传 -1 表示"不适用"（目录的大小、远端没给时间）。
    void setMeta(const QString &dirText, const QString &name, bool isDir, qint64 size, qint64 modifiedMs,
                 bool remote);

private:
    void applyMeta();
    void copyToClipboard(const QString &text, const QString &what);

    Ui::FileMetaInfo *ui_ = nullptr;

    QString dirText_;
    QString name_;
    bool isDir_ = false;
    qint64 size_ = -1;
    qint64 modifiedMs_ = -1;
    bool remote_ = false;
};
