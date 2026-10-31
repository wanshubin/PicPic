# ============================================================================
#  PackagePortable.cmake —— 生成免安装（便携）压缩包
#
#  由 package-portable 目标以 `cmake -P` 方式调用。打包内容：
#   - picpic.exe 与本项目直接使用的 Qt / 第三方 DLL；
#   - 运行必需的 Qt 插件（platforms / styles / iconengines / imageformats）；
#   - lensfun 镜头数据库（含 CC-BY-SA 3.0 许可）；
#   - VC++ 2015–2022 x64 运行库（app-local，用户无需另外安装）；
#   - LICENSE、NOTICE.txt、portable.ini（便携模式标记）与简版说明。
#
#  windeployqt 会顺带部署 Qt6Network、dxcompiler 等本应用用不到的组件，
#  这里按白名单收集，不整目录拷贝；新增依赖时记得同步下面两个清单。
#
#  参数（均由 CMake 目标传入）：
#    BIN_DIR         构建输出目录（windeployqt 已部署依赖）
#    SRC_DIR         源码根目录
#    DIST_DIR        压缩包输出目录
#    PACKAGE_VERSION 版本号
#  可选：
#    VC_REDIST_DIR   VS 运行库 redist 根目录；缺省依次读取环境变量
#                    VCToolsRedistDir 与 VS 安装目录下的最新版本
# ============================================================================

cmake_minimum_required(VERSION 3.21)

foreach(var BIN_DIR SRC_DIR DIST_DIR PACKAGE_VERSION)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "PackagePortable: 缺少参数 ${var}")
    endif()
endforeach()

set(PACKAGE_NAME "PicPic-${PACKAGE_VERSION}-win64-portable")
set(STAGE_DIR "${DIST_DIR}/${PACKAGE_NAME}")

# 清空上次的暂存目录，避免旧文件被打进新包
if(EXISTS "${STAGE_DIR}")
    file(REMOVE_RECURSE "${STAGE_DIR}")
endif()
file(MAKE_DIRECTORY "${STAGE_DIR}")

# ----------------------------------------------------------------------------
#  主程序与运行期 DLL
# ----------------------------------------------------------------------------
set(RUNTIME_FILES
    picpic.exe
    Qt6Core.dll
    Qt6Gui.dll
    Qt6Widgets.dll
    Qt6Svg.dll
    raw.dll
    lcms2-2.dll
    lensfun.dll
    exiv2.dll
    z.dll
    miniz.dll
    glib-2.0-0.dll
    iconv-2.dll
    intl-8.dll
    pcre2-8.dll
)

foreach(file IN LISTS RUNTIME_FILES)
    if(NOT EXISTS "${BIN_DIR}/${file}")
        message(FATAL_ERROR
            "PackagePortable: 缺少 ${BIN_DIR}/${file}，请先完整构建 picpic")
    endif()
    file(COPY "${BIN_DIR}/${file}" DESTINATION "${STAGE_DIR}")
endforeach()

# ----------------------------------------------------------------------------
#  Qt 插件：只收集本应用实际需要的
#
#  注意：generic/qtuiotouchplugin.dll 依赖 Qt6Network，本应用不用 TUIO，
#  因此不收集它，连带省掉 Qt6Network.dll、tls/ 与 networkinformation/。
# ----------------------------------------------------------------------------
set(PLUGIN_FILES
    platforms/qwindows.dll
    styles/qmodernwindowsstyle.dll
    iconengines/qsvgicon.dll
    imageformats/qgif.dll
    imageformats/qicns.dll
    imageformats/qico.dll
    imageformats/qjpeg.dll
    imageformats/qsvg.dll
    imageformats/qtga.dll
    imageformats/qtiff.dll
    imageformats/qwbmp.dll
    imageformats/qwebp.dll
)

foreach(file IN LISTS PLUGIN_FILES)
    if(NOT EXISTS "${BIN_DIR}/${file}")
        message(FATAL_ERROR "PackagePortable: 缺少 Qt 插件 ${BIN_DIR}/${file}")
    endif()
    get_filename_component(subdir "${file}" DIRECTORY)
    file(COPY "${BIN_DIR}/${file}" DESTINATION "${STAGE_DIR}/${subdir}")
endforeach()

# ----------------------------------------------------------------------------
#  lensfun 镜头数据库与许可
# ----------------------------------------------------------------------------
if(NOT EXISTS "${BIN_DIR}/lensfun/version_1")
    message(FATAL_ERROR "PackagePortable: 缺少 ${BIN_DIR}/lensfun/version_1")
endif()
file(COPY "${BIN_DIR}/lensfun" DESTINATION "${STAGE_DIR}")
file(COPY "${SRC_DIR}/third_party/lensfun-db/README.md"
          "${SRC_DIR}/third_party/lensfun-db/cc-by-sa-3.0.txt"
     DESTINATION "${STAGE_DIR}/lensfun")

# ----------------------------------------------------------------------------
#  许可文本
# ----------------------------------------------------------------------------
# LICENSE 在仓库根目录，NOTICE.txt 在 third_party/ 下
foreach(file "${SRC_DIR}/LICENSE" "${SRC_DIR}/third_party/NOTICE.txt")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "PackagePortable: 缺少许可文件 ${file}")
    endif()
    file(COPY "${file}" DESTINATION "${STAGE_DIR}")
endforeach()

# ----------------------------------------------------------------------------
#  便携模式标记与简版说明
# ----------------------------------------------------------------------------
file(WRITE "${STAGE_DIR}/portable.ini"
"; PicPic 便携模式标记\n"
"; 存在本文件时，配置写入程序目录下的 PicPic/PicPic.ini，\n"
"; 日志写入 logs/picpic.log，不写注册表。\n"
"; 删除本文件后，配置与日志会回到系统用户目录（注册表 / %APPDATA%）。\n")

file(WRITE "${STAGE_DIR}/便携版说明.txt"
"PicPic 快速选图工作台 —— 免安装便携版\n"
"\n"
"使用方法：\n"
"  1. 把整个文件夹解压到任意可写位置（桌面、U 盘、移动硬盘均可）；\n"
"  2. 双击 picpic.exe 即可运行，无需安装，也无需另外安装运行库。\n"
"\n"
"文件说明：\n"
"  picpic.exe      主程序\n"
"  *.dll           Qt、libraw、exiv2、lensfun、lcms2 与 VC++ 运行库\n"
"  platforms 等    Qt 插件目录，缺少会无法启动\n"
"  lensfun/        镜头数据库；缺失时镜头校正自动跳过，其余功能正常\n"
"  portable.ini    便携模式标记，删除后设置与日志改回系统用户目录\n"
"                  （配置保存在 PicPic/PicPic.ini，日志在 logs/ 下）\n"
"  LICENSE         GPLv3 许可证\n"
"  NOTICE.txt      第三方库版权与许可说明\n"
"\n"
"卸载：删除整个文件夹即可，不会在系统中留下文件。\n")

# ----------------------------------------------------------------------------
#  VC++ 2015–2022 x64 运行库（app-local 部署）
#
#  当前 exe 与 Qt 使用 /MD 动态链接，缺少这些 DLL 时在纯净系统上会报
#  「找不到 VCRUNTIME140.dll」。把它们放在 exe 旁边即可免安装运行。
# ----------------------------------------------------------------------------
set(redist_root "${VC_REDIST_DIR}")
if(NOT redist_root AND DEFINED ENV{VCToolsRedistDir})
    set(redist_root "$ENV{VCToolsRedistDir}")
endif()

if(NOT redist_root)
    # 回退：在 VS 安装目录下找版本号最大的 redist（14.4x 等）。
    # 只保留真正带 x64 CRT 的目录：同级还有个 v143 目录只放安装器，
    # 直接按名字排序会误选到它。
    file(GLOB _redist_dirs
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*"
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*")
    set(_redist_candidates "")
    foreach(_dir IN LISTS _redist_dirs)
        file(GLOB _crt_dirs "${_dir}/x64/Microsoft.VC*.CRT")
        if(_crt_dirs)
            list(APPEND _redist_candidates "${_dir}")
        endif()
    endforeach()
    if(_redist_candidates)
        list(SORT _redist_candidates)
        list(GET _redist_candidates -1 redist_root)
    endif()
endif()

if(NOT redist_root)
    message(FATAL_ERROR
        "PackagePortable: 找不到 VC++ 运行库 redist 目录。\n"
        "请在 vcvars64 环境下运行打包，或用 -DVC_REDIST_DIR=... 指定。")
endif()

file(TO_CMAKE_PATH "${redist_root}" redist_root)
file(GLOB _vc_runtime_dlls
    "${redist_root}/x64/Microsoft.VC*.CRT/msvcp140*.dll"
    "${redist_root}/x64/Microsoft.VC*.CRT/vcruntime140*.dll"
    "${redist_root}/x64/Microsoft.VC*.CRT/concrt140.dll"
    "${redist_root}/x64/Microsoft.VC*.OpenMP/vcomp140.dll")

foreach(dll IN LISTS _vc_runtime_dlls)
    file(COPY "${dll}" DESTINATION "${STAGE_DIR}")
endforeach()

# 打包后校验运行库是否齐全，避免出现「能打包但用户打不开」的包
foreach(required
        msvcp140.dll msvcp140_1.dll msvcp140_2.dll
        vcruntime140.dll vcruntime140_1.dll vcomp140.dll)
    if(NOT EXISTS "${STAGE_DIR}/${required}")
        message(FATAL_ERROR
            "PackagePortable: 运行库 ${required} 未找到（redist: ${redist_root}）")
    endif()
endforeach()

# ----------------------------------------------------------------------------
#  生成 zip（顶层目录为 PicPic-<version>-win64-portable）
# ----------------------------------------------------------------------------
file(MAKE_DIRECTORY "${DIST_DIR}")
set(ZIP_PATH "${DIST_DIR}/${PACKAGE_NAME}.zip")
if(EXISTS "${ZIP_PATH}")
    file(REMOVE "${ZIP_PATH}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${ZIP_PATH}" --format=zip "${PACKAGE_NAME}"
    WORKING_DIRECTORY "${DIST_DIR}"
    RESULT_VARIABLE _tar_result
    OUTPUT_VARIABLE _tar_output
    ERROR_VARIABLE _tar_output)

if(NOT _tar_result EQUAL 0)
    message(FATAL_ERROR "PackagePortable: 生成 zip 失败：${_tar_output}")
endif()

file(SIZE "${ZIP_PATH}" _zip_size)
math(EXPR _zip_mb "${_zip_size} / 1048576")

message(STATUS "便携版已生成：${ZIP_PATH}")
message(STATUS "压缩包大小：${_zip_mb} MB")
