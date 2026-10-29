# relayer GUI 客户端（app/）。
#
# 这是全仓库唯一允许 #include <Q...> 的地方（架构规则 R5，tools/check_arch.py
# 在 CTest 里强制）。core / filestore / common 全部零 Qt，服务端因此不链接 Qt。
#
# 层次（见 docs/DESIGN.md §2.1 / §3.7）：
#   MainWindow   主窗口，组装各控件
#   ui/          .ui 文件 + 控件类（布局沿用既有设计，逻辑重写）
#   bridge/      Core → Qt 适配层（事件投递、字符串转换、生命周期）
#   model/       业务模型（TaskRowModel 等），控件类不直接持有业务逻辑
#
# .ui 的约定：除 <class> 名统一用 PascalCase 外，布局零改动（docs/DESIGN.md §3.7）。

# GUI 打开但机器上没有 Qt 时，给出能直接照做的错误，而不是一层嵌套的
# "Could not find a package configuration file"。
#
# Network 是给单实例用的（QLocalServer/QLocalSocket 属 Qt Network，
# 不是 Core 里的东西）。它随 qt6-base 一起装，不需要额外的包。
find_package(Qt6 COMPONENTS Widgets Network QUIET)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "RELAYER_BUILD_GUI=ON 但找不到 Qt6（Widgets/Network）。"
        " 两条路任选：安装 Qt6（MSYS2: pacman -S mingw-w64-ucrt-x86_64-qt6-base），"
        " 或关闭 GUI：cmake -DRELAYER_BUILD_GUI=OFF")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
# .ui 集中在 ui/ 下，头文件里 #include "ui_XXX.h" 时到这里找
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui)

# CoreService 单独成库：它是唯一与 Qt 有关、却又**不依赖任何控件**的
# 那一层（只有 QObject / QString）。抽出来的理由是能被单独测试——
# GUI 之前一条自动化用例都没有，"改了不知道有没有坏"。
#
# 只链接 Qt6::Core，不链接 Widgets：想让它依赖 Widgets 时会立刻发现。
add_library(relayer_app_core STATIC
    bridge/CoreService.cpp
)
target_include_directories(relayer_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(relayer_app_core PUBLIC relayer_client Qt6::Core)
relayer_set_warnings(relayer_app_core)

add_executable(relayer WIN32
    main.cpp
    MainWindow.cpp
    SingleInstance.cpp
    Base/InfoDrop.cpp
    model/ProgressTracker.cpp
    ui/ConnectorControl.cpp
    ui/FileMetaInfo.cpp
    ui/LogControl.cpp
    ui/ReplaceArea.cpp
    ui/Setting.cpp
    ui/Control/CheckableComboBox.cpp
    ui/Control/ComparisonControl.cpp
    ui/Control/ExplorerControl.cpp
    ui/Control/OwnTableWidget.cpp
    ui/Control/RelayTaskControl.cpp
    relayer.qrc
)

target_include_directories(relayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    # .ui 里的提升控件写的是 "Control/CheckableComboBox.h"（既有的目录
    # 结构），生成的 ui_XXX.h 会原样引用；把 ui/ 加进搜索路径让它在
    # 当前的目录布局下也能解析，.ui 文件因而不必改动。
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(relayer PRIVATE
    relayer_app_core    # bridge 层；PUBLIC 带着 relayer_client
    Qt6::Widgets
    Qt6::Network        # QLocalServer / QLocalSocket（单实例）
)

# 版本号单一来源是根 CMakeLists.txt 的 project(VERSION)，这里只注入字符串
target_compile_definitions(relayer PRIVATE RELAYER_VERSION="${PROJECT_VERSION}")

relayer_set_warnings(relayer)

# 冒烟测试：一个无窗口的可执行文件，用 CoreService 真连一次中继、真列一次
# 目录。覆盖的是"连接 → 数据连接就绪 → 控制连接并发请求 → 应答配对"这条
# 链路——GUI 里最容易坏、又最难靠手点发现的部分。
if(RELAYER_BUILD_TESTS)
    add_executable(gui_smoke tools/gui_smoke.cpp)
    target_link_libraries(gui_smoke PRIVATE relayer_app_core)
    relayer_set_warnings(gui_smoke)
endif()
