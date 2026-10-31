# ============================================================================
#  PicPic —— 顶层构建脚本
#
#  应用定位为快速选图工作台；修图界面已从构建中移除，
#  但保留 rawcore / rawio 以复用 RAW 解码与预览渲染能力。
#
#  许可：GPLv3（允许静态链接全部依赖，目标支持单文件打包）
#  技术栈：C++17 + Qt6 Widgets + libraw + lcms2 + lensfun + OpenMP
#
#  快速开始见 README.md。核心变量：
#    -DCMAKE_PREFIX_PATH=<Qt6 前缀>         例如 C:/Qt/6.7.2/msvc2019_64
#    -DCMAKE_TOOLCHAIN_FILE=<vcpkg.cmake>   使用 vcpkg 时必填
#    -DVCPKG_TARGET_TRIPLET=x64-windows-static   静态链接（单 exe）
# ============================================================================

cmake_minimum_required(VERSION 3.21)

# 让 find_package 识别 <PACKAGENAME>_ROOT 变量（如 LCMS2_ROOT / LIBRAW_ROOT），
# 这样用户可以用 -DLCMS2_ROOT=... 指定依赖位置而不产生策略警告。
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(PicPic
    VERSION 0.1.0
    DESCRIPTION "PicPic 快速选图工作台"
    LANGUAGES CXX
)

# MSVC 的资源编译器，用于把应用图标嵌入 exe（见 picpic 目标）。
# 只在 Windows 下启用，其余平台没有 RC 编译器。
if(WIN32)
    enable_language(RC)
endif()

# ---------------------------------------------------------------------------
#  全局设置
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置生成器（Ninja/Makefile）下的默认构建类型
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# 统一输出目录，便于运行时定位 DLL 与资源
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 让 MSVC 的多配置生成器也使用统一输出目录
foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_config} _config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# 导出 compile_commands.json，便于 clangd / IDE 索引
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(CompilerOptions)
include(FindRawDependencies)

# 需求 append_v3 §2.1：配置阶段拦截 /MD 与 /MT 混用
rawcore_check_crt_consistency()

# ---------------------------------------------------------------------------
#  选项
# ---------------------------------------------------------------------------

# 需求 append_v3 §2.2：镜头校正模块的编译开关。
# 关闭后源码里 RAWCore_HAS_LENSFUN=0，镜头面板整体置灰，其余功能不受影响。
set(_lensfun_option_preexisting FALSE)
if(DEFINED BUILD_WITH_LENSFUN)
    set(_lensfun_option_preexisting TRUE)
endif()
option(BUILD_WITH_LENSFUN "启用 lensfun 镜头校正" ON)

# 向后兼容旧变量名：-DRAWCore_HAS_LENSFUN=OFF 仍然生效。
# 仅在用户本次没有显式给出 BUILD_WITH_LENSFUN 时读取旧变量，避免旧缓存
# 在用户想重新开启镜头校正时把它强行关掉。
if(NOT _lensfun_option_preexisting
   AND DEFINED RAWCore_HAS_LENSFUN AND NOT RAWCore_HAS_LENSFUN)
    message(STATUS "[RAWCore] RAWCore_HAS_LENSFUN 已弃用，请改用 BUILD_WITH_LENSFUN=OFF")
    set(BUILD_WITH_LENSFUN OFF CACHE BOOL "启用 lensfun 镜头校正" FORCE)
endif()
set(RAWCore_HAS_LENSFUN ${BUILD_WITH_LENSFUN})

option(RAWCore_BUILD_TESTS "构建单元测试" OFF)
option(RAWCore_WARNINGS_AS_ERRORS "把警告视为错误" OFF)

if(RAWCore_WARNINGS_AS_ERRORS)
    add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/WX>)
endif()

# ---------------------------------------------------------------------------
#  Qt6 —— 仅 Widgets 与 Concurrent，严禁 QML（需求 §1.2）
# ---------------------------------------------------------------------------

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)

# 关闭 Qt 的自动 moc/uic 扫描以外的多余特性，避免引入 QML 相关目标
set(QT_NO_CREATE_VERSIONLESS_TARGETS OFF)

# ---------------------------------------------------------------------------
#  第三方依赖探测
# ---------------------------------------------------------------------------

rawcore_find_libraw()
rawcore_find_lcms2()
rawcore_find_lensfun()

# JPG 的 EXIF 由 exiv2 读取；缺失时选图列表仍显示文件名与文件尺寸。
set(RAWCore_HAS_EXIV2 TRUE)
rawcore_find_exiv2()

if(NOT RAWCore_HAS_LENSFUN)
    message(STATUS "[RAWCore] 镜头校正功能已关闭（未找到 lensfun）")
endif()

# ---------------------------------------------------------------------------
#  rawcore —— 纯净管线核心（不依赖 Qt，可复用/便于测试）
#
#  该静态库刻意不含任何 Qt 依赖，为未来移动端复用与单元测试保留可能。
# ---------------------------------------------------------------------------

add_library(rawcore STATIC
    src/core/Types.h
    src/core/Types.cpp
    src/core/PipelineStages.h
    src/core/PipelineStages.cpp
    src/core/LinearMath.h
    src/core/LinearMath.cpp
    src/core/Dither.h
    src/core/EditContext.h
    src/core/EditContext.cpp
    src/core/MaskModel.h
    src/core/MaskModel.cpp
    src/core/RawLoader.h
    src/core/RawLoader.cpp
    src/core/FrameCalibration.h
    src/core/FrameCalibration.cpp
    src/core/ColorCalibration.h
    src/core/ColorCalibration.cpp
    src/core/DcpProfile.h
    src/core/DcpProfile.cpp
    src/core/DcpApply.h
    src/core/DcpApply.cpp
    src/core/ColorManagement.h
    src/core/ColorManagement.cpp
    src/core/LensCorrection.h
    src/core/LensCorrection.cpp
    src/core/Lut3D.h
    src/core/Lut3D.cpp
    src/core/PipelineOps.h
    src/core/PipelineOps.cpp
    src/core/PipelineRenderer.h
    src/core/PipelineRenderer.cpp
)

target_include_directories(rawcore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

rawcore_set_compile_options(rawcore)
rawcore_enable_openmp(rawcore)

# 传递依赖来源的头文件路径定义，供源码条件编译
target_compile_definitions(rawcore PUBLIC
    RAWCore_LIBRAW_HEADER="${LIBRAW_HEADER}"
    RAWCore_HAS_LENSFUN=$<BOOL:${RAWCore_HAS_LENSFUN}>
)

if(RAWCore_HAS_LENSFUN)
    target_compile_definitions(rawcore PUBLIC
        RAWCore_LENSFUN_HEADER="${LENSFUN_HEADER}"
    )
endif()

# 私有依赖：libraw / lcms2 属实现细节，不外泄到上层
target_link_libraries(rawcore
    PRIVATE RAWCore::libraw
    PRIVATE RAWCore::lcms2
)

if(RAWCore_HAS_LENSFUN)
    target_link_libraries(rawcore PRIVATE RAWCore::lensfun)
endif()

# ---------------------------------------------------------------------------
#  rawio —— 选图工作台的文件元数据与侧车标记（依赖 core，不依赖 UI）
# ---------------------------------------------------------------------------

add_library(rawio STATIC
    # 选图工作台：文件元数据探测与侧车标记（不修改 RAW / EXIF）
    src/io/ImageMetadata.h
    src/io/ImageMetadata.cpp
    src/io/PickStore.h
    src/io/PickStore.cpp
    # 普通位图（JPG / PNG / TIFF / WebP / BMP）解码
    src/io/StandardImageLoader.h
    src/io/StandardImageLoader.cpp
)

target_include_directories(rawio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
rawcore_set_compile_options(rawio)

# 使用 Qt 的 QJsonDocument 做 JSON 读写，因此链接 Qt6::Core
target_link_libraries(rawio
    PUBLIC rawcore
    PUBLIC Qt6::Core
    PRIVATE Qt6::Gui
)

# 普通位图的 EXIF 由 exiv2 读取；缺失时仍可显示文件名与文件尺寸。
if(RAWCore_HAS_EXIV2)
    target_link_libraries(rawio PRIVATE RAWCore::exiv2)
    target_compile_definitions(rawio PRIVATE RAWCore_HAS_EXIV2=1)
endif()

# ---------------------------------------------------------------------------
#  rawui —— Qt Widgets 界面层
# ---------------------------------------------------------------------------

set(RAWUI_SOURCES
    # 快速选图工作台主界面（requirements/view.md）。
    # 应用只保留这一套界面；修图相关的窗口、面板与对话框不再参与构建。
    src/ui/CullWindow.h
    src/ui/CullWindow.cpp
    src/ui/CullPreviewWidget.h
    src/ui/CullPreviewWidget.cpp
    src/ui/CullWorkers.h
    src/ui/CullWorkers.cpp
    # 悬浮的 ICC / 显示效果模拟窗口（实时屏幕镜片、测试色卡）
    src/ui/IccSimulatorWindow.h
    src/ui/IccSimulatorWindow.cpp
    # 「帮助」窗口：使用流程、快捷键与关于
    src/ui/HelpDialog.h
    src/ui/HelpDialog.cpp
    # 工作台侧栏复用的通用控件
    src/ui/panels/WheelArbiter.h
    src/ui/panels/WheelArbiter.cpp
    src/ui/panels/CollapsibleSection.h
    src/ui/panels/CollapsibleSection.cpp
)

add_library(rawui STATIC ${RAWUI_SOURCES})

target_include_directories(rawui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
rawcore_set_compile_options(rawui)
# 界面层同样需要 OpenMP：全分辨率预览的 float→RGB888 显示转换在 UI 线程执行，
# 4600 万像素单线程串行会占用上百毫秒，导致拖动结束后仍有一次明显顿挫。
rawcore_enable_openmp(rawui)

# AUTOMOC / AUTOUIC：处理 Q_OBJECT 与 .ui 文件
set_target_properties(rawui PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
)

target_link_libraries(rawui
    PUBLIC rawcore
    PUBLIC rawio
    PUBLIC Qt6::Widgets
    PUBLIC Qt6::Concurrent
)

# ---------------------------------------------------------------------------
#  picpic —— 选图工作台可执行程序
# ---------------------------------------------------------------------------

add_executable(picpic
    src/main.cpp
    src/app/AppLog.h
    src/app/AppLog.cpp
    src/app/PortableMode.h
    resources/resources.qrc
)

target_include_directories(picpic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
rawcore_set_compile_options(picpic)

set_target_properties(picpic PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    WIN32_EXECUTABLE ON   # Windows 下不弹出控制台窗口
)

target_link_libraries(picpic
    PRIVATE rawcore
    PRIVATE rawio
    PRIVATE rawui
    PRIVATE Qt6::Widgets
    PRIVATE Qt6::Concurrent
)

# ---------------------------------------------------------------------------
#  Windows 应用图标
#
#  把多尺寸 ICO 嵌入 exe，资源管理器、任务栏与 Alt+Tab 因此显示应用图标。
#  图标绝对路径在配置阶段写进生成的 .rc，不依赖构建目录的相对路径。
#  运行时窗口图标由 main.cpp 的 setWindowIcon 加载 qrc 中的 icons/app.png。
# ---------------------------------------------------------------------------
if(WIN32)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.ico" APP_ICON_PATH)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/app.rc
        @ONLY)
    target_sources(picpic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

# ---------------------------------------------------------------------------
#  lensfun 镜头数据库部署
#
#  vcpkg 的 lensfun 只带库本体，且 Windows 构建的默认数据路径拼装有问题，
#  因此数据库随程序分发：复制到可执行文件旁的 lensfun/version_1，
#  LensCorrector 会优先从该相对路径加载。
#  数据库来源与许可见 third_party/lensfun-db/README.md（CC-BY-SA 3.0）。
# ---------------------------------------------------------------------------

add_custom_command(TARGET picpic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun-db
            $<TARGET_FILE_DIR:picpic>/lensfun
    COMMENT "部署 lensfun 镜头数据库"
    VERBATIM
)

# ---------------------------------------------------------------------------
#  Windows：链接静态运行库时的额外系统库
# ---------------------------------------------------------------------------

if(WIN32)
    # libraw 在 Windows 上依赖 Winsock；
    # ColorManagement 读取显示器 ICC 用到 GDI（EnumDisplayDevices / GetICMProfile）。
    # 这里显式链接，避免依赖 Qt 目标间接带入——使 rawcore 可以独立使用。
    target_link_libraries(rawcore PRIVATE ws2_32 user32 gdi32)
endif()

# ---------------------------------------------------------------------------
#  单元测试（-DRAWCore_BUILD_TESTS=ON）
#
#  两个测试程序均为独立可执行文件，用 CTest 统一调度：
#    pipeline_smoke  —— 合成拜耳数据跑通整条渲染管线
#    core_units      —— 色温往返、曲线 LUT、CCM 校验与求解、CSV 往返
#
#  运行方式：
#    cmake -S . -B build -DRAWCore_BUILD_TESTS=ON
#    cmake --build build
#    ctest --test-dir build --output-on-failure
# ---------------------------------------------------------------------------

if(RAWCore_BUILD_TESTS)
    enable_testing()

    add_executable(test_pipeline build_check/test_pipeline.cpp)
    target_link_libraries(test_pipeline PRIVATE rawcore)
    # 测试源码同样含中文注释，需要与主目标一致的编译选项（尤其是 MSVC 的 /utf-8）
    rawcore_set_compile_options(test_pipeline)
    add_test(NAME pipeline_smoke COMMAND test_pipeline)

    add_executable(test_core_units build_check/test_core_units.cpp)
    target_link_libraries(test_core_units PRIVATE rawcore)
    rawcore_set_compile_options(test_core_units)
    # 镜头数据库测试使用源码树内的数据，避免依赖系统安装
    target_compile_definitions(test_core_units PRIVATE
        RAWEDIT_TEST_LENSFUN_DB="${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun-db/version_1")
    add_test(NAME core_units COMMAND test_core_units)
endif()

# ---------------------------------------------------------------------------
#  打包辅助目标：复制运行期依赖（动态链接时使用）
# ---------------------------------------------------------------------------

include(Packaging)

# ---------------------------------------------------------------------------
#  构建摘要
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "======================= PicPic =======================")
message(STATUS "  版本            : ${PROJECT_VERSION}")
message(STATUS "  构建类型        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准        : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt 版本         : ${Qt6_VERSION}")
message(STATUS "  libraw 头文件   : ${LIBRAW_HEADER}")
message(STATUS "  lensfun         : ${RAWCore_HAS_LENSFUN}")
message(STATUS "  OpenMP          : ${OpenMP_CXX_FOUND}")
message(STATUS "  单元测试        : ${RAWCore_BUILD_TESTS}")
message(STATUS "  输出目录        : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "==============================================================")
message(STATUS "")
