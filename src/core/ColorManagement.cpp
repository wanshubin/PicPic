// ============================================================================
//  ColorManagement.cpp —— lcms2 封装实现
//
//  内置 profile 全部由代码生成。生成所用的白点与基色坐标为公开标准值：
//   - ProPhotoRGB: 白点 D50, 基色与 gamma 1.8 编码曲线(ROMM RGB 规范)
//   - sRGB:        白点 D65, 基色 Rec.709, sRGB 分段传递函数
//   - AdobeRGB:    白点 D65, 基色 Adobe, gamma 2.19921875
//   - XYZ:         lcms2 内建 cmsCreateXYZProfile
// ============================================================================

#include "ColorManagement.h"

#include "Dither.h"
#include "LinearMath.h"

#include <lcms2.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  include <omp.h>
#  define RAWCORE_CM_HAS_OPENMP 1
#else
#  define RAWCORE_CM_HAS_OPENMP 0
#endif

#if defined(_WIN32)
// windows.h 会定义 min/max 宏，与 <algorithm> 的 std::min/std::max 冲突。
// 必须在包含 windows.h 之前定义 NOMINMAX 屏蔽这些宏。
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace rawcore {

// ============================================================================
//  ColorProfile —— 对 cmsHPROFILE 的轻量封装
// ============================================================================

class ColorProfile {
public:
    explicit ColorProfile(cmsHPROFILE handle = nullptr) : m_handle(handle) {}
    ~ColorProfile()
    {
        if (m_handle != nullptr) {
            cmsCloseProfile(m_handle);
        }
    }

    ColorProfile(const ColorProfile&) = delete;
    ColorProfile& operator=(const ColorProfile&) = delete;

    cmsHPROFILE handle() const { return m_handle; }
    bool isValid() const { return m_handle != nullptr; }

    /// 拿走所有权，调用方负责关闭
    cmsHPROFILE release()
    {
        cmsHPROFILE tmp = m_handle;
        m_handle = nullptr;
        return tmp;
    }

private:
    cmsHPROFILE m_handle = nullptr;
};

namespace {

// ---------------------------------------------------------------------------
//  标准色度坐标常量（CIE 1931 xyY）
// ---------------------------------------------------------------------------

/// 白点
constexpr double kWhiteD50[2] = {0.3457, 0.3585};
constexpr double kWhiteD65[2] = {0.3127, 0.3290};

/// ProPhotoRGB (ROMM RGB) 基色
constexpr double kProPhotoRed[2]   = {0.734699, 0.265301};
constexpr double kProPhotoGreen[2] = {0.159597, 0.840403};
constexpr double kProPhotoBlue[2]  = {0.036598, 0.000105};

/// sRGB / Rec.709 基色
constexpr double kSrgbRed[2]   = {0.6400, 0.3300};
constexpr double kSrgbGreen[2] = {0.3000, 0.6000};
constexpr double kSrgbBlue[2]  = {0.1500, 0.0600};

/// AdobeRGB 基色
constexpr double kAdobeRed[2]   = {0.6400, 0.3300};
constexpr double kAdobeGreen[2] = {0.2100, 0.7100};
constexpr double kAdobeBlue[2]  = {0.1500, 0.0600};

/// Display P3 基色（DCI-P3 原色 + D65 白点）
constexpr double kP3Red[2]   = {0.6800, 0.3200};
constexpr double kP3Green[2] = {0.2650, 0.6900};
constexpr double kP3Blue[2]  = {0.1500, 0.0600};

/// 构造 lcms2 的 xyY 结构
cmsCIExyY makeXyY(double x, double y)
{
    cmsCIExyY value{};
    value.x = x;
    value.y = y;
    value.Y = 1.0;
    return value;
}

/// 构造三基色结构
cmsCIExyYTRIPLE makePrimaries(const double red[2], const double green[2], const double blue[2])
{
    cmsCIExyYTRIPLE primaries{};
    primaries.Red = makeXyY(red[0], red[1]);
    primaries.Green = makeXyY(green[0], green[1]);
    primaries.Blue = makeXyY(blue[0], blue[1]);
    return primaries;
}

/// 创建一条 gamma 曲线，返回句柄与需要释放的曲线数组
cmsToneCurve** makeGammaCurves(double gamma, cmsToneCurve* storage[3])
{
    cmsToneCurve* curve = cmsBuildGamma(nullptr, gamma);
    if (curve == nullptr) {
        return nullptr;
    }
    storage[0] = curve;
    storage[1] = curve;
    storage[2] = curve;
    return storage;
}

}  // namespace

// ============================================================================
//  ColorManagement::Impl
// ============================================================================

struct ColorManagement::Impl {
    /// 内置 profile 缓存，键为 ColorSpace
    std::unordered_map<int, std::unique_ptr<ColorProfile>> builtinProfiles;

    /// 从文件加载的 profile 缓存，键为路径
    std::unordered_map<std::string, std::unique_ptr<ColorProfile>> fileProfiles;

    /// 显示器 profile（可能为 null，表示回退到 sRGB）
    std::unique_ptr<ColorProfile> display;
    bool displayResolved = false;

    /// transform 缓存。键由源/目标 profile 指针与意图、格式组合而成。
    /// 软打样时还要区分打样 profile 与是否模拟纸白。
    struct TransformKey {
        const ColorProfile* src = nullptr;
        const ColorProfile* dst = nullptr;
        RenderIntent intent = RenderIntent::RelativeColorimetric;
        std::uint32_t format = 0;
        const ColorProfile* proof = nullptr;
        bool paperWhite = false;

        bool operator==(const TransformKey& other) const
        {
            return src == other.src && dst == other.dst
                && intent == other.intent && format == other.format
                && proof == other.proof && paperWhite == other.paperWhite;
        }
    };

    struct TransformKeyHash {
        std::size_t operator()(const TransformKey& key) const
        {
            // 简单混合：利用指针、意图与格式的低位，足以避免日常冲突
            std::size_t h = reinterpret_cast<std::size_t>(key.src);
            h ^= reinterpret_cast<std::size_t>(key.dst) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= static_cast<std::size_t>(key.intent) * 0x85ebca6b;
            h ^= static_cast<std::size_t>(key.format) * 0xc2b2ae35;
            h ^= reinterpret_cast<std::size_t>(key.proof) + 0x27d4eb2f + (h << 6) + (h >> 2);
            h ^= static_cast<std::size_t>(key.paperWhite ? 1 : 0) * 0x165667b1;
            return h;
        }
    };

    std::unordered_map<TransformKey, cmsHTRANSFORM, TransformKeyHash> transforms;

    /// 并行转换句柄池：每个句柄只在对应线程内使用（lcms2 的 transform
    /// 不是线程安全的）。大图转换时按线程分块，把单线程的 2~3 秒
    /// 降到数百毫秒甚至更低。
    std::unordered_map<TransformKey, std::vector<cmsHTRANSFORM>, TransformKeyHash> transformPools;

    ~Impl()
    {
        // 先释放 transform，再释放 profile —— 顺序不可颠倒，
        // 因为 transform 内部持有对 profile 的引用
        for (auto& entry : transforms) {
            if (entry.second != nullptr) {
                cmsDeleteTransform(entry.second);
            }
        }
        transforms.clear();

        for (auto& entry : transformPools) {
            for (cmsHTRANSFORM handle : entry.second) {
                if (handle != nullptr) {
                    cmsDeleteTransform(handle);
                }
            }
        }
        transformPools.clear();
        fileProfiles.clear();
        display.reset();
        builtinProfiles.clear();
    }
};

// ============================================================================
//  构造 / 析构
// ============================================================================

ColorManagement::ColorManagement()
    : m_impl(std::make_unique<Impl>())
{
}

ColorManagement::~ColorManagement() = default;

std::string ColorManagement::lcmsVersion()
{
    const int version = cmsGetEncodedCMMversion();
    // lcms2 把版本编码为整数，如 2140 表示 2.14
    return std::to_string(version / 1000) + "."
         + std::to_string((version / 100) % 10) + "."
         + std::to_string(version % 100);
}

// ============================================================================
//  Profile 创建
// ============================================================================

namespace {

/// 按色彩空间创建对应的 cmsHPROFILE。返回 nullptr 表示失败。
cmsHPROFILE createBuiltinProfile(ColorSpace space)
{
    switch (space) {
    case ColorSpace::ProPhotoRGB:
    case ColorSpace::LinearProPhotoRGB: {
        const cmsCIExyY white = makeXyY(kWhiteD50[0], kWhiteD50[1]);
        const cmsCIExyYTRIPLE primaries = makePrimaries(kProPhotoRed, kProPhotoGreen, kProPhotoBlue);

        cmsToneCurve* curves[3] = {nullptr, nullptr, nullptr};
        cmsToneCurve* built[3] = {nullptr, nullptr, nullptr};

        if (space == ColorSpace::LinearProPhotoRGB) {
            // 线性版：gamma = 1.0，用于管线中间态
            if (makeGammaCurves(1.0, built) == nullptr) {
                return nullptr;
            }
            curves[0] = built[0];
            curves[1] = built[1];
            curves[2] = built[2];
        } else {
            // ProPhotoRGB 规范使用 gamma 1.8 编码曲线。
            // 注意：ROMM RGB 精确曲线在暗部有一段线性段，
            // 此处用纯幂函数近似，对修图用途足够。
            if (makeGammaCurves(1.8, built) == nullptr) {
                return nullptr;
            }
            curves[0] = built[0];
            curves[1] = built[1];
            curves[2] = built[2];
        }

        cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
        cmsFreeToneCurve(built[0]);  // 三通道共用同一条曲线，只释放一次
        return profile;
    }

    case ColorSpace::sRGB: {
        // sRGB 使用分段传递函数，lcms2 有内建构造器，比手工拼伽马更准确
        return cmsCreate_sRGBProfile();
    }

    case ColorSpace::AdobeRGB: {
        const cmsCIExyY white = makeXyY(kWhiteD65[0], kWhiteD65[1]);
        const cmsCIExyYTRIPLE primaries = makePrimaries(kAdobeRed, kAdobeGreen, kAdobeBlue);

        cmsToneCurve* built[3] = {nullptr, nullptr, nullptr};
        // AdobeRGB 的精确 gamma 值为 563/256 = 2.19921875
        if (makeGammaCurves(563.0 / 256.0, built) == nullptr) {
            return nullptr;
        }
        cmsToneCurve* curves[3] = {built[0], built[1], built[2]};

        cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
        cmsFreeToneCurve(built[0]);
        return profile;
    }

    case ColorSpace::DisplayP3: {
        // Display P3 = DCI-P3 原色 + D65 白点 + sRGB 传递函数
        const cmsCIExyY white = makeXyY(kWhiteD65[0], kWhiteD65[1]);
        const cmsCIExyYTRIPLE primaries =
            makePrimaries(kP3Red, kP3Green, kP3Blue);

        // lcms2 参数曲线类型 4：y = (a·x + b)^gamma (x >= d)，否则 y = c·x
        const cmsFloat64Number params[5] = {
            2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
        cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, params);
        if (curve == nullptr) {
            return nullptr;
        }
        cmsToneCurve* curves[3] = {curve, curve, curve};
        cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
        cmsFreeToneCurve(curve);  // 三通道共用同一条曲线，只释放一次
        return profile;
    }

    case ColorSpace::XYZ:
        // lcms2 内建的 XYZ profile，白点为 D50
        return cmsCreateXYZProfile();
    }

    return nullptr;
}

/// 把渲染意图映射到 lcms2 常量
int toLcmsIntent(RenderIntent intent)
{
    switch (intent) {
    case RenderIntent::Perceptual: return INTENT_PERCEPTUAL;
    case RenderIntent::RelativeColorimetric: return INTENT_RELATIVE_COLORIMETRIC;
    case RenderIntent::Saturation: return INTENT_SATURATION;
    case RenderIntent::AbsoluteColorimetric: return INTENT_ABSOLUTE_COLORIMETRIC;
    }
    return INTENT_RELATIVE_COLORIMETRIC;
}

}  // namespace

const ColorProfile* ColorManagement::profileFor(ColorSpace space)
{
    const int key = static_cast<int>(space);

    auto it = m_impl->builtinProfiles.find(key);
    if (it != m_impl->builtinProfiles.end()) {
        return it->second.get();
    }

    cmsHPROFILE handle = createBuiltinProfile(space);
    if (handle == nullptr) {
        m_lastError = std::string("无法创建内置色彩配置文件：") + colorSpaceName(space);
        return nullptr;
    }

    auto profile = std::make_unique<ColorProfile>(handle);
    const ColorProfile* raw = profile.get();
    m_impl->builtinProfiles.emplace(key, std::move(profile));
    return raw;
}

// ============================================================================
//  显示器 profile
// ============================================================================

namespace {

#if defined(_WIN32)

/// 从 Windows 显示子系统读取当前显示器 ICC profile。
/// 失败返回空字符串。
std::string queryWindowsDisplayProfile()
{
    // 取主显示器的设备名
    DISPLAY_DEVICEA device{};
    device.cb = sizeof(DISPLAY_DEVICEA);
    if (!EnumDisplayDevicesA(nullptr, 0, &device, 0)) {
        return {};
    }
    if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
        return {};
    }

    HDC dc = CreateDCA("DISPLAY", device.DeviceName, nullptr, nullptr);
    if (dc == nullptr) {
        return {};
    }

    char profilePath[MAX_PATH] = {0};
    DWORD pathLength = static_cast<DWORD>(sizeof(profilePath));

    // GetICMProfileA 返回该显示器当前关联的 ICC profile 路径。
    // 拿不到时返回空串，调用方会回退到 sRGB。
    std::string result;
    if (GetICMProfileA(dc, &pathLength, profilePath)) {
        result = profilePath;
    }

    DeleteDC(dc);
    return result;
}

#else

/// 非 Windows 平台：尝试常见的 ICC 查询方式。
/// MVP 主战场为 Windows，此处仅保证能编译并优雅降级。
std::string queryUnixDisplayProfile()
{
    // 多数桌面环境的显示器配置不在标准位置，直接返回空，
    // 调用方会回退到 sRGB。二期可接入 colord / xiccd。
    return {};
}

#endif

}  // namespace

const ColorProfile* ColorManagement::displayProfile()
{
    if (m_impl->displayResolved) {
        // 解析完成但没有显示器 profile 时，display 为空，
        // 这里必须继续返回 sRGB 回退，而不能返回 nullptr
        // （否则只有第一次调用能拿到 profile，后续调用都会失败，
        //   预览会悄悄退化成"只做 gamma"的错误色彩）。
        return m_impl->display ? m_impl->display.get() : profileFor(ColorSpace::sRGB);
    }
    m_impl->displayResolved = true;

    std::string path;
#if defined(_WIN32)
    path = queryWindowsDisplayProfile();
#else
    path = queryUnixDisplayProfile();
#endif

    if (path.empty()) {
        // 读不到显示器 profile 时回退到 sRGB。
        // 这是需求 §6 精神下的优雅降级：预览色彩略偏总好过无法显示。
        m_lastError = "未能读取系统显示器 ICC 配置文件，预览已回退到 sRGB 色彩空间。";
        const ColorProfile* fallback = profileFor(ColorSpace::sRGB);
        return fallback;
    }

    cmsHPROFILE handle = cmsOpenProfileFromFile(path.c_str(), "r");
    if (handle == nullptr) {
        m_lastError = "显示器 ICC 配置文件无法解析，预览已回退到 sRGB：" + path;
        return profileFor(ColorSpace::sRGB);
    }

    m_impl->display = std::make_unique<ColorProfile>(handle);
    return m_impl->display.get();
}

const ColorProfile* ColorManagement::loadProfileFromFile(const std::string& filePath)
{
    auto it = m_impl->fileProfiles.find(filePath);
    if (it != m_impl->fileProfiles.end()) {
        return it->second.get();
    }

    cmsHPROFILE handle = cmsOpenProfileFromFile(filePath.c_str(), "r");
    if (handle == nullptr) {
        m_lastError = "无法加载 ICC 配置文件：" + filePath;
        return nullptr;
    }

    auto profile = std::make_unique<ColorProfile>(handle);
    const ColorProfile* raw = profile.get();
    m_impl->fileProfiles.emplace(filePath, std::move(profile));
    return raw;
}

// ============================================================================
//  Transform 缓存
// ============================================================================

namespace {

/// 统一创建 lcms2 transform（普通或软打样）。
///
/// 软打样走 cmsCreateProofingTransform：模拟“先转换到打样设备、再显示”
/// 的过程，从而在屏幕上看到打印后的色域与纸白表现。
cmsHTRANSFORM createCmsTransform(const ColorProfile* src,
                                 const ColorProfile* dst,
                                 const ColorProfile* proof,
                                 RenderIntent intent,
                                 bool simulatePaperWhite,
                                 std::uint32_t format,
                                 bool highResPrecalc)
{
    cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION;
    if (highResPrecalc) {
        flags |= cmsFLAGS_HIGHRESPRECALC;
    }

    if (proof != nullptr && proof->isValid()) {
        // “模拟纸白”用绝对比色实现：白点被映射到纸张白，
        // 纸白偏色与最大亮度都会保留下来。
        const cmsUInt32Number proofIntent = simulatePaperWhite
            ? INTENT_ABSOLUTE_COLORIMETRIC
            : INTENT_RELATIVE_COLORIMETRIC;
        return cmsCreateProofingTransform(
            src->handle(), format,
            dst->handle(), format,
            proof->handle(),
            toLcmsIntent(intent),
            proofIntent,
            flags | cmsFLAGS_SOFTPROOFING);
    }

    return cmsCreateTransform(src->handle(), format,
                              dst->handle(), format,
                              toLcmsIntent(intent), flags);
}

}  // namespace

void* ColorManagement::acquireTransform(const ColorProfile* src,
                                        const ColorProfile* dst,
                                        RenderIntent intent,
                                        std::uint32_t format,
                                        const ColorProfile* proof,
                                        bool simulatePaperWhite)
{
    if (src == nullptr || dst == nullptr || !src->isValid() || !dst->isValid()) {
        m_lastError = "色彩转换失败：源或目标配置文件无效。";
        return nullptr;
    }

    Impl::TransformKey key;
    key.src = src;
    key.dst = dst;
    key.intent = intent;
    key.format = format;
    key.proof = proof;
    key.paperWhite = simulatePaperWhite;

    auto it = m_impl->transforms.find(key);
    if (it != m_impl->transforms.end()) {
        return it->second;
    }

    // 注意：lcms2 的 cmsCreateTransform 在相同 profile 间转换时也不会出错，
    // 但为节省开销还是提前判断一下恒等情况。
    if (src == dst) {
        // 恒等转换：返回 nullptr 表示"无需转换"，由调用方直接拷贝
        m_impl->transforms.emplace(key, nullptr);
        return nullptr;
    }

    cmsHTRANSFORM transform = createCmsTransform(
        src, dst, proof, intent, simulatePaperWhite,
        format, /*highResPrecalc=*/true);

    if (transform == nullptr) {
        m_lastError = proof != nullptr
            ? "创建软打样色彩转换失败，可能是打样 profile 与显示器 profile 不兼容。"
            : "创建色彩转换失败，可能是 profile 不兼容或内存不足。";
        return nullptr;
    }

    m_impl->transforms.emplace(key, transform);
    return transform;
}

// ============================================================================
//  像素转换
// ============================================================================

ErrorInfo ColorManagement::convert(const ImageBuffer& source,
                                   ColorSpace fromSpace,
                                   ColorSpace toSpace,
                                   ImageBuffer& dest,
                                   RenderIntent intent)
{
    if (source.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "源图像为空，无法进行色彩转换。");
    }

    const ColorProfile* srcProfile = profileFor(fromSpace);
    const ColorProfile* dstProfile = profileFor(toSpace);
    if (srcProfile == nullptr || dstProfile == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "色彩转换失败：无法获取色彩配置文件。",
                               m_lastError);
    }

    return convertInternal(source, srcProfile, dstProfile, dest, intent);
}

ErrorInfo ColorManagement::convertInternal(const ImageBuffer& source,
                                           const ColorProfile* srcProfile,
                                           const ColorProfile* dstProfile,
                                           ImageBuffer& dest,
                                           RenderIntent intent,
                                           bool prefer16Bit)
{
    // 未启用 OpenMP 的构建里没有 16 位并行路径，显式消掉未使用参数告警
    (void)prefer16Bit;

    dest.allocate(source.width(), source.height());

    // 恒等转换：直接拷贝，避免无谓的 lcms2 往返
    if (srcProfile == dstProfile) {
        std::memcpy(dest.data(), source.data(), source.sampleCount() * sizeof(float));
        return ErrorInfo::ok();
    }

    // lcms2 的 cmsDoTransform 单次调用长度受 cmsUInt32Number 限制，
    // 超大图需要分块处理，否则会溢出。此处按像素分批。
    const std::size_t totalPixels = source.pixelCount();
    constexpr std::size_t kChunkPixels = 1u << 22;  // 每次约 400 万像素

    const float* src = source.data();
    float* dst = dest.data();

    // -------------------------------------------------------------------
    //  大图并行路径
    //
    //  lcms2 单个 transform 句柄不能跨线程使用，因此为每个线程准备
    //  一个同参数的句柄（首次使用时创建并缓存）。46MP 图像的单线程
    //  转换需要 2 秒以上，分块并行后通常能降到数百毫秒。
    // -------------------------------------------------------------------
#if RAWCORE_CM_HAS_OPENMP
    const int maxThreads = omp_get_max_threads();
    constexpr std::size_t kParallelPixelThreshold = 1u << 20;  // 约 100 万像素以上才值得并行
    if (maxThreads > 1 && totalPixels >= kParallelPixelThreshold) {
        // ---------------------------------------------------------------
        //  16 位整数快速路径（仅预览显示使用）
        //
        //  lcms2 的浮点流水线逐像素求值，实测单句柄只有约 8 Mpix/s；
        //  16 位流水线走预计算的 CLUT + 整数插值，同样条件下约 73 Mpix/s。
        //  预览最终只上屏 8 位，16 位中间精度绰绰有余，因此这条路径
        //  在大图上能省掉几百毫秒。浮点↔16 位的往返转换在这里一并做掉。
        // ---------------------------------------------------------------
        if (prefer16Bit) {
            Impl::TransformKey fastKey;
            fastKey.src = srcProfile;
            fastKey.dst = dstProfile;
            fastKey.intent = intent;
            fastKey.format = TYPE_RGB_16;

            auto fastIt = m_impl->transformPools.find(fastKey);
            if (fastIt == m_impl->transformPools.end()) {
                const int poolSize = std::min(maxThreads, 16);
                std::vector<cmsHTRANSFORM> handles;
                handles.reserve(static_cast<std::size_t>(poolSize));
                bool createdAll = true;
                for (int i = 0; i < poolSize; ++i) {
                    // 预览句柄不启用 HIGHRESPRECALC：它会把 CLUT 网格从 17³ 提到 33³，
                    // 每个句柄的创建耗时从约 5ms 涨到约 16ms（16 个句柄合计 0.26 秒，
                    // 都记在首帧上），而转换速度完全一样。预览最终只上屏 8 位，
                    // 17³ 网格的插值误差已在 1/255 以内；导出路径仍用浮点 + 高精度网格。
                    cmsHTRANSFORM handle = cmsCreateTransform(
                        srcProfile->handle(), TYPE_RGB_16,
                        dstProfile->handle(), TYPE_RGB_16,
                        toLcmsIntent(intent),
                        cmsFLAGS_BLACKPOINTCOMPENSATION);
                    if (handle == nullptr) {
                        createdAll = false;
                        break;
                    }
                    handles.push_back(handle);
                }
                if (createdAll) {
                    fastIt = m_impl->transformPools.emplace(fastKey, std::move(handles)).first;
                } else {
                    for (cmsHTRANSFORM handle : handles) {
                        cmsDeleteTransform(handle);
                    }
                }
            }

            if (fastIt != m_impl->transformPools.end() && !fastIt->second.empty()) {
                const auto& handles = fastIt->second;
                const int threads = static_cast<int>(handles.size());
                const std::size_t perThread = (totalPixels + threads - 1) / threads;
                // 每线程每批处理 26 万像素，临时缓冲约 1.5MB，
                // 既能让数据留在缓存里，又不会让 16 个线程把内存吃满。
                constexpr std::size_t kBatchPixels = 1u << 18;

#  pragma omp parallel for schedule(static) num_threads(threads)
                for (int t = 0; t < threads; ++t) {
                    std::vector<unsigned short> inBuffer(kBatchPixels * 3u);
                    std::vector<unsigned short> outBuffer(kBatchPixels * 3u);

                    const std::size_t begin = std::min(
                        totalPixels, static_cast<std::size_t>(t) * perThread);
                    const std::size_t end = std::min(totalPixels, begin + perThread);
                    std::size_t offset = begin;
                    while (offset < end) {
                        const std::size_t count = std::min(kBatchPixels, end - offset);
                        const std::size_t samples = count * 3u;

                        // 浮点 → 16 位：先夹取到 [0,1] 再量化
                        const float* srcPtr = src + offset * 3u;
                        for (std::size_t i = 0; i < samples; ++i) {
                            const float v = std::clamp(srcPtr[i], 0.0f, 1.0f);
                            inBuffer[i] = static_cast<unsigned short>(v * 65535.0f + 0.5f);
                        }

                        cmsDoTransform(handles[static_cast<std::size_t>(t)],
                                       inBuffer.data(), outBuffer.data(),
                                       static_cast<cmsUInt32Number>(count));

                        // 16 位 → 浮点：显示图仍是浮点缓冲，后续统一量化上屏
                        float* dstPtr = dst + offset * 3u;
                        for (std::size_t i = 0; i < samples; ++i) {
                            dstPtr[i] = static_cast<float>(outBuffer[i]) * (1.0f / 65535.0f);
                        }
                        offset += count;
                    }
                }
                return ErrorInfo::ok();
            }
            // 句柄池创建失败则退回浮点路径
        }

        Impl::TransformKey key;
        key.src = srcProfile;
        key.dst = dstProfile;
        key.intent = intent;
        key.format = TYPE_RGB_FLT;

        auto poolIt = m_impl->transformPools.find(key);
        if (poolIt == m_impl->transformPools.end()) {
            // 句柄数量取线程数与 16 的较小值：再多受内存带宽限制，收益有限，
            // 而每个句柄都要预计算 LUT，数量过多会明显拖慢首次使用。
            const int poolSize = std::min(maxThreads, 16);
            std::vector<cmsHTRANSFORM> handles;
            handles.reserve(static_cast<std::size_t>(poolSize));
            bool createdAll = true;
            for (int i = 0; i < poolSize; ++i) {
                cmsHTRANSFORM handle = cmsCreateTransform(
                    srcProfile->handle(), TYPE_RGB_FLT,
                    dstProfile->handle(), TYPE_RGB_FLT,
                    toLcmsIntent(intent),
                    cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_HIGHRESPRECALC);
                if (handle == nullptr) {
                    createdAll = false;
                    break;
                }
                handles.push_back(handle);
            }
            if (createdAll) {
                poolIt = m_impl->transformPools.emplace(key, std::move(handles)).first;
            } else {
                for (cmsHTRANSFORM handle : handles) {
                    cmsDeleteTransform(handle);
                }
            }
        }

        if (poolIt != m_impl->transformPools.end() && !poolIt->second.empty()) {
            const auto& handles = poolIt->second;
            const int threads = static_cast<int>(handles.size());
            const std::size_t perThread = (totalPixels + threads - 1) / threads;

#  pragma omp parallel for schedule(static) num_threads(threads)
            for (int t = 0; t < threads; ++t) {
                const std::size_t begin = std::min(totalPixels,
                                                   static_cast<std::size_t>(t) * perThread);
                const std::size_t end = std::min(totalPixels, begin + perThread);
                std::size_t offset = begin;
                while (offset < end) {
                    const std::size_t chunk = std::min(kChunkPixels, end - offset);
                    cmsDoTransform(handles[static_cast<std::size_t>(t)],
                                   src + offset * 3u,
                                   dst + offset * 3u,
                                   static_cast<cmsUInt32Number>(chunk));
                    offset += chunk;
                }
            }
            return ErrorInfo::ok();
        }
        // 句柄池创建失败时退回单句柄串行路径
    }
#endif

    // -------------------------------------------------------------------
    //  串行路径
    // -------------------------------------------------------------------
    void* transformHandle = acquireTransform(srcProfile, dstProfile, intent, TYPE_RGB_FLT);
    if (transformHandle == nullptr) {
        // 源与目标不同（相同的情况已在函数开头提前返回），
        // 这里的 nullptr 只能是创建失败，不能把源数据当结果返回。
        return ErrorInfo::make(ErrorCode::Unknown,
                               "创建色彩转换失败，可能是 profile 不兼容或内存不足。",
                               m_lastError);
    }

    auto* transform = static_cast<cmsHTRANSFORM>(transformHandle);

    std::size_t offset = 0;
    while (offset < totalPixels) {
        const std::size_t chunk = std::min(kChunkPixels, totalPixels - offset);
        cmsDoTransform(transform,
                       src + offset * 3u,
                       dst + offset * 3u,
                       static_cast<cmsUInt32Number>(chunk));
        offset += chunk;
    }

    return ErrorInfo::ok();
}

ErrorInfo ColorManagement::convertWorkingToDisplay(const ImageBuffer& source, ImageBuffer& dest)
{
    // 内部工作空间的像素值是**线性**的，因此必须使用线性 ProPhoto profile。
    // 若误用带 gamma 1.8 曲线的普通 ProPhoto profile，lcms2 会把线性值
    // 当作已编码值处理，导致整体严重偏暗。
    const ColorProfile* working = profileFor(ColorSpace::LinearProPhotoRGB);
    const ColorProfile* display = displayProfile();
    if (working == nullptr || display == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "无法获取显示器色彩配置文件。",
                               m_lastError);
    }

    // 预览用相对比色，符合需求 §3 模块 C 的"相对比色优先"。
    // 预览走 16 位整数流水线：上屏只有 8 位，精度足够而速度快近一个数量级。
    return convertInternal(source, working, display, dest, RenderIntent::RelativeColorimetric,
                           /*prefer16Bit=*/true);
}

namespace {

/// 把 16 位样本量化到高字节，叠加 ±0.5 个 8 位级别的三角抖动。
///
/// 抖动本身来自 Dither.h，与 8 位导出共用同一套坐标哈希。
inline std::uint8_t quantize16To8Jittered(unsigned int value, float offset)
{
    // 16 位刻度下 1 个 8 位级别 = 256，±0.5 级即 ±128。
    const int dithered = static_cast<int>(value)
        + static_cast<int>(std::floor(offset * 256.0f * kDitherPeakLevels + 0.5f));
    const int clamped = std::clamp(dithered, 0, 65535);
    return static_cast<std::uint8_t>(clamped >> 8);
}

}  // namespace

ErrorInfo ColorManagement::convertWorkingToDisplay8(const ImageBuffer& source,
                                                    std::vector<std::uint8_t>& dest,
                                                    const SoftProofOptions& proof,
                                                    int originX,
                                                    int originY)
{
    if (source.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "源图像为空，无法进行色彩转换。");
    }

    const ColorProfile* working = profileFor(ColorSpace::LinearProPhotoRGB);
    const ColorProfile* display = displayProfile();
    if (working == nullptr || display == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "无法获取显示器色彩配置文件。",
                               m_lastError);
    }

    const ColorProfile* proofProfile = proof.proofingProfile;
    if (proofProfile != nullptr && !proofProfile->isValid()) {
        proofProfile = nullptr;
    }

    const std::size_t totalPixels = source.pixelCount();
    const std::size_t totalSamples = totalPixels * 3u;
    dest.resize(totalSamples);

    const int imageWidth = source.width();
    const float* src = source.data();

    // 恒等情况：直接把浮点值量化到 8 位（同样需要抖动，否则仍有色带）
    const auto quantizeFromFloat = [&]() {
        const float* in = src;
        std::uint8_t* out = dest.data();
        const int height = source.height();
        // 按行遍历：抖动用的坐标直接来自行列号，不必每像素做一次除法取余。
#if RAWCORE_CM_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
        for (int row = 0; row < height; ++row) {
            const std::size_t rowBase = static_cast<std::size_t>(row) * imageWidth;
            const float* inRow = in + rowBase * 3u;
            std::uint8_t* outRow = out + rowBase * 3u;
            const int y = originY + row;
            for (int col = 0; col < imageWidth; ++col) {
                const std::size_t base = static_cast<std::size_t>(col) * 3u;
                for (int c = 0; c < 3; ++c) {
                    const float value = std::clamp(inRow[base + c], 0.0f, 1.0f) * 255.0f
                        + ditherOffset(originX + col, y, c) * kDitherPeakLevels;
                    outRow[base + c] = static_cast<std::uint8_t>(
                        std::clamp(static_cast<int>(std::floor(value + 0.5f)), 0, 255));
                }
            }
        }
    };

    if (working == display) {
        quantizeFromFloat();
        return ErrorInfo::ok();
    }

    constexpr std::size_t kBatchPixels = 1u << 18;  // 每批约 26 万像素

#if RAWCORE_CM_HAS_OPENMP
    const int maxThreads = omp_get_max_threads();
    constexpr std::size_t kParallelPixelThreshold = 1u << 20;
    if (maxThreads > 1 && totalPixels >= kParallelPixelThreshold) {
        Impl::TransformKey key;
        key.src = working;
        key.dst = display;
        key.intent = RenderIntent::RelativeColorimetric;
        key.format = TYPE_RGB_16;
        key.proof = proofProfile;
        key.paperWhite = proof.simulatePaperWhite;

        auto poolIt = m_impl->transformPools.find(key);
        if (poolIt == m_impl->transformPools.end()) {
            const int poolSize = std::min(maxThreads, 16);
            std::vector<cmsHTRANSFORM> handles;
            handles.reserve(static_cast<std::size_t>(poolSize));
            bool createdAll = true;
            for (int i = 0; i < poolSize; ++i) {
                // 16 位流水线 + 默认 17³ CLUT：创建快（约 5ms/句柄）、
                // 转换快（实测约为浮点流水线的 9 倍），8 位上屏精度足够。
                cmsHTRANSFORM handle = createCmsTransform(
                    working, display, proofProfile,
                    RenderIntent::RelativeColorimetric,
                    proof.simulatePaperWhite, TYPE_RGB_16,
                    /*highResPrecalc=*/false);
                if (handle == nullptr) {
                    createdAll = false;
                    break;
                }
                handles.push_back(handle);
            }
            if (createdAll) {
                poolIt = m_impl->transformPools.emplace(key, std::move(handles)).first;
            } else {
                for (cmsHTRANSFORM handle : handles) {
                    cmsDeleteTransform(handle);
                }
            }
        }

        if (poolIt != m_impl->transformPools.end() && !poolIt->second.empty()) {
            const auto& handles = poolIt->second;
            const int threads = static_cast<int>(handles.size());
            const std::size_t perThread = (totalPixels + threads - 1) / threads;

#  pragma omp parallel for schedule(static) num_threads(threads)
            for (int t = 0; t < threads; ++t) {
                std::vector<unsigned short> inBuffer(kBatchPixels * 3u);
                std::vector<unsigned short> outBuffer(kBatchPixels * 3u);

                const std::size_t begin = std::min(totalPixels,
                                                   static_cast<std::size_t>(t) * perThread);
                const std::size_t end = std::min(totalPixels, begin + perThread);
                std::size_t offset = begin;
                while (offset < end) {
                    const std::size_t count = std::min(kBatchPixels, end - offset);
                    const std::size_t samples = count * 3u;

                    const float* srcPtr = src + offset * 3u;
                    for (std::size_t i = 0; i < samples; ++i) {
                        const float v = std::clamp(srcPtr[i], 0.0f, 1.0f);
                        inBuffer[i] = static_cast<unsigned short>(v * 65535.0f + 0.5f);
                    }

                    cmsDoTransform(handles[static_cast<std::size_t>(t)],
                                   inBuffer.data(), outBuffer.data(),
                                   static_cast<cmsUInt32Number>(count));

                    // 16 位 → 8 位：四舍五入到高字节，并叠加三角抖动消除色带。
                    // 行列号用递增计数，避免每像素一次整数除法取余。
                    std::uint8_t* dstPtr = dest.data() + offset * 3u;
                    int x = static_cast<int>(offset % imageWidth);
                    int y = static_cast<int>(offset / imageWidth);
                    for (std::size_t p = 0; p < count; ++p) {
                        const std::size_t base = p * 3u;
                        const int dx = originX + x;
                        const int dy = originY + y;
                        for (int c = 0; c < 3; ++c) {
                            dstPtr[base + static_cast<std::size_t>(c)] =
                                quantize16To8Jittered(outBuffer[base + static_cast<std::size_t>(c)],
                                                      ditherOffset(dx, dy, c));
                        }
                        if (++x == imageWidth) {
                            x = 0;
                            ++y;
                        }
                    }
                    offset += count;
                }
            }
            return ErrorInfo::ok();
        }
        // 句柄池创建失败时退回串行路径
    }
#endif

    // 串行路径：单句柄，同样走 16 位流水线
    void* transformHandle = acquireTransform(working, display,
                                             RenderIntent::RelativeColorimetric, TYPE_RGB_16,
                                             proofProfile, proof.simulatePaperWhite);
    if (transformHandle == nullptr) {
        // working 与 display 是不同 profile，此处只能视为创建失败，
        // 不能把线性工作空间数据直接量化上屏（会明显偏色）。
        return ErrorInfo::make(ErrorCode::Unknown,
                               proofProfile != nullptr
                                   ? "软打样色彩转换创建失败，已保持普通预览。"
                                   : "显示器色彩转换创建失败。",
                               m_lastError);
    }

    auto* transform = static_cast<cmsHTRANSFORM>(transformHandle);
    std::vector<unsigned short> inBuffer(kBatchPixels * 3u);
    std::vector<unsigned short> outBuffer(kBatchPixels * 3u);

    std::size_t offset = 0;
    while (offset < totalPixels) {
        const std::size_t count = std::min(kBatchPixels, totalPixels - offset);
        const std::size_t samples = count * 3u;

        const float* srcPtr = src + offset * 3u;
        for (std::size_t i = 0; i < samples; ++i) {
            const float v = std::clamp(srcPtr[i], 0.0f, 1.0f);
            inBuffer[i] = static_cast<unsigned short>(v * 65535.0f + 0.5f);
        }

        cmsDoTransform(transform, inBuffer.data(), outBuffer.data(),
                       static_cast<cmsUInt32Number>(count));

        std::uint8_t* dstPtr = dest.data() + offset * 3u;
        int x = static_cast<int>(offset % imageWidth);
        int y = static_cast<int>(offset / imageWidth);
        for (std::size_t p = 0; p < count; ++p) {
            const std::size_t base = p * 3u;
            const int dx = originX + x;
            const int dy = originY + y;
            for (int c = 0; c < 3; ++c) {
                dstPtr[base + static_cast<std::size_t>(c)] =
                    quantize16To8Jittered(outBuffer[base + static_cast<std::size_t>(c)],
                                          ditherOffset(dx, dy, c));
            }
            if (++x == imageWidth) {
                x = 0;
                ++y;
            }
        }
        offset += count;
    }

    return ErrorInfo::ok();
}

ErrorInfo ColorManagement::convertWorkingToDisplay16(const ImageBuffer& source,
                                                    std::vector<std::uint16_t>& dest,
                                                    const SoftProofOptions& proof,
                                                    int originX,
                                                    int originY)
{
    (void)originX;
    (void)originY;

    if (source.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "源图像为空，无法进行色彩转换。");
    }

    const ColorProfile* working = profileFor(ColorSpace::LinearProPhotoRGB);
    const ColorProfile* display = displayProfile();
    if (working == nullptr || display == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "无法获取显示器色彩配置文件。",
                               m_lastError);
    }

    const ColorProfile* proofProfile = proof.proofingProfile;
    if (proofProfile != nullptr && !proofProfile->isValid()) {
        proofProfile = nullptr;
    }

    const std::size_t totalPixels = source.pixelCount();
    dest.resize(totalPixels * 3u);
    const float* src = source.data();

    // 恒等：线性浮点直接量化到 16 位（不做抖动，16 位已足够平滑）
    if (working == display && proofProfile == nullptr) {
#if RAWCORE_CM_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
        for (int i = 0; i < static_cast<int>(totalPixels); ++i) {
            for (int c = 0; c < 3; ++c) {
                const float value = std::clamp(
                    src[static_cast<std::size_t>(i) * 3u + static_cast<std::size_t>(c)],
                    0.0f, 1.0f);
                dest[static_cast<std::size_t>(i) * 3u + static_cast<std::size_t>(c)] =
                    static_cast<std::uint16_t>(value * 65535.0f + 0.5f);
            }
        }
        return ErrorInfo::ok();
    }

    void* transformHandle = acquireTransform(working, display,
                                             RenderIntent::RelativeColorimetric,
                                             TYPE_RGB_16,
                                             proofProfile, proof.simulatePaperWhite);
    if (transformHandle == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "显示器高色深转换创建失败。", m_lastError);
    }

    constexpr std::size_t kBatchPixels = 1u << 18;
    auto* transform = static_cast<cmsHTRANSFORM>(transformHandle);
    std::vector<unsigned short> inBuffer(kBatchPixels * 3u);
    std::vector<unsigned short> outBuffer(kBatchPixels * 3u);

    std::size_t offset = 0;
    while (offset < totalPixels) {
        const std::size_t count = std::min(kBatchPixels, totalPixels - offset);
        const std::size_t samples = count * 3u;
        const float* srcPtr = src + offset * 3u;
        for (std::size_t i = 0; i < samples; ++i) {
            const float value = std::clamp(srcPtr[i], 0.0f, 1.0f);
            inBuffer[i] = static_cast<unsigned short>(value * 65535.0f + 0.5f);
        }
        cmsDoTransform(transform, inBuffer.data(), outBuffer.data(),
                       static_cast<cmsUInt32Number>(count));
        std::memcpy(dest.data() + offset * 3u, outBuffer.data(),
                    samples * sizeof(std::uint16_t));
        offset += count;
    }
    return ErrorInfo::ok();
}

ErrorInfo ColorManagement::buildGamutMask(const ImageBuffer& source,
                                          ColorSpace targetSpace,
                                          std::vector<std::uint8_t>& mask)
{
    mask.clear();
    if (source.isEmpty()) {
        return ErrorInfo::make(ErrorCode::InvalidArgument, "色域警告：源图像为空。");
    }

    ImageBuffer target;
    const ErrorInfo error = convertWorkingToOutput(source, targetSpace, target);
    if (!error.isOk()) {
        return error;
    }

    const std::size_t pixels = target.pixelCount();
    mask.assign(pixels, 0u);
    const float* data = target.data();

#if RAWCORE_CM_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < static_cast<int>(pixels); ++i) {
        const float* pixel = data + static_cast<std::size_t>(i) * 3u;
        const bool outOfGamut =
            pixel[0] < -1e-4f || pixel[0] > 1.0f + 1e-4f
            || pixel[1] < -1e-4f || pixel[1] > 1.0f + 1e-4f
            || pixel[2] < -1e-4f || pixel[2] > 1.0f + 1e-4f;
        mask[static_cast<std::size_t>(i)] = outOfGamut ? 1u : 0u;
    }
    return ErrorInfo::ok();
}

namespace {

/// 掩码叠加的通用实现（8 / 16 位）
template <typename DisplayType>
void blendGamutMask(std::vector<DisplayType>& display,
                    const std::vector<std::uint8_t>& mask,
                    float warnR, float warnG, float warnB,
                    float blend, float maxValue)
{
    const std::size_t pixels = std::min(mask.size(), display.size() / 3u);
    const float amount = std::clamp(blend, 0.0f, 1.0f);
    const float wr = std::clamp(warnR, 0.0f, 1.0f) * maxValue;
    const float wg = std::clamp(warnG, 0.0f, 1.0f) * maxValue;
    const float wb = std::clamp(warnB, 0.0f, 1.0f) * maxValue;

#if RAWCORE_CM_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < static_cast<int>(pixels); ++i) {
        if (mask[static_cast<std::size_t>(i)] == 0u) {
            continue;
        }
        DisplayType* out = display.data() + static_cast<std::size_t>(i) * 3u;
        out[0] = static_cast<DisplayType>(
            static_cast<float>(out[0]) * (1.0f - amount) + wr * amount);
        out[1] = static_cast<DisplayType>(
            static_cast<float>(out[1]) * (1.0f - amount) + wg * amount);
        out[2] = static_cast<DisplayType>(
            static_cast<float>(out[2]) * (1.0f - amount) + wb * amount);
    }
}

}  // namespace

void ColorManagement::overlayGamutMask8(std::vector<std::uint8_t>& display,
                                        const std::vector<std::uint8_t>& mask,
                                        float warnR, float warnG, float warnB,
                                        float blend) const
{
    blendGamutMask<std::uint8_t>(display, mask, warnR, warnG, warnB, blend, 255.0f);
}

void ColorManagement::overlayGamutMask16(std::vector<std::uint16_t>& display,
                                         const std::vector<std::uint8_t>& mask,
                                         float warnR, float warnG, float warnB,
                                         float blend) const
{
    blendGamutMask<std::uint16_t>(display, mask, warnR, warnG, warnB, blend, 65535.0f);
}

ErrorInfo ColorManagement::convertWorkingToOutput(const ImageBuffer& source,
                                                  ColorSpace targetSpace,
                                                  ImageBuffer& dest,
                                                  RenderIntent intent)
{
    // 同样使用线性 ProPhoto 作为源 profile（见 convertWorkingToDisplay 的说明）
    const ColorProfile* working = profileFor(ColorSpace::LinearProPhotoRGB);
    const ColorProfile* target = profileFor(targetSpace);
    if (working == nullptr || target == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "无法获取导出目标色彩配置文件。",
                               m_lastError);
    }
    return convertInternal(source, working, target, dest, intent);
}

ErrorInfo ColorManagement::convertXyzToWorking(const ImageBuffer& source, ImageBuffer& dest)
{
    // XYZ → ProPhotoRGB 的转换走 lcms2（需求 §2 管线明确要求 lcms2 负责此步）。
    // 注意 XYZ profile 是 3 通道，与 RGB 同为 3 通道，可直接用 TYPE_RGB_FLT。
    // 目标是**线性**工作空间，因此用线性 ProPhoto profile。
    const ColorProfile* xyz = profileFor(ColorSpace::XYZ);
    const ColorProfile* working = profileFor(ColorSpace::LinearProPhotoRGB);
    if (xyz == nullptr || working == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown,
                               "无法获取 XYZ 或工作空间色彩配置文件。",
                               m_lastError);
    }
    return convertInternal(source, xyz, working, dest, RenderIntent::RelativeColorimetric);
}

ErrorInfo ColorManagement::convertCameraToXyz(const ImageBuffer& source,
                                              const ColorProfile* cameraProfile,
                                              ImageBuffer& dest)
{
    if (cameraProfile == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown, "相机色彩配置文件为空。");
    }
    const ColorProfile* xyz = profileFor(ColorSpace::XYZ);
    if (xyz == nullptr) {
        return ErrorInfo::make(ErrorCode::Unknown, "无法获取 XYZ 色彩配置文件。",
                               m_lastError);
    }
    return convertInternal(source, cameraProfile, xyz, dest, RenderIntent::RelativeColorimetric);
}

// ============================================================================
//  ICC 字节流导出
// ============================================================================

std::vector<std::uint8_t> ColorManagement::profileBytes(const ColorProfile* profile)
{
    std::vector<std::uint8_t> bytes;
    if (profile == nullptr || !profile->isValid()) {
        m_lastError = "无法导出 ICC 数据：profile 无效。";
        return bytes;
    }

    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile->handle(), nullptr, &size) || size == 0) {
        m_lastError = "无法获取 ICC profile 的序列化长度。";
        return bytes;
    }

    bytes.resize(size);
    if (!cmsSaveProfileToMem(profile->handle(), bytes.data(), &size)) {
        m_lastError = "ICC profile 序列化失败。";
        bytes.clear();
        return bytes;
    }

    // lcms2 可能写入少于申请的长度，按实际长度收紧
    bytes.resize(size);
    return bytes;
}

std::vector<std::uint8_t> ColorManagement::profileBytesFor(ColorSpace space)
{
    return profileBytes(profileFor(space));
}

}  // namespace rawcore
