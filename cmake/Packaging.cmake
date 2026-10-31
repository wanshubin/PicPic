# ============================================================================
#  Packaging.cmake —— 运行期依赖部署与打包辅助
#
#  两种打包路径：
#   1. 动态链接（triplet = x64-windows 或 Qt 官方安装器）
#      -> POST_BUILD 调用 windeployqt 把 Qt DLL 收集到输出目录
#   2. 静态链接（triplet = x64-windows-static，Qt 亦为静态构建）
#      -> 无需部署 DLL，exe 本身即为单文件；
#         目标 `package-static` 仅做体积报告
#
#  静态与否通过 Qt6::Core 这个导入目标的类型自动判断，不依赖额外的用户开关。
# ============================================================================

# ----------------------------------------------------------------------------
#  判断 Qt 是否为静态构建
# ----------------------------------------------------------------------------
set(RAWCore_STATIC_LINK OFF)

get_target_property(_qt_core_type Qt6::Core TYPE)
if(_qt_core_type STREQUAL "STATIC_LIBRARY")
    set(RAWCore_STATIC_LINK ON)
endif()
unset(_qt_core_type)

message(STATUS "[RAWCore] Qt 链接方式：${RAWCore_STATIC_LINK}（ON=静态单文件）")

# ----------------------------------------------------------------------------
#  deploy —— 动态链接时收集运行期依赖
# ----------------------------------------------------------------------------
add_custom_target(deploy
    COMMENT "收集运行期依赖到输出目录（动态链接模式）"
    COMMAND ${CMAKE_COMMAND} -E echo "输出目录: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    DEPENDS picpic
    VERBATIM
)

if(WIN32 AND NOT RAWCore_STATIC_LINK)
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS ${Qt6_DIR}/../../../bin
              ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES bin
    )

    if(WINDEPLOYQT_EXECUTABLE)
        # 注意：--debug / --release 必须合并为**一个**生成器表达式并加引号。
        # 若拆成两个独立的 genex，未命中的那个会展开成空字符串参数，
        # 而 windeployqt 不接受空参数，多配置生成器下会直接失败。
        add_custom_command(TARGET picpic POST_BUILD
            COMMENT "使用 windeployqt 部署 Qt 运行期依赖"
            COMMAND ${WINDEPLOYQT_EXECUTABLE}
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "$<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>"
                    $<TARGET_FILE:picpic>
            VERBATIM
        )
    else()
        message(STATUS
            "[RAWCore] 未找到 windeployqt；如需分发动态版本请手工部署 Qt DLL。")
    endif()
endif()

# ----------------------------------------------------------------------------
#  package-static —— 静态链接模式的可分发目录
#
#  exe 本身已包含 Qt 与第三方库；这里把许可文本与 lensfun 数据库一并放入
#  package/，满足"发布产物必须附带 GPLv3 许可证与 NOTICE"的要求。
# ----------------------------------------------------------------------------
set(RAWCore_PACKAGE_DIR ${CMAKE_BINARY_DIR}/package)

add_custom_target(package-static
    COMMENT "整理静态链接发布产物到 ${RAWCore_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RAWCore_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:picpic> ${RAWCore_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${RAWCore_PACKAGE_DIR}/LICENSE
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NOTICE.txt
            ${RAWCore_PACKAGE_DIR}/NOTICE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun-db
            ${RAWCore_PACKAGE_DIR}/lensfun
    COMMAND ${CMAKE_COMMAND} -E echo "发布目录: ${RAWCore_PACKAGE_DIR}"
    DEPENDS picpic
    VERBATIM
)

# ----------------------------------------------------------------------------
#  package-portable —— 免安装压缩包（动态链接版）
#
#  只收集白名单内的运行文件与插件，补上 VC++ 运行库，放进 portable.ini
#  开启便携模式，最后生成 build/dist/PicPic-<版本>-win64-portable.zip。
#  具体收集逻辑见 cmake/PackagePortable.cmake。
# ----------------------------------------------------------------------------
add_custom_target(package-portable
    COMMENT "生成免安装压缩包（含 Qt / 第三方 DLL 与 VC++ 运行库）"
    COMMAND ${CMAKE_COMMAND}
            -DBIN_DIR=$<TARGET_FILE_DIR:picpic>
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DDIST_DIR=${CMAKE_BINARY_DIR}/dist
            -DPACKAGE_VERSION=${PROJECT_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackagePortable.cmake
    DEPENDS picpic
    VERBATIM
)

# ----------------------------------------------------------------------------
#  把第三方许可文本复制到输出目录（GPLv3 合规要求，需求 §8）
# ----------------------------------------------------------------------------
add_custom_command(TARGET picpic POST_BUILD
    COMMENT "复制许可与版权声明文件"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LICENSE
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NOTICE.txt
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/NOTICE.txt
    VERBATIM
)
