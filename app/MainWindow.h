#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

// 主窗口：v1 的 relayFile（Gui/relayFile.{h,cpp}）在 v2 的对应物。
//
// 阶段 E1 只交付"能启动的壳"：组装各控件与 bridge 的接线在 E2/E3 逐块补齐，
// 补齐前不写占位逻辑，避免出现"看起来能点实际什么都没干"的假功能。
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;

private:
    Ui::MainWindow *ui_;
};
