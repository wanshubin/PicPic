// ============================================================================
//  EditContext.cpp —— 参数模型实现
// ============================================================================

#include "EditContext.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace rawcore {

// ---------------------------------------------------------------------------
//  暗电流补偿
// ---------------------------------------------------------------------------

float DarkCurrentSettings::offsetForIso(float iso) const
{
    const float safeIso = std::max(iso, 50.0f);

    // 机型配置提供了 ISO→偏移表时，按 ISO 线性插值；
    // 低于首项取首项，高于末项取末项。
    if (!isoOffsets.empty()) {
        if (safeIso <= isoOffsets.front().first) {
            return isoOffsets.front().second;
        }
        for (std::size_t i = 1; i < isoOffsets.size(); ++i) {
            const float x1 = isoOffsets[i - 1].first;
            const float y1 = isoOffsets[i - 1].second;
            const float x2 = isoOffsets[i].first;
            const float y2 = isoOffsets[i].second;
            if (safeIso <= x2) {
                const float span = std::max(1e-6f, x2 - x1);
                const float t = (safeIso - x1) / span;
                return y1 + t * (y2 - y1);
            }
        }
        return isoOffsets.back().second;
    }

    // 内置通用曲线：ISO 100 以下不补偿，之后每翻一倍增加约 0.00028
    // （归一化满量程），上限 0.004。这是保守估计，避免压掉真实暗部。
    if (safeIso <= 100.0f) {
        return 0.0f;
    }
    const float stops = std::log2(safeIso / 100.0f);
    return std::min(0.004f, 0.00028f * stops);
}

// ---------------------------------------------------------------------------
//  色彩与质感
// ---------------------------------------------------------------------------

bool PresenceSettings::isIdentity() const
{
    // 用极小阈值判断浮点零值，避免序列化往返误差让跳过逻辑失效
    return std::fabs(vibrance) <= 1e-4f
        && std::fabs(saturation) <= 1e-4f
        && std::fabs(clarity) <= 1e-4f
        && std::fabs(texture) <= 1e-4f;
}

// ---------------------------------------------------------------------------
//  污点修复 / 仿制
// ---------------------------------------------------------------------------

const char* retouchModeName(RetouchMode mode)
{
    switch (mode) {
    case RetouchMode::Heal: return "heal";
    case RetouchMode::Clone: return "clone";
    }
    return "heal";
}

RetouchMode retouchModeFromName(const std::string& name)
{
    return (name == "clone") ? RetouchMode::Clone : RetouchMode::Heal;
}

// ---------------------------------------------------------------------------
//  创意白平衡偏移
// ---------------------------------------------------------------------------

bool WhiteBalanceSettings::isIdentity() const
{
    return std::fabs(temperatureOffset) <= 1e-4f
        && std::fabs(tintOffset) <= 1e-4f
        && !useGrayPick;
}

// ---------------------------------------------------------------------------
//  非破坏性裁剪
// ---------------------------------------------------------------------------

bool CropSettings::isFullFrame() const
{
    return x <= 1e-4f && y <= 1e-4f
        && width >= 1.0f - 1e-4f && height >= 1.0f - 1e-4f
        && std::fabs(angle) <= 1e-4f;
}

void CropSettings::mapOutputToSource(float u, float v,
                                     int sourceWidth, int sourceHeight,
                                     float& outU, float& outV) const
{
    outU = u;
    outV = v;
    if (!enable || sourceWidth <= 0 || sourceHeight <= 0) {
        return;
    }

    const float normalizedX = std::clamp(x, 0.0f, 1.0f);
    const float normalizedY = std::clamp(y, 0.0f, 1.0f);
    const float normalizedW = std::clamp(width, 0.0f, 1.0f - normalizedX);
    const float normalizedH = std::clamp(height, 0.0f, 1.0f - normalizedY);
    if (normalizedW <= 1e-4f || normalizedH <= 1e-4f) {
        return;
    }

    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);

    if (std::fabs(angle) <= 1e-4f) {
        outU = normalizedX + clampedU * normalizedW;
        outV = normalizedY + clampedV * normalizedH;
        return;
    }

    // 与 applyCrop 相同的 pixel rect / 自动内接矩形公式
    const int left = std::clamp(
        static_cast<int>(std::floor(normalizedX * sourceWidth)), 0, sourceWidth - 1);
    const int top = std::clamp(
        static_cast<int>(std::floor(normalizedY * sourceHeight)), 0, sourceHeight - 1);
    const int right = std::clamp(
        static_cast<int>(std::ceil((normalizedX + normalizedW) * sourceWidth)),
        left + 1, sourceWidth);
    const int bottom = std::clamp(
        static_cast<int>(std::ceil((normalizedY + normalizedH) * sourceHeight)),
        top + 1, sourceHeight);

    const int cropWidth = right - left;
    const int cropHeight = bottom - top;
    const double theta = std::fabs(angle) * 3.14159265358979323846 / 180.0;
    const double cosTheta = std::cos(theta);
    const double sinTheta = std::sin(theta);
    const double aspect = static_cast<double>(cropHeight) / cropWidth;
    const double scale = std::clamp(
        std::min(1.0 / (cosTheta + aspect * sinTheta),
                 aspect / (sinTheta + aspect * cosTheta)),
        0.0, 1.0);

    const int outputWidth = std::max(
        1, static_cast<int>(std::lround(cropWidth * scale)));
    const int outputHeight = std::max(
        1, static_cast<int>(std::lround(cropHeight * scale)));

    const double direction = (angle >= 0.0f) ? 1.0 : -1.0;
    const double dx = clampedU * outputWidth - outputWidth * 0.5;
    const double dy = clampedV * outputHeight - outputHeight * 0.5;
    const double centerX = (left + right) * 0.5;
    const double centerY = (top + bottom) * 0.5;

    const double sourceX = centerX + dx * cosTheta + direction * dy * sinTheta;
    const double sourceY = centerY - direction * dx * sinTheta + dy * cosTheta;

    outU = std::clamp(static_cast<float>(sourceX / sourceWidth), 0.0f, 1.0f);
    outV = std::clamp(static_cast<float>(sourceY / sourceHeight), 0.0f, 1.0f);
}

float CropSettings::outputShortEdgeRatio(int sourceWidth, int sourceHeight) const
{
    if (!enable || sourceWidth <= 0 || sourceHeight <= 0) {
        return 1.0f;
    }

    const float normalizedX = std::clamp(x, 0.0f, 1.0f);
    const float normalizedY = std::clamp(y, 0.0f, 1.0f);
    const float normalizedW = std::clamp(width, 0.0f, 1.0f - normalizedX);
    const float normalizedH = std::clamp(height, 0.0f, 1.0f - normalizedY);
    if (normalizedW <= 1e-4f || normalizedH <= 1e-4f) {
        return 1.0f;
    }

    double outputWidth = normalizedW * sourceWidth;
    double outputHeight = normalizedH * sourceHeight;

    if (std::fabs(angle) > 1e-4f) {
        const int left = std::clamp(
            static_cast<int>(std::floor(normalizedX * sourceWidth)), 0, sourceWidth - 1);
        const int top = std::clamp(
            static_cast<int>(std::floor(normalizedY * sourceHeight)), 0, sourceHeight - 1);
        const int right = std::clamp(
            static_cast<int>(std::ceil((normalizedX + normalizedW) * sourceWidth)),
            left + 1, sourceWidth);
        const int bottom = std::clamp(
            static_cast<int>(std::ceil((normalizedY + normalizedH) * sourceHeight)),
            top + 1, sourceHeight);

        const double theta = std::fabs(angle) * 3.14159265358979323846 / 180.0;
        const double cosTheta = std::cos(theta);
        const double sinTheta = std::sin(theta);
        const double aspect =
            static_cast<double>(bottom - top) / (right - left);
        const double scale = std::clamp(
            std::min(1.0 / (cosTheta + aspect * sinTheta),
                     aspect / (sinTheta + aspect * cosTheta)),
            0.0, 1.0);
        outputWidth = (right - left) * scale;
        outputHeight = (bottom - top) * scale;
    }

    const double sourceShort = std::min(sourceWidth, sourceHeight);
    const double outputShort = std::min(outputWidth, outputHeight);
    return std::clamp(
        static_cast<float>(outputShort / std::max(1.0, sourceShort)),
        0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
//  黑白混色
// ---------------------------------------------------------------------------

bool BlackWhiteSettings::isIdentity() const
{
    if (!enabled) {
        return true;
    }

    // 启用后即使七通道全零也会转黑白，因此不能只看通道值
    return false;
}

// ---------------------------------------------------------------------------
//  颜色分级与分离色调
// ---------------------------------------------------------------------------

bool ColorGradeWheel::isActive() const
{
    return std::fabs(saturation) > 1e-4f || std::fabs(luminance) > 1e-4f;
}

bool ColorGradingSettings::isIdentity() const
{
    // 平衡 / 混合只是在有实际着色时才参与，单独改动不产生画面变化
    return !shadows.isActive() && !midtones.isActive() && !highlights.isActive();
}

bool SplitToningSettings::isIdentity() const
{
    return std::fabs(highlightsSaturation) <= 1e-4f
        && std::fabs(shadowsSaturation) <= 1e-4f;
}

// ---------------------------------------------------------------------------
//  HSL
// ---------------------------------------------------------------------------

const char* hslBandName(HslBand band)
{
    switch (band) {
    case HslBand::Red: return "红";
    case HslBand::Orange: return "橙";
    case HslBand::Yellow: return "黄";
    case HslBand::Green: return "绿";
    case HslBand::Cyan: return "青";
    case HslBand::Blue: return "蓝";
    case HslBand::Purple: return "紫";
    case HslBand::Count: break;
    }
    return "未知";
}

HslBand hslBandForHue(float hue)
{
    // 归一化到 [0, 360)
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) {
        hue += 360.0f;
    }

    int bestIndex = 0;
    float bestDistance = 360.0f;
    for (int i = 0; i < kHslBandCount; ++i) {
        float distance = std::fabs(hue - kHslBandHueCenters[static_cast<std::size_t>(i)]);
        if (distance > 180.0f) {
            distance = 360.0f - distance;
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return static_cast<HslBand>(bestIndex);
}

bool HslSettings::isIdentity() const
{
    for (const auto& band : bands) {
        // 用极小阈值判断浮点零值，避免序列化往返产生的误差让跳过逻辑失效
        if (std::fabs(band.hue) > 1e-4f
            || std::fabs(band.saturation) > 1e-4f
            || std::fabs(band.luminance) > 1e-4f) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  曲线
// ---------------------------------------------------------------------------

CurveData CurveData::makeIdentity()
{
    CurveData data;
    data.points.push_back(CurvePoint{0.0f, 0.0f});
    data.points.push_back(CurvePoint{1.0f, 1.0f});
    return data;
}

bool CurveData::isIdentity() const
{
    // 仅有两个端点且端点值等于坐标值时，曲线不产生任何变换
    if (points.size() != 2) {
        return false;
    }
    const bool linear = std::fabs(points[0].x - 0.0f) < 1e-4f
                     && std::fabs(points[0].y - 0.0f) < 1e-4f
                     && std::fabs(points[1].x - 1.0f) < 1e-4f
                     && std::fabs(points[1].y - 1.0f) < 1e-4f;
    return linear;
}

CurveSettings::CurveSettings()
{
    red = CurveData::makeIdentity();
    green = CurveData::makeIdentity();
    blue = CurveData::makeIdentity();
    luminance = CurveData::makeIdentity();
}

bool ParametricCurveSettings::isIdentity() const
{
    return std::fabs(shadows) <= 1e-4f
        && std::fabs(midtones) <= 1e-4f
        && std::fabs(highlights) <= 1e-4f;
}

bool CurveSettings::isIdentity() const
{
    return red.isIdentity() && green.isIdentity()
        && blue.isIdentity() && luminance.isIdentity()
        && parametric.isIdentity();
}

// ---------------------------------------------------------------------------
//  彩边 / 色散校正：参数来源掩码
// ---------------------------------------------------------------------------

namespace {

/// 把模块枚举转换为掩码位
constexpr std::uint8_t moduleBit(CorrectionModule module)
{
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(module));
}

}  // namespace

const char* correctionModuleName(CorrectionModule module)
{
    switch (module) {
    case CorrectionModule::Crosstalk: return "串扰补偿";
    case CorrectionModule::HighlightRolloff: return "高光 Roll-off";
    case CorrectionModule::Lca: return "横向色差";
    case CorrectionModule::Loca: return "轴向色散";
    case CorrectionModule::EdgeFalseColor: return "边缘伪色抑制";
    case CorrectionModule::Count: break;
    }
    return "未知";
}

bool FalseColorSettings::isUserOverride(CorrectionModule module) const
{
    return (userOverrideMask & moduleBit(module)) != 0u;
}

bool FalseColorSettings::isLensOverride(CorrectionModule module) const
{
    return (lensOverrideMask & moduleBit(module)) != 0u;
}

void FalseColorSettings::markUserOverride(CorrectionModule module)
{
    userOverrideMask = static_cast<std::uint8_t>(userOverrideMask | moduleBit(module));
}

void FalseColorSettings::markLensOverride(CorrectionModule module)
{
    lensOverrideMask = static_cast<std::uint8_t>(lensOverrideMask | moduleBit(module));
}

void FalseColorSettings::applyCameraDefaults(const FalseColorSettings& cameraDefaults)
{
    // 只填充两个掩码都未置位的模块，保证优先级：
    // 用户实时参数 > 镜头配置 > 机型默认配置 > 全局默认。
    const auto blocked = [this](CorrectionModule module) {
        return isUserOverride(module) || isLensOverride(module);
    };

    if (!blocked(CorrectionModule::Crosstalk)) {
        crosstalk = cameraDefaults.crosstalk;
    }
    if (!blocked(CorrectionModule::HighlightRolloff)) {
        highlightRolloff = cameraDefaults.highlightRolloff;
    }
    if (!blocked(CorrectionModule::Lca)) {
        lca = cameraDefaults.lca;
    }
    if (!blocked(CorrectionModule::Loca)) {
        loca = cameraDefaults.loca;
    }
    if (!blocked(CorrectionModule::EdgeFalseColor)) {
        edgeFalseColor = cameraDefaults.edgeFalseColor;
    }
}

bool FalseColorSettings::applyLensProfile(const LcaSettings& lensLca)
{
    // 用户手动调整过的 LCA 优先于镜头配置，不做覆盖。
    if (isUserOverride(CorrectionModule::Lca)) {
        return false;
    }

    lca = lensLca;
    markLensOverride(CorrectionModule::Lca);
    return true;
}

bool FalseColorSettings::applyLensProfileLoca(const LocaSettings& lensLoca)
{
    // 与 LCA 相同：用户实时参数优先于镜头配置。
    if (isUserOverride(CorrectionModule::Loca)) {
        return false;
    }

    loca = lensLoca;
    markLensOverride(CorrectionModule::Loca);
    return true;
}

void FalseColorSettings::clearOverrides()
{
    userOverrideMask = 0;
    lensOverrideMask = 0;
}

// ---------------------------------------------------------------------------
//  导出格式
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  降噪 / 锐化算法名称（序列化与界面共用）
// ---------------------------------------------------------------------------

const char* noiseReductionMethodName(NoiseReductionMethod method)
{
    switch (method) {
    case NoiseReductionMethod::Wavelet: return "Wavelet";
    case NoiseReductionMethod::NLMeans: return "NLMeans";
    case NoiseReductionMethod::Bm3d: return "BM3D";
    }
    return "Wavelet";
}

NoiseReductionMethod noiseReductionMethodFromString(const std::string& text)
{
    std::string upper;
    upper.reserve(text.size());
    for (char ch : text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (upper == "NLMEANS" || upper == "NLM" || upper == "NONLOCALMEANS") {
        return NoiseReductionMethod::NLMeans;
    }
    if (upper == "BM3D" || upper == "BM3D-LITE" || upper == "DCT") {
        return NoiseReductionMethod::Bm3d;
    }
    return NoiseReductionMethod::Wavelet;
}

const char* sharpenMethodName(SharpenMethod method)
{
    switch (method) {
    case SharpenMethod::UnsharpMask: return "UnsharpMask";
    case SharpenMethod::Deconvolution: return "Deconvolution";
    }
    return "UnsharpMask";
}

SharpenMethod sharpenMethodFromString(const std::string& text)
{
    std::string upper;
    upper.reserve(text.size());
    for (char ch : text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (upper == "DECONVOLUTION" || upper == "RL" || upper == "RICHARDSONLUCY") {
        return SharpenMethod::Deconvolution;
    }
    return SharpenMethod::UnsharpMask;
}

const char* exportFormatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Tiff16: return "16 位 TIFF";
    case ExportFormat::Jpeg8: return "8 位 JPEG";
    case ExportFormat::WebP: return "WebP";
    case ExportFormat::Exr16: return "16 位 EXR";
    case ExportFormat::Png16: return "16 位 PNG";
    case ExportFormat::JpegXl: return "JPEG XL";
    case ExportFormat::Avif: return "AVIF";
    }
    return "未知";
}

const char* resampleFilterName(ResampleFilter filter)
{
    switch (filter) {
    case ResampleFilter::Bilinear: return "Bilinear";
    case ResampleFilter::Bicubic: return "Bicubic";
    case ResampleFilter::Lanczos: return "Lanczos";
    case ResampleFilter::Mitchell: return "Mitchell";
    }
    return "Bilinear";
}

ResampleFilter resampleFilterFromString(const std::string& text)
{
    std::string upper;
    upper.reserve(text.size());
    for (char ch : text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (upper == "BICUBIC" || upper == "CATMULLROM") return ResampleFilter::Bicubic;
    if (upper == "LANCZOS" || upper == "LANCZOS3") return ResampleFilter::Lanczos;
    if (upper == "MITCHELL") return ResampleFilter::Mitchell;
    return ResampleFilter::Bilinear;
}

const char* watermarkPositionName(WatermarkPosition position)
{
    switch (position) {
    case WatermarkPosition::TopLeft: return "TopLeft";
    case WatermarkPosition::TopRight: return "TopRight";
    case WatermarkPosition::BottomLeft: return "BottomLeft";
    case WatermarkPosition::BottomRight: return "BottomRight";
    case WatermarkPosition::Center: return "Center";
    }
    return "BottomRight";
}

WatermarkPosition watermarkPositionFromString(const std::string& text)
{
    if (text == "TopLeft") return WatermarkPosition::TopLeft;
    if (text == "TopRight") return WatermarkPosition::TopRight;
    if (text == "BottomLeft") return WatermarkPosition::BottomLeft;
    if (text == "Center") return WatermarkPosition::Center;
    return WatermarkPosition::BottomRight;
}

// ---------------------------------------------------------------------------
//  EditContext
// ---------------------------------------------------------------------------

void EditContext::resolveLegacyWhiteBalance(bool isRgbSource,
                                            float asShotTemperature,
                                            float asShotTint)
{
    if (!baseWhiteBalance.presentInProject) {
        if (isRgbSource) {
            // 普通位图像的像素已完成白平衡，不需要基准校正
            baseWhiteBalance = BaseWhiteBalanceSettings{};
            baseWhiteBalance.fromAsShot = false;
        } else {
            baseWhiteBalance.enable = true;
            baseWhiteBalance.locked = false;
            baseWhiteBalance.temperature =
                (asShotTemperature > 0.0f) ? asShotTemperature : 5500.0f;
            baseWhiteBalance.tint =
                (asShotTemperature > 0.0f) ? asShotTint : 0.0f;
            baseWhiteBalance.fromAsShot = (asShotTemperature > 0.0f);
        }
        // 迁移完成后视为"已显式确定"，后续快照 / 保存不会再被 as-shot 覆盖
        baseWhiteBalance.presentInProject = true;
    }

    if (!whiteBalance.legacyAbsolute) {
        return;
    }

    const float referenceTemperature = isRgbSource
        ? 5000.0f
        : (baseWhiteBalance.enable ? baseWhiteBalance.temperature : 0.0f);
    const float referenceTint = isRgbSource
        ? 0.0f
        : (baseWhiteBalance.enable ? baseWhiteBalance.tint : 0.0f);

    whiteBalance.temperatureOffset = std::clamp(
        whiteBalance.legacyTemperature - referenceTemperature, -4000.0f, 4000.0f);
    whiteBalance.tintOffset = std::clamp(
        whiteBalance.legacyTint - referenceTint, -100.0f, 100.0f);
    whiteBalance.legacyAbsolute = false;
}

void EditContext::resetToDefaults()
{
    // 保留源路径，其余全部回到默认值。
    // 用赋值一个默认构造的临时对象再搬回路径的方式，避免逐个字段手写易漏。
    const std::string keepPath = sourceRawPath;
    *this = EditContext{};
    sourceRawPath = keepPath;
}

bool EditContext::isDefault() const
{
    // 逐一比对是否与默认构造的实例一致。
    // 注意 sourceRawPath 与 ccm.initializedFromCamera 属于"记录状态"而非
    // "调整参数"，因此不参与判断。
    const EditContext defaults;

    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };

    // RAW 设置
    if (!near(raw.blackLevelOffset, defaults.raw.blackLevelOffset)) return false;
    if (!near(raw.whiteLevelOverride, defaults.raw.whiteLevelOverride)) return false;
    if (raw.forceBayerPattern != defaults.raw.forceBayerPattern) return false;
    if (raw.disableCameraMatrix != defaults.raw.disableCameraMatrix) return false;
    if (raw.demosaicAlgorithm != defaults.raw.demosaicAlgorithm) return false;
    if (!near(raw.demosaicSmooth, defaults.raw.demosaicSmooth)) return false;
    if (raw.enableFbdd != defaults.raw.enableFbdd) return false;
    if (!near(raw.fbddStrength, defaults.raw.fbddStrength)) return false;
    if (raw.cropLeft != 0 || raw.cropTop != 0 || raw.cropRight != 0 || raw.cropBottom != 0) return false;

    // CCM
    if (ccm.enable) return false;

    // 镜头
    if (lens.enable) return false;

    // 彩边 / 色散校正：与默认参数比较（默认模块是开启的，
    // 因此不能用“是否启用”判断，必须逐项比较数值）
    const auto& fc = falseColor;
    const auto& fcDefault = defaults.falseColor;
    const auto matrixNear = [&near](const Matrix3x3& a, const Matrix3x3& b) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (!near(a[r][c], b[r][c])) {
                    return false;
                }
            }
        }
        return true;
    };

    if (fc.crosstalk.enable != fcDefault.crosstalk.enable) return false;
    if (!near(fc.crosstalk.strength, fcDefault.crosstalk.strength)) return false;
    if (!matrixNear(fc.crosstalk.decouplingMatrix, fcDefault.crosstalk.decouplingMatrix)) return false;
    if (fc.highlightRolloff.enable != fcDefault.highlightRolloff.enable) return false;
    if (!near(fc.highlightRolloff.knee, fcDefault.highlightRolloff.knee)) return false;
    if (!near(fc.highlightRolloff.slope, fcDefault.highlightRolloff.slope)) return false;
    if (fc.lca.enable != fcDefault.lca.enable) return false;
    if (!near(fc.lca.redScale, fcDefault.lca.redScale)) return false;
    if (!near(fc.lca.blueScale, fcDefault.lca.blueScale)) return false;
    if (!near(fc.lca.radialStrength, fcDefault.lca.radialStrength)) return false;
    if (fc.loca.enable != fcDefault.loca.enable) return false;
    if (!near(fc.loca.strength, fcDefault.loca.strength)) return false;
    if (!near(fc.loca.highlightThreshold, fcDefault.loca.highlightThreshold)) return false;
    if (fc.edgeFalseColor.enable != fcDefault.edgeFalseColor.enable) return false;
    if (!near(fc.edgeFalseColor.strength, fcDefault.edgeFalseColor.strength)) return false;
    if (!near(fc.edgeFalseColor.edgeSensitivity,
              fcDefault.edgeFalseColor.edgeSensitivity)) {
        return false;
    }
    if (fc.edgeFalseColor.smoothingRadius != fcDefault.edgeFalseColor.smoothingRadius) {
        return false;
    }

    // 暗电流补偿
    if (darkCurrent.enable != defaults.darkCurrent.enable) return false;
    if (!near(darkCurrent.strength, defaults.darkCurrent.strength)) return false;
    if (darkCurrent.isoOffsets != defaults.darkCurrent.isoOffsets) return false;

    // 暗场帧 / 平场帧
    if (frameCalibration.enableDarkFrame != defaults.frameCalibration.enableDarkFrame) {
        return false;
    }
    if (frameCalibration.darkFramePath != defaults.frameCalibration.darkFramePath) {
        return false;
    }
    if (!near(frameCalibration.darkFrameStrength,
              defaults.frameCalibration.darkFrameStrength)) {
        return false;
    }
    if (frameCalibration.enableFlatFrame != defaults.frameCalibration.enableFlatFrame) {
        return false;
    }
    if (frameCalibration.flatFramePath != defaults.frameCalibration.flatFramePath) {
        return false;
    }
    if (!near(frameCalibration.flatFrameStrength,
              defaults.frameCalibration.flatFrameStrength)) {
        return false;
    }

    // 创意白平衡偏移（基准白平衡属于 RAW 标定，不计入"是否调整过"）
    if (!near(whiteBalance.temperatureOffset,
              defaults.whiteBalance.temperatureOffset)) {
        return false;
    }
    if (!near(whiteBalance.tintOffset, defaults.whiteBalance.tintOffset)) return false;
    if (whiteBalance.useGrayPick) return false;

    // 光影
    if (!near(light.exposure, 0.0f) || !near(light.highlight, 0.0f)
        || !near(light.shadow, 0.0f) || !near(light.contrast, 0.0f)
        || !near(light.dehaze, 0.0f)) {
        return false;
    }

    // 非破坏性裁剪 / 透视 / 修复（默认关闭 / 恒等）
    if (crop.isActive()) return false;
    if (!perspective.isIdentity()) return false;
    if (!retouch.isIdentity()) return false;

    // 色彩与质感（默认全为 0）
    if (!presence.isIdentity()) return false;

    // HSL、黑白混色、颜色分级与曲线
    if (!hsl.isIdentity()) return false;
    if (!blackWhite.isIdentity()) return false;
    if (!colorGrading.isIdentity()) return false;
    if (!splitToning.isIdentity()) return false;
    if (!curve.isIdentity()) return false;

    // 细节：与默认值比较，而不是与 0 比较。
    // 默认方案里带有轻度降噪与锐化，用 0 比较会让所有文件一打开就被判为"已调整"。
    if (!near(detail.noiseLuma, defaults.detail.noiseLuma)) return false;
    if (!near(detail.noiseChroma, defaults.detail.noiseChroma)) return false;
    if (!near(detail.sharpenAmount, defaults.detail.sharpenAmount)) return false;
    if (!near(detail.sharpenRadius, defaults.detail.sharpenRadius)) return false;
    if (!near(detail.sharpenMasking, defaults.detail.sharpenMasking)) return false;
    if (detail.noiseMethod != defaults.detail.noiseMethod) return false;
    if (detail.nlMeansSearchRadius != defaults.detail.nlMeansSearchRadius) return false;
    if (detail.sharpenMethod != defaults.detail.sharpenMethod) return false;
    if (detail.deconvolutionIterations != defaults.detail.deconvolutionIterations) {
        return false;
    }
    if (detail.outputSharpenEnable
        || !near(detail.outputSharpenAmount, defaults.detail.outputSharpenAmount)
        || !near(detail.outputSharpenRadius, defaults.detail.outputSharpenRadius)) {
        return false;
    }

    // DCP 配置属于 RAW 标定层：启用即视为做过调整
    if (dcp.isActive()) return false;

    // 自定义管线顺序
    if (!pipelineOrder.isIdentity()) return false;

    // LUT（MVP 不生效，但用户可能勾选过）
    if (lut.enable) return false;

    // 软打样属于显示设置；只要不是完全默认状态就认为做过调整，
    // 避免界面上明明开着软打样却提示“未做任何调整”。
    if (softProof.enabled || softProof.simulatePaperWhite
        || !softProof.profilePath.empty()) {
        return false;
    }

    return true;
}

}  // namespace rawcore
