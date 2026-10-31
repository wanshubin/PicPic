// ============================================================================
//  LensCorrection.cpp —— lensfun 封装实现（针对 lensfun 0.3.4+ 的 C 接口）
//
//  编译期开关：CMake 定义 RAWCore_HAS_LENSFUN。
//  未启用（或未找到 lensfun）时，本文件提供等价的空实现，
//  保证管线代码无需任何条件编译即可统一调用。
//
//  为什么用 C 接口而不是 C++ 类接口：
//  lensfun 0.3.2 与 0.3.4 的 C++ 类接口差异较大，而 C 接口在 0.3.4 起
//  保持稳定（lf_modifier_* 系列）。CMake 在配置阶段会校验头文件里是否
//  存在这些符号，不存在则自动禁用镜头校正并给出提示，因此不会造成
//  难以理解的编译失败。
//
//  接口要点：
//   - lf_modifier_new(lens, crop, width, height) 创建修改器
//   - lf_modifier_initialize(...) 传入焦距/光圈/对焦距离等拍摄参数
//   - lf_lens_interpolate_* 按焦距插值出该拍摄条件下的校准模型
//   - lf_modifier_add_*_callback_* 把模型挂到修改器上
//   - lf_modifier_apply_geometry_distortion 返回**坐标映射表**，
//     由我们自行做重采样。这比直接传像素更可控，也便于实现强度混合。
// ============================================================================

#include "LensCorrection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  include <omp.h>
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

#if RAWCore_HAS_LENSFUN
#  include RAWCore_LENSFUN_HEADER
#endif

namespace rawcore {

#if RAWCore_HAS_LENSFUN
namespace {

/// 当前可执行文件所在目录。
/// 数据库按"随程序分发"的约定放在 exe 旁的 lensfun/version_1；
/// core 层不依赖 Qt，因此这里直接用平台 API 取路径。
std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(1024);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length <= 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return std::filesystem::path(std::string(buffer.begin(), buffer.end()))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return {};
#endif
}

/// std::filesystem::path → UTF-8（Windows 下 GLib/lensfun 期望 UTF-8 路径）
std::string toUtf8(const std::filesystem::path& path)
{
#if defined(_WIN32)
    return path.u8string();
#else
    return path.string();
#endif
}

/// 读取环境变量（Windows 下避免 getenv 的 C4996 警告）
std::string environmentValue(const char* name)
{
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

/// lensfun 数据库候选目录，按优先级排列。
std::vector<std::string> lensDatabaseCandidates()
{
    std::vector<std::string> candidates;

    // 1. 显式环境变量：便于开发与自定义安装
    const std::string envDatabase = environmentValue("RAWEDIT_LENSFUN_DB");
    if (!envDatabase.empty()) {
        candidates.push_back(envDatabase);
    }

    // 2. 随程序分发 / 源码构建输出
    const std::filesystem::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        candidates.push_back(toUtf8(exeDir / "lensfun" / "version_1"));
        candidates.push_back(toUtf8(exeDir / ".." / "share" / "lensfun" / "version_1"));
    }

    // 3. 用户导入目录（D7）：导入的 XML 放在这里，无需管理员权限
    const std::string userDatabase = LensCorrector::writableDatabaseDirectory();
    if (!userDatabase.empty()) {
        candidates.push_back(userDatabase);
    }

    // 4. 系统级数据包
#if defined(_WIN32)
    const std::string programData = environmentValue("ProgramData");
    if (!programData.empty()) {
        candidates.push_back(
            toUtf8(std::filesystem::u8path(programData) / "lensfun" / "version_1"));
    }
#elif defined(__linux__)
    candidates.emplace_back("/usr/share/lensfun/version_1");
    candidates.emplace_back("/usr/local/share/lensfun/version_1");
    candidates.emplace_back("/var/lib/lensfun-updates/version_1");
#endif

    return candidates;
}

/// 尝试把数据库加载进 db。
/// 不再依赖 lf_db_load() 的默认路径：lensfun 0.3.4 的 Windows 构建把
/// 数据目录拼成了"dll 目录 + 绝对路径"，默认加载必然失败。
///
/// 关键：**所有**存在的候选目录都要加载，而不是遇到第一个就返回。
/// 用户导入的 XML 位于用户目录，随程序分发的完整数据库位于 exe 目录；
/// 只加载第一个会让导入的镜头被内置数据库"挡住"而永远不生效。
/// lf_db_load_directory 可以多次调用，结果会追加到同一个 db。
///
/// @param usedPath 成功时写入第一个实际加载成功的目录
/// @return 是否至少加载到一个 XML 文件
bool tryLoadLensDatabase(lfDatabase* db, std::string& usedPath)
{
    bool loadedAny = false;
    for (const std::string& directory : lensDatabaseCandidates()) {
        std::error_code error;
        const std::filesystem::path path = std::filesystem::u8path(directory);
        if (!std::filesystem::is_directory(path, error)) {
            continue;
        }
        if (lf_db_load_directory(db, directory.c_str())) {
            if (usedPath.empty()) {
                usedPath = directory;
            }
            loadedAny = true;
        }
    }

    if (loadedAny) {
        return true;
    }

    // 兜底：lensfun 自己的默认查找（未来版本修复路径拼装后可能生效）
    if (lf_db_load(db) == LF_NO_ERROR) {
        usedPath = "（lensfun 默认路径）";
        return true;
    }

    return false;
}

}  // namespace
#endif  // RAWCore_HAS_LENSFUN

// ============================================================================
//  Impl
// ============================================================================

struct LensCorrector::Impl {
#if RAWCore_HAS_LENSFUN
    /// lensfun 镜头数据库句柄
    lfDatabase* database = nullptr;

    /// 匹配到的镜头与相机（指向数据库内部数据，不可释放）
    const lfLens* lens = nullptr;
    const lfCamera* camera = nullptr;

    /// 裁切系数
    float cropFactor = 1.0f;

    /// 拍摄参数，用于选择校准参数组
    float focalLength = 0.0f;
    float aperture = 0.0f;
    float focusDistance = 0.0f;

    std::string matchedLensName;
    std::string matchMessage;
    /// 实际使用的数据库目录（空表示未加载成功）
    std::string databasePath;
    bool initialized = false;
#else
    std::string matchMessage = "本次构建未包含 lensfun 支持，镜头校正不可用。";
    std::string databasePath;
    std::string matchedLensName;
#endif
};

// ============================================================================
//  构造 / 析构
// ============================================================================

LensCorrector::LensCorrector()
    : m_impl(std::make_unique<Impl>())
{
}

LensCorrector::~LensCorrector()
{
#if RAWCore_HAS_LENSFUN
    if (m_impl->database != nullptr) {
        lf_db_destroy(m_impl->database);
        m_impl->database = nullptr;
    }
#endif
}

bool LensCorrector::isSupported()
{
#if RAWCore_HAS_LENSFUN
    return true;
#else
    return false;
#endif
}

std::string LensCorrector::writableDatabaseDirectory()
{
#if !RAWCore_HAS_LENSFUN
    return {};
#elif defined(_WIN32)
    const std::string appData = environmentValue("APPDATA");
    if (!appData.empty()) {
        return toUtf8(std::filesystem::u8path(appData)
                      / "RawEdit" / "lensfun" / "version_1");
    }
#elif defined(__linux__)
    const std::string xdg = environmentValue("XDG_DATA_HOME");
    if (!xdg.empty()) {
        return toUtf8(std::filesystem::u8path(xdg)
                      / "RawEdit" / "lensfun" / "version_1");
    }
    const std::string home = environmentValue("HOME");
    if (!home.empty()) {
        return toUtf8(std::filesystem::u8path(home)
                      / ".local" / "share" / "RawEdit" / "lensfun" / "version_1");
    }
#endif
    return {};
}

const std::string& LensCorrector::lastMatchMessage() const
{
    return m_impl->matchMessage;
}

const std::string& LensCorrector::databasePath() const
{
    return m_impl->databasePath;
}

bool LensCorrector::databaseLoaded() const
{
#if RAWCore_HAS_LENSFUN
    return m_impl->database != nullptr;
#else
    return false;
#endif
}

const std::string& LensCorrector::matchedLensName() const
{
    return m_impl->matchedLensName;
}

// ============================================================================
//  镜头匹配
// ============================================================================

bool LensCorrector::findLens(const RawMetadata& metadata,
                             const LensCorrectionSettings* settings)
{
#if !RAWCore_HAS_LENSFUN
    (void)metadata;
    (void)settings;
    m_impl->matchMessage = "本次构建未包含 lensfun 支持，镜头校正不可用。";
    return false;
#else
    Impl& impl = *m_impl;

    impl.focalLength = metadata.focalLength;
    impl.aperture = metadata.aperture;
    impl.focusDistance = metadata.focusDistance;

    // D7：手动选择优先；焦距 / 光圈可覆盖 EXIF（转接环、增距镜常用）
    const bool manual = settings != nullptr && settings->useManualLens
        && !settings->manualLensName.empty();
    if (manual) {
        if (settings->manualFocalLength > 0.0f) {
            impl.focalLength = settings->manualFocalLength;
        }
        if (settings->manualAperture > 0.0f) {
            impl.aperture = settings->manualAperture;
        }
    }

    // ---- 首次使用时加载镜头数据库 ----
    if (!impl.initialized) {
        impl.initialized = true;

        impl.database = lf_db_new();
        if (impl.database == nullptr) {
            impl.matchMessage = "无法创建 lensfun 镜头数据库对象。";
            return false;
        }

        if (!tryLoadLensDatabase(impl.database, impl.databasePath)) {
            std::string searched;
            for (const std::string& directory : lensDatabaseCandidates()) {
                searched += (searched.empty() ? "" : "、") + directory;
            }
            impl.matchMessage =
                "未能加载 lensfun 镜头数据库，镜头校正已跳过。已尝试的目录："
                + searched;
            lf_db_destroy(impl.database);
            impl.database = nullptr;
            return false;
        }
    }

    if (impl.database == nullptr) {
        return false;  // 之前的加载已失败
    }

    const char* maker = metadata.cameraMake.empty() ? nullptr : metadata.cameraMake.c_str();
    const char* model = metadata.cameraModel.empty() ? nullptr : metadata.cameraModel.c_str();

    // ---- 查找相机 ----
    // lf_db_find_cameras 返回以 nullptr 结尾的指针数组，须用 lf_free 释放。
    const lfCamera** cameras = lf_db_find_cameras(impl.database, maker,
                                                  model ? model : "");
    const lfCamera* camera = (cameras != nullptr && cameras[0] != nullptr) ? cameras[0] : nullptr;
    if (cameras != nullptr) {
        lf_free(const_cast<lfCamera**>(cameras));
    }
    impl.camera = camera;

    // ---- 查找镜头 ----
    const std::string& wantedName = manual ? settings->manualLensName
                                           : metadata.lensModel;
    if (wantedName.empty()) {
        impl.matchMessage = "该 RAW 未记录镜头型号，无法匹配镜头校准数据，"
                            "镜头校正已自动关闭。可手动选择镜头后重试。";
        return false;
    }

    // 手动选择时先按当前相机过滤；找不到再放宽到全库（跨品牌转接常见）。
    const lfLens** lenses = lf_db_find_lenses_hd(impl.database, camera, maker,
                                                 wantedName.c_str(), 0);
    const lfLens* lens = (lenses != nullptr && lenses[0] != nullptr) ? lenses[0] : nullptr;
    if (lenses != nullptr) {
        lf_free(const_cast<lfLens**>(lenses));
    }
    if (lens == nullptr && manual) {
        const lfLens** globalLenses =
            lf_db_find_lenses_hd(impl.database, nullptr, nullptr,
                                 wantedName.c_str(), 0);
        lens = (globalLenses != nullptr && globalLenses[0] != nullptr)
            ? globalLenses[0] : nullptr;
        if (globalLenses != nullptr) {
            lf_free(const_cast<lfLens**>(globalLenses));
        }
    }

    if (lens == nullptr && manual && !metadata.lensModel.empty()
        && metadata.lensModel != wantedName) {
        // 手动型号失效（改名 / 换库）时回退到 EXIF 自动匹配，
        // 并在提示里说明，避免用户以为校正完全失效。
        const lfLens** fallbackLenses = lf_db_find_lenses_hd(
            impl.database, camera, maker, metadata.lensModel.c_str(), 0);
        lens = (fallbackLenses != nullptr && fallbackLenses[0] != nullptr)
            ? fallbackLenses[0] : nullptr;
        if (fallbackLenses != nullptr) {
            lf_free(const_cast<lfLens**>(fallbackLenses));
        }
        if (lens != nullptr) {
            impl.matchMessage = "手动镜头未找到，已回退到 EXIF 自动匹配："
                + metadata.lensModel;
        }
    }

    if (lens == nullptr) {
        impl.matchMessage = (manual ? "手动选择的镜头 \"" : "lensfun 数据库中未找到镜头 \"")
                          + wantedName
                          + "\" 的校准数据，镜头校正已自动关闭。";
        return false;
    }

    impl.lens = lens;

    // ---- 取裁切系数 ----
    // 相机记录优先；缺失时退回到镜头校准时的裁切系数。
    impl.cropFactor = 1.0f;
    if (impl.camera != nullptr && impl.camera->CropFactor > 0.0f) {
        impl.cropFactor = impl.camera->CropFactor;
    } else if (impl.lens->CropFactor > 0.0f) {
        impl.cropFactor = impl.lens->CropFactor;
    }

    // 镜头名存放在 lfMLstr（带多语言变体），取英文主串
    const char* name = lf_mlstr_get(impl.lens->Model);
    impl.matchedLensName = (name != nullptr) ? name : metadata.lensModel;

    impl.matchMessage = "已匹配镜头：" + impl.matchedLensName;
    return true;
#endif
}

std::vector<std::string> LensCorrector::availableLensNames(const RawMetadata& metadata,
                                                           int maxCount)
{
    std::vector<std::string> names;
#if !RAWCore_HAS_LENSFUN
    (void)metadata;
    (void)maxCount;
    return names;
#else
    Impl& impl = *m_impl;

    // 惰性加载数据库（与 findLens 相同的候选目录探测）
    if (!impl.initialized) {
        impl.initialized = true;
        impl.database = lf_db_new();
        if (impl.database == nullptr
            || !tryLoadLensDatabase(impl.database, impl.databasePath)) {
            if (impl.database != nullptr) {
                lf_db_destroy(impl.database);
                impl.database = nullptr;
            }
            return names;
        }
    }
    if (impl.database == nullptr) {
        return names;
    }

    const char* maker = metadata.cameraMake.empty() ? nullptr
                                                    : metadata.cameraMake.c_str();
    const char* model = metadata.cameraModel.empty() ? nullptr
                                                     : metadata.cameraModel.c_str();

    const lfCamera** cameras = lf_db_find_cameras(impl.database, maker,
                                                  model ? model : "");
    const lfCamera* camera = (cameras != nullptr && cameras[0] != nullptr)
        ? cameras[0] : nullptr;
    if (cameras != nullptr) {
        lf_free(const_cast<lfCamera**>(cameras));
    }

    // 空型号串表示"该相机可用的全部镜头"；无匹配相机时返回全库，
    // 让转接 / 改机用户仍能手动选择。
    const lfLens** lenses = lf_db_find_lenses_hd(impl.database, camera, maker, "", 0);
    if (lenses == nullptr) {
        return names;
    }

    names.reserve(static_cast<std::size_t>(std::min(maxCount, 256)));
    for (int i = 0; lenses[i] != nullptr; ++i) {
        const char* name = lf_mlstr_get(lenses[i]->Model);
        if (name != nullptr && name[0] != '\0') {
            names.emplace_back(name);
        }
    }
    lf_free(const_cast<lfLens**>(lenses));

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (maxCount > 0 && static_cast<int>(names.size()) > maxCount) {
        names.resize(static_cast<std::size_t>(maxCount));
    }
    return names;
#endif
}

bool LensCorrector::importDatabaseFile(const std::string& xmlPath,
                                       std::string& savedPath,
                                       std::string& error)
{
    savedPath.clear();
    error.clear();

#if !RAWCore_HAS_LENSFUN
    (void)xmlPath;
    error = "本次构建未包含 lensfun 支持，无法导入镜头数据库。";
    return false;
#else
    std::error_code ec;
    const std::filesystem::path source = std::filesystem::u8path(xmlPath);
    if (!std::filesystem::exists(source, ec)
        || !std::filesystem::is_regular_file(source, ec)) {
        error = "文件不存在：" + xmlPath;
        return false;
    }

    std::string extension = source.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (extension != ".xml") {
        error = "lensfun 数据库文件应为 .xml。";
        return false;
    }

    // 只做最基本的格式检查：lensfun 的根节点是 <lensdatabase>。
    // 真正的合法性仍由 lensfun 在加载时判断；这里避免把任意文件复制进去。
    {
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            error = "无法读取文件：" + xmlPath;
            return false;
        }
        std::string head(4096, '\0');
        file.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(file.gcount()));
        if (head.find("<lensdatabase") == std::string::npos) {
            error = "不是 lensfun 数据库文件（缺少 <lensdatabase> 根节点）。";
            return false;
        }
    }

    const std::string targetDirectory = writableDatabaseDirectory();
    if (targetDirectory.empty()) {
        error = "无法确定用户 lensfun 数据库目录。";
        return false;
    }

    const std::filesystem::path directory = std::filesystem::u8path(targetDirectory);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "无法创建目录：" + targetDirectory;
        return false;
    }

    // 同名文件追加序号，绝不覆盖已有数据库
    std::filesystem::path target = directory / source.filename();
    int suffix = 2;
    while (std::filesystem::exists(target, ec)) {
        target = directory / (source.stem().u8string()
                              + " (" + std::to_string(suffix++) + ")"
                              + source.extension().u8string());
    }

    std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec);
    if (ec) {
        error = "复制文件失败：" + ec.message();
        return false;
    }

    savedPath = target.u8string();
    return true;
#endif
}

bool LensCorrector::reloadDatabase()
{
#if !RAWCore_HAS_LENSFUN
    return false;
#else
    Impl& impl = *m_impl;
    if (impl.database != nullptr) {
        lf_db_destroy(impl.database);
        impl.database = nullptr;
    }
    impl.initialized = false;
    impl.lens = nullptr;
    impl.camera = nullptr;
    impl.databasePath.clear();
    impl.matchedLensName.clear();
    impl.matchMessage.clear();

    impl.initialized = true;
    impl.database = lf_db_new();
    if (impl.database == nullptr) {
        impl.matchMessage = "无法创建 lensfun 镜头数据库对象。";
        return false;
    }
    if (!tryLoadLensDatabase(impl.database, impl.databasePath)) {
        impl.matchMessage = "重新加载 lensfun 数据库失败。";
        lf_db_destroy(impl.database);
        impl.database = nullptr;
        return false;
    }
    impl.matchMessage = "镜头数据库已重新加载。";
    return true;
#endif
}

// ============================================================================
//  内部工具：三次卷积采样与强度混合
// ============================================================================

#if RAWCore_HAS_LENSFUN
namespace {

/// 像素坐标夹取
inline int clampCoord(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

/// Catmull-Rom 三次卷积权重（4 个抽头）。
/// t 为采样点相对左抽头的小数位置，抽头位于 -1 / 0 / 1 / 2。
inline void cubicWeights(float t, float w[4])
{
    // w0 = -0.5t³ + t² - 0.5t
    // w1 =  1.5t³ - 2.5t² + 1
    // w2 = -1.5t³ + 2t² + 0.5t
    // w3 =  0.5t³ - 0.5t²
    w[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
    w[1] = ((1.5f * t - 2.5f) * t) * t + 1.0f;
    w[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
    w[3] = ((0.5f * t - 0.5f) * t) * t;
}

/// 单通道三次卷积采样（越界夹取到边缘）。
///
/// 相比双线性，三次卷积在校正量较大的画面边缘/角落能保留更多高频，
/// 避免"几何校正后再糊一次"。Catmull-Rom 有负旁瓣，高反差处可能出现
/// 过冲；这里把结果夹到 4x4 抽头的局部最小/最大值，既抑制振铃，
/// 又不会像全局 clamp 那样裁掉合法的 HDR 高光。
inline float sampleBicubicChannel(const float* source, int width, int height,
                                  float px, float py, int channel)
{
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(x0);
    const float fy = py - static_cast<float>(y0);

    float wx[4];
    float wy[4];
    cubicWeights(fx, wx);
    cubicWeights(fy, wy);

    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool first = true;
    float sum = 0.0f;

    for (int j = 0; j < 4; ++j) {
        const int sy = clampCoord(y0 - 1 + j, 0, height - 1);
        const float* row = source + (static_cast<std::size_t>(sy) * width) * 3u
                         + static_cast<std::size_t>(channel);

        float rowSum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const int sx = clampCoord(x0 - 1 + i, 0, width - 1);
            const float value = row[static_cast<std::size_t>(sx) * 3u];

            if (first) {
                minValue = value;
                maxValue = value;
                first = false;
            } else {
                minValue = std::min(minValue, value);
                maxValue = std::max(maxValue, value);
            }
            rowSum += value * wx[i];
        }
        sum += rowSum * wy[j];
    }

    return std::clamp(sum, minValue, maxValue);
}

}  // namespace
#endif

// ============================================================================
//  执行校正
// ============================================================================

LensCorrectionResult LensCorrector::apply(ImageBuffer& image,
                                          const LensCorrectionSettings& settings)
{
    LensCorrectionResult result;

    if (image.isEmpty()) {
        result.message = "图像为空，无法执行镜头校正。";
        return result;
    }

    if (!settings.enable) {
        result.message = "镜头校正未启用。";
        return result;
    }

#if !RAWCore_HAS_LENSFUN
    result.message = "本次构建未包含 lensfun 支持，镜头校正已跳过。";
    return result;
#else
    Impl& impl = *m_impl;

    if (impl.lens == nullptr) {
        // 需求 §6：无匹配镜头数据时自动关闭校正并给出提示
        result.message = impl.matchMessage.empty()
            ? "未匹配到镜头校准数据，镜头校正已自动关闭。"
            : impl.matchMessage;
        return result;
    }

    result.lensMatched = true;
    result.matchedLensName = impl.matchedLensName;

    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0) {
        result.message = "图像尺寸无效，镜头校正已跳过。";
        return result;
    }

    // ---- 创建并初始化修改器 ----
    lfModifier* modifier = lf_modifier_new(impl.lens, impl.cropFactor, width, height);
    if (modifier == nullptr) {
        result.message = "创建 lensfun 修改器失败，镜头校正已跳过。";
        return result;
    }

    const float focal = (impl.focalLength > 0.0f) ? impl.focalLength : 50.0f;
    const float aperture = (impl.aperture > 0.0f) ? impl.aperture : 8.0f;
    // 对焦距离单位与 lensfun 一致为米；未知时用很大的值近似无穷远
    const float distance = (impl.focusDistance > 0.0f) ? impl.focusDistance : 1000.0f;

    lf_modifier_initialize(modifier, impl.lens, LF_PF_F32,
                           focal, aperture, distance,
                           1.0f,            // scale
                           LF_RECTILINEAR,  // 目标几何：直线投影（普通镜头）
                           0,               // flags
                           false);          // reverse = false 表示"校正"

    // ---- 按拍摄参数插值出校准模型并挂到修改器 ----
    bool hasDistortion = false;
    bool hasTca = false;
    bool hasVignetting = false;

    lfLensCalibDistortion distortionCalib{};
    if (settings.enableDistortion
        && lf_lens_interpolate_distortion(impl.lens, focal, &distortionCalib)) {
        hasDistortion = lf_modifier_add_coord_callback_distortion(
            modifier, &distortionCalib, false);
    }

    lfLensCalibTCA tcaCalib{};
    if (settings.enableChromaticAberration
        && lf_lens_interpolate_tca(impl.lens, focal, &tcaCalib)) {
        hasTca = lf_modifier_add_subpixel_callback_TCA(modifier, &tcaCalib, false);
    }

    lfLensCalibVignetting vignettingCalib{};
    if (settings.enableVignetting
        && lf_lens_interpolate_vignetting(impl.lens, focal, aperture, distance,
                                          &vignettingCalib)) {
        hasVignetting = lf_modifier_add_color_callback_vignetting(
            modifier, &vignettingCalib, LF_PF_F32, false);
    }

    if (!hasDistortion && !hasTca && !hasVignetting) {
        lf_modifier_destroy(modifier);
        result.message = "该镜头在此焦距/光圈下没有可用的校准数据，镜头校正已跳过。";
        return result;
    }

    // 自动缩放：畸变校正会把画面向外/向内重映射，固定 scale=1 时四角可能
    // 采样到图像范围之外（表现为黑角或边缘被拉伸）。scale=0 让 lensfun
    // 根据完整坐标链计算最小缩放系数，保证校正后的画面仍然铺满。
    // 必须在畸变 callback 添加之后调用（GetAutoScale 依赖完整坐标链）。
    if (hasDistortion) {
        lf_modifier_add_coord_callback_scale(modifier, 0.0f, false);
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // ---- 1. 暗角校正（逐像素增益，可就地应用）----
    if (hasVignetting) {
        const float strength = std::clamp(settings.vignettingScale, 0.0f, 2.0f);

        if (std::fabs(strength - 1.0f) < 1e-3f) {
            // 标准强度：直接就地应用，无需额外内存
            lf_modifier_apply_color_modification(
                modifier, data, 0.0f, 0.0f, width, height,
                LF_CR_3(RED, GREEN, BLUE),
                width * 3 * static_cast<int>(sizeof(float)));
        } else if (strength > 1e-3f) {
            // 非标准强度：先在副本上应用完整校正，再按强度混合
            std::vector<float> corrected(data, data + image.sampleCount());
            lf_modifier_apply_color_modification(
                modifier, corrected.data(), 0.0f, 0.0f, width, height,
                LF_CR_3(RED, GREEN, BLUE),
                width * 3 * static_cast<int>(sizeof(float)));

            for (std::size_t i = 0; i < pixels * 3u; ++i) {
                data[i] = data[i] + (corrected[i] - data[i]) * strength;
            }
        }
    }

    // ---- 2. 几何校正（畸变 + 轴向色差）----
    //
    // lensfun 提供的是"目标像素 → 源像素"的坐标映射表，
    // 由我们自己做双线性重采样。这样强度混合只是坐标插值，代价极低。
    if (hasDistortion || hasTca) {
        // 保存原始像素用于采样（重采样必须从原始数据读取，否则会自采样污染）
        const std::vector<float> source(data, data + image.sampleCount());

        const float distortionStrength = std::clamp(settings.distortionScale, 0.0f, 2.0f);
        const float tcaStrength = std::clamp(settings.chromaticAberrationScale, 0.0f, 2.0f);
        const bool applyTca = hasTca && tcaStrength > 1e-3f;
        const bool applyDistortion = hasDistortion && distortionStrength > 1e-3f;

        // 畸变：每像素 2 个分量（x, y）
        std::vector<float> geometry;
        if (applyDistortion) {
            geometry.resize(pixels * 2u);
            lf_modifier_apply_geometry_distortion(
                modifier, 0.0f, 0.0f, width, height, geometry.data());
        }

        // 色差：每像素 2 个分量 × 3 个通道（R/G/B 各自的 x, y）
        std::vector<float> tca;
        if (applyTca) {
            tca.resize(pixels * 2u * 3u);
            lf_modifier_apply_subpixel_distortion(
                modifier, 0.0f, 0.0f, width, height, tca.data());
        }

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t pixelIndex = static_cast<std::size_t>(y) * width + x;

                // 目标像素的中心（lensfun 的坐标以图像左上角为原点，连续取值）
                const float targetX = static_cast<float>(x) + 0.5f;
                const float targetY = static_cast<float>(y) + 0.5f;

                // ---- 基准位置：几何（畸变）校正后的采样点 ----
                float baseX = targetX;
                float baseY = targetY;

                if (applyDistortion) {
                    const float mappedX = geometry[pixelIndex * 2u];
                    const float mappedY = geometry[pixelIndex * 2u + 1u];
                    // 按强度在"不校正"与"完全校正"之间插值
                    baseX = targetX + (mappedX - targetX) * distortionStrength;
                    baseY = targetY + (mappedY - targetY) * distortionStrength;
                }

                float* pixel = data + pixelIndex * 3u;

                if (applyTca) {
                    // lensfun 的 subpixel 映射表**已经包含**几何变换，
                    // 直接使用会与上面的基准位置重复计算。
                    // 因此以绿色通道为基准，只取出各通道相对绿色的**纯色差偏移**，
                    // 再叠加到基准位置上。这样畸变强度与色差强度可以独立调节。
                    const std::size_t greenOffset = pixelIndex * 6u + 2u;  // 通道 1(G) 的 x
                    const float greenX = tca[greenOffset];
                    const float greenY = tca[greenOffset + 1u];

                    for (int c = 0; c < 3; ++c) {
                        const std::size_t offset = pixelIndex * 6u
                                                 + static_cast<std::size_t>(c) * 2u;
                        const float channelDx = tca[offset] - greenX;
                        const float channelDy = tca[offset + 1u] - greenY;

                        const float sampleX = baseX + channelDx * tcaStrength;
                        const float sampleY = baseY + channelDy * tcaStrength;

                        pixel[c] = sampleBicubicChannel(source.data(), width, height,
                                                        sampleX - 0.5f, sampleY - 0.5f, c);
                    }
                } else {
                    for (int c = 0; c < 3; ++c) {
                        pixel[c] = sampleBicubicChannel(source.data(), width, height,
                                                       baseX - 0.5f, baseY - 0.5f, c);
                    }
                }
            }
        }
    }

    lf_modifier_destroy(modifier);

    result.applied = true;
    result.message = "镜头校正已应用：" + result.matchedLensName;
    return result;
#endif
}

}  // namespace rawcore
