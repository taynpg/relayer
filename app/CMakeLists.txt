# relayer GUI 客户端（app/）。
#
# 这是全仓库唯一允许 #include <Q...> 的地方（架构规则 R5，tools/check_arch.py
# 在 CTest 里强制）。core / filestore / common 全部零 Qt，服务端因此不链接 Qt。
#
# 层次（REWRITE-PLAN §2.1 / §3.7）：
#   MainWindow   主窗口，组装各控件
#   ui/          .ui 文件 + 控件类（布局自 v1 原样搬迁，逻辑重写）
#   bridge/      Core → Qt 适配层（事件投递、字符串转换、生命周期）
#   model/       业务模型（TaskRowModel 等），控件类不直接持有业务逻辑
#
# .ui 的来源：v1 仓库 D:/Code/relayFile/Gui/，除 <class> 名统一改为 PascalCase
# 外，布局零改动（REWRITE-PLAN §3.7.1）。

# GUI 打开但机器上没有 Qt 时，给出能直接照做的错误，而不是一层嵌套的
# "Could not find a package configuration file"。
find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "RELAYER_BUILD_GUI=ON 但找不到 Qt6（Widgets）。"
        " 两条路任选：安装 Qt6（MSYS2: pacman -S mingw-w64-ucrt-x86_64-qt6-base），"
        " 或关闭 GUI：cmake -DRELAYER_BUILD_GUI=OFF")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
# .ui 集中在 ui/ 下，头文件里 #include "ui_XXX.h" 时到这里找
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/ui)

add_executable(relayer WIN32
    main.cpp
    MainWindow.cpp
    bridge/CoreService.cpp
    ui/ConnectorControl.cpp
    ui/LogControl.cpp
    relayer.qrc
)

target_include_directories(relayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(relayer PRIVATE
    relayer_client      # bridge 直接复用 CLI 的 Client 门面（PUBLIC 带 core/asio/platform）
    Qt6::Widgets
)

# 版本号单一来源是根 CMakeLists.txt 的 project(VERSION)，这里只注入字符串
target_compile_definitions(relayer PRIVATE RELAYER_VERSION="${PROJECT_VERSION}")

relayer_set_warnings(relayer)
