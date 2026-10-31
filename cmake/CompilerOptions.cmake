# ============================================================================
#  编译器选项与运行时库策略
#
#  关键点：CRT 选择不由本文件硬编码，而是跟随 vcpkg 三元组。
#   - triplet = x64-windows-static  -> MSVC_RUNTIME_LIBRARY = MultiThreaded（/MT）
#   - triplet = x64-windows         -> MSVC_RUNTIME_LIBRARY = MultiThreadedDLL（/MD）
#  CMake 3.15+ 会依据 CMAKE_MSVC_RUNTIME_LIBRARY 自动设置该目标属性。
#  这样"静态单 exe"与"动态 DLL 分发"两种打包方式共用同一份 CMakeLists。
# ============================================================================

# ----------------------------------------------------------------------------
# CRT 一致性检查（需求 append_v3 §2.1）
#
#  MSVC 的全部翻译单元必须使用同一种 CRT：
#    /MD（MultiThreadedDLL）或 /MT（MultiThreaded）
#  vcpkg 三元组决定了预编译依赖使用的 CRT，而工程自身的
#  CMAKE_MSVC_RUNTIME_LIBRARY / CMAKE_CXX_FLAGS 又可能被用户覆盖。
#  两者混用通常表现为 LNK2038（RuntimeLibrary 不匹配）或运行期堆损坏，
#  排查成本很高，因此在配置阶段直接给出明确错误。
# ----------------------------------------------------------------------------
function(rawcore_check_crt_consistency)
    if(NOT MSVC)
        return()
    endif()

    # ---- 1. vcpkg 侧：优先读 VCPKG_CRT_LINKAGE，其次按三元组名称推断 ----
    set(_vcpkg_crt "")
    if(DEFINED VCPKG_CRT_LINKAGE)
        string(TOLOWER "${VCPKG_CRT_LINKAGE}" _crt_linkage)
        if(_crt_linkage STREQUAL "static")
            set(_vcpkg_crt "MT")
        elseif(_crt_linkage STREQUAL "dynamic")
            set(_vcpkg_crt "MD")
        endif()
    endif()
    if(NOT _vcpkg_crt AND DEFINED VCPKG_TARGET_TRIPLET)
        string(TOLOWER "${VCPKG_TARGET_TRIPLET}" _triplet)
        # 注意：x64-windows-static-md 是"静态库 + 动态 CRT"，仍是 /MD
        if(_triplet MATCHES "-static-md$")
            set(_vcpkg_crt "MD")
        elseif(_triplet MATCHES "-static")
            set(_vcpkg_crt "MT")
        elseif(_triplet MATCHES "windows")
            set(_vcpkg_crt "MD")
        endif()
    endif()

    # ---- 2. 工程侧：先看 CMAKE_MSVC_RUNTIME_LIBRARY，再让显式编译选项覆盖 ----
    set(_project_crt "")
    if(CMAKE_MSVC_RUNTIME_LIBRARY)
        string(TOLOWER "${CMAKE_MSVC_RUNTIME_LIBRARY}" _runtime)
        # MultiThreadedDLL / MultiThreadedDebugDLL -> /MD
        if(_runtime MATCHES "multithreadeddll")
            set(_project_crt "MD")
        elseif(_runtime MATCHES "multithreaded")
            set(_project_crt "MT")
        endif()
    endif()

    # 汇总所有配置的编译选项，检查是否显式写了 /MT 或 /MD
    set(_all_flags "${CMAKE_CXX_FLAGS}")
    foreach(_config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${_config}" _config_upper)
        string(APPEND _all_flags " ${CMAKE_CXX_FLAGS_${_config_upper}}")
    endforeach()

    set(_flags_has_mt FALSE)
    set(_flags_has_md FALSE)
    if(_all_flags MATCHES "(^|[ /])/MTd?([ /]|$)")
        set(_flags_has_mt TRUE)
    endif()
    if(_all_flags MATCHES "(^|[ /])/MDd?([ /]|$)")
        set(_flags_has_md TRUE)
    endif()

    if(_flags_has_mt AND _flags_has_md)
        message(FATAL_ERROR
            "[RAWCore] CRT 配置冲突：编译选项中同时出现 /MT 与 /MD。\n"
            "请只保留一种运行时库，并确保 vcpkg 三元组与之匹配：\n"
            "  /MT -> VCPKG_TARGET_TRIPLET=x64-windows-static\n"
            "  /MD -> VCPKG_TARGET_TRIPLET=x64-windows（或 x64-windows-static-md）")
    elseif(_flags_has_mt)
        set(_project_crt "MT")
    elseif(_flags_has_md)
        set(_project_crt "MD")
    endif()

    # 未显式指定时，MSVC 默认使用动态 CRT（/MD）
    if(NOT _project_crt)
        set(_project_crt "MD")
    endif()

    # ---- 3. 交叉校验 ----
    if(_vcpkg_crt AND NOT _vcpkg_crt STREQUAL _project_crt)
        if(_vcpkg_crt STREQUAL "MT")
            set(_matching_runtime "MultiThreaded")
            set(_matching_triplet "x64-windows-static")
        else()
            set(_matching_runtime "MultiThreadedDLL")
            set(_matching_triplet "x64-windows 或 x64-windows-static-md")
        endif()
        message(FATAL_ERROR
            "[RAWCore] CRT 混用：vcpkg 三元组 ${VCPKG_TARGET_TRIPLET} 使用 "
            "/${_vcpkg_crt}，但工程编译选项使用 /${_project_crt}。\n"
            "这会导致 LNK2038 链接错误或运行期堆损坏，构建已终止。\n"
            "修复方式（二选一）：\n"
            "  1) 让工程跟随三元组：删除自定义的 /MT 或 /MD，"
            "或设置 -DCMAKE_MSVC_RUNTIME_LIBRARY=${_matching_runtime}"
            "（当前三元组应使用 /${_vcpkg_crt}）；\n"
            "  2) 改用与工程编译选项匹配的三元组："
            "/${_project_crt} 对应 ${_matching_triplet}。")
    endif()

    if(_vcpkg_crt)
        message(STATUS "[RAWCore] CRT 检查通过：vcpkg=/${_vcpkg_crt}，工程=/${_project_crt}")
    else()
        message(STATUS "[RAWCore] CRT 检查：工程=/${_project_crt}（未检测到 vcpkg 三元组）")
    endif()
endfunction()

# ----------------------------------------------------------------------------
# 通用编译选项
# ----------------------------------------------------------------------------
function(rawcore_set_compile_options target)
    target_compile_features(${target} PUBLIC cxx_std_17)

    if(WIN32)
        # windows.h 默认定义 min/max 宏，会破坏 <algorithm> 的 std::min/std::max。
        # Windows 目标上一律屏蔽，避免各处零散地重复处理。
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4                 # 较高警告等级
            /permissive-        # 严格标准一致性
            /utf-8              # 源码与执行字符集均为 UTF-8（源码含中文注释）
            /Zc:__cplusplus     # 让 __cplusplus 宏正确反映标准版本
            /Zc:preprocessor    # 使用符合标准的预处理器
            /bigobj             # 大对象文件（模板实例多时必需）
            /MP                 # 并行编译
        )
        # 关闭部分噪音警告（第三方头文件与浮点比较）
        target_compile_options(${target} PRIVATE
            /wd4244 /wd4267     # 整型收窄转换（libraw 回调中常见）
        )
    else()
        # GCC / Clang（为未来跨平台复用保留，MVP 主战场是 MSVC）
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter
        )
    endif()
endfunction()

# ----------------------------------------------------------------------------
# OpenMP：管线并行加速
# 找不到时降级为单线程，不阻止构建（只是性能下降）
# ----------------------------------------------------------------------------
function(rawcore_enable_openmp target)
    find_package(OpenMP QUIET COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(${target} PRIVATE OpenMP::OpenMP_CXX)
        target_compile_definitions(${target} PRIVATE RAWCore_HAS_OPENMP=1)
        message(STATUS "[RAWCore] OpenMP 已启用（${OpenMP_CXX_VERSION}）")
    else()
        target_compile_definitions(${target} PRIVATE RAWCore_HAS_OPENMP=0)
        message(WARNING "[RAWCore] 未找到 OpenMP，管线将单线程运行。")
    endif()
endfunction()
