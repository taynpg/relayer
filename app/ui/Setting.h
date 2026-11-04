#pragma once

#include <QDialog>

namespace Ui {
class Setting;
}

// 设置对话框（对照 v1 Gui/Form/Setting 重写）。
//
// 与 v1 的对应关系：
//   - v1 的「压缩传输」→「传输 · 压缩发送的数据」。v1 的组里还有一个
//     "单包阈值(MB)"，那是打包流程的大小上限，v2 把它做成了协议常量
//     （PROTOCOL §11.10.3），不做成选项——选项只在真能被消费时才有意义。
//   - v1 的「加密传输」→「传输 · 加密载荷」。v1 是独立口令，v2 也一样：
//     连接口令是给中继的（中继知道），载荷口令只在对端之间共享。
//     两者绝不能共用一个值，界面文案里写死了这一条。
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
    void onEncryptToggled();
    void onSave();

private:
    void loadSettings();

    Ui::Setting* ui_;
};
