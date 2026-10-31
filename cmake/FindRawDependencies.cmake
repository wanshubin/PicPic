# ============================================================================
#  find_libraw / find_lcms2 / find_lensfun 兼容层
#
#  背景：三个依赖库在不同安装渠道下暴露的 CMake 包名与头文件路径不一致，
#  例如 vcpkg 提供 LibRaw::raw，而 apt/brew 提供 libraw::libraw；
#  头文件也可能是 <libraw/libraw.h> 或 <libraw.h>。
#
#  本模块统一收敛为三个接口目标：
#     RAWCore::libraw     RAWCore::lcms2     RAWCore::lensfun
#  上层只需要链接这三个名字，不必关心依赖来源。
# ============================================================================

include(CMakePrintHelpers)

# ----------------------------------------------------------------------------
# 内部工具：探测某个头文件是否能被编译器找到
# 用法：_rawcore_check_header(<变量名> <头文件路径>)
# 结果写入 <变量名>，值为 TRUE / FALSE
# ----------------------------------------------------------------------------
function(_rawcore_check_header out_var header_name)
    # 复用 CMake 自带的 CheckIncludeFile，避免手写编译器探测逻辑
    include(CheckIncludeFileCXX)
    # 头文件路径含斜杠时（如 libraw/libraw.h），CHECK_INCLUDE_FILE_CXX 会
    # 把名称转成大写下划线宏名，这里手工构造一个合法的变量名
    string(MAKE_C_IDENTIFIER "HAVE_${header_name}" _check_var)
    check_include_file_cxx("${header_name}" ${_check_var})
    if(${_check_var})
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ============================================================================
#  libraw —— RAW 解码
# ============================================================================
function(rawcore_find_libraw)
    if(TARGET RAWCore::libraw)
        return()  # 已经找到过，避免重复执行
    endif()

    # 优先走标准 find_package，vcpkg / 部分系统包会提供这些配置
    find_package(LibRaw QUIET CONFIG)
    if(NOT LibRaw_FOUND)
        find_package(libraw QUIET CONFIG)
    endif()

    # 收集候选目标名（不同发行版命名习惯不同）
    foreach(_candidate LibRaw::raw libraw::libraw LibRaw::LibRaw)
        if(TARGET ${_candidate})
            add_library(RAWCore::libraw ALIAS ${_candidate})
            message(STATUS "[RAWCore] libraw 已通过 CMake 包定位 -> ${_candidate}")
            return()
        endif()
    endforeach()

    # 退化路径：手工查找头文件与库文件（覆盖源码编译/非标准安装）
    find_path(LIBRAW_INCLUDE_DIR
        NAMES libraw/libraw.h libraw.h
        PATH_SUFFIXES include
        HINTS ${LIBRAW_ROOT} $ENV{LIBRAW_ROOT}
    )
    find_library(LIBRAW_LIBRARY
        NAMES raw libraw libraw_r raw_r
        PATH_SUFFIXES lib lib64
        HINTS ${LIBRAW_ROOT} $ENV{LIBRAW_ROOT}
    )

    if(LIBRAW_INCLUDE_DIR AND LIBRAW_LIBRARY)
        add_library(RAWCore::libraw UNKNOWN IMPORTED)
        set_target_properties(RAWCore::libraw PROPERTIES
            IMPORTED_LOCATION "${LIBRAW_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBRAW_INCLUDE_DIR}"
        )
        # 判定头文件实际路径，供源码里 #include 使用
        if(EXISTS "${LIBRAW_INCLUDE_DIR}/libraw/libraw.h")
            set(LIBRAW_HEADER "libraw/libraw.h" CACHE INTERNAL "libraw 头文件包含路径")
        else()
            set(LIBRAW_HEADER "libraw.h" CACHE INTERNAL "libraw 头文件包含路径")
        endif()
        message(STATUS "[RAWCore] libraw 手工定位 -> ${LIBRAW_LIBRARY}")
        return()
    endif()

    message(FATAL_ERROR
        "未找到 libraw。请通过 vcpkg 安装（vcpkg install libraw），"
        "或指定 -DLIBRAW_ROOT=<libraw 安装前缀>")
endfunction()

# ============================================================================
#  lcms2 —— 色彩管理
# ============================================================================
function(rawcore_find_lcms2)
    if(TARGET RAWCore::lcms2)
        return()
    endif()

    find_package(lcms2 QUIET CONFIG)
    if(NOT lcms2_FOUND)
        find_package(LCMS2 QUIET CONFIG)
    endif()

    foreach(_candidate lcms2::lcms2 LCMS2::LCMS2 lcms2)
        if(TARGET ${_candidate})
            add_library(RAWCore::lcms2 ALIAS ${_candidate})
            message(STATUS "[RAWCore] lcms2 已通过 CMake 包定位 -> ${_candidate}")
            return()
        endif()
    endforeach()

    find_path(LCMS2_INCLUDE_DIR
        NAMES lcms2.h
        PATH_SUFFIXES include lcms2
        HINTS ${LCMS2_ROOT} $ENV{LCMS2_ROOT}
    )
    find_library(LCMS2_LIBRARY
        NAMES lcms2 lcms2_static
        PATH_SUFFIXES lib lib64
        HINTS ${LCMS2_ROOT} $ENV{LCMS2_ROOT}
    )

    if(LCMS2_INCLUDE_DIR AND LCMS2_LIBRARY)
        add_library(RAWCore::lcms2 UNKNOWN IMPORTED)
        set_target_properties(RAWCore::lcms2 PROPERTIES
            IMPORTED_LOCATION "${LCMS2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}"
        )
        message(STATUS "[RAWCore] lcms2 手工定位 -> ${LCMS2_LIBRARY}")
        return()
    endif()

    message(FATAL_ERROR
        "未找到 lcms2。请通过 vcpkg 安装（vcpkg install lcms），"
        "或指定 -DLCMS2_ROOT=<lcms2 安装前缀>")
endfunction()

# ============================================================================
#  lensfun —— 镜头校正（畸变 / 暗角 / 轴向色差）
#  注意：lensfun 是"可选但推荐"依赖。缺失时不终止配置，
#  而是关闭 RAWCore_HAS_LENSFUN，管线里镜头校正环节自动旁路。
# ============================================================================
function(rawcore_find_lensfun)
    if(TARGET RAWCore::lensfun OR NOT RAWCore_HAS_LENSFUN)
        return()
    endif()

    find_package(lensfun QUIET CONFIG)

    foreach(_candidate lensfun::lensfun Lensfun::lensfun lensfun)
        if(TARGET ${_candidate})
            add_library(RAWCore::lensfun ALIAS ${_candidate})
            message(STATUS "[RAWCore] lensfun 已通过 CMake 包定位 -> ${_candidate}")
            return()
        endif()
    endforeach()

    find_path(LENSFUN_INCLUDE_DIR
        NAMES lensfun.h lensfun/lensfun.h
        PATH_SUFFIXES include include/lensfun
        HINTS ${LENSFUN_ROOT} $ENV{LENSFUN_ROOT}
    )
    find_library(LENSFUN_LIBRARY
        NAMES lensfun
        PATH_SUFFIXES lib lib64
        HINTS ${LENSFUN_ROOT} $ENV{LENSFUN_ROOT}
    )

    if(LENSFUN_INCLUDE_DIR AND LENSFUN_LIBRARY)
        add_library(RAWCore::lensfun UNKNOWN IMPORTED)
        set_target_properties(RAWCore::lensfun PROPERTIES
            IMPORTED_LOCATION "${LENSFUN_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LENSFUN_INCLUDE_DIR}"
        )
        # 不同版本头文件位置不同，供源码条件包含使用
        if(EXISTS "${LENSFUN_INCLUDE_DIR}/lensfun/lensfun.h")
            set(LENSFUN_HEADER "lensfun/lensfun.h" CACHE INTERNAL "lensfun 头文件包含路径")
        else()
            set(LENSFUN_HEADER "lensfun.h" CACHE INTERNAL "lensfun 头文件包含路径")
        endif()
        message(STATUS "[RAWCore] lensfun 手工定位 -> ${LENSFUN_LIBRARY}")
        return()
    endif()

    # 找不到时降级处理：不阻止构建，只关掉该功能
    message(WARNING
        "[RAWCore] 未找到 lensfun，镜头校正功能将在运行时自动旁路。"
        "如需该功能：vcpkg install lensfun 或指定 -DLENSFUN_ROOT=<前缀>")
    set(RAWCore_HAS_LENSFUN OFF PARENT_SCOPE)
endfunction()

# ============================================================================
#  exiv2 —— 导出时写入 EXIF（append_v3 §3.8）
#
#  与 lensfun 一样属于"可选但推荐"：缺失时导出仍可用，只是不写入 EXIF，
#  并在日志与导出结果里如实说明，而不是静默丢弃。
# ============================================================================
function(rawcore_find_exiv2)
    if(TARGET RAWCore::exiv2)
        return()
    endif()

    find_package(Exiv2 QUIET CONFIG)
    if(NOT Exiv2_FOUND)
        find_package(exiv2 QUIET CONFIG)
    endif()

    foreach(_candidate Exiv2::exiv2lib Exiv2::exiv2 exiv2lib exiv2)
        if(TARGET ${_candidate})
            add_library(RAWCore::exiv2 ALIAS ${_candidate})
            message(STATUS "[RAWCore] exiv2 已通过 CMake 包定位 -> ${_candidate}")
            return()
        endif()
    endforeach()

    # 找不到 exiv2 时不需要报错，选图列表仍可显示文件名与尺寸
    message(STATUS "[RAWCore] 未找到 exiv2，JPG 的 EXIF 信息将不可用")
    set(RAWCore_HAS_EXIV2 OFF PARENT_SCOPE)
endfunction()

