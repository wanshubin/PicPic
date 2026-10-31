// ============================================================================
//  DcpApply.cpp —— DCP 完整色彩链的渲染实现（D1）
// ============================================================================

#include "DcpApply.h"

#include "LinearMath.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rawcore {
namespace ops {

namespace {

/// 按 mired 线性插值两组标定矩阵；拍摄色温缺失或端点非法时退回 b。
Matrix3x3 interpolateByTemperature(const Matrix3x3& a, float tempA,
                                   const Matrix3x3& b, float tempB,
                                   float asShotTemperature)
{
    if (asShotTemperature <= 0.0f || tempA <= 0.0f || tempB <= 0.0f
        || std::fabs(tempA - tempB) < 1e-3f) {
        return b;
    }

    const float miredA = 1.0e6f / tempA;
    const float miredB = 1.0e6f / tempB;
    const float miredShot = 1.0e6f / asShotTemperature;
    const float span = miredB - miredA;
    if (std::fabs(span) < 1e-3f) {
        return b;
    }

    const float t = math::clamp((miredShot - miredA) / span, 0.0f, 1.0f);
    Matrix3x3 result = a;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            result[row][col] = a[row][col] + t * (b[row][col] - a[row][col]);
        }
    }
    return result;
}

/// DCP 的光源色温；未声明时按标准 A / D65 处理（与加载时的提示一致）
float illuminantTemperature(const DcpProfile& profile, bool second)
{
    const float declared = second ? profile.illuminant2Temperature
                                  : profile.illuminant1Temperature;
    if (declared > 0.0f) {
        return declared;
    }
    return second ? 6504.0f : 2856.0f;
}

/// 线性 RGB → HSV。V 允许大于 1（HDR），H 归一化到 [0,1)。
void rgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    const float maxValue = std::max({r, g, b});
    const float minValue = std::min({r, g, b});
    v = maxValue;

    const float delta = maxValue - minValue;
    if (maxValue <= 1e-8f || delta <= 1e-8f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }

    s = delta / maxValue;

    if (maxValue == r) {
        h = (g - b) / delta;
    } else if (maxValue == g) {
        h = 2.0f + (b - r) / delta;
    } else {
        h = 4.0f + (r - g) / delta;
    }
    h /= 6.0f;
    if (h < 0.0f) {
        h += 1.0f;
    }
    if (h >= 1.0f) {
        h -= 1.0f;
    }
}

/// HSV → 线性 RGB（S / V 可以超出 [0,1]，以保留 HDR 高光）
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    h = h - std::floor(h);
    s = std::max(0.0f, s);

    const float sector = h * 6.0f;
    const int index = static_cast<int>(std::floor(sector)) % 6;
    const float f = sector - std::floor(sector);

    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (index) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

/// sRGB 编码（线性 → 编码）。对 >1 的 HDR 值沿幂函数段外推，
/// 对 <=0 的值返回 0，保证查表索引始终有限。
float srgbEncode(float value)
{
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

/// sRGB 解码（编码 → 线性），与 srgbEncode 互逆。
float srgbDecode(float value)
{
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

/// 读取 Hue/Sat 表节点（三线性插值的单个角点）
///
/// DNG 规范中表的排列是 **[value][hue][saturation]**：
/// saturation 变化最快，value 最慢。若按 [hue][sat][val] 取节点，
/// 会读到完全不相关的表项，表现为整体过饱和、偏色。
void nodeAt(const DcpHueSatMap& map, int hueIndex, int satIndex, int valIndex,
            float& hueDelta, float& satScale, float& valScale)
{
    const int hue = ((hueIndex % map.hueDivisions) + map.hueDivisions) % map.hueDivisions;
    const std::size_t index =
        ((static_cast<std::size_t>(valIndex) * static_cast<std::size_t>(map.hueDivisions)
          + static_cast<std::size_t>(hue))
             * static_cast<std::size_t>(map.satDivisions)
         + static_cast<std::size_t>(satIndex))
        * 3u;

    if (index + 2 >= map.data.size()) {
        hueDelta = 0.0f;
        satScale = 1.0f;
        valScale = 1.0f;
        return;
    }

    hueDelta = map.data[index];
    satScale = map.data[index + 1];
    valScale = map.data[index + 2];
}

/// 对单个像素做 Hue/Sat/Look 三线性查找
///
/// @param lookTablePath true 表示这是 LookTable（step2）阶段。DNG / RawTherapee
///        在该阶段会先把负的 ProPhoto 通道钳制为 0 再转 HSV：广色域像素
///        （例如深蓝天空）经 ForwardMatrix 后可能出现负通道，若不钳制，
///        饱和度会被虚增、色相也会偏移。LookTable 之后还会把 S 钳制到
///        [0,1]，避免过饱和像素经 HSV→RGB 产生新的负通道。
void applyMapToPixel(const DcpHueSatMap& map, float strength, bool lookTablePath,
                     float& r, float& g, float& b)
{
    if (lookTablePath) {
        // 与 RawTherapee 的 FCLIP（下界 0）一致：表只定义在非负 RGB 上
        r = std::max(0.0f, r);
        g = std::max(0.0f, g);
        b = std::max(0.0f, b);
    }

    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    rgbToHsv(r, g, b, h, s, v);

    // 饱和度与明度方向按节点数插值；色相方向循环插值。
    const float huePosition = h * static_cast<float>(map.hueDivisions);
    const int hue0 = static_cast<int>(std::floor(huePosition));
    const int hue1 = hue0 + 1;
    const float hueFrac = huePosition - static_cast<float>(hue0);

    int sat0 = 0;
    int sat1 = 0;
    float satFrac = 0.0f;
    if (map.satDivisions > 1) {
        const float satPosition =
            math::clamp(s, 0.0f, 1.0f) * static_cast<float>(map.satDivisions - 1);
        sat0 = static_cast<int>(std::floor(satPosition));
        sat1 = std::min(sat0 + 1, map.satDivisions - 1);
        satFrac = satPosition - static_cast<float>(sat0);
    }

    // DCP 的明度（V）轴可以按 sRGB 编码建立索引（ProfileHueSatMapEncoding /
    // ProfileLookTableEncoding = 1）。此时用编码后的 V 查表，
    // 查到的明度缩放也作用在编码值上，最后再解码回线性 V。
    // 这一步不能省略：表是按编码坐标分布的，直接拿线性 V 索引会让
    // 暗部与中间调匹配到错误的节点，画面明显的偏色 / 反差异常。
    const bool encodedValue = map.srgbEncoding && map.valDivisions > 1;
    const float lookupValue = encodedValue ? srgbEncode(v) : v;

    int val0 = 0;
    int val1 = 0;
    float valFrac = 0.0f;
    if (map.valDivisions > 1) {
        const float valPosition =
            math::clamp(lookupValue, 0.0f, 1.0f) * static_cast<float>(map.valDivisions - 1);
        val0 = static_cast<int>(std::floor(valPosition));
        val1 = std::min(val0 + 1, map.valDivisions - 1);
        valFrac = valPosition - static_cast<float>(val0);
    }

    float hueDelta = 0.0f;
    float satScale = 0.0f;
    float valScale = 0.0f;

    const int hueIndices[2] = {hue0, hue1};
    const int satIndices[2] = {sat0, sat1};
    const int valIndices[2] = {val0, val1};

    for (int hi = 0; hi < 2; ++hi) {
        const float hueWeight = (hi == 0) ? (1.0f - hueFrac) : hueFrac;
        if (hueWeight <= 1e-6f) {
            continue;
        }
        for (int si = 0; si < 2; ++si) {
            const float satWeight = (si == 0) ? (1.0f - satFrac) : satFrac;
            if (satWeight <= 1e-6f) {
                continue;
            }
            for (int vi = 0; vi < 2; ++vi) {
                const float valWeight = (vi == 0) ? (1.0f - valFrac) : valFrac;
                if (valWeight <= 1e-6f) {
                    continue;
                }
                float nodeHue = 0.0f;
                float nodeSat = 0.0f;
                float nodeVal = 0.0f;
                nodeAt(map, hueIndices[hi], satIndices[si], valIndices[vi],
                       nodeHue, nodeSat, nodeVal);

                const float weight = hueWeight * satWeight * valWeight;
                hueDelta += nodeHue * weight;
                satScale += nodeSat * weight;
                valScale += nodeVal * weight;
            }
        }
    }

    // 表中的“恒等”节点（色相 0、饱和度 / 明度倍率 1）不应引入任何误差：
    // RGB → HSV → RGB 往返即使数值相同也会有末位差，跳过可保证未修改像素逐位不变。
    if (std::fabs(hueDelta) < 1e-7f && std::fabs(satScale - 1.0f) < 1e-6f
        && std::fabs(valScale - 1.0f) < 1e-6f) {
        return;
    }

    // 中性像素（s≈0）的色相无意义，不做色相偏移，避免灰面被染色
    if (s > 1e-5f) {
        h += hueDelta * strength;
    }
    // LookTable 阶段与 RawTherapee 的 CLIP01 一致，饱和度不超过 1；
    // Hue/Sat 表阶段保留宽松上限，允许厂商表主动提高饱和度。
    const float saturationLimit = lookTablePath ? 1.0f : 4.0f;
    const float newS = std::clamp(s * math::lerp(1.0f, satScale, strength),
                                  0.0f, saturationLimit);
    const float valueScale = math::lerp(1.0f, valScale, strength);
    const float newV = encodedValue
        ? srgbDecode(lookupValue * valueScale)
        : std::max(0.0f, v * valueScale);

    float nr = r;
    float ng = g;
    float nb = b;
    hsvToRgb(h, newS, newV, nr, ng, nb);

    r = math::lerp(r, nr, strength);
    g = math::lerp(g, ng, strength);
    b = math::lerp(b, nb, strength);
}

/// 把 DCP 色调曲线编译为 LUT（分段线性）
std::vector<float> compileDcpCurve(const DcpToneCurve& curve, int size)
{
    std::vector<float> lut;
    if (curve.isEmpty()) {
        return lut;
    }

    const std::size_t count = curve.coordinates.size() / 2u;
    std::vector<float> xs(count);
    std::vector<float> ys(count);
    for (std::size_t i = 0; i < count; ++i) {
        xs[i] = curve.coordinates[i * 2u];
        ys[i] = curve.coordinates[i * 2u + 1u];
    }

    // 规范要求 x 升序；遇到未排序的文件按索引排序，避免查表结果混乱
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&xs](std::size_t a, std::size_t b) { return xs[a] < xs[b]; });

    lut.resize(static_cast<std::size_t>(size) + 1u);
    std::size_t segment = 0;
    for (int i = 0; i <= size; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(size);
        while (segment + 1 < count && xs[order[segment + 1]] < x) {
            ++segment;
        }
        if (segment + 1 >= count) {
            lut[static_cast<std::size_t>(i)] = ys[order[count - 1]];
            continue;
        }
        const float x0 = xs[order[segment]];
        const float x1 = xs[order[segment + 1]];
        const float y0 = ys[order[segment]];
        const float y1 = ys[order[segment + 1]];
        const float t = (x1 - x0 > 1e-9f)
            ? math::clamp((x - x0) / (x1 - x0), 0.0f, 1.0f)
            : 0.0f;
        lut[static_cast<std::size_t>(i)] = math::lerp(y0, y1, t);
    }
    return lut;
}

}  // namespace

// ---------------------------------------------------------------------------
//  矩阵插值
// ---------------------------------------------------------------------------

bool dcpInterpolatedMatrix(const DcpProfile& profile,
                           bool forward,
                           float asShotTemperature,
                           Matrix3x3& output)
{
    if (forward) {
        if (!profile.hasForwardMatrix1 && !profile.hasForwardMatrix2) {
            return false;
        }
        const Matrix3x3& first = profile.hasForwardMatrix1
            ? profile.forwardMatrix1 : profile.forwardMatrix2;
        const Matrix3x3& second = profile.hasForwardMatrix2
            ? profile.forwardMatrix2 : profile.forwardMatrix1;
        const float t1 = profile.hasForwardMatrix1
            ? illuminantTemperature(profile, false) : illuminantTemperature(profile, true);
        const float t2 = profile.hasForwardMatrix2
            ? illuminantTemperature(profile, true) : t1;
        output = interpolateByTemperature(first, t1, second, t2, asShotTemperature);
        return true;
    }

    // ColorMatrix 的语义是 XYZ → 相机。DNG 的插值对象是这两个矩阵本身，
    // 混合后再求逆；先分别求逆再插值会在两个光源之间产生细微偏差。
    Matrix3x3 blended{};
    if (profile.hasColorMatrix1 || profile.hasColorMatrix2) {
        const Matrix3x3& first = profile.hasColorMatrix1
            ? profile.colorMatrix1 : profile.colorMatrix2;
        const Matrix3x3& second = profile.hasColorMatrix2
            ? profile.colorMatrix2 : profile.colorMatrix1;
        const float t1 = profile.hasColorMatrix1
            ? illuminantTemperature(profile, false) : illuminantTemperature(profile, true);
        const float t2 = profile.hasColorMatrix2
            ? illuminantTemperature(profile, true) : t1;
        blended = interpolateByTemperature(first, t1, second, t2, asShotTemperature);
    } else {
        return false;
    }

    if (!math::invert(blended, output)) {
        return false;
    }
    return true;
}

bool dcpInterpolatedHueSatMap(const DcpProfile& profile,
                              float asShotTemperature,
                              DcpHueSatMap& output)
{
    const DcpHueSatMap& first = profile.hueSatMap1;
    const DcpHueSatMap& second = profile.hueSatMap2;

    if (first.isEmpty() && second.isEmpty()) {
        return false;
    }
    if (first.isEmpty()) {
        output = second;
        return true;
    }
    if (second.isEmpty()) {
        output = first;
        return true;
    }

    const float t1 = illuminantTemperature(profile, false);
    const float t2 = illuminantTemperature(profile, true);

    // 维度一致时逐元素插值；不一致时无法混合，取更接近拍摄色温的一组。
    if (first.hueDivisions != second.hueDivisions
        || first.satDivisions != second.satDivisions
        || first.valDivisions != second.valDivisions
        || first.data.size() != second.data.size()) {
        float t = 0.5f;
        if (asShotTemperature > 0.0f && std::fabs(t1 - t2) > 1e-3f) {
            const float mired1 = 1.0e6f / t1;
            const float mired2 = 1.0e6f / t2;
            const float miredShot = 1.0e6f / asShotTemperature;
            t = math::clamp((miredShot - mired1) / (mired2 - mired1), 0.0f, 1.0f);
        }
        output = (t < 0.5f) ? first : second;
        return true;
    }

    float t = 1.0f;
    if (asShotTemperature > 0.0f && std::fabs(t1 - t2) > 1e-3f) {
        const float mired1 = 1.0e6f / t1;
        const float mired2 = 1.0e6f / t2;
        const float miredShot = 1.0e6f / asShotTemperature;
        t = math::clamp((miredShot - mired1) / (mired2 - mired1), 0.0f, 1.0f);
    }

    output = first;
    for (std::size_t i = 0; i < output.data.size(); ++i) {
        output.data[i] = math::lerp(first.data[i], second.data[i], t);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Hue / Sat 表
// ---------------------------------------------------------------------------

void applyDcpHueSatMap(ImageBuffer& image, const DcpHueSatMap& map, float strength,
                       bool lookTablePath)
{
    if (image.isEmpty() || map.isEmpty() || strength <= 1e-4f) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < static_cast<int>(pixels); ++i) {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        float r = pixel[0];
        float g = pixel[1];
        float b = pixel[2];
        applyMapToPixel(map, amount, lookTablePath, r, g, b);
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
    }
}

// ---------------------------------------------------------------------------
//  色调曲线
// ---------------------------------------------------------------------------

void applyDcpToneCurve(ImageBuffer& image, const DcpToneCurve& curve)
{
    if (image.isEmpty() || curve.isEmpty()) {
        return;
    }

    constexpr int kLutSize = 4096;
    const std::vector<float> lut = compileDcpCurve(curve, kLutSize);
    if (lut.empty()) {
        return;
    }

    // 高光端点斜率：大于 1 的值按该斜率线性外推，而不是直接钳制。
    // 工作空间在基准白平衡之后可能仍有 >1 的 HDR 数据，直接截断会让
    // 高光层次在 DCP 阶段提前丢失。
    const float endSlope =
        (lut[static_cast<std::size_t>(kLutSize)]
         - lut[static_cast<std::size_t>(kLutSize - 1)]) * static_cast<float>(kLutSize);
    const float endValue = lut[static_cast<std::size_t>(kLutSize)];

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < static_cast<int>(pixels); ++i) {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        for (int c = 0; c < 3; ++c) {
            const float value = pixel[c];
            if (value > 1.0f) {
                pixel[c] = std::max(0.0f, endValue + (value - 1.0f) * endSlope);
                continue;
            }
            const float x = math::clamp(value, 0.0f, 1.0f);
            const float position = x * static_cast<float>(kLutSize);
            const int index = std::min(static_cast<int>(position), kLutSize - 1);
            const float fraction = position - static_cast<float>(index);
            pixel[c] = math::lerp(lut[static_cast<std::size_t>(index)],
                                  lut[static_cast<std::size_t>(index + 1)],
                                  fraction);
        }
    }
}

}  // namespace ops
}  // namespace rawcore
