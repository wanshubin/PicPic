// ============================================================================
//  MaskModel.cpp —— 蒙版数据模型与几何求值
// ============================================================================

#include "MaskModel.h"

#include "LinearMath.h"

#include <algorithm>
#include <cmath>

#ifdef RAWCore_HAS_OPENMP
#  include <omp.h>
#endif

// 与 PipelineOps 保持相同的并行策略；关闭 OpenMP 时退化为串行循环
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  define RAWCORE_MASK_PARALLEL_FOR(INDEX, COUNT) \
     _Pragma("omp parallel for schedule(static)") for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#else
#  define RAWCORE_MASK_PARALLEL_FOR(INDEX, COUNT) \
     for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#endif

namespace rawcore {

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float clamp01(float value)
{
    return (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/// 平滑阶跃：edge0 → edge1 之间从 0 平滑过渡到 1
inline float smoothStep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) {
        return (x < edge0) ? 0.0f : 1.0f;
    }
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

const char* maskShapeName(MaskShape shape)
{
    switch (shape) {
        case MaskShape::Radial: return "radial";
        case MaskShape::Linear: return "linear";
        case MaskShape::Brush: return "brush";
        case MaskShape::Range: return "range";
        case MaskShape::Luminance: return "luminance";
        case MaskShape::ColorRange: return "colorRange";
    }
    return "radial";
}

MaskShape maskShapeFromName(const std::string& name)
{
    if (name == "linear") {
        return MaskShape::Linear;
    }
    if (name == "brush") {
        return MaskShape::Brush;
    }
    if (name == "range") {
        return MaskShape::Range;
    }
    if (name == "luminance") {
        return MaskShape::Luminance;
    }
    if (name == "colorRange" || name == "color_range" || name == "color") {
        return MaskShape::ColorRange;
    }
    return MaskShape::Radial;
}

const char* luminanceMaskModeName(LuminanceMaskMode mode)
{
    return mode == LuminanceMaskMode::ProPhotoY ? "prophotoY" : "bt709";
}

LuminanceMaskMode luminanceMaskModeFromName(const std::string& name)
{
    return name == "prophotoY" ? LuminanceMaskMode::ProPhotoY
                               : LuminanceMaskMode::Bt709;
}

float LuminanceMaskSettings::weightAtLinear(float linearLuma) const
{
    const float lower = std::min(low, high);
    const float upper = std::max(low, high);
    const float soft = std::clamp(softness, 0.001f, 0.5f);
    const float x = std::max(0.0f, linearLuma);

    if (x >= lower && x <= upper) {
        return 1.0f;
    }
    if (x < lower) {
        // 从下界外侧向区间过渡：低于 lower-soft 为 0，到 lower 为 1
        return smoothStep(lower - soft, lower, x);
    }
    return 1.0f - smoothStep(upper, upper + soft, x);
}

float ColorRangeMaskSettings::weightAtLab(float l, float a, float b) const
{
    if (targets.empty()) {
        return 1.0f;
    }

    float hue = 0.0f;
    const float chroma = std::sqrt(a * a + b * b);
    if (chroma > 1e-6f) {
        hue = std::atan2(b, a) * 180.0f / kPi;
        if (hue < 0.0f) {
            hue += 360.0f;
        }
    }

    const float hueTol = std::max(0.5f, hueTolerance);
    const float satTol = std::max(0.5f, satTolerance);
    const float soft = std::clamp(softness, 0.0f, 1.0f);

    float best = 0.0f;
    for (const LabColorPoint& target : targets) {
        float targetHue = 0.0f;
        const float targetChroma = std::sqrt(target.a * target.a
                                             + target.b * target.b);
        if (targetChroma > 1e-6f) {
            targetHue = std::atan2(target.b, target.a) * 180.0f / kPi;
            if (targetHue < 0.0f) {
                targetHue += 360.0f;
            }
        }

        float hueDistance = std::fabs(hue - targetHue);
        if (hueDistance > 180.0f) {
            hueDistance = 360.0f - hueDistance;
        }
        const float satDistance = std::fabs(chroma - targetChroma);

        // 容差以内为 1；超出后按 softness 决定的宽度衰减到 0
        const float hueOuter = hueTol + soft * std::max(hueTol, 1.0f);
        const float satOuter = satTol + soft * std::max(satTol, 1.0f);
        const float hueWeight = 1.0f - smoothStep(hueTol, hueOuter, hueDistance);
        const float satWeight = 1.0f - smoothStep(satTol, satOuter, satDistance);
        float weight = hueWeight * satWeight;

        if (lightnessEnabled) {
            const float lower = std::min(lightnessLow, lightnessHigh);
            const float upper = std::max(lightnessLow, lightnessHigh);
            const float normalized = std::clamp(l / 100.0f, 0.0f, 1.0f);
            const float rangeSoft = std::clamp(softnessRange, 0.001f, 0.5f);
            float lightnessWeight = 1.0f;
            if (normalized < lower) {
                lightnessWeight =
                    smoothStep(lower - rangeSoft, lower, normalized);
            } else if (normalized > upper) {
                lightnessWeight =
                    1.0f - smoothStep(upper, upper + rangeSoft, normalized);
            }
            weight *= lightnessWeight;
        }

        best = std::max(best, weight);
    }
    return best;
}

bool RangeMaskSettings::isActive() const
{
    return enableLuminance || enableColor;
}

float RangeMaskSettings::weightAt(float luma, float hue, float saturation) const
{
    if (!isActive()) {
        return 1.0f;
    }

    float weight = 1.0f;

    if (enableLuminance) {
        const float low = std::min(luminanceMin, luminanceMax);
        const float high = std::max(luminanceMin, luminanceMax);
        const float feather = std::clamp(luminanceFeather, 0.001f, 0.5f);

        const float lowWeight = smoothStep(low - feather, low, clamp01(luma));
        const float highWeight =
            1.0f - smoothStep(high, high + feather, clamp01(luma));
        weight *= lowWeight * highWeight;
    }

    if (enableColor) {
        const float tolerance = std::clamp(colorTolerance, 0.0f, 180.0f);
        float hueDistance = std::fabs(hue - colorHue);
        if (hueDistance > 180.0f) {
            hueDistance = 360.0f - hueDistance;
        }

        const float hueWeight = tolerance > 1e-3f
            ? 1.0f - smoothStep(tolerance * 0.5f, tolerance, hueDistance)
            : (hueDistance < 1.0f ? 1.0f : 0.0f);

        const float saturationFloor =
            std::clamp(colorSaturationMin, 0.0f, 1.0f);
        const float saturationWeight =
            smoothStep(saturationFloor, saturationFloor + 0.10f,
                       clamp01(saturation));

        weight *= hueWeight * saturationWeight;
    }

    return clamp01(weight);
}

bool LocalAdjustments::isIdentity() const
{
    return std::fabs(exposure) <= 1e-4f
        && std::fabs(contrast) <= 1e-4f
        && std::fabs(highlights) <= 1e-4f
        && std::fabs(shadows) <= 1e-4f
        && std::fabs(temperature) <= 1e-3f
        && std::fabs(tint) <= 1e-4f
        && std::fabs(saturation) <= 1e-4f;
}

namespace {

/// 把指定范围的笔画栅格化进 out（与 buildShapeWeightMap 的画笔语义一致）。
/// @param startIndex 从第几个笔画点开始；之前的点假定已经写入 out。
void rasterizeBrushStrokes(const MaskLayer& layer, int width, int height,
                           std::size_t startIndex, std::vector<float>& out)
{
    if (width <= 0 || height <= 0
        || out.size() < static_cast<std::size_t>(width) * height) {
        return;
    }

    const float featherWidth = clamp01(layer.feather);
    const float shortEdge = static_cast<float>(std::min(width, height));

    for (std::size_t i = startIndex; i < layer.strokes.size(); ++i) {
        const MaskStrokePoint& point = layer.strokes[i];
        const float centerPx = point.x * static_cast<float>(width);
        const float centerPy = point.y * static_cast<float>(height);
        const float radiusPx = std::max(1.0f, point.radius * shortEdge);
        const float flow = clamp01(point.flow);

        const int x0 = std::max(0, static_cast<int>(std::floor(centerPx - radiusPx)));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(centerPx + radiusPx)));
        const int y0 = std::max(0, static_cast<int>(std::floor(centerPy - radiusPx)));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(centerPy + radiusPx)));

        for (int y = y0; y <= y1; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) - centerPy;
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - centerPx;
                const float normalized = std::sqrt(dx * dx + dy * dy) / radiusPx;
                if (normalized >= 1.0f) {
                    continue;
                }

                const float falloff =
                    1.0f - smoothStep(1.0f - featherWidth, 1.0f, normalized);
                float& dest = out[static_cast<std::size_t>(y) * width + x];
                dest = std::max(dest, falloff * flow);
            }
        }
    }
}

/// 把形状权重转换为最终权重（应用反相与不透明度）。
void finalizeMaskWeights(const MaskLayer& layer,
                         const std::vector<float>& raw,
                         std::vector<float>& out)
{
    out.resize(raw.size());
    const float opacityValue = clamp01(layer.opacity);

    if (!layer.invert && opacityValue >= 1.0f) {
        out = raw;
        return;
    }

    RAWCORE_MASK_PARALLEL_FOR(i, static_cast<int>(raw.size()))
    {
        float value = raw[static_cast<std::size_t>(i)];
        if (layer.invert) {
            value = 1.0f - value;
        }
        out[static_cast<std::size_t>(i)] = clamp01(value) * opacityValue;
    }
}

}  // namespace

void MaskLayer::buildWeightMap(int width, int height, std::vector<float>& out) const
{
    buildShapeWeightMap(width, height, out);
    if (out.empty()) {
        return;
    }

    // 反相与不透明度都是恒等时直接返回，避免一次无意义的整幅拷贝
    if (!invert && clamp01(opacity) >= 1.0f) {
        return;
    }
    finalizeMaskWeights(*this, out, out);
}

void MaskLayer::buildShapeWeightMap(int width, int height,
                                    std::vector<float>& out) const
{
    if (width <= 0 || height <= 0) {
        out.clear();
        return;
    }

    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    out.assign(count, 0.0f);

    const float featherWidth = clamp01(feather);

    switch (shape) {
    case MaskShape::Radial: {
        const float centerPx = centerX * static_cast<float>(width);
        const float centerPy = centerY * static_cast<float>(height);
        const float radiusPx = std::max(1e-3f, std::fabs(radiusX) * width);
        const float radiusPy = std::max(1e-3f, std::fabs(radiusY) * height);

        // 与 weightAt 相同的顺时针旋转约定：把像素反旋到椭圆本地坐标
        const float angle = -rotation * kPi / 180.0f;
        const float cosA = std::cos(angle);
        const float sinA = std::sin(angle);

        RAWCORE_MASK_PARALLEL_FOR(y, height)
        {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = 0; x < width; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float dx = px - centerPx;
                const float dy = py - centerPy;

                float localX = dx;
                float localY = dy;
                if (std::fabs(rotation) > 1e-6f) {
                    localX = dx * cosA - dy * sinA;
                    localY = dx * sinA + dy * cosA;
                }

                const float nx = localX / radiusPx;
                const float ny = localY / radiusPy;
                const float distance = std::sqrt(nx * nx + ny * ny);

                out[static_cast<std::size_t>(y) * width + x] =
                    1.0f - smoothStep(1.0f - featherWidth, 1.0f, distance);
            }
        }
        break;
    }
    case MaskShape::Linear: {
        const float dirX = endX - startX;
        const float dirY = endY - startY;
        const float lengthSquared = dirX * dirX + dirY * dirY;

        if (lengthSquared < 1e-8f) {
            std::fill(out.begin(), out.end(), 1.0f);
        } else {
            RAWCORE_MASK_PARALLEL_FOR(y, height)
            {
                const float v = (static_cast<float>(y) + 0.5f) / height;
                for (int x = 0; x < width; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / width;
                    const float t = ((u - startX) * dirX + (v - startY) * dirY)
                        / lengthSquared;
                    out[static_cast<std::size_t>(y) * width + x] = clamp01(t);
                }
            }
        }
        break;
    }
    case MaskShape::Range:
    case MaskShape::Luminance:
    case MaskShape::ColorRange:
        // 像素范围类蒙版：基础权重全幅为 1，筛选在 applyLocalAdjustments 中完成
        std::fill(out.begin(), out.end(), 1.0f);
        break;
    case MaskShape::Brush:
        // 画笔半径按图像短边解释；逐笔画栅格化，避免每个像素遍历全部点
        rasterizeBrushStrokes(*this, width, height, 0, out);
        break;
    }
}

namespace {

std::uint64_t mixHash(std::uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t hashMaskShape(const MaskLayer& layer, int width, int height)
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto mixInt = [&hash](int value) {
        hash = mixHash(hash, &value, sizeof(value));
    };
    const auto mixFloat = [&hash](float value) {
        hash = mixHash(hash, &value, sizeof(value));
    };
    const auto mixShape = [&hash](MaskShape value) {
        hash = mixHash(hash, &value, sizeof(value));
    };

    mixShape(layer.shape);
    mixInt(width);
    mixInt(height);
    mixFloat(layer.feather);

    switch (layer.shape) {
    case MaskShape::Radial:
        mixFloat(layer.centerX);
        mixFloat(layer.centerY);
        mixFloat(layer.radiusX);
        mixFloat(layer.radiusY);
        mixFloat(layer.rotation);
        break;
    case MaskShape::Linear:
        mixFloat(layer.startX);
        mixFloat(layer.startY);
        mixFloat(layer.endX);
        mixFloat(layer.endY);
        break;
    case MaskShape::Brush:
    case MaskShape::Range:
    case MaskShape::Luminance:
    case MaskShape::ColorRange:
        break;
    }
    return hash;
}

std::uint64_t hashMaskStrokes(const MaskLayer& layer, std::size_t count)
{
    std::uint64_t hash = 1469598103934665603ull;
    const std::size_t limit = std::min(count, layer.strokes.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const MaskStrokePoint& point = layer.strokes[i];
        hash = mixHash(hash, &point.x, sizeof(point.x));
        hash = mixHash(hash, &point.y, sizeof(point.y));
        hash = mixHash(hash, &point.radius, sizeof(point.radius));
        hash = mixHash(hash, &point.flow, sizeof(point.flow));
    }
    return hash;
}

}  // namespace

namespace {

/// 单个像素的原始范围权重（不含反相 / 不透明度）
float rawRangeWeightAt(const MaskLayer& layer, float r, float g, float b)
{
    if (layer.shape == MaskShape::Luminance) {
        const float luma =
            layer.luminance.mode == LuminanceMaskMode::ProPhotoY
                ? (0.2880402f * r + 0.7118741f * g + 0.0000857f * b)
                : (0.2126f * r + 0.7152f * g + 0.0722f * b);
        return layer.luminance.weightAtLinear(luma);
    }
    if (layer.shape == MaskShape::ColorRange) {
        float labL = 0.0f;
        float labA = 0.0f;
        float labB = 0.0f;
        math::linearProPhotoToLabD50(std::max(0.0f, r), std::max(0.0f, g),
                                     std::max(0.0f, b), labL, labA, labB);
        return layer.colorRange.weightAtLab(labL, labA, labB);
    }

    // 旧版 B3 范围：感知域亮度 + HSV
    const float pr = math::linearToSrgbGamma(std::max(0.0f, r));
    const float pg = math::linearToSrgbGamma(std::max(0.0f, g));
    const float pb = math::linearToSrgbGamma(std::max(0.0f, b));
    const float luma = 0.2126f * pr + 0.7152f * pg + 0.0722f * pb;

    const float maxValue = std::max({pr, pg, pb});
    const float minValue = std::min({pr, pg, pb});
    const float delta = maxValue - minValue;
    const float saturation = maxValue > 1e-8f ? delta / maxValue : 0.0f;
    float hue = 0.0f;
    if (delta > 1e-8f) {
        if (maxValue == pr) {
            hue = 60.0f * std::fmod((pg - pb) / delta, 6.0f);
        } else if (maxValue == pg) {
            hue = 60.0f * (((pb - pr) / delta) + 2.0f);
        } else {
            hue = 60.0f * (((pr - pg) / delta) + 4.0f);
        }
        if (hue < 0.0f) {
            hue += 360.0f;
        }
    }
    return layer.range.weightAt(luma, hue, saturation);
}

std::uint64_t hashRangeParams(const MaskLayer& layer)
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    mix(&layer.shape, sizeof(layer.shape));
    mix(&layer.luminance.low, sizeof(layer.luminance.low));
    mix(&layer.luminance.high, sizeof(layer.luminance.high));
    mix(&layer.luminance.softness, sizeof(layer.luminance.softness));
    mix(&layer.luminance.mode, sizeof(layer.luminance.mode));

    for (const LabColorPoint& point : layer.colorRange.targets) {
        mix(&point, sizeof(point));
    }
    mix(&layer.colorRange.hueTolerance, sizeof(layer.colorRange.hueTolerance));
    mix(&layer.colorRange.satTolerance, sizeof(layer.colorRange.satTolerance));
    mix(&layer.colorRange.softness, sizeof(layer.colorRange.softness));
    mix(&layer.colorRange.lightnessEnabled, sizeof(layer.colorRange.lightnessEnabled));
    mix(&layer.colorRange.lightnessLow, sizeof(layer.colorRange.lightnessLow));
    mix(&layer.colorRange.lightnessHigh, sizeof(layer.colorRange.lightnessHigh));

    mix(&layer.range, sizeof(layer.range));
    return hash;
}

}  // namespace

void buildRangeWeightMap(const MaskLayer& layer,
                         const ImageBuffer& image,
                         std::vector<float>& out)
{
    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    out.resize(pixels);
    const float* data = image.data();
    if (pixels == 0) {
        return;
    }

    RAWCORE_MASK_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        out[index] = rawRangeWeightAt(layer, data[index * 3u],
                                      data[index * 3u + 1u],
                                      data[index * 3u + 2u]);
    }
}

const std::vector<float>& MaskWeightCache::rangeWeightMap(const MaskLayer& layer,
                                                          const ImageBuffer& image,
                                                          int width, int height)
{
    m_rangeScratch.clear();
    if (width <= 0 || height <= 0 || image.width() != width
        || image.height() != height) {
        return m_rangeScratch;
    }

    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (count == 0) {
        return m_rangeScratch;
    }

    // 全尺寸 / 大图直接计算，避免 tile 缓存额外占用大量内存
    constexpr std::size_t kMaxTiledPixels = 8'000'000;
    if (count > kMaxTiledPixels) {
        buildRangeWeightMap(layer, image, m_rangeScratch);
        return m_rangeScratch;
    }

    constexpr int kTile = 256;
    constexpr std::size_t kMaxTileBytes = 32ull * 1024ull * 1024ull;
    const std::uint64_t baseKey = hashRangeParams(layer)
        ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(image.data()))
        ^ (static_cast<std::uint64_t>(width) << 32)
        ^ static_cast<std::uint64_t>(height);

    m_rangeScratch.resize(count);
    const float* data = image.data();

    for (int tileY = 0; tileY < height; tileY += kTile) {
        const int tileHeight = std::min(kTile, height - tileY);
        for (int tileX = 0; tileX < width; tileX += kTile) {
            const int tileWidth = std::min(kTile, width - tileX);
            std::uint64_t key = baseKey;
            key ^= static_cast<std::uint64_t>(tileX) * 0x9e3779b97f4a7c15ull;
            key ^= static_cast<std::uint64_t>(tileY) * 0xc2b2ae3d27d4eb4full;
            key ^= static_cast<std::uint64_t>(tileWidth) << 16;
            key ^= static_cast<std::uint64_t>(tileHeight);

            RangeTile* tile = nullptr;
            for (RangeTile& entry : m_rangeTiles) {
                if (entry.key == key) {
                    tile = &entry;
                    break;
                }
            }

            if (tile == nullptr) {
                RangeTile entry;
                entry.key = key;
                entry.x = tileX;
                entry.y = tileY;
                entry.width = tileWidth;
                entry.height = tileHeight;
                entry.weights.resize(static_cast<std::size_t>(tileWidth) * tileHeight);
                RAWCORE_MASK_PARALLEL_FOR(i, tileWidth * tileHeight)
                {
                    const int ly = i / tileWidth;
                    const int lx = i % tileWidth;
                    const std::size_t src =
                        (static_cast<std::size_t>(tileY + ly) * width + (tileX + lx)) * 3u;
                    entry.weights[static_cast<std::size_t>(i)] =
                        rawRangeWeightAt(layer, data[src], data[src + 1u], data[src + 2u]);
                }
                m_rangeTiles.push_back(std::move(entry));
                tile = &m_rangeTiles.back();
            }
            tile->lastUse = ++m_clock;

            for (int ly = 0; ly < tileHeight; ++ly) {
                float* dst = m_rangeScratch.data()
                    + static_cast<std::size_t>(tileY + ly) * width + tileX;
                const float* src = tile->weights.data()
                    + static_cast<std::size_t>(ly) * tileWidth;
                std::copy(src, src + tileWidth, dst);
            }

            // 内存上限：淘汰最久未使用的 tile（放在拷贝之后，
            // 避免 vector 重新分配 / 删除使上面的 tile 指针失效）
            std::size_t tileBytes = 0;
            for (const RangeTile& entry : m_rangeTiles) {
                tileBytes += entry.weights.size() * sizeof(float);
            }
            while (tileBytes > kMaxTileBytes && m_rangeTiles.size() > 1) {
                auto oldest = m_rangeTiles.begin();
                for (auto it = m_rangeTiles.begin(); it != m_rangeTiles.end(); ++it) {
                    if (it->lastUse < oldest->lastUse) {
                        oldest = it;
                    }
                }
                if (oldest->key == key) {
                    break;
                }
                tileBytes -= oldest->weights.size() * sizeof(float);
                m_rangeTiles.erase(oldest);
            }
        }
    }
    return m_rangeScratch;
}

const std::vector<float>& MaskWeightCache::weightMap(const MaskLayer& layer,
                                                     int width, int height)
{
    m_scratch.clear();
    if (width <= 0 || height <= 0) {
        return m_scratch;
    }

    const std::size_t count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    // 全尺寸导出 / 大幅裁剪后尺寸过大时不做缓存，避免单层权重图占用上百 MB。
    constexpr std::size_t kMaxCachedPixels = 4'000'000;
    constexpr std::size_t kMaxCachedBytes = 256ull * 1024ull * 1024ull;
    constexpr std::size_t kMaxEntries = 8;

    if (count > kMaxCachedPixels) {
        layer.buildWeightMap(width, height, m_scratch);
        return m_scratch;
    }

    const std::uint64_t shapeKey = hashMaskShape(layer, width, height);
    Entry* chosen = nullptr;

    if (layer.shape == MaskShape::Brush) {
        const std::size_t currentCount = layer.strokes.size();
        const std::uint64_t currentStrokeHash = hashMaskStrokes(layer, currentCount);

        for (Entry& entry : m_entries) {
            if (entry.shapeKey != shapeKey || entry.width != width
                || entry.height != height) {
                continue;
            }
            if (entry.strokeCount > currentCount) {
                continue;
            }
            if (hashMaskStrokes(layer, entry.strokeCount) != entry.strokeHash) {
                continue;
            }
            if (chosen == nullptr || entry.strokeCount > chosen->strokeCount) {
                chosen = &entry;
            }
        }

        if (chosen == nullptr) {
            while (!m_entries.empty() && m_entries.size() >= kMaxEntries) {
                const auto oldest = std::min_element(
                    m_entries.begin(), m_entries.end(),
                    [](const Entry& a, const Entry& b) {
                        return a.lastUse < b.lastUse;
                    });
                m_entries.erase(oldest);
            }

            m_entries.push_back(Entry{});
            chosen = &m_entries.back();
            chosen->shapeKey = shapeKey;
            chosen->width = width;
            chosen->height = height;
            chosen->rawWeights.assign(count, 0.0f);
            rasterizeBrushStrokes(layer, width, height, 0, chosen->rawWeights);
            chosen->strokeCount = currentCount;
            chosen->strokeHash = currentStrokeHash;
        } else if (chosen->strokeCount < currentCount) {
            // 只栅格化新增笔画；已缓存的原始权重保持不变
            rasterizeBrushStrokes(layer, width, height,
                                  chosen->strokeCount, chosen->rawWeights);
            chosen->strokeCount = currentCount;
            chosen->strokeHash = currentStrokeHash;
        }
    } else {
        for (Entry& entry : m_entries) {
            if (entry.shapeKey == shapeKey && entry.width == width
                && entry.height == height) {
                chosen = &entry;
                break;
            }
        }

        if (chosen == nullptr) {
            while (!m_entries.empty() && m_entries.size() >= kMaxEntries) {
                const auto oldest = std::min_element(
                    m_entries.begin(), m_entries.end(),
                    [](const Entry& a, const Entry& b) {
                        return a.lastUse < b.lastUse;
                    });
                m_entries.erase(oldest);
            }

            m_entries.push_back(Entry{});
            chosen = &m_entries.back();
            chosen->shapeKey = shapeKey;
            chosen->width = width;
            chosen->height = height;
            layer.buildShapeWeightMap(width, height, chosen->rawWeights);
        }
    }

    chosen->lastUse = ++m_clock;
    finalizeMaskWeights(layer, chosen->rawWeights, m_scratch);

    // 内存上限：从最久未使用的条目开始淘汰
    while (m_entries.size() > kMaxEntries
           || (m_entries.size() > 1 && cachedBytes() > kMaxCachedBytes)) {
        const auto oldest = std::min_element(
            m_entries.begin(), m_entries.end(),
            [](const Entry& a, const Entry& b) {
                return a.lastUse < b.lastUse;
            });
        if (oldest == m_entries.end() || &*oldest == chosen) {
            break;
        }
        m_entries.erase(oldest);
    }

    return m_scratch;
}

void MaskWeightCache::clear()
{
    m_entries.clear();
    m_rangeTiles.clear();
    m_scratch.clear();
    m_rangeScratch.clear();
    m_clock = 0;
}

std::size_t MaskWeightCache::cachedBytes() const
{
    std::size_t bytes = (m_scratch.size() + m_rangeScratch.size()) * sizeof(float);
    for (const Entry& entry : m_entries) {
        bytes += entry.rawWeights.size() * sizeof(float);
    }
    for (const RangeTile& tile : m_rangeTiles) {
        bytes += tile.weights.size() * sizeof(float);
    }
    return bytes;
}

float MaskLayer::weightAt(float u, float v) const
{
    float value = 0.0f;

    switch (shape) {
        case MaskShape::Radial: {
            // 平移到中心，并按旋转角反向旋转到椭圆本地坐标系
            const float dx = u - centerX;
            const float dy = v - centerY;
            float localX = dx;
            float localY = dy;
            if (std::fabs(rotation) > 1e-6f) {
                const float angle = -rotation * kPi / 180.0f;
                const float cosA = std::cos(angle);
                const float sinA = std::sin(angle);
                localX = dx * cosA - dy * sinA;
                localY = dx * sinA + dy * cosA;
            }
            const float rx = std::max(1e-4f, std::fabs(radiusX));
            const float ry = std::max(1e-4f, std::fabs(radiusY));
            // 归一化椭圆距离：1 表示正好落在边界上
            const float nx = localX / rx;
            const float ny = localY / ry;
            const float distance = std::sqrt(nx * nx + ny * ny);

            // 羽化从边界内侧开始，0 表示硬边
            const float featherWidth = clamp01(feather);
            value = 1.0f - smoothStep(1.0f - featherWidth, 1.0f, distance);
            break;
        }
        case MaskShape::Linear: {
            const float dirX = endX - startX;
            const float dirY = endY - startY;
            const float lengthSquared = dirX * dirX + dirY * dirY;
            if (lengthSquared < 1e-8f) {
                // 起止点重合：退化为中心为 1 的常量蒙版
                value = 1.0f;
            } else {
                const float t = ((u - startX) * dirX + (v - startY) * dirY)
                    / lengthSquared;
                value = clamp01(t);
            }
            break;
        }
        case MaskShape::Range:
        case MaskShape::Luminance:
        case MaskShape::ColorRange:
            // 像素范围蒙版的基础权重为整幅 1，实际筛选在管线里按像素计算
            value = 1.0f;
            break;
        case MaskShape::Brush: {
            const float featherWidth = clamp01(feather);
            for (const MaskStrokePoint& point : strokes) {
                const float dx = u - point.x;
                const float dy = v - point.y;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float radius = std::max(1e-4f, point.radius);
                const float normalized = distance / radius;
                if (normalized >= 1.0f) {
                    continue;
                }
                const float falloff =
                    1.0f - smoothStep(1.0f - featherWidth, 1.0f, normalized);
                value = std::max(value, clamp01(falloff * point.flow));
            }
            break;
        }
    }

    if (invert) {
        value = 1.0f - value;
    }
    return clamp01(value * clamp01(opacity));
}

float MaskStack::weightAt(float u, float v) const
{
    if (!enable) {
        return 0.0f;
    }
    float value = 0.0f;
    for (const MaskLayer& layer : layers) {
        if (!layer.enable) {
            continue;
        }
        value = std::max(value, layer.weightAt(u, v));
    }
    return value;
}

}  // namespace rawcore
