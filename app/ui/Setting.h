#pragma once

#include <QDialog>

namespace Ui {
class Setting;
}

// 设置对话框（对照 v1 Gui/Form/Setting 重写）。
//
// 与 v1 的差异是刻意的，不是没做完：
//   - v1 的「压缩传输」（tar.xz 打包 + 单包阈值）不迁移：v2 的任务表单
//     明确不做归档（RelayTaskControl.cpp 顶部注释写了理由），做了开关
//     也不生效——不放假开关。
//   - v1 的「加密传输」（独立口令）不迁移：v2 的口令是连接参数（连接
//     面板），协议没有独立的载荷加密开关。
//   - v1 的「重连」对应 v2 的自动接收失败重试（MainWindow 的延迟重试，
//     原为硬编码 3 次 × 3 秒），语义保留、字段照搬。
//   - 「共享目录」已删除：v2 取消共享根，路径一律绝对（DESIGN.md §3.8）。
//   - 新增「发送前检查目标是否已存在」：v1 的"基本检查 + 覆盖提示"。
//     接收端不再有覆盖开关——决策在发送方做完了，接收端收到什么就是
//     对方明确要发的（见 CoreService::makeConfig 的注释）。
//
// 所有设置写入 QSettings，读取方（MainWindow / CoreService::makeConfig）
// 都在各自动作发生时现读，保存后立即生效，无需重启或通知。
class Setting : public QDialog
{
    Q_OBJECT

public:
    explicit Setting(QWidget* parent = nullptr);
    ~Setting() override;

    Setting(const Setting&) = delete;
    Setting& operator=(const Setting&) = delete;

private slots:
    void onRetryModeChanged();
    void onSave();

private:
    void loadSettings();

    Ui::Setting* ui_;
};
