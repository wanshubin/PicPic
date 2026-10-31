// ============================================================================
//  PipelineOps.cpp —— 管线各阶段图像运算实现
//
//  空间约定速查（对应需求 §1.3.1 —— 严格遵守）：
//    线性域：白电平裁剪、拜耳 CCM、Demosaic、白平衡、曝光/高光/阴影、
//            降噪、锐化、色域映射
//    感知域：HSL 的色相/饱和度计算、对比度与去雾的 S 曲线
//            （这些量在感知域才符合视觉直觉，内部做临时 gamma 往返）
//    Gamma域：仅 applyGammaEncode 一步，位于管线末端
// ============================================================================

#include "PipelineOps.h"

#include "LinearMath.h"
#include "Lut3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

#ifdef RAWCore_HAS_OPENMP
#  include <omp.h>
#endif

namespace rawcore {
namespace ops {

namespace {

// ---------------------------------------------------------------------------
//  OpenMP 并行循环宏
//
//  统一封装，保证关闭 OpenMP 时也能编译（退化为串行 for）。
//  所有需要并行的像素循环都通过这个宏，便于集中控制并行策略。
// ---------------------------------------------------------------------------

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  define RAWCORE_PARALLEL_FOR(INDEX, COUNT) \
     _Pragma("omp parallel for schedule(static)") for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#else
#  define RAWCORE_PARALLEL_FOR(INDEX, COUNT) for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#endif

/// 采样坐标夹取
inline int clampIndex(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

/// 每个线程独占一条缓存行的累加槽。
/// 并行统计（直方图分位数、噪声 sigma）需要按线程局部累计再归并；
/// 如果这些局部量在 vector 里紧挨着，32 个线程会反复争抢同一条缓存行
/// （伪共享），实测能把一遍并行扫描拖成串行速度。
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4324)  // 对齐到缓存行导致的结构填充是有意为之
#endif
template <typename T>
struct alignas(64) PaddedSlot {
    T value = T(0);
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

/// 在线性域与"感知域"之间往返的临时 gamma。
/// 仅用于对比度、去雾、HSL 这类需要在感知空间计算的量。
/// 这不是最终的图像 gamma 编码，只是局部计算中介。
inline float toPerceptual(float linear)
{
    return math::linearToSrgbGamma(linear);
}

inline float fromPerceptual(float perceptual)
{
    return math::srgbGammaToLinear(perceptual);
}

/// 线性 ProPhotoRGB 的亮度系数（Y 通道响应）。
/// 自动曝光统计、S 曲线与光影调整共用同一套亮度定义。
inline float luminanceOf(float r, float g, float b)
{
    return 0.2880402f * r + 0.7118741f * g + 0.0000857f * b;
}

/// 平滑阶跃：x <= edge0 时为 0，x >= edge1 时为 1，中间为三次 Hermite。
/// 用于把梯度 / DoG 这类连续量转换为柔和的掩码权重。
inline float smoothStep01(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) {
        return x >= edge1 ? 1.0f : 0.0f;
    }
    const float t = math::saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

/// 大半径高斯模糊的快速近似。
///
/// 清晰度使用约 40px 支撑域的模糊；在 45MP 图上直接做可分离高斯，
/// 每个像素需要上百次乘加，交互和导出都会明显变慢。这里先在面积平均
/// 降采样的小图上做高斯，再双线性放大回去：
///  - 清晰度本身只关心低频亮度结构，降采样不会损失它要的高通细节；
///  - 成本从 O(N × radius) 降到 O(N + N/factor² × radius')；
///  - 小半径（纹理）仍走精确高斯，避免降采样影响高频语义。
void blurPlaneLargeRadius(const std::vector<float>& src,
                          std::vector<float>& dst,
                          int width, int height, float radius)
{
    if (radius < 8.0f || width < 8 || height < 8) {
        gaussianBlurPlane(src, dst, width, height, radius);
        return;
    }

    const int factor = std::clamp(
        static_cast<int>(std::lround(radius / 8.0f)), 2, 8);
    const int smallWidth = std::max(1, width / factor);
    const int smallHeight = std::max(1, height / factor);

    // ---- 面积平均降采样 ----
    std::vector<float> small(
        static_cast<std::size_t>(smallWidth) * smallHeight, 0.0f);

    RAWCORE_PARALLEL_FOR(sy, smallHeight)
    {
        for (int sx = 0; sx < smallWidth; ++sx) {
            const int x0 = sx * factor;
            const int y0 = sy * factor;
            const int x1 = std::min(width, x0 + factor);
            const int y1 = std::min(height, y0 + factor);

            double sum = 0.0;
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                const float* row = src.data() + static_cast<std::size_t>(y) * width;
                for (int x = x0; x < x1; ++x) {
                    sum += row[x];
                    ++count;
                }
            }
            small[static_cast<std::size_t>(sy) * smallWidth + sx] =
                static_cast<float>(sum / std::max(1, count));
        }
    }

    std::vector<float> smallBlur(small.size());
    gaussianBlurPlane(small, smallBlur, smallWidth, smallHeight,
                      std::max(1.0f, radius / static_cast<float>(factor)));

    // ---- 双线性放大 ----
    dst.resize(src.size());
    RAWCORE_PARALLEL_FOR(y, height)
    {
        const float sourceY =
            (static_cast<float>(y) + 0.5f) * smallHeight / height - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)),
                                  0, smallHeight - 1);
        const int y1 = std::min(y0 + 1, smallHeight - 1);
        const float fy = std::clamp(sourceY - static_cast<float>(y0), 0.0f, 1.0f);

        for (int x = 0; x < width; ++x) {
            const float sourceX =
                (static_cast<float>(x) + 0.5f) * smallWidth / width - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sourceX)),
                                      0, smallWidth - 1);
            const int x1 = std::min(x0 + 1, smallWidth - 1);
            const float fx = std::clamp(sourceX - static_cast<float>(x0), 0.0f, 1.0f);

            const float v00 = smallBlur[static_cast<std::size_t>(y0) * smallWidth + x0];
            const float v10 = smallBlur[static_cast<std::size_t>(y0) * smallWidth + x1];
            const float v01 = smallBlur[static_cast<std::size_t>(y1) * smallWidth + x0];
            const float v11 = smallBlur[static_cast<std::size_t>(y1) * smallWidth + x1];

            const float top = v00 + (v10 - v00) * fx;
            const float bottom = v01 + (v11 - v01) * fx;
            dst[static_cast<std::size_t>(y) * width + x] =
                top + (bottom - top) * fy;
        }
    }
}

/// 按渲染尺寸换算滤波半径的倍数。
///
/// 以长边 6000 像素为基准：全尺寸（约 8256 长边）得到 1.38，
/// 2x2 或更高倍率缩小的预览会得到更小的倍数，从而保证预览与导出
/// 在**归一化尺寸**上使用同一个滤波核，观感一致。
inline float filterScaleFor(int width, int height)
{
    const float longest = static_cast<float>(std::max(width, height));
    return math::clamp(longest / 6000.0f, 0.15f, 4.0f);
}

/// 判断矩阵是否在数值上等于单位矩阵。
/// 用于跳过"未标定 / 未校正"时的整幅遍历（串扰补偿默认矩阵就是单位阵）。
inline bool isIdentityMatrix(const Matrix3x3& matrix, float tolerance = 1e-6f)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float expected = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(matrix[static_cast<std::size_t>(r)]
                                [static_cast<std::size_t>(c)] - expected) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

/// 在单通道浮点平面上做夹取边界的双线性采样。
/// 供 LCA 的亚像素重采样使用。
inline float samplePlaneBilinear(const std::vector<float>& plane,
                                 int width, int height,
                                 float x, float y)
{
    const float clampedX = math::clamp(x, 0.0f, static_cast<float>(width - 1));
    const float clampedY = math::clamp(y, 0.0f, static_cast<float>(height - 1));

    const int x0 = static_cast<int>(std::floor(clampedX));
    const int y0 = static_cast<int>(std::floor(clampedY));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float fx = clampedX - static_cast<float>(x0);
    const float fy = clampedY - static_cast<float>(y0);

    const std::size_t row0 = static_cast<std::size_t>(y0) * width;
    const std::size_t row1 = static_cast<std::size_t>(y1) * width;

    const float top = math::lerp(plane[row0 + static_cast<std::size_t>(x0)],
                                 plane[row0 + static_cast<std::size_t>(x1)], fx);
    const float bottom = math::lerp(plane[row1 + static_cast<std::size_t>(x0)],
                                    plane[row1 + static_cast<std::size_t>(x1)], fx);
    return math::lerp(top, bottom, fy);
}

// ---------------------------------------------------------------------------
//  色相角度辅助（HSL 用）
// ---------------------------------------------------------------------------

/// 把 RGB 转换为 HSV 的色相（0~360）与饱和度（0~1）。
/// 亮度分量不需要——HSL 调整中亮度单独在线性域处理。
void rgbToHueSaturation(float r, float g, float b, float& outHue, float& outSaturation)
{
    const float maxValue = std::max({r, g, b});
    const float minValue = std::min({r, g, b});
    const float delta = maxValue - minValue;

    outSaturation = (maxValue > 1e-8f) ? (delta / maxValue) : 0.0f;

    if (delta < 1e-8f) {
        outHue = 0.0f;  // 灰色，色相无定义
        return;
    }

    float hue = 0.0f;
    if (maxValue == r) {
        hue = 60.0f * std::fmod((g - b) / delta, 6.0f);
    } else if (maxValue == g) {
        hue = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        hue = 60.0f * (((r - g) / delta) + 4.0f);
    }

    if (hue < 0.0f) {
        hue += 360.0f;
    }
    outHue = hue;
}

/// 计算某色相相对于目标色相中心的角度距离（0~180）
inline float hueDistance(float hueA, float hueB)
{
    float diff = std::fabs(hueA - hueB);
    if (diff > 180.0f) {
        diff = 360.0f - diff;
    }
    return diff;
}

/// HSV → RGB（hue 0~360，saturation / value 0~1）。
/// 颜色分级只需要“某个色相 + 饱和度”的纯色方向，因此 value 固定为 1。
void hsvToRgb(float hue, float saturation, float value, float out[3])
{
    hue = std::fmod(hue, 360.0f);
    if (hue < 0.0f) {
        hue += 360.0f;
    }

    const float chroma = value * saturation;
    const float hPrime = hue / 60.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(hPrime, 2.0f) - 1.0f));
    const float m = value - chroma;

    float rgb[3] = {0.0f, 0.0f, 0.0f};
    const int sector = static_cast<int>(hPrime) % 6;
    switch (sector) {
    case 0: rgb[0] = chroma; rgb[1] = x; break;
    case 1: rgb[0] = x; rgb[1] = chroma; break;
    case 2: rgb[1] = chroma; rgb[2] = x; break;
    case 3: rgb[1] = x; rgb[2] = chroma; break;
    case 4: rgb[0] = x; rgb[2] = chroma; break;
    default: rgb[0] = chroma; rgb[2] = x; break;
    }

    for (int c = 0; c < 3; ++c) {
        out[c] = rgb[c] + m;
    }
}

/// 明暗分段的软权重（高斯钟形）。
/// center 为 0 / 0.5 / 1，分别对应阴影 / 中间调 / 高光；
/// sigma 由「混合」参数控制，越大三段重叠越多、过渡越柔和。
inline float toneWeight(float y, float center, float sigma)
{
    const float d = (y - center) / std::max(sigma, 1e-4f);
    return std::exp(-0.5f * d * d);
}

/// 单个通道的影响半宽（度）。相邻通道会有交叠，形成平滑过渡。
/// 通道中心角使用 EditContext.h 中的 kHslBandHueCenters，
/// 保证 HSL 调色与目标调整工具共用同一张表。
constexpr float kHslBandHalfWidth = 40.0f;

}  // namespace

// ===========================================================================
//  1. 黑白电平
// ===========================================================================

void applyWhiteLevelClip(BayerImage& bayer, float whiteLevel)
{
    if (bayer.isEmpty() || whiteLevel <= 0.0f) {
        return;
    }

    const float inverseWhite = 1.0f / whiteLevel;
    const std::size_t count = bayer.sampleCount();
    float* data = bayer.data.data();

    // 黑电平已在 RawLoader 中扣除，此处只做饱和裁剪 + 归一化到 0~1。
    // 归一化后，后续所有运算的输入范围统一为 0~1（线性），便于理解与调试。
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
    {
        const float value = data[i] * inverseWhite;
        // 上限 1.0 是饱和裁剪；下限 0.0 兜住黑电平偏移为负的情况
        data[i] = math::clamp(value, 0.0f, 1.0f);
    }
}

void applyWhiteLevelClipPerChannel(BayerImage& bayer,
                                   const std::array<float, 4>& whiteLevels,
                                   float fallbackWhiteLevel)
{
    if (bayer.isEmpty()) {
        return;
    }

    const int width = bayer.width;
    const int height = bayer.height;
    float* data = bayer.data.data();

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            int code = 1;
            if (!bayer.usesGenericCfa()) {
                const int px = x % 2;
                const int py = y % 2;
                switch (bayer.pattern) {
                case BayerPattern::RGGB: code = (py == 0 && px == 0) ? 0 : ((py == 1 && px == 1) ? 2 : ((px == 1) ? 1 : 3)); break;
                case BayerPattern::BGGR: code = (py == 0 && px == 0) ? 2 : ((py == 1 && px == 1) ? 0 : ((px == 1) ? 1 : 3)); break;
                case BayerPattern::GRBG: code = (py == 0 && px == 0) ? 1 : ((py == 0 && px == 1) ? 0 : ((py == 1 && px == 0) ? 2 : 3)); break;
                case BayerPattern::GBRG: code = (py == 0 && px == 0) ? 1 : ((py == 0 && px == 1) ? 2 : ((py == 1 && px == 0) ? 0 : 3)); break;
                default: code = bayer.channelAt(x, y); break;
                }
            } else {
                code = bayer.channelAt(x, y);
            }

            float threshold = whiteLevels[static_cast<std::size_t>(std::clamp(code, 0, 3))];
            if (threshold <= 0.0f) {
                threshold = fallbackWhiteLevel;
            }
            if (threshold <= 0.0f) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            data[index] = math::clamp(data[index] / threshold, 0.0f, 1.0f);
        }
    }
}

void repairBadPixels(BayerImage& bayer, float threshold)
{
    if (bayer.isEmpty() || bayer.width < 3 || bayer.height < 3) {
        return;
    }

    const int width = bayer.width;
    const int height = bayer.height;

    // 先复制一份用于读取。就地修改会污染邻域计算，导致误差扩散。
    const std::vector<float> source = bayer.data;

    const float effectiveThreshold = std::max(1.5f, threshold);

    // X-Trans 的同色采样不在固定的 ±2 位置，改用 5x5 窗口扫描同通道邻居。
    // 逻辑与 2x2 路径一致：以邻域中位数为预期值，按局部噪声自适应判定坏点。
    if (bayer.usesGenericCfa()) {
        RAWCORE_PARALLEL_FOR(y, height)
        {
            if (y == 0 || y == height - 1) {
                continue;
            }

            for (int x = 1; x < width - 1; ++x)
            {
                const int ownChannel = bayer.channelAt(x, y);
                const std::size_t index = static_cast<std::size_t>(y) * width + x;

                float neighbors[24];
                int neighborCount = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= height) {
                        continue;
                    }
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = x + dx;
                        if (nx < 0 || nx >= width) {
                            continue;
                        }
                        if (bayer.channelAt(nx, ny) != ownChannel) {
                            continue;
                        }
                        neighbors[neighborCount++] =
                            source[static_cast<std::size_t>(ny) * width + nx];
                    }
                }

                if (neighborCount < 2) {
                    continue;
                }

                std::sort(neighbors, neighbors + neighborCount);
                const float median = (neighborCount % 2 == 1)
                    ? neighbors[neighborCount / 2]
                    : 0.5f * (neighbors[neighborCount / 2 - 1]
                              + neighbors[neighborCount / 2]);

                float meanAbsDeviation = 0.0f;
                for (int k = 0; k < neighborCount; ++k) {
                    meanAbsDeviation += std::fabs(neighbors[k] - median);
                }
                meanAbsDeviation /= static_cast<float>(neighborCount);

                const float tolerance =
                    std::max(meanAbsDeviation * effectiveThreshold, 0.004f);
                if (std::fabs(source[index] - median) > tolerance) {
                    bayer.data[index] = median;
                }
            }
        }
        return;
    }

    RAWCORE_PARALLEL_FOR(y, height)
    {
        // 跳过最外圈——没有完整邻域可用
        if (y == 0 || y == height - 1) {
            continue;
        }

        for (int x = 1; x < width - 1; ++x)
        {
            // 拜耳域中，同色邻居位于距离 2 的位置（同通道像素呈棋盘格分布）
            float neighbors[4];
            int neighborCount = 0;

            const int offsets[4][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}};
            for (const auto& offset : offsets) {
                const int nx = x + offset[0];
                const int ny = y + offset[1];
                if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                    neighbors[neighborCount++] = source[static_cast<std::size_t>(ny) * width + nx];
                }
            }

            if (neighborCount < 2) {
                continue;
            }

            // 取中位数作为该点的"预期值"。中位数比均值更抗异常值。
            std::sort(neighbors, neighbors + neighborCount);
            const float median = (neighborCount % 2 == 1)
                ? neighbors[neighborCount / 2]
                : 0.5f * (neighbors[neighborCount / 2 - 1] + neighbors[neighborCount / 2]);

            const float actual = source[static_cast<std::size_t>(y) * width + x];

            // 计算邻域的平均绝对偏差，作为该区域噪点水平的估计。
            // 用固定阈值会把高 ISO 的正常噪点全判为坏点，因此阈值随局部噪声自适应。
            float meanAbsDeviation = 0.0f;
            for (int k = 0; k < neighborCount; ++k) {
                meanAbsDeviation += std::fabs(neighbors[k] - median);
            }
            meanAbsDeviation /= static_cast<float>(neighborCount);

            // 至少留一个最小容差，避免平坦区域（偏差≈0）把微小噪声当坏点
            const float tolerance = std::max(meanAbsDeviation * effectiveThreshold, 0.004f);

            if (std::fabs(actual - median) > tolerance) {
                bayer.data[static_cast<std::size_t>(y) * width + x] = median;
            }
        }
    }
}

void applyDarkCurrentCompensation(BayerImage& bayer, float strength, float offset)
{
    if (bayer.isEmpty()) {
        return;
    }

    const float amount = math::saturate(strength);
    const float baseOffset = std::max(0.0f, offset);
    if (amount <= 1e-4f || baseOffset <= 1e-7f) {
        return;
    }

    // 暗电流是加性偏置，直接在每个采样点上扣除。
    // 扣除后可能为负，夹取到 0（黑色没有负光强）。
    const float delta = amount * baseOffset;
    const std::size_t count = bayer.sampleCount();
    float* data = bayer.data.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
    {
        data[i] = std::max(0.0f, data[i] - delta);
    }
}

// ===========================================================================
//  3. Demosaic
// ===========================================================================

namespace {

/// 双线性去拜耳。作为基础实现与其它算法的回退。
void demosaicBilinear(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    output.allocate(width, height);
    float* out = output.data();
    const float* in = bayer.data.data();

    // 对每个像素，先确定它自身通道的已知值，再插值出另外两个通道。
    // 拜耳域中同通道邻居位于 ±2 的偏移处。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            float rgb[3] = {0.0f, 0.0f, 0.0f};

            // 取得 3x3 邻域内各通道的可用值
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int count[3] = {0, 0, 0};

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = clampIndex(x + dx, 0, width - 1);
                    const int ny = clampIndex(y + dy, 0, height - 1);
                    const int channel = bayerChannelAt(pattern, nx, ny);
                    sum[channel] += in[static_cast<std::size_t>(ny) * width + nx];
                    count[channel] += 1;
                }
            }

            for (int c = 0; c < 3; ++c) {
                rgb[c] = (count[c] > 0) ? (sum[c] / static_cast<float>(count[c])) : 0.0f;
            }

            // 自身通道用原值，保留最高保真度（插值会引入不必要的模糊）
            const int ownChannel = bayerChannelAt(pattern, x, y);
            rgb[ownChannel] = in[index];

            float* pixel = out + index * 3u;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
        }
    }
}

// ---------------------------------------------------------------------------
//  VNG4：可变数量梯度去拜耳
//
//  核心思想（源自 dcraw 的 VNG / Hamilton-Adams 梯度插值）：
//   在红/蓝像素处，只有水平、垂直两组邻居是真实绿色采样。分别用这两组
//   绿色估计该方向的绿色值，并以"相邻绿差 + 同通道二阶差分"作为梯度，
//   按梯度反比加权。平坦区域两方向均衡，抑制噪声；边缘处只保留最平滑
//   的方向，避免跨越边缘插值产生拉链伪影。
//
//  红蓝重建采用色差法：先插值绿色平面，再对 (C - G) 做局部加权平均，
//  最后加回绿色，色差法比直接插值 C 更能保持边缘处的色相。
// ---------------------------------------------------------------------------
void demosaicVng4(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    output.allocate(width, height);
    float* out = output.data();
    const float* in = bayer.data.data();

    // 越界夹取采样：反射会放大边缘伪影，夹取更安全
    const auto sample = [&](int x, int y) -> float {
        return in[static_cast<std::size_t>(clampIndex(y, 0, height - 1)) * width
                  + clampIndex(x, 0, width - 1)];
    };

    // ---- 第一遍：插值绿色通道（其余通道的位置暂不写） ----
    //
    // 注意拜耳阵列的采样结构：红/蓝像素的正上、正下、正左、正右才是绿色
    // 采样，而对角邻居与自身同色（红像素的对角是蓝，反之亦然）。把对角
    // 邻居当成绿色、或拿同色像素当"外侧绿色"去算梯度，都会让平坦区域
    // 插值出错误的绿色分量，表现为整幅 1 像素棋盘状彩色噪点。
    //
    // 因此这里只用水平、垂直两组真实绿色采样：
    //   Gh = (G_left + G_right) / 2 + (2*C - C_left2 - C_right2) / 4
    // 第二项用同通道 ±2 采样的二阶差分补偿局部色调变化（Hamilton-Adams
    // 校正），避免边缘处绿色被拉平；两方向按梯度反比加权，平坦区域两者
    // 均衡，边缘处只保留沿边缘的方向。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);

            if (ownChannel == 1) {
                out[index * 3u + 1u] = in[index];
                continue;
            }

            const float center = in[index];
            // 真实绿色邻居
            const float greenLeft = sample(x - 1, y);
            const float greenRight = sample(x + 1, y);
            const float greenUp = sample(x, y - 1);
            const float greenDown = sample(x, y + 1);
            // 与中心同色的 ±2 采样，用于二阶差分校正
            const float colorLeft2 = sample(x - 2, y);
            const float colorRight2 = sample(x + 2, y);
            const float colorUp2 = sample(x, y - 2);
            const float colorDown2 = sample(x, y + 2);

            // 同通道二阶差分：2*C - C-2 - C+2
            const float curvatureH = 2.0f * center - colorLeft2 - colorRight2;
            const float curvatureV = 2.0f * center - colorUp2 - colorDown2;

            const float greenH = 0.5f * (greenLeft + greenRight) + 0.25f * curvatureH;
            const float greenV = 0.5f * (greenUp + greenDown) + 0.25f * curvatureV;

            // 梯度：一阶相邻绿差 + 同通道二阶差分，值越小说明该方向越平滑
            const float gradientH = std::fabs(greenLeft - greenRight) + std::fabs(curvatureH);
            const float gradientV = std::fabs(greenUp - greenDown) + std::fabs(curvatureV);

            const float weightH = 1.0f / (gradientH + 1e-5f);
            const float weightV = 1.0f / (gradientV + 1e-5f);

            float green = (greenH * weightH + greenV * weightV) / (weightH + weightV);

            // 校正项在强边缘处可能过冲，夹回四个真实绿色邻居的范围内
            const float minGreen = std::min(std::min(greenLeft, greenRight),
                                            std::min(greenUp, greenDown));
            const float maxGreen = std::max(std::max(greenLeft, greenRight),
                                            std::max(greenUp, greenDown));
            green = std::min(std::max(green, minGreen), maxGreen);

            out[index * 3u + 1u] = green;
        }
    }

    // ---- 第二遍：用色差插值红蓝通道 ----
    //
    // 绿色平面此时已完整。对缺失通道 C，收集 5x5 内所有 C 采样点的
    // (C - G) 色差并按距离加权平均，再叠回本像素的绿色。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);
            const float green = out[index * 3u + 1u];

            float rgb[3] = {green, green, green};
            rgb[ownChannel] = in[index];

            for (int c = 0; c < 3; ++c) {
                if (c == ownChannel) {
                    continue;
                }

                float chromaSum = 0.0f;
                float weightTotal = 0.0f;

                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }

                        const int nx = clampIndex(x + dx, 0, width - 1);
                        const int ny = clampIndex(y + dy, 0, height - 1);
                        if (bayerChannelAt(pattern, nx, ny) != c) {
                            continue;
                        }

                        const std::size_t neighbor =
                            static_cast<std::size_t>(ny) * width + nx;
                        const float chroma = in[neighbor] - out[neighbor * 3u + 1u];

                        const float distance2 = static_cast<float>(dx * dx + dy * dy);
                        const float weight = 1.0f / (distance2 + 0.5f);

                        chromaSum += chroma * weight;
                        weightTotal += weight;
                    }
                }

                rgb[c] = (weightTotal > 0.0f)
                    ? (green + chromaSum / weightTotal)
                    : green;
            }

            float* pixel = out + index * 3u;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
        }
    }
}

// ---------------------------------------------------------------------------
//  AMaZE：方向估计 + 5x5 色差插值
//
//  相比 VNG4 的差异：
//   1. 绿色插值不只是取两点平均，而是加入中心色差校正项：
//        G = (G1+G2)/2 + (2*C0 - C-2 - C+2)/4
//      这一项利用同通道的二阶差分补偿局部色调变化（AMaZE/AHD 通行的估计式）。
//   2. 方向权重用梯度反比平方，平滑过渡而不做硬性阈值筛选；
//      梯度估计还纳入 ±3 的绿色样本，细密纹理的方向判断更稳定。
//   3. 红蓝重建使用 Malvar-He-Cutler 5x5 核（公开算法），每个插值点都能
//      用到 8 个左右的真实采样，比"只在 ±2 轴上取同色点再求中值"保留更多
//      色彩细节，边缘色带与 zipper 也更少。
// ---------------------------------------------------------------------------
void demosaicAmaze(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    output.allocate(width, height);
    float* out = output.data();
    const float* in = bayer.data.data();

    const auto sample = [&](int x, int y) -> float {
        return in[static_cast<std::size_t>(clampIndex(y, 0, height - 1)) * width
                  + clampIndex(x, 0, width - 1)];
    };

    // ---- 第一遍：绿色通道，方向估计 + 色差校正 ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);

            if (ownChannel == 1) {
                out[index * 3u + 1u] = in[index];
                continue;
            }

            const float center = in[index];

            const float greenLeft = sample(x - 1, y);
            const float greenRight = sample(x + 1, y);
            const float greenUp = sample(x, y - 1);
            const float greenDown = sample(x, y + 1);

            // R/B 位置沿轴 ±3 仍然是绿色采样（±2 是同色）。把外侧的绿色
            // 也纳入梯度估计：只看 ±1 时，细密纹理很容易让两个方向看起来
            // 一样"平"而退化成各向同性平均；加入 ±3 后方向判断更稳定，
            // 斜边/织物类高频纹理的拉链伪影更少。
            const float greenLeft3 = sample(x - 3, y);
            const float greenRight3 = sample(x + 3, y);
            const float greenUp3 = sample(x, y - 3);
            const float greenDown3 = sample(x, y + 3);

            // 中心同色（R 或 B）的 ±2 采样，用于色差校正项
            const float colorLeft = sample(x - 2, y);
            const float colorRight = sample(x + 2, y);
            const float colorUp = sample(x, y - 2);
            const float colorDown = sample(x, y + 2);

            const float horizontalCorrection = 2.0f * center - colorLeft - colorRight;
            const float verticalCorrection = 2.0f * center - colorUp - colorDown;

            const float horizontalEstimate =
                0.5f * (greenLeft + greenRight) + 0.25f * horizontalCorrection;
            const float verticalEstimate =
                0.5f * (greenUp + greenDown) + 0.25f * verticalCorrection;

            // 梯度：中心两侧差 + 外侧绿色变化的一半 + 色差校正项的大小。
            // 外侧只取一半权重，避免把远处纹理误当成当前像素的方向依据。
            const float horizontalGradient =
                std::fabs(greenLeft - greenRight)
                + 0.5f * (std::fabs(greenLeft3 - greenLeft)
                          + std::fabs(greenRight - greenRight3))
                + std::fabs(horizontalCorrection);
            const float verticalGradient =
                std::fabs(greenUp - greenDown)
                + 0.5f * (std::fabs(greenUp3 - greenUp)
                          + std::fabs(greenDown - greenDown3))
                + std::fabs(verticalCorrection);

            // 反比平方权重：梯度小的方向几乎完全主导，过渡区平滑混合
            const float horizontalWeight = 1.0f / (horizontalGradient * horizontalGradient + 1e-6f);
            const float verticalWeight = 1.0f / (verticalGradient * verticalGradient + 1e-6f);

            out[index * 3u + 1u] =
                (horizontalEstimate * horizontalWeight + verticalEstimate * verticalWeight)
                / (horizontalWeight + verticalWeight);
        }
    }

    // ---- 第二遍：红蓝重建（Malvar-He-Cutler 5x5 线性插值） ----
    //
    // 采用公开的 Malvar-He-Cutler 方法（ICASSP 2004）。它在 5x5 窗口内
    // 同时利用"同色样本"与"绿色样本"：
    //   - 绿色位置上的彩色分量以左右（或上下）两个紧邻的同色样本为主干，
    //     其余绿色样本按 -1 / +0.5 的系数修正局部亮度变化；
    //   - 红蓝位置上的另一种彩色来自四个对角样本，同样带同色轴样本的修正。
    // 每个插值点因此能用到 8 个左右的真实采样，比旧实现"只在 ±2 轴上取
    // 同色点再求中值"保留更多色彩细节，边缘色带与 zipper 也明显更少。
    // 每个核的系数之和恰为 8（即单位增益），不会整体改变亮度。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);

            const auto at = [&](int dx, int dy) -> float {
                return sample(x + dx, y + dy);
            };

            // 自身通道保留原始采样值；绿色来自第一遍的方向插值
            float rgb[3] = {
                out[index * 3u],
                out[index * 3u + 1u],
                out[index * 3u + 2u],
            };
            rgb[ownChannel] = in[index];
            const float center = in[index];

            for (int c = 0; c < 3; ++c) {
                if (c == ownChannel) {
                    continue;
                }

                if (ownChannel == 1) {
                    // 绿色位置：待插值的彩色在某一轴上只隔 1 像素。
                    // 与中心同一行时用水平核，否则用旋转 90 度的垂直核。
                    if (bayerChannelAt(pattern, x + 1, y) == c) {
                        rgb[c] = (5.0f * center
                                  + 4.0f * (at(-1, 0) + at(1, 0))
                                  - (at(-2, 0) + at(2, 0))
                                  - (at(-1, -1) + at(1, -1) + at(-1, 1) + at(1, 1))
                                  + 0.5f * (at(0, -2) + at(0, 2))) * 0.125f;
                    } else {
                        rgb[c] = (5.0f * center
                                  + 4.0f * (at(0, -1) + at(0, 1))
                                  - (at(0, -2) + at(0, 2))
                                  - (at(-1, -1) + at(1, -1) + at(-1, 1) + at(1, 1))
                                  + 0.5f * (at(-2, 0) + at(2, 0))) * 0.125f;
                    }
                } else {
                    // 红 / 蓝位置：绿色已由第一遍的方向插值给出，这里不能覆盖，
                    // 只重建另一种彩色。缺失的另一种彩色位于四条对角线上
                    //（相距 1 像素），±2 轴上的同色样本用来扣除该方向的亮度起伏。
                    if (c == 1) {
                        continue;
                    }
                    rgb[c] = (6.0f * center
                              + 2.0f * (at(-1, -1) + at(1, -1) + at(-1, 1) + at(1, 1))
                              - 1.5f * (at(-2, 0) + at(2, 0) + at(0, -2) + at(0, 2)))
                             * 0.125f;
                }
            }

            float* pixel = out + index * 3u;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
        }
    }
}

// ---------------------------------------------------------------------------
//  RCD：比值 / 色差校正的方向自适应去马赛克（D4）
//
//  与 AMaZE 的区别在红蓝重建：
//    - 绿色仍用方向梯度加权，避免跨边缘插值；
//    - 红 / 蓝统一在**色差域（C − G）**插值。色差在局部近似常数，
//      因此跨越亮度边缘时不会产生彩边，这比直接对 C 插值稳定得多；
//    - 对角位置的另一种彩色用四个对角样本的色差加权平均，
//      按绿色梯度做反比加权，边缘方向上的样本权重更大。
//
//  说明：本实现遵循 RCD 的核心思想（方向自适应 + 色差/比值校正），
//  但不是对原论文逐位复刻；目标是稳定地减少拉链伪影与彩色边缘。
// ---------------------------------------------------------------------------
void demosaicRcd(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    output.allocate(width, height);
    float* out = output.data();
    const float* in = bayer.data.data();

    const auto sample = [&](int x, int y) -> float {
        return in[static_cast<std::size_t>(clampIndex(y, 0, height - 1)) * width
                  + clampIndex(x, 0, width - 1)];
    };

    // ---- 第一遍：绿色平面 ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int own = bayerChannelAt(pattern, x, y);
            if (own == 1) {
                out[index * 3u + 1u] = in[index];
                continue;
            }

            const float center = in[index];
            const float gL = sample(x - 1, y);
            const float gR = sample(x + 1, y);
            const float gU = sample(x, y - 1);
            const float gD = sample(x, y + 1);
            const float cL = sample(x - 2, y);
            const float cR = sample(x + 2, y);
            const float cU = sample(x, y - 2);
            const float cD = sample(x, y + 2);

            // 水平 / 垂直色差校正后的绿色估计
            const float horizontal = 0.5f * (gL + gR)
                + 0.25f * (2.0f * center - cL - cR);
            const float vertical = 0.5f * (gU + gD)
                + 0.25f * (2.0f * center - cU - cD);

            // 方向判据同时考虑近邻绿色差与同色色差变化，
            // 后者能识别"绿色两侧相同但同色通道有边缘"的细线。
            const float gradH = std::fabs(gL - gR)
                + std::fabs(2.0f * center - cL - cR);
            const float gradV = std::fabs(gU - gD)
                + std::fabs(2.0f * center - cU - cD);

            const float wH = 1.0f / (gradH * gradH + 1e-6f);
            const float wV = 1.0f / (gradV * gradV + 1e-6f);
            out[index * 3u + 1u] = (horizontal * wH + vertical * wV) / (wH + wV);
        }
    }

    // ---- 第二遍：色差域重建红 / 蓝 ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int own = bayerChannelAt(pattern, x, y);
            float* pixel = out + index * 3u;

            const float green = pixel[1];
            pixel[own] = in[index];

            if (own == 1) {
                // 绿色位置：红 / 蓝在水平或垂直方向紧邻，
                // 用两侧色差的平均值作为该像素的色差。
                for (int c = 0; c < 3; c += 2) {
                    if (bayerChannelAt(pattern, x + 1, y) == c) {
                        const float rawL = sample(x - 1, y);
                        const float rawR = sample(x + 1, y);
                        const float greenL = out[(index - 1u) * 3u + 1u];
                        const float greenR = out[(index + 1u) * 3u + 1u];
                        const float diff = 0.5f
                            * ((rawL - greenL) + (rawR - greenR));
                        pixel[c] = green + diff;
                    } else {
                        const float rawU = sample(x, y - 1);
                        const float rawD = sample(x, y + 1);
                        const float greenU =
                            out[(index - static_cast<std::size_t>(width)) * 3u + 1u];
                        const float greenD =
                            out[(index + static_cast<std::size_t>(width)) * 3u + 1u];
                        const float diff = 0.5f
                            * ((rawU - greenU) + (rawD - greenD));
                        pixel[c] = green + diff;
                    }
                }
            } else {
                // 红 / 蓝位置：另一种彩色在四个对角位置。
                // 用绿色梯度做反比加权，避免跨过亮度边缘取平均。
                const int missing = (own == 0) ? 2 : 0;
                float diffSum = 0.0f;
                float weightSum = 0.0f;
                static constexpr int kDx[4] = {-1, 1, -1, 1};
                static constexpr int kDy[4] = {-1, -1, 1, 1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = clampIndex(x + kDx[k], 0, width - 1);
                    const int ny = clampIndex(y + kDy[k], 0, height - 1);
                    const std::size_t nIndex =
                        static_cast<std::size_t>(ny) * width + nx;
                    const float neighborGreen = out[nIndex * 3u + 1u];
                    const float gradient = std::fabs(neighborGreen - green);
                    const float weight = 1.0f / (gradient * gradient + 1e-5f);
                    diffSum += weight * (in[nIndex] - neighborGreen);
                    weightSum += weight;
                }
                pixel[missing] = green + (weightSum > 1e-8f
                                              ? diffSum / weightSum : 0.0f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  LMMSE：局部加权统计（线性最小均方误差）色差估计
//
//  与前两者的区别在于红蓝重建方式：
//   在 5x5 窗口内收集同色采样点的 (C - G) 色差，按高斯距离加权，
//   同时计算局部均值与方差。平坦区域（方差小、最近样本与均值接近）
//   采用窗口均值，获得良好的去噪与去伪彩效果；
//   边缘区域（色差突变）保留最近样本的色差，维持边缘锐度。
//   权重系数由"最近样本与均值的偏离 / 局部标准差"决定，
//   正是 MMSE 估计中"信号方差与噪声方差之比"的工程化形式。
// ---------------------------------------------------------------------------
void demosaicLmmse(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    output.allocate(width, height);
    float* out = output.data();
    const float* in = bayer.data.data();

    const auto sample = [&](int x, int y) -> float {
        return in[static_cast<std::size_t>(clampIndex(y, 0, height - 1)) * width
                  + clampIndex(x, 0, width - 1)];
    };

    // ---- 第一遍：绿色通道 ----
    // 用 4 方向的梯度反比加权（不做硬阈值），得到平滑的方向估计。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);

            if (ownChannel == 1) {
                out[index * 3u + 1u] = in[index];
                continue;
            }

            const float greenLeft = sample(x - 1, y);
            const float greenRight = sample(x + 1, y);
            const float greenUp = sample(x, y - 1);
            const float greenDown = sample(x, y + 1);

            const float horizontalGradient = std::fabs(greenLeft - greenRight);
            const float verticalGradient = std::fabs(greenUp - greenDown);

            const float horizontalWeight = 1.0f / (horizontalGradient * horizontalGradient + 1e-6f);
            const float verticalWeight = 1.0f / (verticalGradient * verticalGradient + 1e-6f);

            out[index * 3u + 1u] =
                (0.5f * (greenLeft + greenRight) * horizontalWeight
                 + 0.5f * (greenUp + greenDown) * verticalWeight)
                / (horizontalWeight + verticalWeight);
        }
    }

    // ---- 第二遍：局部加权统计估计色差 ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayerChannelAt(pattern, x, y);
            const float green = out[index * 3u + 1u];

            float rgb[3] = {green, green, green};
            rgb[ownChannel] = in[index];

            for (int c = 0; c < 3; ++c) {
                if (c == ownChannel) {
                    continue;
                }

                float weightTotal = 0.0f;
                float chromaWeighted = 0.0f;
                float chromaSquaredWeighted = 0.0f;

                // 最近一圈同色样本的色差（用于保边）
                float nearestDistance = 1e9f;
                float nearestChroma = 0.0f;
                int nearestCount = 0;

                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }

                        const int nx = clampIndex(x + dx, 0, width - 1);
                        const int ny = clampIndex(y + dy, 0, height - 1);
                        if (bayerChannelAt(pattern, nx, ny) != c) {
                            continue;
                        }

                        const std::size_t neighbor =
                            static_cast<std::size_t>(ny) * width + nx;
                        const float chroma = in[neighbor] - out[neighbor * 3u + 1u];

                        // 高斯权重，sigma = 1.5 像素
                        const float distance2 = static_cast<float>(dx * dx + dy * dy);
                        const float weight = std::exp(-distance2 / 4.5f);

                        weightTotal += weight;
                        chromaWeighted += chroma * weight;
                        chromaSquaredWeighted += chroma * chroma * weight;

                        if (distance2 < nearestDistance - 1e-6f) {
                            nearestDistance = distance2;
                            nearestChroma = chroma;
                            nearestCount = 1;
                        } else if (std::fabs(distance2 - nearestDistance) <= 1e-6f) {
                            nearestChroma += chroma;
                            ++nearestCount;
                        }
                    }
                }

                if (weightTotal <= 0.0f || nearestCount <= 0) {
                    rgb[c] = green;
                    continue;
                }

                const float mean = chromaWeighted / weightTotal;
                const float variance = std::max(
                    0.0f, chromaSquaredWeighted / weightTotal - mean * mean);
                const float nearest = nearestChroma / static_cast<float>(nearestCount);

                // 最近样本与局部均值差异越大，说明越可能处于色差边缘，
                // 此时越信任最近样本（保边）；平坦区则以均值为主（去噪）。
                const float edgeDifference = std::fabs(nearest - mean);
                // 0.0025 是线性域的噪声底（约 -8.6 EV 的正则项），
                // 防止暗部方差估计过小导致权重抖动
                const float blend = edgeDifference
                    / (edgeDifference + std::sqrt(variance) + 0.0025f);

                rgb[c] = green + math::lerp(mean, nearest, blend);
            }

            float* pixel = out + index * 3u;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
        }
    }
}

/// 摩尔纹抑制：对色度做按强度混合的轻度模糊。
///
/// 摩尔纹主要表现为色度上的高频伪彩，模糊色度可以显著减轻而不损明度细节。
/// 旧实现有两个问题：
///   1. 强度只被映射成模糊半径，最后却**完全替换**色度——strength=0.01
///      和 1.0 的抑制效果几乎一样，滑块形同虚设；
///   2. 半径直接传给 gaussianBlurPlane，而该函数接受的是 3σ 支撑半径，
///      默认 0.15 实际只对应 σ≈0.22，接近空转。
/// 现在按高斯 σ 标定半径，并按 strength 在原始色度与平滑色度之间插值，
/// 保证强度单调、0 时严格不改变图像。
void suppressMoire(ImageBuffer& image, float strength)
{
    if (strength <= 1e-4f || image.isEmpty()) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const int width = image.width();
    const int height = image.height();
    if (width < 3 || height < 3) {
        return;
    }

    // σ 随强度增大：0.45px（只压最高频）~ 2.0px（大面积伪彩）。
    // gaussianBlurPlane 的参数是 3σ 支撑半径，因此乘 3。
    const float sigma = 0.45f + amount * 1.55f;
    const float supportRadius = sigma * 3.0f;

    const std::size_t pixels = image.pixelCount();

    // 分离出亮度与色度
    std::vector<float> luma(pixels);
    std::vector<float> chroma1(pixels);
    std::vector<float> chroma2(pixels);

    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float r = data[index * 3u];
        const float g = data[index * 3u + 1];
        const float b = data[index * 3u + 2];
        // YCbCr 式的简单分离
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        luma[index] = y;
        chroma1[index] = r - y;
        chroma2[index] = b - y;
    }

    // 只模糊色度
    std::vector<float> blurredC1;
    std::vector<float> blurredC2;
    gaussianBlurPlane(chroma1, blurredC1, width, height, supportRadius);
    gaussianBlurPlane(chroma2, blurredC2, width, height, supportRadius);

    // 按强度混合：amount=0 时完全保留原色度，amount=1 时完全采用平滑值。
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float y = luma[index];
        const float c1 = math::lerp(chroma1[index], blurredC1[index], amount);
        const float c2 = math::lerp(chroma2[index], blurredC2[index], amount);

        data[index * 3u]     = y + c1;
        data[index * 3u + 1] = y - (0.299f / 0.587f) * c1 - (0.114f / 0.587f) * c2;
        data[index * 3u + 2] = y + c2;
    }
}

// ---------------------------------------------------------------------------
//  X-Trans 去马赛克（6x6 通用 CFA）
//
//  X-Trans 没有 2x2 拜耳的"红蓝格点"结构，所有"±2 必为同色邻居"的假设
//  都不成立，因此单独实现：
//   1. 绿色平面：沿水平 / 垂直方向分别找最近的绿色采样，按距离线性插值，
//      再按方向梯度反比加权。采样距离可变，但保留了 VNG4 的方向选择思想；
//   2. 红蓝平面：在 5x5 窗口内对同色采样的 (C - G) 色差做距离加权平均，
//      再加回绿色。色差法比直接插值 R/B 更能保持边缘色相；
//   3. 原始采样点始终保持原值，避免把真实数据再平滑一遍。
//
//  质量定位：明显优于双线性，低于 Markesteijn 多遍算法的极限；换来的是
//  无 GPL 依赖、可读、可回归测试的工程实现。全局的摩尔纹平滑仍由外层
//  demosaic() 统一施加。
// ---------------------------------------------------------------------------
void demosaicXTrans(const BayerImage& bayer, ImageBuffer& output)
{
    const int width = bayer.width;
    const int height = bayer.height;
    output.allocate(width, height);

    float* out = output.data();
    const float* in = bayer.data.data();
    const std::size_t count = static_cast<std::size_t>(width) * height;

    // 预展开通道表：内层循环里反复做取模 + 查表代价太高。
    std::vector<std::uint8_t> color(count);
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            color[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(bayer.channelAt(x, y));
        }
    }

    // ---- 第一遍：绿色平面（方向感知、可变采样距离） ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (color[index] == 1) {
                out[index * 3u + 1u] = in[index];
                continue;
            }

            float axisEstimate[2] = {0.0f, 0.0f};
            float axisGradient[2] = {0.0f, 0.0f};
            bool axisValid[2] = {false, false};
            float greenMin = 1e30f;
            float greenMax = -1e30f;

            // axis 0 = 水平，1 = 垂直；每个方向最多扫 3 像素找最近绿色
            for (int axis = 0; axis < 2; ++axis) {
                bool hasPos = false;
                bool hasNeg = false;
                int distPos = 0;
                int distNeg = 0;
                float greenPos = 0.0f;
                float greenNeg = 0.0f;

                for (int d = 1; d <= 3; ++d) {
                    const int nx = x + ((axis == 0) ? d : 0);
                    const int ny = y + ((axis == 1) ? d : 0);
                    if (!hasPos && nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        const std::size_t neighbor =
                            static_cast<std::size_t>(ny) * width + nx;
                        if (color[neighbor] == 1) {
                            hasPos = true;
                            distPos = d;
                            greenPos = in[neighbor];
                        }
                    }

                    const int mx = x - ((axis == 0) ? d : 0);
                    const int my = y - ((axis == 1) ? d : 0);
                    if (!hasNeg && mx >= 0 && mx < width && my >= 0 && my < height) {
                        const std::size_t neighbor =
                            static_cast<std::size_t>(my) * width + mx;
                        if (color[neighbor] == 1) {
                            hasNeg = true;
                            distNeg = d;
                            greenNeg = in[neighbor];
                        }
                    }

                    if (hasPos && hasNeg) {
                        break;
                    }
                }

                if (hasPos && hasNeg) {
                    // 按距离线性插值：距离越远权重越小
                    axisEstimate[axis] =
                        (greenPos * static_cast<float>(distNeg)
                         + greenNeg * static_cast<float>(distPos))
                        / static_cast<float>(distPos + distNeg);
                    axisGradient[axis] = std::fabs(greenPos - greenNeg);
                    axisValid[axis] = true;
                } else if (hasPos || hasNeg) {
                    // 只有单侧绿色：可用，但可信度低于双侧估计，
                    // 用固定的中等梯度避免它压过真正平滑的双侧方向。
                    axisEstimate[axis] = hasPos ? greenPos : greenNeg;
                    axisGradient[axis] = 0.05f;
                    axisValid[axis] = true;
                }

                if (hasPos) {
                    greenMin = std::min(greenMin, greenPos);
                    greenMax = std::max(greenMax, greenPos);
                }
                if (hasNeg) {
                    greenMin = std::min(greenMin, greenNeg);
                    greenMax = std::max(greenMax, greenNeg);
                }
            }

            float green = 0.0f;
            if (axisValid[0] && axisValid[1]) {
                const float wH = 1.0f / (axisGradient[0] * axisGradient[0] + 1e-6f);
                const float wV = 1.0f / (axisGradient[1] * axisGradient[1] + 1e-6f);
                green = (axisEstimate[0] * wH + axisEstimate[1] * wV) / (wH + wV);
            } else if (axisValid[0]) {
                green = axisEstimate[0];
            } else if (axisValid[1]) {
                green = axisEstimate[1];
            } else {
                // 极端情况（细碎图案）：退回 5x5 内全部绿色的距离加权平均
                float sum = 0.0f;
                float weightTotal = 0.0f;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = clampIndex(x + dx, 0, width - 1);
                        const int ny = clampIndex(y + dy, 0, height - 1);
                        const std::size_t neighbor =
                            static_cast<std::size_t>(ny) * width + nx;
                        if (color[neighbor] != 1) {
                            continue;
                        }
                        const float weight =
                            1.0f / (static_cast<float>(dx * dx + dy * dy) + 0.5f);
                        sum += in[neighbor] * weight;
                        weightTotal += weight;
                    }
                }
                green = (weightTotal > 0.0f) ? (sum / weightTotal) : in[index];
            }

            // 方向校正可能过冲，夹回已知绿色采样的范围
            if (greenMax >= greenMin) {
                green = std::min(std::max(green, greenMin), greenMax);
            }
            out[index * 3u + 1u] = green;
        }
    }

    // ---- 第二遍：用同色采样点的 (C − G) 色差重建红蓝 ----
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = color[index];
            const float green = out[index * 3u + 1u];

            float rgb[3] = {green, green, green};
            rgb[ownChannel] = in[index];

            for (int c = 0; c < 3; ++c) {
                if (c == ownChannel) {
                    continue;
                }

                float chromaSum = 0.0f;
                float weightTotal = 0.0f;

                // 先在 5x5 内找；找不到再扩到 7x7（X-Trans 的同色间距最大约 3px）
                for (int radius = 2; radius <= 3 && weightTotal <= 0.0f; ++radius) {
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            if (dx == 0 && dy == 0) {
                                continue;
                            }
                            const int nx = clampIndex(x + dx, 0, width - 1);
                            const int ny = clampIndex(y + dy, 0, height - 1);
                            const std::size_t neighbor =
                                static_cast<std::size_t>(ny) * width + nx;
                            if (color[neighbor] != c) {
                                continue;
                            }

                            const float chroma = in[neighbor] - out[neighbor * 3u + 1u];
                            const float weight =
                                1.0f / (static_cast<float>(dx * dx + dy * dy) + 0.5f);
                            chromaSum += chroma * weight;
                            weightTotal += weight;
                        }
                    }
                }

                if (weightTotal > 0.0f) {
                    rgb[c] = std::max(0.0f, green + chromaSum / weightTotal);
                }
            }

            float* pixel = out + index * 3u;
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
        }
    }
}

/// X-Trans 的方向色差精修（D4，简化版 Markesteijn）。
///
/// Markesteijn 的核心是"多方向假设 + 加权融合"：在不同方向上分别插值，
/// 再用局部梯度决定信任哪个方向。这里保留这一思想，但以通用 X-Trans
/// 去马赛克结果为基础，只对**缺失通道的色差平面**做 3 遍方向精修：
///   - 已知采样点始终保留原始值，不会被平滑；
///   - 每个方向以两侧色差的均值作为估计，权重由绿色梯度与色差梯度共同决定；
///   - 迭代使色差在纹理内部保持一致，减少 6×6 阵列带来的拉链与彩斑。
///
/// 这不是对原论文的逐位复刻，但比单遍通用插值更稳定，且完全可复现。
void refineXTransChroma(ImageBuffer& image, const BayerImage& bayer, int passes)
{
    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    std::vector<float> diffR(pixels);
    std::vector<float> diffB(pixels);

    static constexpr int kDx[4] = {1, 0, 1, 1};
    static constexpr int kDy[4] = {0, 1, 1, -1};

    for (int pass = 0; pass < passes; ++pass) {
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            const std::size_t index = static_cast<std::size_t>(i);
            diffR[index] = data[index * 3u] - data[index * 3u + 1u];
            diffB[index] = data[index * 3u + 2u] - data[index * 3u + 1u];
        }

        RAWCORE_PARALLEL_FOR(y, height)
        {
            for (int x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const int own = bayer.channelAt(x, y);
                const float green = data[index * 3u + 1u];

                for (int c = 0; c < 3; c += 2) {
                    if (c == own) {
                        continue;  // 真实采样，保持原值
                    }
                    const std::vector<float>& diff = (c == 0) ? diffR : diffB;

                    float estimate = 0.0f;
                    float weightSum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        const int nx1 = clampIndex(x + kDx[k], 0, width - 1);
                        const int ny1 = clampIndex(y + kDy[k], 0, height - 1);
                        const int nx2 = clampIndex(x - kDx[k], 0, width - 1);
                        const int ny2 = clampIndex(y - kDy[k], 0, height - 1);
                        const std::size_t i1 =
                            static_cast<std::size_t>(ny1) * width + nx1;
                        const std::size_t i2 =
                            static_cast<std::size_t>(ny2) * width + nx2;

                        const float d1 = diff[i1];
                        const float d2 = diff[i2];
                        const float g1 = data[i1 * 3u + 1u];
                        const float g2 = data[i2 * 3u + 1u];

                        const float greenGradient = std::fabs(g1 - g2);
                        const float diffGradient = std::fabs(d1 - d2);
                        const float gradient = greenGradient + 0.5f * diffGradient;
                        const float weight = 1.0f / (gradient * gradient + 1e-6f);

                        estimate += weight * 0.5f * (d1 + d2);
                        weightSum += weight;
                    }

                    if (weightSum > 1e-8f) {
                        data[index * 3u + static_cast<std::size_t>(c)] =
                            std::max(0.0f, green + estimate / weightSum);
                    }
                }
            }
        }
    }
}

void demosaicXTransMarkesteijn(const BayerImage& bayer, ImageBuffer& output)
{
    // 先得到结构正确的三通道基础结果，再做方向色差精修
    demosaicXTrans(bayer, output);
    refineXTransChroma(output, bayer, 3);
}

}  // namespace

ErrorInfo binBayerToRgb(const BayerImage& bayer,
                        ImageBuffer& output,
                        std::vector<std::uint8_t>* clipMask)
{
    if (bayer.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "拜耳数据为空，无法执行 2x2 合并。");
    }
    if (bayer.width < 2 || bayer.height < 2) {
        return ErrorInfo::make(ErrorCode::Unknown, "图像尺寸过小，无法执行 2x2 合并。");
    }
    if (bayer.usesGenericCfa()) {
        // X-Trans 的 6x6 单元里同色采样不是 2x2 对角结构，
        // 强行合并会产生严重伪色；由调用方改用全分辨率去马赛克 + RGB 降采样。
        return ErrorInfo::make(ErrorCode::Unknown,
                               "该传感器不是 2x2 拜耳阵列，无法执行 2x2 合并。");
    }

    // 由排列确定 2x2 单元内红/蓝像素的坐标；绿色取另外两个位置的平均。
    int redX = 0;
    int redY = 0;
    switch (bayer.pattern) {
    case BayerPattern::RGGB: redX = 0; redY = 0; break;
    case BayerPattern::BGGR: redX = 1; redY = 1; break;
    case BayerPattern::GRBG: redX = 1; redY = 0; break;
    case BayerPattern::GBRG: redX = 0; redY = 1; break;
    default:
        return ErrorInfo::make(ErrorCode::Unknown,
                               "未知的拜耳排列，无法执行 2x2 合并。");
    }
    const int blueX = 1 - redX;
    const int blueY = 1 - redY;

    const int outWidth = bayer.width / 2;
    const int outHeight = bayer.height / 2;
    output.allocate(outWidth, outHeight);

    const int stride = bayer.width;
    const float* input = bayer.data.data();
    float* out = output.data();

    // 饱和判定：管线在合并之前已把数据归一化并裁剪到 [0,1]，
    // 因此达到 1.0 的采样点就是被白电平截断的饱和点。
    constexpr float kClipLevel = 1.0f - 1e-6f;

    if (clipMask != nullptr) {
        clipMask->assign(static_cast<std::size_t>(outWidth) * outHeight, 0u);
    }

    RAWCORE_PARALLEL_FOR(y, outHeight)
    {
        const int y0 = y * 2;
        const float* row0 = input + static_cast<std::size_t>(y0) * stride;
        const float* row1 = row0 + stride;
        float* dst = out + static_cast<std::size_t>(y) * outWidth * 3u;

        for (int x = 0; x < outWidth; ++x) {
            const int x0 = x * 2;

            const float r = (redY == 0) ? row0[x0 + redX] : row1[x0 + redX];
            const float b = (blueY == 0) ? row0[x0 + blueX] : row1[x0 + blueX];

            // 两个绿色样本位于 2x2 单元内与红点正交的两个角：
            //   (1-redX, redY) 与 (redX, 1-redY)，即与红点同行异列、同列异行。
            const float* greenRowA = (redY == 0) ? row0 : row1;  // 含 (1-redX, redY) 的行
            const float* greenRowB = (redY == 0) ? row1 : row0;  // 含 (redX, 1-redY) 的行
            const float g1 = greenRowA[x0 + (redX == 0 ? 1 : 0)];
            const float g2 = greenRowB[x0 + (redX == 0 ? 0 : 1)];

            dst[x * 3 + 0] = r;
            dst[x * 3 + 1] = 0.5f * (g1 + g2);
            dst[x * 3 + 2] = b;

            if (clipMask != nullptr) {
                std::uint8_t flags = 0u;
                if (r >= kClipLevel) {
                    flags |= kClipRed;
                }
                if (g1 >= kClipLevel || g2 >= kClipLevel) {
                    flags |= kClipGreen;
                }
                if (b >= kClipLevel) {
                    flags |= kClipBlue;
                }
                (*clipMask)[static_cast<std::size_t>(y) * outWidth + x] = flags;
            }
        }
    }

    return ErrorInfo::ok();
}

bool buildClipMaskFromBayer(const BayerImage& bayer,
                            std::vector<std::uint8_t>& mask)
{
    if (bayer.isEmpty() || bayer.width < 2 || bayer.height < 2) {
        return false;
    }
    if (!bayer.usesGenericCfa() && bayer.pattern == BayerPattern::Unknown) {
        return false;
    }

    const int width = bayer.width;
    const int height = bayer.height;
    const BayerPattern pattern = bayer.pattern;

    // 预先展开 2x2 单元的通道查表，避免在内层循环里反复调用
    // bayerChannelAt（它对每个采样都要做取模与 switch）。
    int channelOfParity[2][2] = {{0, 0}, {0, 0}};
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            const int channel = bayerChannelAt(pattern, px, py);
            if (channel < 0 || channel > 2) {
                return false;
            }
            channelOfParity[py][px] = channel;
        }
    }

    // 与 binBayerToRgb 保持一致：管线已把数据归一化并裁到 [0,1]，
    // 达到 1.0 的采样点就是被白电平截断的饱和点。
    constexpr float kClipLevel = 1.0f - 1e-6f;
    constexpr std::uint8_t kAllFlags = kClipRed | kClipGreen | kClipBlue;

    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    mask.assign(pixelCount, 0u);

    const float* input = bayer.data.data();

    // 非 2x2 阵列：预展开像素级通道表，5x5 内查同色饱和点。
    if (bayer.usesGenericCfa()) {
        std::vector<std::uint8_t> color(pixelCount);
        RAWCORE_PARALLEL_FOR(y, height)
        {
            for (int x = 0; x < width; ++x) {
                color[static_cast<std::size_t>(y) * width + x] =
                    static_cast<std::uint8_t>(bayer.channelAt(x, y));
            }
        }

        RAWCORE_PARALLEL_FOR(y, height)
        {
            const int y0 = std::max(0, y - 2);
            const int y1 = std::min(height - 1, y + 2);

            for (int x = 0; x < width; ++x) {
                const int x0 = std::max(0, x - 2);
                const int x1 = std::min(width - 1, x + 2);

                std::uint8_t flags = 0u;
                for (int ny = y0; ny <= y1 && flags != kAllFlags; ++ny) {
                    const float* row = input + static_cast<std::size_t>(ny) * width;
                    const std::uint8_t* colorRow =
                        color.data() + static_cast<std::size_t>(ny) * width;

                    for (int nx = x0; nx <= x1; ++nx) {
                        if (row[nx] >= kClipLevel) {
                            flags = static_cast<std::uint8_t>(
                                flags | (1u << colorRow[nx]));
                            if (flags == kAllFlags) {
                                break;
                            }
                        }
                    }
                }

                mask[static_cast<std::size_t>(y) * width + x] = flags;
            }
        }

        return true;
    }

    RAWCORE_PARALLEL_FOR(y, height)
    {
        const int y0 = std::max(0, y - 2);
        const int y1 = std::min(height - 1, y + 2);

        for (int x = 0; x < width; ++x) {
            const int x0 = std::max(0, x - 2);
            const int x1 = std::min(width - 1, x + 2);

            std::uint8_t flags = 0u;
            for (int ny = y0; ny <= y1 && flags != kAllFlags; ++ny) {
                const float* row = input + static_cast<std::size_t>(ny) * width;
                const int* parityRow = channelOfParity[ny & 1];

                for (int nx = x0; nx <= x1; ++nx) {
                    if (row[nx] >= kClipLevel) {
                        flags = static_cast<std::uint8_t>(flags | (1u << parityRow[nx & 1]));
                        if (flags == kAllFlags) {
                            break;
                        }
                    }
                }
            }

            mask[static_cast<std::size_t>(y) * width + x] = flags;
        }
    }

    return true;
}

void downsampleRgbArea(const ImageBuffer& input, ImageBuffer& output, int factor)
{
    if (factor <= 1 || input.isEmpty()) {
        output = input;
        return;
    }

    const int inWidth = input.width();
    const int inHeight = input.height();
    const int outWidth = std::max(1, inWidth / factor);
    const int outHeight = std::max(1, inHeight / factor);
    output.allocate(outWidth, outHeight);

    const float inverse = 1.0f / static_cast<float>(factor * factor);
    const float* src = input.data();
    float* dst = output.data();

    RAWCORE_PARALLEL_FOR(y, outHeight)
    {
        float* outRow = dst + static_cast<std::size_t>(y) * outWidth * 3u;
        for (int x = 0; x < outWidth; ++x) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            for (int dy = 0; dy < factor; ++dy) {
                const float* inRow = src
                    + (static_cast<std::size_t>(y * factor + dy) * inWidth + x * factor) * 3u;
                for (int dx = 0; dx < factor; ++dx) {
                    r += inRow[dx * 3 + 0];
                    g += inRow[dx * 3 + 1];
                    b += inRow[dx * 3 + 2];
                }
            }

            outRow[x * 3 + 0] = r * inverse;
            outRow[x * 3 + 1] = g * inverse;
            outRow[x * 3 + 2] = b * inverse;
        }
    }
}

ErrorInfo demosaic(const BayerImage& bayer,
                   ImageBuffer& output,
                   DemosaicAlgorithm algorithm,
                   float smooth)
{
    if (bayer.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "拜耳数据为空，无法执行去拜耳。");
    }
    if (bayer.width < 3 || bayer.height < 3) {
        return ErrorInfo::make(ErrorCode::Unknown, "图像尺寸过小，无法执行去拜耳。");
    }

    try {
        if (bayer.usesGenericCfa()) {
            // X-Trans 等非 2x2 阵列走专用实现；只有 Markesteijn 选项
            // 会在通用结果上追加 3 遍方向色差精修，其余算法名视为通用路径。
            if (algorithm == DemosaicAlgorithm::Markesteijn) {
                demosaicXTransMarkesteijn(bayer, output);
            } else {
                demosaicXTrans(bayer, output);
            }
        } else {
            switch (algorithm) {
            case DemosaicAlgorithm::Bilinear:
                demosaicBilinear(bayer, output);
                break;

            case DemosaicAlgorithm::VNG4:
                demosaicVng4(bayer, output);
                break;

            case DemosaicAlgorithm::AMAZE:
                demosaicAmaze(bayer, output);
                break;

            case DemosaicAlgorithm::LMMSE:
                demosaicLmmse(bayer, output);
                break;

            case DemosaicAlgorithm::RCD:
                demosaicRcd(bayer, output);
                break;

            case DemosaicAlgorithm::Markesteijn:
                // Markesteijn 只对 X-Trans 有意义；2x2 拜耳退回 RCD，
                // 避免用户误选后得到未实现的空结果。
                demosaicRcd(bayer, output);
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        output.release();
        return ErrorInfo::make(ErrorCode::OutOfMemory,
                               "内存不足，无法完成去拜耳运算。",
                               "demosaic 分配失败");
    }

    // 摩尔纹平滑（需求 §3 模块 A 的 demosaic_smooth 参数）
    if (smooth > 1e-4f) {
        suppressMoire(output, smooth);
    }

    return ErrorInfo::ok();
}

// ===========================================================================
//  3.5 彩边 / 色散校正
// ===========================================================================

ErrorInfo applyBayerLinearMatrix(BayerImage& bayer, const Matrix3x3& matrix)
{
    if (bayer.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "拜耳数据为空，无法执行矩阵变换。");
    }

    // 单位矩阵不改变任何数据，直接返回。
    // 机型尚未提供串扰标定矩阵时（默认就是单位阵），这一步完全不产生开销。
    if (isIdentityMatrix(matrix)) {
        return ErrorInfo::ok();
    }

    const int width = bayer.width;
    const int height = bayer.height;
    const bool genericCfa = bayer.usesGenericCfa();

    // 就地计算需要读取邻域，而邻域会被并行改写，因此先复制一份只读源。
    // 这里只复制一路拜耳数据（约 1 字节/像素的 float），
    // 不再像早期实现那样分配整幅三通道临时图像（3 倍内存），
    // 对 4500 万像素的预览渲染尤其重要。
    const std::vector<float> source = bayer.data;
    float* out = bayer.data.data();

    // 2x2 拜耳的 3x3 邻域一定包含三种颜色；X-Trans 的 6x6 排列里
    // 某个 3x3 窗口可能缺少 R 或 B，因此通用路径扩到 5x5。
    const int radius = genericCfa ? 2 : 1;

    // 三通道估计采用与去马赛克一致的加权平均规则，
    // 保证矩阵变换与"先插值去拜耳再变换"的结果一致；
    // 自身通道仍使用原始采样值，避免不必要的模糊。
    // 矩阵结果只写回该像素自身的 CFA 通道，保持拜耳结构不变。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int count[3] = {0, 0, 0};

            for (int dy = -radius; dy <= radius; ++dy) {
                const int ny = clampIndex(y + dy, 0, height - 1);
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int nx = clampIndex(x + dx, 0, width - 1);
                    const int channel = bayer.channelAt(nx, ny);
                    sum[channel] += source[static_cast<std::size_t>(ny) * width + nx];
                    ++count[channel];
                }
            }

            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int ownChannel = bayer.channelAt(x, y);

            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = (count[c] > 0)
                    ? (sum[c] / static_cast<float>(count[c]))
                    : 0.0f;
            }
            rgb[ownChannel] = source[index];

            float transformed[3];
            math::multiply(matrix, rgb, transformed);
            out[index] = transformed[ownChannel];
        }
    }

    return ErrorInfo::ok();
}

void applyCrosstalkCompensation(BayerImage& bayer,
                                const Matrix3x3& decouplingMatrix,
                                float strength)
{
    if (bayer.isEmpty()) {
        return;
    }

    const float amount = math::saturate(strength);
    if (amount <= 1e-4f) {
        return;
    }

    // 强度在单位矩阵与标定矩阵之间线性插值：
    // 强度 0 = 不补偿，强度 1 = 完全按标定矩阵解耦。
    Matrix3x3 effective = identityMatrix3x3();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float identityValue = (r == c) ? 1.0f : 0.0f;
            effective[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] =
                identityValue
                + amount * (decouplingMatrix[static_cast<std::size_t>(r)]
                                            [static_cast<std::size_t>(c)]
                            - identityValue);
        }
    }

    // 尚未标定时解耦矩阵就是单位阵，插值后仍是单位阵：
    // 不做任何遍历（高像素机型的拖动预览对此很敏感）。
    if (isIdentityMatrix(effective)) {
        return;
    }

    if (!applyBayerLinearMatrix(bayer, effective).isOk()) {
        return;
    }

    // 解耦矩阵可能产生轻微负值或超过白电平的结果。
    // 负的传感器响应没有物理意义，会污染后续白平衡与插值，因此夹取到 [0,1]。
    const std::size_t count = bayer.sampleCount();
    float* data = bayer.data.data();
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
    {
        data[i] = math::saturate(data[i]);
    }
}

void applyFbddDenoise(BayerImage& bayer, float strength)
{
    const float amount = math::saturate(strength);
    if (amount <= 1e-4f || bayer.width < 5 || bayer.height < 5) {
        return;
    }

    const int width = bayer.width;
    const int height = bayer.height;

    // 先复制一份只读源数据：就地修改会污染邻域统计，导致误差沿图像扩散。
    const std::vector<float> source = bayer.data;

    // X-Trans：四个方向上的最近同色采样距离不固定（1~3 px），
    // 逐方向扫描取最近同色点，再沿用同一套中值 + 强度混合逻辑。
    if (bayer.usesGenericCfa()) {
        RAWCORE_PARALLEL_FOR(y, height)
        {
            if (y < 2 || y >= height - 2) {
                continue;
            }

            for (int x = 2; x < width - 2; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const int ownChannel = bayer.channelAt(x, y);

                float neighbors[4];
                int neighborCount = 0;
                const int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& direction : directions) {
                    for (int d = 1; d <= 3; ++d) {
                        const int nx = x + direction[0] * d;
                        const int ny = y + direction[1] * d;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                            break;
                        }
                        if (bayer.channelAt(nx, ny) == ownChannel) {
                            neighbors[neighborCount++] =
                                source[static_cast<std::size_t>(ny) * width + nx];
                            break;
                        }
                    }
                }

                if (neighborCount < 2) {
                    continue;
                }

                float sum = 0.0f;
                float minimum = neighbors[0];
                float maximum = neighbors[0];
                for (int k = 0; k < neighborCount; ++k) {
                    sum += neighbors[k];
                    minimum = std::min(minimum, neighbors[k]);
                    maximum = std::max(maximum, neighbors[k]);
                }
                // 中位数：n=2 时取均值；n=3/4 时用 (总和 − 极值) / (n − 2)
                float median = 0.0f;
                if (neighborCount == 2) {
                    median = 0.5f * sum;
                } else {
                    median = (sum - minimum - maximum)
                        / static_cast<float>(neighborCount - 2);
                }

                float deviationSum = 0.0f;
                for (int k = 0; k < neighborCount; ++k) {
                    deviationSum += std::fabs(neighbors[k] - median);
                }
                const float localVariation = deviationSum / static_cast<float>(neighborCount);

                const float tolerance = std::max(0.002f, localVariation * 2.5f);
                const float center = source[index];
                const float deviation = std::fabs(center - median);
                if (deviation <= tolerance) {
                    continue;
                }

                const float excess = (deviation - tolerance) / (tolerance + 1e-6f);
                const float blend = amount * math::saturate(excess);
                bayer.data[index] = math::lerp(center, median, blend);
            }
        }
        return;
    }

    RAWCORE_PARALLEL_FOR(y, height)
    {
        // 最外两圈没有完整的同色邻域，保持原值。
        if (y < 2 || y >= height - 2) {
            continue;
        }

        for (int x = 2; x < width - 2; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const float center = source[index];

            // 四方向距离 2 的邻点与中心处于同一 CFA 通道。
            float neighbors[4] = {
                source[index - 2],
                source[index + 2],
                source[index - static_cast<std::size_t>(width) * 2u],
                source[index + static_cast<std::size_t>(width) * 2u],
            };

            // 4 个数的中位数 = (总和 − 最小值 − 最大值) / 2，
            // 比排序更快；这是预览高频路径，值得避免 std::sort。
            float sum = 0.0f;
            float minimum = neighbors[0];
            float maximum = neighbors[0];
            for (float value : neighbors) {
                sum += value;
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
            const float median = 0.5f * (sum - minimum - maximum);

            // 局部波动：四个邻点相对中值的平均绝对偏差。
            float deviationSum = 0.0f;
            for (float value : neighbors) {
                deviationSum += std::fabs(value - median);
            }
            const float localVariation = deviationSum * 0.25f;

            // 容差随局部波动自适应：平坦区只压明显离群点，
            // 强纹理区则允许更大的自然差异（与坏点修复同一思路，阈值更保守）。
            const float tolerance = std::max(0.002f, localVariation * 2.5f);
            const float deviation = std::fabs(center - median);
            if (deviation <= tolerance) {
                continue;
            }

            // 超出容差越多，越信任中值；最终混合量再乘用户强度。
            const float excess = (deviation - tolerance) / (tolerance + 1e-6f);
            const float blend = amount * math::saturate(excess);
            bayer.data[index] = math::lerp(center, median, blend);
        }
    }
}

void applyHighlightRolloff(ImageBuffer& image, float knee, float slope)
{
    if (image.isEmpty()) {
        return;
    }

    const float kneeValue = math::clamp(knee, 0.1f, 0.99f);
    const float slopeValue = math::saturate(slope);

    // 指数软膝：在拐点处一阶连续（斜率恰好为 1），
    // 之后平滑逼近 1.0。softness 越小，压缩越快。
    const float softness = std::max(0.08f, 1.0f - 0.92f * slopeValue);
    const float span = std::max(1e-4f, 1.0f - kneeValue);

    const std::size_t samples = image.sampleCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(samples))
    {
        const float value = data[i];
        if (value <= kneeValue || value <= 0.0f) {
            continue;
        }
        const float t = (value - kneeValue) / span;
        data[i] = kneeValue + span * (1.0f - std::exp(-t / softness));
    }
}

void applyLateralChromaticAberration(ImageBuffer& image,
                                     float redScale,
                                     float blueScale,
                                     float radialStrength)
{
    if (image.isEmpty()) {
        return;
    }

    const float redFactor = math::clamp(redScale, 0.9f, 1.1f);
    const float blueFactor = math::clamp(blueScale, 0.9f, 1.1f);
    // 需求 v2 §1.4 给定范围 -0.008 ~ +0.008：
    // 四角在基础缩放之外再额外缩放至多 ±0.8%。
    const float radial = math::clamp(radialStrength, -0.008f, 0.008f);

    const bool radialActive = std::fabs(radial) > 1e-7f;
    const bool redActive = std::fabs(redFactor - 1.0f) > 1e-6f || radialActive;
    const bool blueActive = std::fabs(blueFactor - 1.0f) > 1e-6f || radialActive;
    if (!redActive && !blueActive) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    const float* data = image.data();

    // 保存原始 R / B 平面，采样时不能读取已被改写的像素。
    std::vector<float> originalRed(pixels);
    std::vector<float> originalBlue(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        originalRed[static_cast<std::size_t>(i)] = data[static_cast<std::size_t>(i) * 3u];
        originalBlue[static_cast<std::size_t>(i)] = data[static_cast<std::size_t>(i) * 3u + 2u];
    }

    const float centerX = 0.5f * static_cast<float>(width - 1);
    const float centerY = 0.5f * static_cast<float>(height - 1);
    const float maxRadius = std::sqrt(centerX * centerX + centerY * centerY);
    const float inverseRadiusSquared =
        (maxRadius > 1e-6f) ? (1.0f / (maxRadius * maxRadius)) : 0.0f;

    float* out = image.data();

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;

            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;

            // 归一化半径平方：中心 0，四角约 1。
            const float radiusSquared = (dx * dx + dy * dy) * inverseRadiusSquared;

            // 径向增强：把"校正量"本身按 (1 + radial × r²) 放大，
            // 中心为 1（不改变基础校正），四角最强。
            // radial 是相对比例（±0.008），因此与图像分辨率无关。
            const float correctionGain = 1.0f + radial * radiusSquared;

            // 采样位置 = 原点 + d × [1 + (1/scale - 1) × correctionGain]
            // 这样无论 scale 大于还是小于 1，正向 radial 都会加强边缘校正，
            // 而不是把缩放比例本身直接乘上权重（那会在一侧减弱校正）。
            if (redActive) {
                // 以图像中心为原点做亚像素缩放。
                // redFactor > 1 时采样半径变小，相当于把 R 通道整体放大。
                const float redSampleGain =
                    1.0f + (1.0f / redFactor - 1.0f) * correctionGain;
                const float sampleX = centerX + dx * redSampleGain;
                const float sampleY = centerY + dy * redSampleGain;
                out[index * 3u] =
                    samplePlaneBilinear(originalRed, width, height, sampleX, sampleY);
            }

            if (blueActive) {
                const float blueSampleGain =
                    1.0f + (1.0f / blueFactor - 1.0f) * correctionGain;
                const float sampleX = centerX + dx * blueSampleGain;
                const float sampleY = centerY + dy * blueSampleGain;
                out[index * 3u + 2u] =
                    samplePlaneBilinear(originalBlue, width, height, sampleX, sampleY);
            }
        }
    }
}

void applyLongitudinalChromaticAberration(ImageBuffer& image,
                                          float strength,
                                          float highlightThreshold)
{
    const float amount = math::saturate(strength);
    if (image.isEmpty() || amount <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    const float* data = image.data();

    const float threshold = math::clamp(highlightThreshold, 0.05f, 1.0f);
    const float scale = filterScaleFor(width, height);
    const float radiusSmall = std::max(1.0f, 2.5f * scale);
    const float radiusLarge = radiusSmall * 2.0f;

    std::vector<float> luma(pixels);
    std::vector<float> chromaRed(pixels);
    std::vector<float> chromaBlue(pixels);

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float r = data[index * 3u];
        const float g = data[index * 3u + 1u];
        const float b = data[index * 3u + 2u];
        luma[index] = 0.299f * r + 0.587f * g + 0.114f * b;
        chromaRed[index] = r - g;
        chromaBlue[index] = b - g;
    }

    // DoG：小半径模糊与大半径模糊之差，正响应对应点状 / 小面积高光。
    std::vector<float> lumaSmall;
    std::vector<float> lumaLarge;
    std::vector<float> chromaRedBlurred;
    std::vector<float> chromaBlueBlurred;
    gaussianBlurPlane(luma, lumaSmall, width, height, radiusSmall);
    gaussianBlurPlane(luma, lumaLarge, width, height, radiusLarge);
    gaussianBlurPlane(chromaRed, chromaRedBlurred, width, height, radiusSmall);
    gaussianBlurPlane(chromaBlue, chromaBlueBlurred, width, height, radiusSmall);

    float* out = image.data();
    const float highlightUpper = std::min(1.0f, threshold + 0.2f);

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float bright = lumaSmall[index];

        // 高光门槛：只处理足够亮的区域。
        const float highlight = smoothStep01(threshold, highlightUpper, bright);
        if (highlight <= 1e-4f) {
            continue;
        }

        // 相对 DoG：除以局部亮度，使掩码不受曝光绝对值影响。
        const float reference = std::max(bright, 1e-4f);
        const float dog = (lumaSmall[index] - lumaLarge[index]) / reference;
        const float blob = smoothStep01(0.0f, 0.18f, dog);
        const float mask = amount * highlight * blob;
        if (mask <= 1e-4f) {
            continue;
        }

        // 色差向局部平滑值靠拢：保留大面积的真实色彩，
        // 只削弱光斑边缘的高频色散。
        // G 通道不修改，因此用当前 G 重建 R / B。
        const float g = out[index * 3u + 1u];
        out[index * 3u] =
            g + math::lerp(chromaRed[index], chromaRedBlurred[index], mask);
        out[index * 3u + 2u] =
            g + math::lerp(chromaBlue[index], chromaBlueBlurred[index], mask);
    }
}

void suppressEdgeFalseColor(ImageBuffer& image,
                            float strength,
                            float edgeSensitivity,
                            int smoothingRadius)
{
    const float amount = math::saturate(strength);
    if (image.isEmpty() || amount <= 1e-4f
        || image.width() < 3 || image.height() < 3) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    const float* data = image.data();

    const float sensitivity = math::saturate(edgeSensitivity);
    // 敏感度越高，阈值越低：0 只处理强边缘，1 覆盖较弱边缘。
    const float threshold = math::lerp(0.16f, 0.012f, sensitivity);
    const float upperThreshold = threshold * 3.0f + 1e-4f;

    // 平滑半径按渲染分辨率自适应：预览用小半径、导出用大半径，
    // 使两者在归一化尺寸上具有相同的平滑范围。
    const float scaledRadius =
        static_cast<float>(std::max(1, smoothingRadius)) * filterScaleFor(width, height);
    // 上限 6：避免极端配置下每像素取样过多，同时保证邻域数组可放在栈上。
    const int radius = std::clamp(static_cast<int>(std::lround(scaledRadius)), 1, 6);

    // 边缘掩码与半径无关，但邻域越界判断需要留出足够边距。
    const int margin = radius;

    // ---- 第一遍：用 G 通道 Sobel 梯度生成边缘掩码 ----
    std::vector<float> edgeMask(pixels, 0.0f);
    std::size_t maskedCount = 0;

    RAWCORE_PARALLEL_FOR(y, height)
    {
        if (y < 1 || y >= height - 1) {
            continue;
        }

        std::size_t rowMasked = 0;

        for (int x = 1; x < width - 1; ++x) {
            const auto greenAt = [&](int sx, int sy) -> float {
                return data[(static_cast<std::size_t>(sy) * width + sx) * 3u + 1u];
            };

            // Sobel 算子：对绿色通道求梯度。
            const float gx =
                (greenAt(x + 1, y - 1) + 2.0f * greenAt(x + 1, y) + greenAt(x + 1, y + 1))
                - (greenAt(x - 1, y - 1) + 2.0f * greenAt(x - 1, y) + greenAt(x - 1, y + 1));
            const float gy =
                (greenAt(x - 1, y + 1) + 2.0f * greenAt(x, y + 1) + greenAt(x + 1, y + 1))
                - (greenAt(x - 1, y - 1) + 2.0f * greenAt(x, y - 1) + greenAt(x + 1, y - 1));

            // 0.25 把 Sobel 的 4 倍增益归一化回单像素梯度量级。
            const float magnitude = 0.25f * std::sqrt(gx * gx + gy * gy);
            const float mask = smoothStep01(threshold, upperThreshold, magnitude);

            edgeMask[static_cast<std::size_t>(y) * width + x] = mask;
            if (mask > 1e-4f) {
                ++rowMasked;
            }
        }

        // 每个线程统计自己的行，最后累加（避免原子操作）
        if (rowMasked > 0) {
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp atomic
#endif
            maskedCount += rowMasked;
        }
    }

    // 平坦图像没有任何边缘，直接返回，省掉大块内存分配。
    if (maskedCount == 0) {
        return;
    }

    // ---- 第二遍：仅在边缘处对色差（R−G / B−G）做中值平滑 ----
    //
    // 关键差异：伪色本质是"色度异常"而不是"亮度异常"。直接对 R / B 取中值
    // 会把边缘上的真实亮度细节一起抹平，画面发软；改为只处理色差平面，
    // G 通道完全不动，等于在保持亮度结构的前提下压缩色度噪声。
    // 复制原始色差，避免读取已被改写的邻点。
    std::vector<float> originalChromaR(pixels);
    std::vector<float> originalChromaB(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float green = data[index * 3u + 1u];
        originalChromaR[index] = data[index * 3u] - green;
        originalChromaB[index] = data[index * 3u + 2u] - green;
    }

    // 半径上限 6 → (2×6+1)² = 169 个样本；放在栈上避免逐像素堆分配。
    constexpr int kMaxSamples = 13 * 13;

    float* out = image.data();

    RAWCORE_PARALLEL_FOR(y, height)
    {
        if (y < margin || y >= height - margin) {
            continue;
        }

        for (int x = margin; x < width - margin; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const float mask = edgeMask[index];
            if (mask <= 1e-4f) {
                continue;
            }

            float redSamples[kMaxSamples];
            float blueSamples[kMaxSamples];
            int sampleIndex = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const std::size_t neighbor =
                        (static_cast<std::size_t>(y + dy) * width + (x + dx));
                    redSamples[sampleIndex] = originalChromaR[neighbor];
                    blueSamples[sampleIndex] = originalChromaB[neighbor];
                    ++sampleIndex;
                }
            }

            // 奇数个样本，取中间位置即可得到中值；
            // nth_element 只做部分排序，比完整排序更快。
            const int medianIndex = sampleIndex / 2;
            std::nth_element(redSamples, redSamples + medianIndex,
                             redSamples + sampleIndex);
            std::nth_element(blueSamples, blueSamples + medianIndex,
                             blueSamples + sampleIndex);

            const float blend = amount * mask;
            // G 通道保持不变（out[index*3+1] 不做任何写入），
            // 只把色差向邻域中值靠拢，再还原成 R / B。
            // 色差跨亮度边缘混合后可能把 R / B 推过动态范围，
            // 这里夹回 [0, 1]，避免产生新的高光过冲。
            const float green = data[index * 3u + 1u];
            out[index * 3u] = math::clamp(
                green + math::lerp(originalChromaR[index],
                                   redSamples[medianIndex], blend),
                0.0f, 1.0f);
            out[index * 3u + 2u] = math::clamp(
                green + math::lerp(originalChromaB[index],
                                   blueSamples[medianIndex], blend),
                0.0f, 1.0f);
        }
    }
}

// ===========================================================================
//  4. 白平衡
// ===========================================================================

namespace {

/// 计算"把指定色温/色调的光源白点补偿回工作空间 D50 白点"的三通道增益。
/// 增益按亮度归一化，保证调色温不会明显改变曝光。
///
/// 说明：输出白点直接取 xyzToProPhoto 对目标白点的响应，因此对 RAW 的
/// as-shot 色温（由同一条黑体轨迹反算）可以做到精确补偿。
void computeWhiteBalanceGain(float temperature, float tint, float outGain[3])
{
    // 色温/色调模式：把"参考白点（D50，工作空间基准）"映射到"目标白点"。
    //
    // 工作空间 ProPhotoRGB 的白点是 D50。用户指定的色温描述的是
    // 拍摄场景的光源颜色，需要对其进行补偿，使该光源下的中性物体
    // 在输出中呈现为中性。
    float targetXyz[3];
    math::whitePointFromTemperatureTint(temperature, tint, targetXyz);

    // ProPhotoRGB 三通道对 XYZ 的响应：用标准 ProPhoto 原色矩阵的列。
    // D50 参考白点下的三通道响应均为 1.0，目标是让 targetXyz 也变为 1.0。
    const Matrix3x3 xyzToProPhoto = {{
        {{ 1.3459433f, -0.2556075f, -0.0511118f}},
        {{-0.5445989f,  1.5081673f,  0.0205351f}},
        {{ 0.0000000f,  0.0000000f,  1.2118128f}},
    }};

    float targetProPhoto[3];
    math::multiply(xyzToProPhoto, targetXyz, targetProPhoto);

    const float epsilon = 1e-6f;
    for (int c = 0; c < 3; ++c) {
        // 增益 = 参考（D50 下为 1.0）/ 当前响应
        outGain[c] = (std::fabs(targetProPhoto[c]) > epsilon)
            ? (1.0f / targetProPhoto[c]) : 1.0f;
    }

    // 归一化增益，避免整体亮度随色温剧烈变化
    // （否则调色温会同时改变曝光，手感很差）
    const float lumaGain = 0.2880402f * outGain[0] + 0.7118741f * outGain[1]
        + 0.0000857f * outGain[2];
    if (lumaGain > epsilon) {
        for (int c = 0; c < 3; ++c) {
            outGain[c] /= lumaGain;
        }
    }
}

}  // namespace

void applyDemosaicSpatialStrategy(ImageBuffer& image,
                                  const DemosaicSpatialSettings& settings,
                                  float pixelScale)
{
    if (image.isEmpty() || settings.isIdentity()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // 感知域色差，与边缘伪色抑制保持同一尺度
    std::vector<float> green(pixels);
    std::vector<float> diffR(pixels);
    std::vector<float> diffB(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float g = std::sqrt(std::max(0.0f, data[index * 3u + 1u]));
        green[index] = g;
        diffR[index] = std::sqrt(std::max(0.0f, data[index * 3u])) - g;
        diffB[index] = std::sqrt(std::max(0.0f, data[index * 3u + 2u])) - g;
    }

    const float threshold = 0.005f + std::clamp(settings.edgeThreshold / 100.0f,
                                                0.0f, 1.0f) * 0.12f;
    const float protect = std::clamp(settings.protectStrength, 0.0f, 1.0f);
    const float suppress = std::clamp(settings.suppressStrength, 0.0f, 1.0f);
    const float scale = std::clamp(pixelScale, 0.2f, 2.0f);
    const auto brushWeight = [&](const std::vector<MaskStrokePoint>& strokes,
                                 float u, float v) {
        float weight = 0.0f;
        for (const MaskStrokePoint& point : strokes) {
            const float dx = u - point.x;
            const float dy = v - point.y;
            const float radius = std::max(1e-4f, point.radius * scale);
            const float distance = std::sqrt(dx * dx + dy * dy) / radius;
            if (distance < 1.0f) {
                weight = std::max(weight, (1.0f - distance) * point.flow);
            }
        }
        return weight;
    };

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const int xl = std::max(0, x - 1);
            const int xr = std::min(width - 1, x + 1);
            const int yu = std::max(0, y - 1);
            const int yd = std::min(height - 1, y + 1);
            const float gx = green[static_cast<std::size_t>(y) * width + xr]
                - green[static_cast<std::size_t>(y) * width + xl];
            const float gy = green[static_cast<std::size_t>(yd) * width + x]
                - green[static_cast<std::size_t>(yu) * width + x];
            const float gradient = std::sqrt(gx * gx + gy * gy);
            const float edge = math::saturate(gradient / std::max(threshold, 1e-6f));

            const float u = (static_cast<float>(x) + 0.5f) / width;
            const float v = (static_cast<float>(y) + 0.5f) / height;
            const float protectBrush = brushWeight(settings.protectStrokes, u, v);
            const float suppressBrush = brushWeight(settings.suppressStrokes, u, v);

            float amount = suppress * (0.25f + 0.75f * (1.0f - edge));
            amount *= (1.0f - protect * protectBrush * 0.9f);
            amount += suppressBrush * 0.8f;
            amount = std::clamp(amount, 0.0f, 1.0f);
            if (amount <= 1e-4f) {
                continue;
            }

            // 3x3 色差中值
            float valuesR[9];
            float valuesB[9];
            int count = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = clampIndex(y + dy, 0, height - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = clampIndex(x + dx, 0, width - 1);
                    const std::size_t n = static_cast<std::size_t>(ny) * width + nx;
                    valuesR[count] = diffR[n];
                    valuesB[count] = diffB[n];
                    ++count;
                }
            }
            std::sort(valuesR, valuesR + count);
            std::sort(valuesB, valuesB + count);
            const float medianR = valuesR[count / 2];
            const float medianB = valuesB[count / 2];

            const float g = green[index];
            const float r = g + math::lerp(diffR[index], medianR, amount);
            const float b = g + math::lerp(diffB[index], medianB, amount);
            data[index * 3u] = std::max(0.0f, r * r);
            data[index * 3u + 2u] = std::max(0.0f, b * b);
        }
    }
}

void applyWhiteBalance(ImageBuffer& image,
                       float temperature,
                       float tint,
                       float grayPickX,
                       float grayPickY,
                       bool useGrayPick,
                       float referenceTemperature,
                       float referenceTint,
                       bool normalizeReferenceLuma)
{
    if (image.isEmpty()) {
        return;
    }

    float gain[3] = {1.0f, 1.0f, 1.0f};

    if (useGrayPick && grayPickX >= 0.0f && grayPickY >= 0.0f) {
        // 灰点拾取模式：把采样点的颜色归一化为白点。
        // 直接求倒数得到增益，这是最直观的"点哪里哪里变灰"实现。
        const std::array<float, 3> gray = samplePixel(image, grayPickX, grayPickY);
        const float epsilon = 1e-6f;

        if (gray[0] > epsilon && gray[1] > epsilon && gray[2] > epsilon) {
            // 以绿色为基准保持整体亮度不变（绿色贡献最大亮度）
            const float reference = gray[1];
            gain[0] = reference / gray[0];
            gain[1] = 1.0f;
            gain[2] = reference / gray[2];
        }
    } else {
        computeWhiteBalanceGain(temperature, tint, gain);

        // referenceTemperature > 0：把该色温视为"已经中性的基准"，
        // 只补偿它与当前色温的差异。普通位图（JPG / PNG …）的像素已经
        // 做过白平衡，工作空间里中性像素就是 (v, v, v)；若直接套用绝对
        // 增益，色温拟合公式在 5000K 处与 D50 的细微偏差会给整幅图
        // 压上一层约 1% 的偏色。以 5000K 为基准后，该处增益严格为 1。
        if (referenceTemperature > 0.0f) {
            float referenceGain[3] = {1.0f, 1.0f, 1.0f};
            computeWhiteBalanceGain(referenceTemperature, referenceTint, referenceGain);
            for (int c = 0; c < 3; ++c) {
                gain[c] /= (std::fabs(referenceGain[c]) > 1e-6f)
                    ? referenceGain[c] : 1.0f;
            }

            // 单独使用相对校正时，再归一化一次亮度；
            // 若调用方已经先应用了基准增益，则必须保留原比例，
            // 否则"基准 × 相对"不再等于目标白点的绝对增益。
            if (normalizeReferenceLuma) {
                const float lumaGain = 0.2880402f * gain[0]
                    + 0.7118741f * gain[1] + 0.0000857f * gain[2];
                if (lumaGain > 1e-6f) {
                    for (int c = 0; c < 3; ++c) {
                        gain[c] /= lumaGain;
                    }
                }
            }
        }
    }

    // 应用增益（线性域对角缩放，符合线性光运算要求）
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        pixel[0] *= gain[0];
        pixel[1] *= gain[1];
        pixel[2] *= gain[2];
    }
}

std::array<float, 3> samplePixel(const ImageBuffer& image, float normalizedX, float normalizedY)
{
    std::array<float, 3> result{{0.0f, 0.0f, 0.0f}};
    if (image.isEmpty()) {
        return result;
    }

    const int centerX = clampIndex(static_cast<int>(normalizedX * image.width()), 0, image.width() - 1);
    const int centerY = clampIndex(static_cast<int>(normalizedY * image.height()), 0, image.height() - 1);

    // 取 3x3 邻域平均，降低单点噪声对拾取结果的影响
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int count = 0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = centerX + dx;
            const int y = centerY + dy;
            if (x < 0 || x >= image.width() || y < 0 || y >= image.height()) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                sum[c] += image.at(x, y, c);
            }
            ++count;
        }
    }

    if (count > 0) {
        for (int c = 0; c < 3; ++c) {
            result[static_cast<std::size_t>(c)] = sum[c] / static_cast<float>(count);
        }
    }
    return result;
}

bool grayPointToTemperatureTint(const std::array<float, 3>& grayRgb,
                                float& outTemperature,
                                float& outTint)
{
    // 由灰点的 ProPhotoRGB 值反推白点色度，再转换为色温/色调。
    // 步骤：ProPhoto → XYZ → xy 色度 → 色温（McCamy 近似）
    //
    // ProPhoto→XYZ 直接对管线使用的 XYZ→ProPhoto 矩阵求逆，
    // 保证两处严格互逆。手写常数容易在不同资料的不同归一化约定间出错。
    const Matrix3x3 xyzToProPhoto = {{
        {{ 1.3459433f, -0.2556075f, -0.0511118f}},
        {{-0.5445989f,  1.5081673f,  0.0205351f}},
        {{ 0.0000000f,  0.0000000f,  1.2118128f}},
    }};

    Matrix3x3 proPhotoToXyz;
    if (!math::invert(xyzToProPhoto, proPhotoToXyz)) {
        return false;  // 理论上不可能失败，防御性处理
    }

    float xyz[3];
    math::multiply(proPhotoToXyz, grayRgb.data(), xyz);

    const float sum = xyz[0] + xyz[1] + xyz[2];
    if (sum <= 1e-6f) {
        return false;
    }

    const float x = xyz[0] / sum;
    const float y = xyz[1] / sum;

    const float kelvin = math::xyToTemperature(x, y);
    if (kelvin < 0.0f) {
        return false;
    }

    outTemperature = kelvin;

    // 色调：以该色温下黑体轨迹的 y 为基准，实际 y 偏高则偏绿（负 tint）、
    // 偏低则偏品红（正 tint）。方向与 Lightroom 手感保持一致。
    float blackbodyX = 0.0f;
    float blackbodyY = 0.0f;
    math::temperatureToXy(kelvin, blackbodyX, blackbodyY);

    // 0.0005 是把 y 偏差转换为 tint 单位的经验系数
    outTint = math::clamp((y - blackbodyY) / 0.0005f * -1.0f, -100.0f, 100.0f);

    return true;
}

// ===========================================================================
//  4.5 预览影调基准（自动曝光归一 + 弱 S 曲线）
// ===========================================================================

namespace {

/// 自动曝光直方图的分档数。8192 档覆盖 0 ~ 8 的线性亮度，
/// 分档宽度约 0.001，对乘性的曝光增益而言精度足够。
constexpr int kAutoExposureBinCount = 8192;

/// 直方图覆盖的最大线性亮度（约 +3 EV 白点）。
constexpr float kAutoExposureMaxLuma = 8.0f;

/// 自动曝光允许抬到的中位亮度上限（线性 ProPhoto 亮度）。
///
/// 99.5% 分位归一只回答"白点在哪里"，并不判断画面整体应该有多亮。
/// 遇到大面积高调的场景（阴天天空、雾、雪）时，白点与中位亮度相差很小，
/// 按白点归一会把整个中间调推到接近纯白，暗部随之抬起、层次丢失，
/// 画面看起来发灰、不通透。这里再用中位亮度封一次顶。
///
/// 0.32 约对应 sRGB 编码 0.6（8 位 153），是日常场景中间调的合理上限。
/// 该限制只压缩**提亮**，本身已经比目标亮的画面不会被强行压暗。
constexpr float kAutoExposureMedianCap = 0.32f;

}  // namespace

float computeAutoExposureScale(const ImageBuffer& image, float percentile)
{
    if (image.isEmpty()) {
        return 1.0f;
    }

    percentile = math::clamp(percentile, 0.5f, 1.0f);

    // 用固定大小的直方图统计亮度分位：只线性扫描一遍、内存与图像尺寸无关，
    // 全尺寸导出（4500 万像素）也不会为了求分位额外分配几十 MB 样本数组。
    //
    // 这一遍扫描在每次预览渲染（包括只改曝光/白平衡的缓存热渲染）里都要做，
    // 4500 万像素单线程要上百毫秒，因此按线程各自累计直方图后再归并。
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
    const int workerCount = std::max(1, omp_get_max_threads());
#else
    const int workerCount = 1;
#endif

    std::vector<std::uint32_t> histogram(
        static_cast<std::size_t>(kAutoExposureBinCount) * static_cast<std::size_t>(workerCount), 0u);
    std::vector<PaddedSlot<std::uint64_t>> partialCount(static_cast<std::size_t>(workerCount));

    const std::size_t pixels = image.pixelCount();
    const float* data = image.data();
    const float binScale =
        static_cast<float>(kAutoExposureBinCount - 1) / kAutoExposureMaxLuma;

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
        const int worker = omp_get_thread_num();
#else
        const int worker = 0;
#endif
        std::uint32_t* localHistogram =
            histogram.data() + static_cast<std::size_t>(worker) * kAutoExposureBinCount;

        const float* pixel = data + static_cast<std::size_t>(i) * 3u;
        const float luma = luminanceOf(pixel[0], pixel[1], pixel[2]);
        if (!std::isfinite(luma)) {
            continue;
        }

        // 先在浮点域夹取再转 int：异常的大/小亮度直接转 int 是未定义行为
        const float scaled = math::clamp(
            luma * binScale, 0.0f, static_cast<float>(kAutoExposureBinCount - 1));
        const int bin = static_cast<int>(scaled);
        ++localHistogram[bin];
        ++partialCount[static_cast<std::size_t>(worker)].value;
    }

    // 归并到第 0 份直方图
    for (int worker = 1; worker < workerCount; ++worker) {
        const std::uint32_t* localHistogram =
            histogram.data() + static_cast<std::size_t>(worker) * kAutoExposureBinCount;
        for (int bin = 0; bin < kAutoExposureBinCount; ++bin) {
            histogram[static_cast<std::size_t>(bin)] += localHistogram[bin];
        }
    }

    std::uint64_t validCount = 0;
    for (const PaddedSlot<std::uint64_t>& count : partialCount) {
        validCount += count.value;
    }

    if (validCount < 16u) {
        return 1.0f;  // 有效像素太少，统计没有意义
    }

    // 累计到 percentile 所在的分档。
    // 用四舍五入而不是向上取整：percentile 是二进制浮点，0.995 * 1000
    // 会算成 995.0000047，向上取整会跳过一个像素，把 0.5% 的极端高光
    // 误当成白点——分位数统计的意义就没了。
    const std::uint64_t targetCount = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(percentile) * static_cast<double>(validCount)));

    std::uint64_t cumulative = 0;
    int whiteBin = kAutoExposureBinCount - 1;
    for (int b = 0; b < kAutoExposureBinCount; ++b) {
        cumulative += histogram[static_cast<std::size_t>(b)];
        if (cumulative >= targetCount) {
            whiteBin = b;
            break;
        }
    }

    // 取该档的上界作为白点：宁可略低一点增益，也不把高光推得过狠。
    float whitePoint = static_cast<float>(whiteBin + 1) / binScale;
    whitePoint = std::min(whitePoint, kAutoExposureMaxLuma);
    if (!(whitePoint > 1e-5f)) {
        return 1.0f;  // 全黑图像，无从归一
    }

    float scale = 1.0f / whitePoint;

    // 中位亮度封顶：大面积高调场景不能只按白点归一，否则整幅会被抬成灰白。
    // 与白点共用同一份直方图，额外开销只是一次 8192 档的累加。
    std::uint64_t medianCumulative = 0;
    int medianBin = 0;
    const std::uint64_t medianCount = validCount / 2;
    for (int b = 0; b < kAutoExposureBinCount; ++b) {
        medianCumulative += histogram[static_cast<std::size_t>(b)];
        if (medianCumulative >= medianCount) {
            medianBin = b;
            break;
        }
    }

    const float medianLuma =
        (static_cast<float>(medianBin) + 0.5f) / binScale;
    if (medianLuma > 1e-5f) {
        const float capScale = kAutoExposureMedianCap / medianLuma;
        // 只限制提亮：capScale 小于 1 说明画面本来就比目标亮，不要主动压暗
        scale = std::min(scale, std::max(1.0f, capScale));
    }

    return math::clamp(scale, 1.0f / 8.0f, 8.0f);  // 限制在约 ±3 EV
}

void applyPreviewTone(ImageBuffer& image, float exposureScale, bool applySCurve)
{
    if (image.isEmpty()) {
        return;
    }

    if (!std::isfinite(exposureScale) || exposureScale <= 0.0f) {
        exposureScale = 1.0f;
    }

    const bool needExposure = std::fabs(exposureScale - 1.0f) > 1e-4f;
    if (!needExposure && !applySCurve) {
        return;
    }

    // 预览 S 曲线强度。用户对比度滑块满量程对应 0.75，
    // 这里取 0.7：足以补回线性数据缺少的暗部脚趾与亮部肩部，
    // 让默认预览接近相机直出的通透感，又不会盖过用户自己的对比度设置。
    //
    // 之前取 0.18 时在感知中灰附近只偏移不到 1 个 8 位级别，
    // 实际等于没有影调曲线，自动曝光抬亮后的画面会显得发灰。
    constexpr float kSCurveStrength = 0.7f;

    // 把「toPerceptual → 三次 S 曲线 → fromPerceptual」合成成一条查表曲线。
    // 这条曲线只依赖曝光后的线性值，逐像素调用 pow / exp 在 4500 万像素规模上
    // 要两百多毫秒；预计算成 LUT + 线性插值后降到几毫秒，结果差异在 1e-6 量级。
    constexpr int kToneLutEntries = 8192;
    std::vector<float> toneLut;
    if (applySCurve) {
        toneLut.resize(static_cast<std::size_t>(kToneLutEntries) + 1);
        for (int entry = 0; entry <= kToneLutEntries; ++entry) {
            const float linear = static_cast<float>(entry) / static_cast<float>(kToneLutEntries);
            float perceptual = toPerceptual(linear);
            if (perceptual < 1.0f) {
                perceptual += kSCurveStrength * perceptual * (1.0f - perceptual)
                              * (2.0f * perceptual - 1.0f);
                perceptual = math::saturate(perceptual);
            }
            toneLut[static_cast<std::size_t>(entry)] = fromPerceptual(perceptual);
        }
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        if (needExposure) {
            pixel[0] *= exposureScale;
            pixel[1] *= exposureScale;
            pixel[2] *= exposureScale;
        }

        if (!applySCurve) {
            continue;
        }

        // 与 applyLightAdjustments 的对比度相同的三次多项式 S 曲线，
        // 但逐通道施加在感知域上：不改变白点、不改变整体色相。
        // 查表版本对白点以上（含）的像素保持原值，交给管线末尾的软膝压缩。
        for (int c = 0; c < 3; ++c) {
            const float linear = pixel[c];
            if (!(linear > 0.0f)) {
                pixel[c] = 0.0f;  // 负值按原有逻辑压到 0
                continue;
            }
            if (linear >= 1.0f) {
                continue;
            }
            const float position = linear * static_cast<float>(kToneLutEntries);
            const int index = static_cast<int>(position);
            const float fraction = position - static_cast<float>(index);
            const float lower = toneLut[static_cast<std::size_t>(index)];
            const float upper = toneLut[static_cast<std::size_t>(index) + 1];
            pixel[c] = lower + (upper - lower) * fraction;
        }
    }
}

// ===========================================================================
//  5. 光影调整
// ===========================================================================

void applyLightAdjustments(ImageBuffer& image, const LightSettings& settings)
{
    if (image.isEmpty()) {
        return;
    }

    // ---- 预处理：把参数转换为实际运算系数 ----

    // 曝光：线性域直接乘以 2^EV，这是线性光下的正确做法
    const float exposureGain = std::pow(2.0f, settings.exposure);

    // 高光 / 阴影：-100~100 映射到 0~1 的强度
    const float highlightAmount = settings.highlight / 100.0f;
    const float shadowAmount = settings.shadow / 100.0f;

    // 对比度：-100~100 映射到 -1~1，再转为 S 曲线的强度
    const float contrastAmount = settings.contrast / 100.0f;

    // 去雾：-100~100
    const float dehazeAmount = settings.dehaze / 100.0f;

    // 提前判断是否需要执行各步骤，全零时整段跳过（性能优化）
    const bool needExposure = std::fabs(settings.exposure) > 1e-4f;
    const bool needHighlight = std::fabs(highlightAmount) > 1e-4f;
    const bool needShadow = std::fabs(shadowAmount) > 1e-4f;
    const bool needContrast = std::fabs(contrastAmount) > 1e-4f;
    const bool needDehaze = std::fabs(dehazeAmount) > 1e-4f;

    if (!needExposure && !needHighlight && !needShadow && !needContrast && !needDehaze) {
        return;  // 无任何光影调整，直接跳过
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        // ---- 1) 曝光：线性域乘法 ----
        if (needExposure) {
            pixel[0] *= exposureGain;
            pixel[1] *= exposureGain;
            pixel[2] *= exposureGain;
        }

        // ---- 2) 高光压缩与阴影提升 ----
        // 这两个操作按亮度施加蒙版：高光只影响亮部、阴影只影响暗部。
        // 蒙版基于线性亮度计算，调整量为线性域的乘性因子。
        if (needHighlight || needShadow) {
            const float luma = luminanceOf(pixel[0], pixel[1], pixel[2]);
            float multiplier = 1.0f;

            if (needHighlight) {
                // 亮度越高蒙版越强；用平方使过渡更集中在最亮部分
                const float mask = math::saturate(luma) * math::saturate(luma);
                // 正值提亮高光、负值压暗
                multiplier *= (1.0f + highlightAmount * mask * 0.8f);
            }

            if (needShadow) {
                // 亮度越低蒙版越强
                const float inverse = 1.0f - math::saturate(luma);
                const float mask = inverse * inverse;
                multiplier *= (1.0f + shadowAmount * mask * 1.2f);
            }

            // 保证乘性因子为正，避免把像素乘成负值（会产生非物理颜色）
            multiplier = std::max(multiplier, 0.0f);

            pixel[0] *= multiplier;
            pixel[1] *= multiplier;
            pixel[2] *= multiplier;
        }

        // ---- 3) 对比度与去雾 ----
        // 这两项基于感知亮度做 S 曲线，因此做临时 gamma 往返。
        // 注意：这是局部计算中介，不是管线的最终 gamma 编码。
        if (needContrast || needDehaze) {
            float perceptual[3];
            for (int c = 0; c < 3; ++c) {
                perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
            }

            if (needContrast) {
                // 围绕中灰 0.5 的 S 曲线。
                // 使用三次多项式近似 sigmoid：f(x) = x + k*x*(1-x)*(2x-1)
                // 该式在 x=0.5 处为 0（不动点），且导数随 k 增大而增大。
                const float k = contrastAmount * 0.75f;
                for (int c = 0; c < 3; ++c) {
                    const float x = perceptual[c];
                    const float s = x + k * x * (1.0f - x) * (2.0f * x - 1.0f);
                    perceptual[c] = math::saturate(s);
                }
            }

            if (needDehaze) {
                // 去雾：正量压低黑场提升通透度，负量抬升黑场模拟雾感。
                //
                // 采用"黑场平移 + 归一化"：
                //   out = (x - lift) / (1 - lift * (1 - x))
                // lift > 0 时暗部被压向 0（正的去雾），lift < 0 时黑场被抬起（加雾）。
                // 分母保证 x = 1 时输出仍为 1，白点不受影响。
                for (int c = 0; c < 3; ++c) {
                    const float x = perceptual[c];
                    const float blackLift = 0.12f * dehazeAmount;
                    const float denominator = 1.0f - blackLift * (1.0f - x);
                    if (std::fabs(denominator) < 1e-6f) {
                        perceptual[c] = x;
                        continue;
                    }
                    const float adjusted = (x - blackLift) / denominator;
                    perceptual[c] = math::saturate(adjusted);
                }
            }

            for (int c = 0; c < 3; ++c) {
                // 回到线性域。负值已在上面 clamp 过，此处不会产生 NaN。
                pixel[c] = fromPerceptual(perceptual[c]);
            }
        }
    }
}

// ===========================================================================
//  5.5 色彩与质感（A1）
// ===========================================================================

void applyPresence(ImageBuffer& image, const PresenceSettings& settings)
{
    if (image.isEmpty() || settings.isIdentity()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    const float vibranceAmount = math::clamp(settings.vibrance / 100.0f, -1.0f, 1.0f);
    const float saturationAmount = math::clamp(settings.saturation / 100.0f, -1.0f, 1.0f);
    const float clarityAmount = math::clamp(settings.clarity / 100.0f, -1.0f, 1.0f);
    const float textureAmount = math::clamp(settings.texture / 100.0f, -1.0f, 1.0f);

    const bool needSaturation = std::fabs(saturationAmount) > 1e-4f;
    const bool needVibrance = std::fabs(vibranceAmount) > 1e-4f;
    const bool needClarity = std::fabs(clarityAmount) > 1e-4f;
    const bool needTexture = std::fabs(textureAmount) > 1e-4f;

    if (!needSaturation && !needVibrance && !needClarity && !needTexture) {
        return;
    }

    // ---- 局部对比需要的亮度平面 ----
    //
    // 清晰度 / 纹理只改变亮度结构，因此在一张**感知域亮度平面**上做高通，
    // 再把同样的增量加回三个通道：色相与色度关系保持不变，不会像直接对
    // RGB 分别锐化那样放大彩噪。
    //
    // 半径按图像长边换算（filterScaleFor 以 6000px 为基准），这样 fit 预览、
    // 1:1 与全尺寸导出在高频尺度上保持一致，而不是预览被过度涂抹。
    std::vector<float> luma;
    std::vector<float> clarityBlur;
    std::vector<float> textureBlur;

    const float detailScale = filterScaleFor(width, height);

    if (needClarity || needTexture) {
        luma.resize(pixels);

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            const float* pixel = data + static_cast<std::size_t>(i) * 3u;
            const float r = toPerceptual(std::max(0.0f, pixel[0]));
            const float g = toPerceptual(std::max(0.0f, pixel[1]));
            const float b = toPerceptual(std::max(0.0f, pixel[2]));
            // 感知域亮度（Rec.709 系数），与 sRGB 显示亮度一致
            luma[static_cast<std::size_t>(i)] =
                0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        if (needClarity) {
            // 大半径支撑域：6000px 长边时约 40px，45MP 约 55px
            const float radius = std::max(1.0f, 40.0f * detailScale);
            clarityBlur.resize(pixels);
            blurPlaneLargeRadius(luma, clarityBlur, width, height, radius);
        }

        if (needTexture) {
            // 小半径：只抓织物、头发、树皮这类细节，不碰轮廓
            const float radius = std::max(0.6f, 3.0f * detailScale);
            textureBlur.resize(pixels);
            gaussianBlurPlane(luma, textureBlur, width, height, radius);
        }
    }

    // 亮度平面只用于生成两张模糊图；主循环会用逐像素 y 重新计算亮度。
    // 45MP 全尺寸渲染时一张 float 平面约 184MB，及时释放可显著降低峰值内存。
    if (!luma.empty()) {
        std::vector<float>().swap(luma);
    }

    // ---- 主循环：饱和度 / 自然饱和度 / 清晰度 / 纹理 ----
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        float perceptual[3];
        for (int c = 0; c < 3; ++c) {
            perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
        }

        // 饱和度变换保持亮度 Y 不变（变换后三通道加权和仍为 Y），
        // 因此可以先算一次 Y，后面清晰度也直接复用。
        const float y = 0.2126f * perceptual[0]
                      + 0.7152f * perceptual[1]
                      + 0.0722f * perceptual[2];

        if (needSaturation || needVibrance) {
            float scale = 1.0f;

            if (needSaturation) {
                // -100 → 0（完全去色），+100 → 2（色度翻倍）
                scale *= (1.0f + saturationAmount);
            }

            if (needVibrance) {
                float hue = 0.0f;
                float saturation = 0.0f;
                rgbToHueSaturation(perceptual[0], perceptual[1], perceptual[2],
                                   hue, saturation);

                // 低饱和颜色权重更高：这正是“自然饱和度”与普通饱和度的区别
                const float weight = math::saturate(1.0f - saturation);

                // 肤色保护：橙色（约 25°）附近减半，避免人脸先变塑料。
                // 只对真正有颜色的像素启用，灰色像素不会因色相 0° 被误判。
                float skinProtect = 1.0f;
                if (saturation > 0.06f) {
                    const float distance = hueDistance(hue, 25.0f);
                    if (distance < 30.0f) {
                        const float bell =
                            0.5f * (1.0f + std::cos(distance / 30.0f * 3.14159265f));
                        skinProtect -= 0.6f * bell;
                    }
                }

                scale *= (1.0f + vibranceAmount * weight * skinProtect);
            }

            scale = std::max(scale, 0.0f);
            for (int c = 0; c < 3; ++c) {
                perceptual[c] = std::max(0.0f, y + (perceptual[c] - y) * scale);
            }
        }

        if (needClarity || needTexture) {
            // 中间调掩码：保护黑场与高光，避免清晰度把噪点/高光边缘推出来。
            // 纹理的保护范围略宽，允许在偏暗的织物与高光的花瓣上发挥作用。
            const float clarityMask =
                smoothStep01(0.02f, 0.20f, y) * (1.0f - smoothStep01(0.80f, 0.98f, y));
            const float textureMask =
                smoothStep01(0.01f, 0.08f, y) * (1.0f - smoothStep01(0.92f, 0.995f, y));

            float delta = 0.0f;
            if (needClarity) {
                delta += clarityAmount * 1.5f
                    * (y - clarityBlur[static_cast<std::size_t>(i)]) * clarityMask;
            }
            if (needTexture) {
                delta += textureAmount
                    * (y - textureBlur[static_cast<std::size_t>(i)]) * textureMask;
            }

            // 增量限幅：即使在高对比边缘也限制在 ±0.5 感知域，
            // 防止出现明显白边 / 黑边（USM 光晕）。
            delta = math::clamp(delta, -0.5f, 0.5f);
            for (int c = 0; c < 3; ++c) {
                perceptual[c] = std::max(0.0f, perceptual[c] + delta);
            }
        }

        for (int c = 0; c < 3; ++c) {
            pixel[c] = fromPerceptual(perceptual[c]);
        }
    }
}

// ===========================================================================
//  6. HSL 调色
// ===========================================================================

void applyHsl(ImageBuffer& image, const HslSettings& settings)
{
    if (image.isEmpty() || settings.isIdentity()) {
        return;
    }

    // 把 7 个通道的参数预先整理成数组，避免内层循环反复访问枚举
    struct BandParams {
        float hueCenter = 0.0f;
        float hueShift = 0.0f;
        float saturationShift = 0.0f;
        float luminanceShift = 0.0f;
        bool active = false;
    };

    std::array<BandParams, 7> bandParams{};
    for (int b = 0; b < kHslBandCount; ++b) {
        const auto& adjust = settings.bands[static_cast<std::size_t>(b)];
        bandParams[static_cast<std::size_t>(b)].hueCenter = kHslBandHueCenters[b];
        // 色相偏移映射为角度：±100 对应 ±30 度（超过 30 度会与相邻通道严重重叠）
        bandParams[static_cast<std::size_t>(b)].hueShift = adjust.hue / 100.0f * 30.0f;
        bandParams[static_cast<std::size_t>(b)].saturationShift = adjust.saturation / 100.0f;
        bandParams[static_cast<std::size_t>(b)].luminanceShift = adjust.luminance / 100.0f;
        bandParams[static_cast<std::size_t>(b)].active =
            std::fabs(adjust.hue) > 1e-4f
            || std::fabs(adjust.saturation) > 1e-4f
            || std::fabs(adjust.luminance) > 1e-4f;
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        // HSL 的色相与饱和度计算在感知域进行（符合视觉直觉）
        float perceptual[3];
        for (int c = 0; c < 3; ++c) {
            perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
        }

        float hue = 0.0f;
        float saturation = 0.0f;
        rgbToHueSaturation(perceptual[0], perceptual[1], perceptual[2], hue, saturation);

        // 灰色像素无饱和度，调整色相无意义；但亮度调整仍有意义，故不直接跳过
        const bool chromatic = saturation > 1e-3f;

        float totalHueShift = 0.0f;
        float totalSaturationScale = 1.0f;
        float totalLuminanceScale = 1.0f;

        if (chromatic) {
            // 累加各通道的影响，权重按色相距离衰减（余弦钟形）
            for (const auto& band : bandParams) {
                if (!band.active) {
                    continue;
                }

                const float distance = hueDistance(hue, band.hueCenter);
                if (distance >= kHslBandHalfWidth) {
                    continue;  // 超出影响范围
                }

                // 余弦钟形权重：中心处为 1，边界处平滑趋近 0
                const float normalized = distance / kHslBandHalfWidth;
                const float weight = 0.5f * (1.0f + std::cos(normalized * 3.14159265f));

                totalHueShift += band.hueShift * weight;
                totalSaturationScale *= (1.0f + band.saturationShift * weight);
                totalLuminanceScale *= (1.0f + band.luminanceShift * weight);
            }
        } else {
            // 近中性色：只应用亮度调整（若有）
            for (const auto& band : bandParams) {
                if (band.active) {
                    totalLuminanceScale *= (1.0f + band.luminanceShift * 0.5f);
                }
            }
        }

        // 应用色相旋转
        if (std::fabs(totalHueShift) > 1e-4f && chromatic) {
            hue = std::fmod(hue + totalHueShift + 360.0f, 360.0f);
        }

        // 色相变化后需要重新构造 RGB。
        // 用简单的 HSV→RGB（S 为饱和度、V 取原最大分量），保持明度感知不变。
        if (chromatic) {
            const float value = std::max({perceptual[0], perceptual[1], perceptual[2]});
            const float newSaturation = math::clamp(saturation * totalSaturationScale, 0.0f, 1.0f);

            const float c = value * newSaturation;
            const float hPrime = hue / 60.0f;
            const float x = c * (1.0f - std::fabs(std::fmod(hPrime, 2.0f) - 1.0f));
            const float m = value - c;

            float rgb[3] = {0.0f, 0.0f, 0.0f};
            const int sector = static_cast<int>(hPrime) % 6;
            switch (sector) {
            case 0: rgb[0] = c; rgb[1] = x; break;
            case 1: rgb[0] = x; rgb[1] = c; break;
            case 2: rgb[1] = c; rgb[2] = x; break;
            case 3: rgb[1] = x; rgb[2] = c; break;
            case 4: rgb[0] = x; rgb[2] = c; break;
            default: rgb[0] = c; rgb[2] = x; break;
            }

            for (int ch = 0; ch < 3; ++ch) {
                perceptual[ch] = rgb[ch] + m;
            }
        }

        // 回到线性域
        for (int c = 0; c < 3; ++c) {
            pixel[c] = fromPerceptual(math::saturate(perceptual[c]));
        }

        // 明亮度调整直接作用于线性域（亮度是线性量的语义）
        if (std::fabs(totalLuminanceScale - 1.0f) > 1e-4f) {
            const float scale = std::max(totalLuminanceScale, 0.0f);
            pixel[0] *= scale;
            pixel[1] *= scale;
            pixel[2] *= scale;
        }
    }
}

// ===========================================================================
//  6.2 黑白混色（A3）
// ===========================================================================

void applyBlackWhite(ImageBuffer& image, const BlackWhiteSettings& settings)
{
    if (image.isEmpty() || !settings.enabled) {
        return;
    }

    // 与 HSL 使用同一套色相中心与影响半宽，UI 上的通道语义完全一致
    struct BandParams {
        float hueCenter = 0.0f;
        float luminanceShift = 0.0f;
        bool active = false;
    };

    std::array<BandParams, static_cast<std::size_t>(kHslBandCount)> bands{};
    for (int b = 0; b < kHslBandCount; ++b) {
        const float shift = settings.luminance[static_cast<std::size_t>(b)];
        bands[static_cast<std::size_t>(b)].hueCenter = kHslBandHueCenters[b];
        bands[static_cast<std::size_t>(b)].luminanceShift =
            math::clamp(shift / 100.0f, -1.0f, 1.0f);
        bands[static_cast<std::size_t>(b)].active = std::fabs(shift) > 1e-4f;
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        float perceptual[3];
        for (int c = 0; c < 3; ++c) {
            perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
        }

        const float y = 0.2126f * perceptual[0]
                      + 0.7152f * perceptual[1]
                      + 0.0722f * perceptual[2];

        float hue = 0.0f;
        float saturation = 0.0f;
        rgbToHueSaturation(perceptual[0], perceptual[1], perceptual[2],
                           hue, saturation);

        float totalShift = 0.0f;
        if (saturation > 1e-3f) {
            for (const auto& band : bands) {
                if (!band.active) {
                    continue;
                }
                const float distance = hueDistance(hue, band.hueCenter);
                if (distance >= kHslBandHalfWidth) {
                    continue;
                }
                const float normalized = distance / kHslBandHalfWidth;
                const float weight =
                    0.5f * (1.0f + std::cos(normalized * 3.14159265f));
                totalShift += band.luminanceShift * weight;
            }
        }

        // 相邻通道会平滑重叠，累加后钳制在 ±1，避免极端参数把画面压成纯黑。
        totalShift = math::clamp(totalShift, -1.0f, 1.0f);

        // 乘性调整：保留黑场与白场，避免整体发灰。
        const float targetLuma =
            math::saturate(y * std::max(0.0f, 1.0f + totalShift * 0.8f));

        // 严格中性灰：三通道写入同一个值
        const float gray = fromPerceptual(targetLuma);
        pixel[0] = gray;
        pixel[1] = gray;
        pixel[2] = gray;
    }
}

// ===========================================================================
//  6.5 颜色分级与分离色调（A2）
// ===========================================================================

void applyColorGrading(ImageBuffer& image,
                       const ColorGradingSettings& grading,
                       const SplitToningSettings& splitToning)
{
    if (image.isEmpty()) {
        return;
    }
    if (grading.isIdentity() && splitToning.isIdentity()) {
        return;
    }

    // ---- 预计算三段色轮 ----
    //
    // tint 存的是"去掉自身亮度后的色偏方向"，因此叠加时只改变颜色，
    // 不会顺带改变该区域的明暗；明暗由 luminanceShift 单独负责。
    struct WheelEval {
        bool active = false;
        float tint[3] = {0.0f, 0.0f, 0.0f};
        float strength = 0.0f;
        float luminanceShift = 0.0f;
    };

    const auto buildWheel = [](const ColorGradeWheel& wheel) -> WheelEval {
        WheelEval result;
        if (!wheel.isActive()) {
            return result;
        }

        result.active = true;
        const float saturation = math::clamp(wheel.saturation / 100.0f, 0.0f, 1.0f);

        float rgb[3] = {0.0f, 0.0f, 0.0f};
        hsvToRgb(wheel.hue, saturation, 1.0f, rgb);
        const float tintLuma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        for (int c = 0; c < 3; ++c) {
            result.tint[c] = rgb[c] - tintLuma;
        }

        // 0.55 是经验强度：饱和度拉满时仍能保留原物体的明暗结构，
        // 不会像直接叠一层纯色那样把暗部染成一片死色。
        result.strength = 0.55f * saturation;
        result.luminanceShift =
            math::clamp(wheel.luminance / 100.0f, -1.0f, 1.0f) * 0.18f;
        return result;
    };

    const WheelEval wheels[3] = {
        buildWheel(grading.shadows),
        buildWheel(grading.midtones),
        buildWheel(grading.highlights),
    };

    // ---- 分离色调 ----
    struct SplitEval {
        bool active = false;
        float tint[3] = {0.0f, 0.0f, 0.0f};
        float strength = 0.0f;
    };

    const auto buildSplit = [](float hue, float saturationPercent) -> SplitEval {
        SplitEval result;
        const float saturation = math::clamp(saturationPercent / 100.0f, 0.0f, 1.0f);
        if (saturation <= 1e-4f) {
            return result;
        }

        result.active = true;
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        hsvToRgb(hue, saturation, 1.0f, rgb);
        const float tintLuma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        for (int c = 0; c < 3; ++c) {
            result.tint[c] = rgb[c] - tintLuma;
        }
        result.strength = 0.55f * saturation;
        return result;
    };

    const SplitEval splitHighlights =
        buildSplit(splitToning.highlightsHue, splitToning.highlightsSaturation);
    const SplitEval splitShadows =
        buildSplit(splitToning.shadowsHue, splitToning.shadowsSaturation);

    // ---- 权重参数 ----
    // 平衡正值把作用重心推向高光（toneY 整体上移），负值推向阴影。
    const float balanceShift =
        math::clamp(grading.balance / 100.0f, -1.0f, 1.0f) * 0.15f;
    const float sigma = 0.12f + math::clamp(grading.blending / 100.0f, 0.0f, 1.0f) * 0.18f;

    const float splitBalanceShift =
        math::clamp(splitToning.balance / 100.0f, -1.0f, 1.0f) * 0.15f;

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        float perceptual[3];
        for (int c = 0; c < 3; ++c) {
            perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
        }

        const float y = 0.2126f * perceptual[0]
                      + 0.7152f * perceptual[1]
                      + 0.0722f * perceptual[2];

        // ---- 颜色分级：三段高斯权重 ----
        const float toneY = math::saturate(y + balanceShift);
        float weights[3] = {
            toneWeight(toneY, 0.0f, sigma),
            toneWeight(toneY, 0.5f, sigma),
            toneWeight(toneY, 1.0f, sigma),
        };
        const float weightSum = weights[0] + weights[1] + weights[2];
        if (weightSum > 1e-6f) {
            for (float& weight : weights) {
                weight /= weightSum;
            }
        }

        for (int k = 0; k < 3; ++k) {
            const WheelEval& wheel = wheels[k];
            if (!wheel.active) {
                continue;
            }

            const float weight = weights[k];
            if (wheel.strength > 1e-6f) {
                for (int c = 0; c < 3; ++c) {
                    perceptual[c] += wheel.tint[c] * wheel.strength * weight;
                }
            }
            if (std::fabs(wheel.luminanceShift) > 1e-6f) {
                for (int c = 0; c < 3; ++c) {
                    perceptual[c] += wheel.luminanceShift * weight;
                }
            }
        }

        // ---- 分离色调：高光 / 阴影各占一端 ----
        if (splitHighlights.active || splitShadows.active) {
            const float splitY = math::saturate(y + splitBalanceShift);
            const float highlightWeight = smoothStep01(0.35f, 0.65f, splitY);
            const float shadowWeight = 1.0f - highlightWeight;

            if (splitShadows.active) {
                for (int c = 0; c < 3; ++c) {
                    perceptual[c] += splitShadows.tint[c]
                        * splitShadows.strength * shadowWeight;
                }
            }
            if (splitHighlights.active) {
                for (int c = 0; c < 3; ++c) {
                    perceptual[c] += splitHighlights.tint[c]
                        * splitHighlights.strength * highlightWeight;
                }
            }
        }

        for (int c = 0; c < 3; ++c) {
            // 只夹取负值；高于 1 的高光交给管线末尾的软膝压缩，
            // 避免在这里硬截断造成高光断层。
            pixel[c] = fromPerceptual(std::max(0.0f, perceptual[c]));
        }
    }
}

// ===========================================================================
//  6.8 局部调整（B1）
// ===========================================================================

void applyLocalAdjustments(ImageBuffer& image,
                           const MaskStack& masks,
                           float globalTemperature,
                           float globalTint,
                           MaskWeightCache* weightCache)
{
    if (image.isEmpty() || !masks.enable || masks.layers.empty()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    std::vector<float> fallbackWeights;

    for (const MaskLayer& layer : masks.layers) {
        if (!layer.enable || layer.adjustments.isIdentity()) {
            continue;
        }

        const std::vector<float>* weightSource = nullptr;
        if (weightCache != nullptr) {
            weightSource = &weightCache->weightMap(layer, width, height);
        } else {
            layer.buildWeightMap(width, height, fallbackWeights);
            weightSource = &fallbackWeights;
        }

        const std::vector<float>& weights = *weightSource;
        if (weights.size() != pixels) {
            continue;
        }

        const bool pixelRangeShape = layer.shape == MaskShape::Range
            || layer.shape == MaskShape::Luminance
            || layer.shape == MaskShape::ColorRange;

        // 亮度 / 颜色范围权重：单独缓存，只改局部调整参数时不会重算。
        std::vector<float> fallbackRange;
        const std::vector<float>* rangeWeights = nullptr;
        if (pixelRangeShape || layer.range.isActive()) {
            if (weightCache != nullptr) {
                rangeWeights = &weightCache->rangeWeightMap(layer, image, width, height);
            } else {
                buildRangeWeightMap(layer, image, fallbackRange);
                rangeWeights = &fallbackRange;
            }
            if (rangeWeights->size() != pixels) {
                continue;
            }
        }

        const LocalAdjustments& adjust = layer.adjustments;

        const float exposureGain =
            std::pow(2.0f, math::clamp(adjust.exposure, -5.0f, 5.0f));
        const float contrastAmount =
            math::clamp(adjust.contrast / 100.0f, -1.0f, 1.0f);
        const float highlightAmount =
            math::clamp(adjust.highlights / 100.0f, -1.0f, 1.0f);
        const float shadowAmount =
            math::clamp(adjust.shadows / 100.0f, -1.0f, 1.0f);
        const float saturationAmount =
            math::clamp(adjust.saturation / 100.0f, -1.0f, 1.0f);

        const bool needContrast = std::fabs(contrastAmount) > 1e-4f;
        const bool needSaturation = std::fabs(saturationAmount) > 1e-4f;
        const bool needHighlight = std::fabs(highlightAmount) > 1e-4f;
        const bool needShadow = std::fabs(shadowAmount) > 1e-4f;

        // 相对白平衡增益：以当前全局色温/色调为基准，只补偿局部偏移。
        // 这样局部色温不会破坏已经完成的白平衡，也不会把整幅图推偏。
        float whiteBalanceGain[3] = {1.0f, 1.0f, 1.0f};
        if (std::fabs(adjust.temperature) > 1e-3f
            || std::fabs(adjust.tint) > 1e-4f) {
            const float baseTemperature =
                (globalTemperature > 1.0f) ? globalTemperature : 5500.0f;
            const float targetTemperature = math::clamp(
                baseTemperature + adjust.temperature, 1000.0f, 40000.0f);
            const float targetTint =
                math::clamp(globalTint + adjust.tint, -100.0f, 100.0f);

            float baseGain[3] = {1.0f, 1.0f, 1.0f};
            float targetGain[3] = {1.0f, 1.0f, 1.0f};
            computeWhiteBalanceGain(baseTemperature, globalTint, baseGain);
            computeWhiteBalanceGain(targetTemperature, targetTint, targetGain);

            for (int c = 0; c < 3; ++c) {
                whiteBalanceGain[c] = targetGain[c]
                    / (std::fabs(baseGain[c]) > 1e-6f ? baseGain[c] : 1.0f);
            }

            // 相对增益也要保持亮度，否则局部调色温会顺带改变曝光
            const float lumaGain = 0.2880402f * whiteBalanceGain[0]
                + 0.7118741f * whiteBalanceGain[1]
                + 0.0000857f * whiteBalanceGain[2];
            if (lumaGain > 1e-6f) {
                for (int c = 0; c < 3; ++c) {
                    whiteBalanceGain[c] /= lumaGain;
                }
            }
        }

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float weight = weights[static_cast<std::size_t>(i)];
            const bool pixelRangeShape = layer.shape == MaskShape::Range
                || layer.shape == MaskShape::Luminance
                || layer.shape == MaskShape::ColorRange;
            // 像素范围蒙版的实际权重由像素内容重新计算；形状权重可能因反相为 0，
            // 不能在这里提前跳过。
            if (!pixelRangeShape && weight <= 1e-5f) {
                continue;
            }

            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            const float base[3] = {pixel[0], pixel[1], pixel[2]};

            // 亮度 / 颜色范围权重来自缓存（只改局部调整参数不会失效）。
            //  - 纯范围蒙版（Range / Luminance / ColorRange）：范围权重就是蒙版本身，
            //    反相要反转“范围”，而不是把基础权重乘成 0；
            //  - 其它形状的旧版 range 筛选：与形状权重相乘。
            if (pixelRangeShape) {
                float rangeWeight = (*rangeWeights)[static_cast<std::size_t>(i)];
                if (layer.invert) {
                    rangeWeight = 1.0f - rangeWeight;
                }
                weight = math::saturate(layer.opacity) * rangeWeight;
                if (weight <= 1e-5f) {
                    continue;
                }
            } else if (rangeWeights != nullptr) {
                weight *= (*rangeWeights)[static_cast<std::size_t>(i)];
                if (weight <= 1e-5f) {
                    continue;
                }
            }

            float adjusted[3] = {
                base[0] * whiteBalanceGain[0],
                base[1] * whiteBalanceGain[1],
                base[2] * whiteBalanceGain[2],
            };

            // ---- 线性域：曝光与高光 / 阴影 ----
            for (int c = 0; c < 3; ++c) {
                adjusted[c] *= exposureGain;
            }

            if (needHighlight || needShadow) {
                const float luma = luminanceOf(adjusted[0], adjusted[1], adjusted[2]);
                float multiplier = 1.0f;
                if (needHighlight) {
                    const float mask = math::saturate(luma) * math::saturate(luma);
                    multiplier *= (1.0f + highlightAmount * mask * 0.8f);
                }
                if (needShadow) {
                    const float inverse = 1.0f - math::saturate(luma);
                    const float mask = inverse * inverse;
                    multiplier *= (1.0f + shadowAmount * mask * 1.2f);
                }
                multiplier = std::max(multiplier, 0.0f);
                for (int c = 0; c < 3; ++c) {
                    adjusted[c] *= multiplier;
                }
            }

            // ---- 感知域：对比度与饱和度 ----
            if (needContrast || needSaturation) {
                float perceptual[3];
                for (int c = 0; c < 3; ++c) {
                    perceptual[c] = toPerceptual(std::max(0.0f, adjusted[c]));
                }

                if (needContrast) {
                    const float k = contrastAmount * 0.75f;
                    for (int c = 0; c < 3; ++c) {
                        const float x = perceptual[c];
                        perceptual[c] = math::saturate(
                            x + k * x * (1.0f - x) * (2.0f * x - 1.0f));
                    }
                }

                if (needSaturation) {
                    const float y = 0.2126f * perceptual[0]
                        + 0.7152f * perceptual[1]
                        + 0.0722f * perceptual[2];
                    const float factor = std::max(0.0f, 1.0f + saturationAmount);
                    for (int c = 0; c < 3; ++c) {
                        perceptual[c] =
                            std::max(0.0f, y + (perceptual[c] - y) * factor);
                    }
                }

                for (int c = 0; c < 3; ++c) {
                    adjusted[c] = fromPerceptual(std::max(0.0f, perceptual[c]));
                }
            }

            // ---- 按蒙版权重混合 ----
            for (int c = 0; c < 3; ++c) {
                pixel[c] = base[c] + (adjusted[c] - base[c]) * weight;
            }
        }
    }
}

// ===========================================================================
//  7. 曲线
// ===========================================================================

std::vector<float> compileCurveLut(const std::vector<CurvePoint>& points, int size)
{
    std::vector<float> lut;
    if (points.size() < 2 || size < 2) {
        return lut;
    }

    // 确保控制点按 x 升序（UI 可能产生乱序点）
    std::vector<CurvePoint> sorted = points;
    std::sort(sorted.begin(), sorted.end(),
              [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });

    lut.resize(static_cast<std::size_t>(size));

    // 用单调三次插值（PCHIP）而非普通样条：
    // 普通三次样条会在控制点附近产生过冲，导致曲线非单调，
    // 表现为画面出现异常亮暗带。PCHIP 保证单调性，更适合色调曲线。
    const std::size_t count = sorted.size();

    // 预先计算斜率
    std::vector<float> slopes(count - 1);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const float dx = sorted[i + 1].x - sorted[i].x;
        slopes[i] = (dx > 1e-8f) ? (sorted[i + 1].y - sorted[i].y) / dx : 0.0f;
    }

    // PCHIP 切线
    std::vector<float> tangents(count, 0.0f);
    for (std::size_t i = 1; i + 1 < count; ++i) {
        const float leftSlope = slopes[i - 1];
        const float rightSlope = slopes[i];
        // 异号时切线取 0（极值点），保证单调
        if (leftSlope * rightSlope <= 0.0f) {
            tangents[i] = 0.0f;
        } else {
            tangents[i] = 0.5f * (leftSlope + rightSlope);
        }
    }
    // 端点用单侧斜率
    tangents[0] = slopes.empty() ? 0.0f : slopes.front();
    tangents[count - 1] = slopes.empty() ? 0.0f : slopes.back();

    // 采样
    for (int i = 0; i < size; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(size - 1);

        // 找到 x 所在的区间
        std::size_t segment = 0;
        if (x <= sorted.front().x) {
            // 左端外推：直接取第一个点的 y
            lut[static_cast<std::size_t>(i)] = sorted.front().y;
            continue;
        }
        if (x >= sorted.back().x) {
            // 右端外推：取最后一个点的 y
            lut[static_cast<std::size_t>(i)] = sorted.back().y;
            continue;
        }

        for (std::size_t s = 0; s + 1 < count; ++s) {
            if (x >= sorted[s].x && x <= sorted[s + 1].x) {
                segment = s;
                break;
            }
        }

        const float x0 = sorted[segment].x;
        const float x1 = sorted[segment + 1].x;
        const float y0 = sorted[segment].y;
        const float y1 = sorted[segment + 1].y;
        const float h = x1 - x0;

        if (h < 1e-8f) {
            lut[static_cast<std::size_t>(i)] = y0;
            continue;
        }

        // Hermite 基函数
        const float t = (x - x0) / h;
        const float t2 = t * t;
        const float t3 = t2 * t;

        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;

        const float value = h00 * y0 + h10 * h * tangents[segment]
                          + h01 * y1 + h11 * h * tangents[segment + 1];
        lut[static_cast<std::size_t>(i)] = math::saturate(value);
    }

    return lut;
}

namespace {

/// 用 LUT 对三个通道应用同一条曲线
void applyCurveLut(ImageBuffer& image, const std::vector<float>& lut)
{
    if (lut.empty() || image.isEmpty()) {
        return;
    }

    const int lutSize = static_cast<int>(lut.size());
    const float scale = static_cast<float>(lutSize - 1);

    // 曲线的定义域与值域都是 0~1，输入超出范围时应当用端点值而非夹取坐标。
    // 但像素值可能略大于 1（前序步骤的增益），这里对坐标做夹取是合理的，
    // 因为曲线端点已经表达了"超出范围如何处理"的意图。
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        for (int c = 0; c < 3; ++c) {
            const float x = math::saturate(pixel[c]);
            const float position = x * scale;
            const int index = static_cast<int>(position);

            if (index >= lutSize - 1) {
                pixel[c] = lut[static_cast<std::size_t>(lutSize - 1)];
            } else {
                // 线性插值，避免 LUT 量化产生色带
                const float fraction = position - static_cast<float>(index);
                const float a = lut[static_cast<std::size_t>(index)];
                const float b = lut[static_cast<std::size_t>(index + 1)];
                pixel[c] = math::lerp(a, b, fraction);
            }
        }
    }
}

}  // namespace

void applyParametricCurve(ImageBuffer& image,
                          const ParametricCurveSettings& settings)
{
    if (image.isEmpty() || settings.isIdentity()) {
        return;
    }

    const float shadowAmount = math::clamp(settings.shadows / 100.0f, -1.0f, 1.0f);
    const float midtoneAmount = math::clamp(settings.midtones / 100.0f, -1.0f, 1.0f);
    const float highlightAmount = math::clamp(settings.highlights / 100.0f, -1.0f, 1.0f);

    const float shadowRange = math::clamp(settings.shadowRange, 0.05f, 0.5f);
    const float midtoneRange = math::clamp(settings.midtoneRange, 0.05f, 0.5f);
    const float highlightRange = math::clamp(settings.highlightRange, 0.05f, 0.5f);

    // 三个区域的中心固定在感知亮度的暗部 / 中间 / 亮部；
    // Range 控制高斯宽度，即"影响范围"。
    constexpr float kShadowCenter = 0.20f;
    constexpr float kMidtoneCenter = 0.50f;
    constexpr float kHighlightCenter = 0.80f;

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;

        float perceptual[3];
        for (int c = 0; c < 3; ++c) {
            perceptual[c] = toPerceptual(std::max(0.0f, pixel[c]));
        }

        const float y = 0.2126f * perceptual[0]
                      + 0.7152f * perceptual[1]
                      + 0.0722f * perceptual[2];

        const auto weightAt = [](float value, float center, float range) {
            const float d = (value - center) / range;
            return std::exp(-0.5f * d * d);
        };

        float weights[3] = {
            weightAt(y, kShadowCenter, shadowRange),
            weightAt(y, kMidtoneCenter, midtoneRange),
            weightAt(y, kHighlightCenter, highlightRange),
        };
        const float sum = weights[0] + weights[1] + weights[2];
        if (sum > 1e-6f) {
            for (float& w : weights) {
                w /= sum;
            }
        }

        const float delta = 0.25f * (shadowAmount * weights[0]
                                     + midtoneAmount * weights[1]
                                     + highlightAmount * weights[2]);

        if (std::fabs(delta) > 1e-6f) {
            for (int c = 0; c < 3; ++c) {
                perceptual[c] = std::max(0.0f, perceptual[c] + delta);
            }
        }

        for (int c = 0; c < 3; ++c) {
            pixel[c] = fromPerceptual(perceptual[c]);
        }
    }
}

void applyCurves(ImageBuffer& image, const CurveSettings& settings)
{
    if (image.isEmpty()) {
        return;
    }

    // ---- 0) 参数曲线先在感知域建立整体影调 ----
    applyParametricCurve(image, settings.parametric);

    // ---- 1) 先应用 R/G/B 三条通道曲线 ----
    // 注意：三条曲线在**线性域**上作用于各自的通道。
    // 曲线控制点由用户在 UI 上编辑，UI 呈现的是感知上的曲线形状，
    // 但作用域是线性光——这符合需求 §1.3.1 的硬性约束。
    const std::vector<float> redLut = compileCurveLut(settings.red.points);
    const std::vector<float> greenLut = compileCurveLut(settings.green.points);
    const std::vector<float> blueLut = compileCurveLut(settings.blue.points);

    const bool hasRed = !redLut.empty() && !settings.red.isIdentity();
    const bool hasGreen = !greenLut.empty() && !settings.green.isIdentity();
    const bool hasBlue = !blueLut.empty() && !settings.blue.isIdentity();

    if (hasRed || hasGreen || hasBlue) {
        const std::size_t pixels = image.pixelCount();
        float* data = image.data();

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;

            const auto applySingle = [](float value, const std::vector<float>& lut) -> float {
                const int lutSize = static_cast<int>(lut.size());
                const float x = math::saturate(value);
                const float position = x * static_cast<float>(lutSize - 1);
                const int index = static_cast<int>(position);
                if (index >= lutSize - 1) {
                    return lut[static_cast<std::size_t>(lutSize - 1)];
                }
                const float fraction = position - static_cast<float>(index);
                return math::lerp(lut[static_cast<std::size_t>(index)],
                                  lut[static_cast<std::size_t>(index + 1)], fraction);
            };

            if (hasRed)   { pixel[0] = applySingle(pixel[0], redLut); }
            if (hasGreen) { pixel[1] = applySingle(pixel[1], greenLut); }
            if (hasBlue)  { pixel[2] = applySingle(pixel[2], blueLut); }
        }
    }

    // ---- 2) 再应用 L 明度曲线 ----
    // 需求 §2 管线：单通道曲线（R/G/B/L），L 在最后。
    const std::vector<float> lumaLut = compileCurveLut(settings.luminance.points);
    if (!lumaLut.empty() && !settings.luminance.isIdentity()) {
        const std::size_t pixels = image.pixelCount();
        float* data = image.data();

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;

            const float luma = luminanceOf(pixel[0], pixel[1], pixel[2]);
            if (luma <= 1e-8f) {
                continue;  // 全黑像素无明度信息，无法应用明度曲线
            }

            const float x = math::saturate(luma);
            const float position = x * static_cast<float>(lumaLut.size() - 1);
            const int index = static_cast<int>(position);
            const int lutSize = static_cast<int>(lumaLut.size());

            const float targetLuma = (index >= lutSize - 1)
                ? lumaLut[static_cast<std::size_t>(lutSize - 1)]
                : math::lerp(lumaLut[static_cast<std::size_t>(index)],
                             lumaLut[static_cast<std::size_t>(index + 1)],
                             position - static_cast<float>(index));

            // 用比例缩放保持色相与饱和度不变
            const float scale = targetLuma / luma;
            pixel[0] *= scale;
            pixel[1] *= scale;
            pixel[2] *= scale;
        }
    }
}

// ===========================================================================
//  8. 3D LUT
// ===========================================================================

ErrorInfo applyLut(ImageBuffer& image, const LutSettings& settings)
{
    if (!settings.enable || image.isEmpty()) {
        return ErrorInfo::ok();
    }

    // 解析结果带缓存：拖动强度滑块时不会反复读盘解析同一个文件
    ErrorInfo error;
    const std::shared_ptr<const Lut3D> lut = Lut3D::loadCached(settings.path, error);
    if (!lut) {
        return error;
    }

    lut->apply(image, settings.opacity);
    return ErrorInfo::ok();
}

// ===========================================================================
//  9. 降噪
// ===========================================================================

namespace {

/// B3 样条（1,4,6,4,1）/16 的可分离 à trous 模糊。
/// step 是抽头间距，第 l 层取 2^l，从而实现多尺度分解。
void b3SplineBlur(const std::vector<float>& src,
                  std::vector<float>& dst,
                  std::vector<float>& temp,
                  int width, int height, int step)
{
    static constexpr float kKernel[5] = {
        1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

    // 水平方向
    const int margin = 2 * step;
    RAWCORE_PARALLEL_FOR(y, height)
    {
        const float* row = src.data() + static_cast<std::size_t>(y) * width;
        float* outRow = temp.data() + static_cast<std::size_t>(y) * width;

        // 中间段抽头都在行内，不需要逐抽头夹取（边界段仍走夹取版本）
        const int interiorBegin = std::min(margin, width);
        const int interiorEnd = std::max(interiorBegin, width - margin);

        for (int x = 0; x < interiorBegin; ++x) {
            float sum = 0.0f;
            for (int k = -2; k <= 2; ++k) {
                const int sx = clampIndex(x + k * step, 0, width - 1);
                sum += row[sx] * kKernel[k + 2];
            }
            outRow[x] = sum;
        }

        for (int x = interiorBegin; x < interiorEnd; ++x) {
            outRow[x] = row[x - margin] * kKernel[0]
                      + row[x - step] * kKernel[1]
                      + row[x] * kKernel[2]
                      + row[x + step] * kKernel[3]
                      + row[x + margin] * kKernel[4];
        }

        for (int x = interiorEnd; x < width; ++x) {
            float sum = 0.0f;
            for (int k = -2; k <= 2; ++k) {
                const int sx = clampIndex(x + k * step, 0, width - 1);
                sum += row[sx] * kKernel[k + 2];
            }
            outRow[x] = sum;
        }
    }

    // 垂直方向：同样按行累加。抽头间距是 step，逐列取抽点相当于跨 step 行
    // 的随机访问，按行累加则保持顺序访存，第 2、3 层的收益尤其明显。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        float* outRow = dst.data() + static_cast<std::size_t>(y) * width;
        std::fill(outRow, outRow + width, 0.0f);
        for (int k = -2; k <= 2; ++k) {
            const int sy = clampIndex(y + k * step, 0, height - 1);
            const float* srcRow = temp.data() + static_cast<std::size_t>(sy) * width;
            const float weight = kKernel[k + 2];
            for (int x = 0; x < width; ++x) {
                outRow[x] += weight * srcRow[x];
            }
        }
    }
}

/// 用 |detail| 的 30% 分位估计噪声标准差。
///
/// 直接求全局均方根会把真实纹理也算成噪声，导致细节区被过度平滑；
/// 细节系数通常稀疏，30% 分位仍落在噪声分布内，估计更稳。
/// 对零均值高斯噪声，E|X| 的 30% 分位约为 0.385σ。
/// maxAbs 由调用方在生成 detail 的同一次遍历里顺带求出，省掉一整遍扫描。
float robustNoiseSigma(const std::vector<float>& detail, float maxAbs)
{
    if (detail.empty() || maxAbs <= 1e-8f) {
        return 0.0f;
    }

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
    const int workerCount = std::max(1, omp_get_max_threads());
#else
    const int workerCount = 1;
#endif

    const int elementCount = static_cast<int>(detail.size());

    constexpr int kBins = 512;
    // 每线程一份直方图（512 个 int，线程间不共享缓存行），循环结束后再合并
    std::vector<int> histogram(static_cast<std::size_t>(kBins) * static_cast<std::size_t>(workerCount), 0);
    const float scale = static_cast<float>(kBins) / maxAbs;

    RAWCORE_PARALLEL_FOR(i, elementCount)
    {
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
        int* localHistogram = histogram.data() + static_cast<std::size_t>(omp_get_thread_num()) * kBins;
#else
        int* localHistogram = histogram.data();
#endif
        int bin = static_cast<int>(std::fabs(detail[static_cast<std::size_t>(i)]) * scale);
        if (bin >= kBins) {
            bin = kBins - 1;
        }
        ++localHistogram[bin];
    }

    // 合并到第 0 份直方图上
    for (int worker = 1; worker < workerCount; ++worker) {
        const int* localHistogram = histogram.data() + static_cast<std::size_t>(worker) * kBins;
        for (int bin = 0; bin < kBins; ++bin) {
            histogram[static_cast<std::size_t>(bin)] += localHistogram[bin];
        }
    }

    const std::size_t target = std::max<std::size_t>(1u, detail.size() * 3u / 10u);
    std::size_t cumulative = 0;
    int bin = 0;
    for (; bin < kBins; ++bin) {
        cumulative += static_cast<std::size_t>(histogram[static_cast<std::size_t>(bin)]);
        if (cumulative >= target) {
            break;
        }
    }

    const float p30 = (static_cast<float>(bin) + 0.5f) / scale;
    return p30 / 0.385f;
}

/// 小波降噪复用的临时缓冲（全尺寸平面，4500 万像素时每份约 180 MB）。
/// 一次降噪里多个平面、多层分解共用同一组缓冲，避免反复分配/清零。
struct WaveletScratch {
    std::vector<float> approx;
    std::vector<float> blurred;
    std::vector<float> detail;
};

/// à trous 小波软阈值降噪（3 层）。
///
/// 分解：approx_{l+1} = blur(approx_l, step=2^l)，detail_l = approx_l - approx_{l+1}。
/// 重建：approx_3 + Σ soft(detail_l, T_l)。
/// 软阈值保留大系数（边缘/纹理），只收缩噪声级别的小系数，
/// 比高斯模糊更能同时做到"平处干净、边缘不糊"。
///
/// scratch 是复用的全尺寸临时缓冲：4500 万像素时一份就是 180 MB，
/// 每次调用都重新分配并清零会带来大量缺页和写带宽开销
/// （色度两遍、每遍三层，加起来就是十几次全图清零）。
void waveletDenoisePlane(std::vector<float>& plane,
                         int width, int height, float strength, float pixelScale,
                         WaveletScratch& scratch)
{
    if (strength <= 1e-4f || plane.empty() || width < 2 || height < 2) {
        return;
    }

    const std::size_t pixels = plane.size();
    std::vector<float>& approx = scratch.approx;
    std::vector<float>& blurred = scratch.blurred;
    std::vector<float>& detail = scratch.detail;

    approx.assign(plane.begin(), plane.end());
    // 两份缓冲在每一层都是"先写完再读"，不需要清零；只在尺寸变化时分配一次
    if (blurred.size() != pixels) {
        blurred.resize(pixels);
        detail.resize(pixels);
    }

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
    const int workerCount = std::max(1, omp_get_max_threads());
#else
    const int workerCount = 1;
#endif
    std::vector<PaddedSlot<float>> partialMax(static_cast<std::size_t>(workerCount));

    // plane 直接作为重建累加器：第一层直接写入，省掉一次全平面清零
    bool accumulate = false;

    for (int level = 0; level < 3; ++level) {
        // 小波层间距按"原始像素"解释：降采样预览的 1 像素对应原图多个像素，
        // 若仍按 1/2/4 递增，平滑半径会被放大，fit 视图比 1:1 更"糊"。
        const int step = std::max(
            1, static_cast<int>(std::lround(
                   static_cast<float>(1 << level) * std::max(0.05f, pixelScale))));

        // 水平结果先写入 detail，垂直结果写入 blurred
        b3SplineBlur(approx, blurred, detail, width, height, step);

        // 求细节系数的同时顺带统计绝对值最大值，供分位数估计复用
        std::fill(partialMax.begin(), partialMax.end(), PaddedSlot<float>{0.0f});

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            const std::size_t index = static_cast<std::size_t>(i);
            const float value = approx[index] - blurred[index];
            detail[index] = value;
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
            float& localMax = partialMax[static_cast<std::size_t>(omp_get_thread_num())].value;
#else
            float& localMax = partialMax[0].value;
#endif
            const float magnitude = std::fabs(value);
            if (magnitude > localMax) {
                localMax = magnitude;
            }
        }

        float maxAbs = 0.0f;
        for (const PaddedSlot<float>& localMax : partialMax) {
            maxAbs = std::max(maxAbs, localMax.value);
        }

        const float sigma = robustNoiseSigma(detail, maxAbs);
        // 2.5σ 是软阈值的常用起点：强度 1.0 时去掉绝大部分噪声系数，
        // 强度 0.15（默认色度）只做很轻的收缩。
        const float threshold = strength * 2.5f * sigma;

        if (threshold > 1e-7f) {
            if (accumulate) {
                RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
                {
                    const std::size_t index = static_cast<std::size_t>(i);
                    const float value = detail[index];
                    const float magnitude = std::fabs(value);
                    if (magnitude > threshold) {
                        plane[index] += (value > 0.0f)
                            ? (magnitude - threshold)
                            : -(magnitude - threshold);
                    }
                }
            } else {
                RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
                {
                    const std::size_t index = static_cast<std::size_t>(i);
                    const float value = detail[index];
                    const float magnitude = std::fabs(value);
                    plane[index] = (magnitude > threshold)
                        ? ((value > 0.0f) ? (magnitude - threshold) : -(magnitude - threshold))
                        : 0.0f;
                }
            }
        } else if (!accumulate) {
            // 第一层阈值小到可以忽略：累加器仍要归零，后面几层才正确
            std::fill(plane.begin(), plane.end(), 0.0f);
        }
        accumulate = true;

        approx.swap(blurred);
    }

    // 重建：三层细节系数之和 + 最粗一级的近似系数
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        plane[index] += approx[index];
    }
}

/// 非局部均值（NLM）明度降噪（D5）。
///
/// 对每个像素在搜索窗口内比较 3×3 邻域块，块越相似权重越大：
///   w = exp(-max(0, d² - 2σ²) / h²)
/// 减去 2σ² 是 NLM 的标准噪声偏置修正，避免将纯噪声误判为相似结构。
///
/// 只处理亮度平面，色度仍走小波（色度噪声频率低、小波更高效），
/// 这正是需求中的"亮度 / 色度分离"。
///
/// 性能：搜索半径按像素比例缩放，复杂度约为 N × (2r+1)² × 9。
/// 因此它定位为"最终输出质量优先"的选项，交互预览仍建议用小波。
void nlMeansDenoisePlane(std::vector<float>& plane, int width, int height,
                         float strength, int searchRadius, float pixelScale)
{
    if (strength <= 1e-4f || plane.empty() || width < 5 || height < 5) {
        return;
    }

    const std::size_t pixels = plane.size();

    // 用一层 B3 样条高通估计噪声标准差；真实纹理稀疏，30% 分位仍落在噪声内。
    // b3SplineBlur 不会自行分配输出缓冲，必须先按平面尺寸准备好。
    std::vector<float> blurred(pixels);
    std::vector<float> temp(pixels);
    std::vector<float> detail(pixels);
    b3SplineBlur(plane, blurred, temp, width, height, 1);

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
    const int workerCount = std::max(1, omp_get_max_threads());
#else
    const int workerCount = 1;
#endif
    std::vector<PaddedSlot<float>> partialMax(static_cast<std::size_t>(workerCount));
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float value = plane[index] - blurred[index];
        detail[index] = value;
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
        float& localMax = partialMax[static_cast<std::size_t>(omp_get_thread_num())].value;
#else
        float& localMax = partialMax[0].value;
#endif
        const float magnitude = std::fabs(value);
        if (magnitude > localMax) {
            localMax = magnitude;
        }
    }
    float maxAbs = 0.0f;
    for (const PaddedSlot<float>& localMax : partialMax) {
        maxAbs = std::max(maxAbs, localMax.value);
    }
    const float sigma = robustNoiseSigma(detail, maxAbs);
    if (sigma <= 1e-8f) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const float h = std::max(amount * 2.5f * sigma, 1e-6f);
    const float h2 = h * h;
    const float noiseFloor = 2.0f * sigma * sigma;

    // 搜索半径按"原始像素"解释：预览降采样后相同场景尺度只占更少像素。
    const int radius = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(searchRadius)
                                     * math::clamp(pixelScale, 0.25f, 2.0f))),
        1, 7);

    constexpr int kPatchRadius = 1;
    std::vector<float> output(pixels);

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width
                + static_cast<std::size_t>(x);
            const float center = plane[index];

            double weightSum = 0.0;
            double valueSum = 0.0;

            const int y0 = std::max(0, y - radius);
            const int y1 = std::min(height - 1, y + radius);
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(width - 1, x + radius);

            for (int ny = y0; ny <= y1; ++ny) {
                // 3×3 块在边界处钳制，块距离始终可计算
                const int py0 = std::max(0, ny - kPatchRadius);
                const int py1 = std::min(height - 1, ny + kPatchRadius);
                for (int nx = x0; nx <= x1; ++nx) {
                    const int px0 = std::max(0, nx - kPatchRadius);
                    const int px1 = std::min(width - 1, nx + kPatchRadius);

                    float distance = 0.0f;
                    int samples = 0;
                    for (int py = py0; py <= py1; ++py) {
                        const int cy = std::min(height - 1,
                                                std::max(0, y + (py - ny)));
                        const float* a = plane.data()
                            + static_cast<std::size_t>(cy) * width;
                        const float* b = plane.data()
                            + static_cast<std::size_t>(py) * width;
                        for (int px = px0; px <= px1; ++px) {
                            const int cx = std::min(width - 1,
                                                    std::max(0, x + (px - nx)));
                            const float delta = a[cx] - b[px];
                            distance += delta * delta;
                            ++samples;
                        }
                        // 提前退出：距离已经远大于阈值，权重必然接近 0
                        if (samples > 0
                            && distance / static_cast<float>(samples) > 9.0f * h2) {
                            break;
                        }
                    }
                    if (samples == 0) {
                        continue;
                    }
                    distance /= static_cast<float>(samples);
                    if (distance > 9.0f * h2) {
                        continue;
                    }

                    const float weight = std::exp(
                        -std::max(0.0f, distance - noiseFloor) / h2);
                    weightSum += static_cast<double>(weight);
                    valueSum += static_cast<double>(weight)
                        * static_cast<double>(plane[static_cast<std::size_t>(ny) * width
                                                    + static_cast<std::size_t>(nx)]);
                }
            }

            if (weightSum > 1e-12) {
                output[index] = static_cast<float>(valueSum / weightSum);
            } else {
                output[index] = center;
            }
        }
    }

    plane.swap(output);
}

}  // namespace

// ---------------------------------------------------------------------------
//  BM3D 单阶段近似（D5）：块匹配 + 2D DCT 硬阈值收缩
// ---------------------------------------------------------------------------
//
//  BM3D 的核心是"协同滤波"：先在邻域里找出与参考块相似的一组块，
//  把它们一起变换到频域，噪声在小系数上被阈值去掉，再逆变换并聚合。
//  这里实现的是单阶段硬阈值版本（相当于 BM3D Step 1）：
//    1. 8×8 参考块 + 搜索窗口内按 SSD 选最多 12 个相似块；
//    2. 每个块乘 Hann 窗后做 8×8 DCT-II，系数绝对值小于阈值的置零；
//    3. 逆变换并按"非零系数越多权重越低"聚合回原位置。
//
//  与完整 BM3D 的差异：没有第二阶段 Wiener 滤波、没有分组内 3D 变换、
//  用 2D DCT 代替 3D 变换。质量介于 NLM 与完整 BM3D 之间，但实现稳定、
//  可复现，且只依赖标准 C++。它定位为最终导出的高质量选项。
// ---------------------------------------------------------------------------
namespace {

constexpr int kBm3dBlock = 8;
constexpr int kBm3dMaxGroup = 12;

/// 8×8 正交 DCT-II 的基函数表
struct Dct8Tables {
    float cosine[kBm3dBlock][kBm3dBlock] = {};
    Dct8Tables()
    {
        constexpr double kPi = 3.14159265358979323846;
        for (int u = 0; u < kBm3dBlock; ++u) {
            const double alpha = (u == 0)
                ? std::sqrt(1.0 / kBm3dBlock)
                : std::sqrt(2.0 / kBm3dBlock);
            for (int x = 0; x < kBm3dBlock; ++x) {
                cosine[u][x] = static_cast<float>(
                    alpha * std::cos((2.0 * x + 1.0) * u * kPi
                                     / (2.0 * kBm3dBlock)));
            }
        }
    }
};

/// 一维 DCT-II / DCT-III（正交归一化）。
///
/// 正变换 out[u] = Σ_x C[u][x]·in[x]；逆变换 out[x] = Σ_u C[u][x]·in[u]。
/// 二维正变换是 C·X·Cᵀ，逆变换必须是 Cᵀ·Z·C；两者不能共用同一段
/// "行列都乘 C" 的代码（C 不是对称矩阵）。
void dct8_1d(float* values, const Dct8Tables& tables, bool inverse)
{
    float output[kBm3dBlock];
    if (!inverse) {
        for (int u = 0; u < kBm3dBlock; ++u) {
            float sum = 0.0f;
            for (int x = 0; x < kBm3dBlock; ++x) {
                sum += tables.cosine[u][x] * values[x];
            }
            output[u] = sum;
        }
    } else {
        for (int x = 0; x < kBm3dBlock; ++x) {
            float sum = 0.0f;
            for (int u = 0; u < kBm3dBlock; ++u) {
                sum += tables.cosine[u][x] * values[u];
            }
            output[x] = sum;
        }
    }
    for (int i = 0; i < kBm3dBlock; ++i) {
        values[i] = output[i];
    }
}

/// 8×8 二维 DCT / 逆 DCT（就地）
void dct8_2d(float block[kBm3dBlock][kBm3dBlock],
             const Dct8Tables& tables,
             bool inverse)
{
    float line[kBm3dBlock];
    for (int y = 0; y < kBm3dBlock; ++y) {
        for (int x = 0; x < kBm3dBlock; ++x) {
            line[x] = block[y][x];
        }
        dct8_1d(line, tables, inverse);
        for (int x = 0; x < kBm3dBlock; ++x) {
            block[y][x] = line[x];
        }
    }
    for (int x = 0; x < kBm3dBlock; ++x) {
        for (int y = 0; y < kBm3dBlock; ++y) {
            line[y] = block[y][x];
        }
        dct8_1d(line, tables, inverse);
        for (int y = 0; y < kBm3dBlock; ++y) {
            block[y][x] = line[y];
        }
    }
}

/// 块匹配 + DCT 收缩。plane 为亮度平面，就地修改。
void bm3dDenoisePlane(std::vector<float>& plane, int width, int height,
                      float strength, int searchRadius, float pixelScale)
{
    if (strength <= 1e-4f || plane.empty()
        || width < kBm3dBlock + 2 || height < kBm3dBlock + 2) {
        return;
    }

    const std::size_t pixels = plane.size();

    // 用一层高通估计噪声标准差（与 NLM 相同）
    std::vector<float> blurred(pixels);
    std::vector<float> temp(pixels);
    std::vector<float> detail(pixels);
    b3SplineBlur(plane, blurred, temp, width, height, 1);

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
    const int workerCount = std::max(1, omp_get_max_threads());
#else
    const int workerCount = 1;
#endif
    std::vector<PaddedSlot<float>> partialMax(static_cast<std::size_t>(workerCount));
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        detail[index] = plane[index] - blurred[index];
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
        float& localMax = partialMax[static_cast<std::size_t>(omp_get_thread_num())].value;
#else
        float& localMax = partialMax[0].value;
#endif
        localMax = std::max(localMax, std::fabs(detail[index]));
    }
    float maxAbs = 0.0f;
    for (const PaddedSlot<float>& localMax : partialMax) {
        maxAbs = std::max(maxAbs, localMax.value);
    }
    const float sigma = robustNoiseSigma(detail, maxAbs);
    if (sigma <= 1e-8f) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const float threshold = amount * 2.7f * sigma;
    const float sigmaSquared = std::max(sigma * sigma, 1e-10f);

    // 块间距 / 组大小：大图降低参考块密度与相似块数量，避免单次渲染
    // 耗时失控（45MP 全尺寸导出仍会有数十秒级开销，这是质量选项的代价）。
    // 块间距必须 <= 8，否则会有像素不被任何参考块覆盖。
    const bool largeImage = pixels > 8u * 1024u * 1024u;
    const int stride = largeImage ? 6 : 4;
    const int maxGroup = largeImage ? 8 : kBm3dMaxGroup;
    const int radius = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(searchRadius)
                                     * math::clamp(pixelScale, 0.5f, 2.0f))),
        3, largeImage ? 6 : 14);

    // 说明：这里使用矩形窗（不做分析 / 合成加窗）。
    // 加窗会把块估计变成 "窗 × 信号"，若聚合时不除以窗函数，块边界会被
    // 系统性压暗（平坦区反而出现高方差）。矩形窗配合重叠块与加权聚合
    // 更简单也更稳定；块边界不连续由多组估计平均来抑制。
    const Dct8Tables tables;
    std::vector<float> accum(pixels, 0.0f);
    std::vector<float> weightSum(pixels, 0.0f);

    // 参考块位置：等间距覆盖全图，最后一列 / 行贴边补齐
    std::vector<int> xPositions;
    std::vector<int> yPositions;
    for (int x = 0; x <= width - kBm3dBlock; x += stride) {
        xPositions.push_back(x);
    }
    if (xPositions.empty() || xPositions.back() != width - kBm3dBlock) {
        xPositions.push_back(width - kBm3dBlock);
    }
    for (int y = 0; y <= height - kBm3dBlock; y += stride) {
        yPositions.push_back(y);
    }
    if (yPositions.empty() || yPositions.back() != height - kBm3dBlock) {
        yPositions.push_back(height - kBm3dBlock);
    }

    struct Candidate {
        float distance = 0.0f;
        int dx = 0;
        int dy = 0;
    };

    const int blockCount = static_cast<int>(xPositions.size() * yPositions.size());

    RAWCORE_PARALLEL_FOR(blockIndex, blockCount)
    {
        const int bx = xPositions[static_cast<std::size_t>(blockIndex)
                                  % xPositions.size()];
        const int by = yPositions[static_cast<std::size_t>(blockIndex)
                                  / xPositions.size()];

        // ---- 1) 块匹配 ----
        Candidate best[kBm3dMaxGroup];
        int bestCount = 0;
        float worstDistance = 1e30f;

        for (int dy = -radius; dy <= radius; ++dy) {
            const int ny = by + dy;
            if (ny < 0 || ny > height - kBm3dBlock) {
                continue;
            }
            for (int dx = -radius; dx <= radius; ++dx) {
                const int nx = bx + dx;
                if (nx < 0 || nx > width - kBm3dBlock) {
                    continue;
                }

                // 以最差候选做提前终止：已经比当前组差就不需要算完
                float distance = 0.0f;
                bool earlyExit = false;
                for (int yy = 0; yy < kBm3dBlock && !earlyExit; ++yy) {
                    const float* a = plane.data()
                        + static_cast<std::size_t>(by + yy) * width + bx;
                    const float* b = plane.data()
                        + static_cast<std::size_t>(ny + yy) * width + nx;
                    for (int xx = 0; xx < kBm3dBlock; ++xx) {
                        const float delta = a[xx] - b[xx];
                        distance += delta * delta;
                        if (bestCount == maxGroup && distance > worstDistance) {
                            earlyExit = true;
                            break;
                        }
                    }
                }
                if (earlyExit) {
                    continue;
                }

                if (bestCount < maxGroup) {
                    // 未满：插入排序，保持距离升序
                    int position = bestCount;
                    while (position > 0 && best[position - 1].distance > distance) {
                        best[position] = best[position - 1];
                        --position;
                    }
                    best[position].distance = distance;
                    best[position].dx = dx;
                    best[position].dy = dy;
                    ++bestCount;
                } else if (distance < worstDistance) {
                    // 已满：替换最差项。从末尾向前移动，不会越界。
                    int position = maxGroup - 1;
                    while (position > 0 && best[position - 1].distance > distance) {
                        best[position] = best[position - 1];
                        --position;
                    }
                    best[position].distance = distance;
                    best[position].dx = dx;
                    best[position].dy = dy;
                }
                worstDistance = best[bestCount - 1].distance;
            }
        }

        // OpenMP 结构化块内不允许 return：没有候选块时直接跳过本参考块
        if (bestCount > 0) {
        // ---- 2) 协同变换 + 硬阈值 ----
        for (int k = 0; k < bestCount; ++k) {
            const int nx = bx + best[k].dx;
            const int ny = by + best[k].dy;

            float block[kBm3dBlock][kBm3dBlock];
            for (int yy = 0; yy < kBm3dBlock; ++yy) {
                const float* row = plane.data()
                    + static_cast<std::size_t>(ny + yy) * width + nx;
                for (int xx = 0; xx < kBm3dBlock; ++xx) {
                    block[yy][xx] = row[xx];
                }
            }

            dct8_2d(block, tables, false);

            int kept = 0;
            for (int yy = 0; yy < kBm3dBlock; ++yy) {
                for (int xx = 0; xx < kBm3dBlock; ++xx) {
                    if (std::fabs(block[yy][xx]) < threshold) {
                        block[yy][xx] = 0.0f;
                    } else {
                        ++kept;
                    }
                }
            }

            dct8_2d(block, tables, true);

            const float groupWeight =
                1.0f / (sigmaSquared * static_cast<float>(std::max(1, kept)));

            for (int yy = 0; yy < kBm3dBlock; ++yy) {
                const std::size_t rowIndex =
                    static_cast<std::size_t>(ny + yy) * width + nx;
                for (int xx = 0; xx < kBm3dBlock; ++xx) {
                    const std::size_t index = rowIndex + static_cast<std::size_t>(xx);
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp atomic
#endif
                    accum[index] += block[yy][xx] * groupWeight;
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp atomic
#endif
                    weightSum[index] += groupWeight;
                }
            }
        }
        }  // bestCount > 0
    }

    // ---- 3) 聚合，并按强度与原图混合 ----
    const float blend = math::clamp(amount * 1.25f, 0.0f, 1.0f);
    std::vector<float> output(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float filtered = (weightSum[index] > 1e-8f)
            ? accum[index] / weightSum[index] : plane[index];
        output[index] = math::lerp(plane[index], filtered, blend);
    }

    plane.swap(output);
}

}  // namespace

void applyNoiseReduction(ImageBuffer& image, float lumaStrength, float chromaStrength,
                         float pixelScale,
                         NoiseReductionMethod method,
                         int searchRadius)
{
    if (image.isEmpty()) {
        return;
    }

    const float lumaAmount = math::clamp(lumaStrength / 100.0f, 0.0f, 1.0f);
    const float chromaAmount = math::clamp(chromaStrength / 100.0f, 0.0f, 1.0f);

    if (lumaAmount <= 1e-4f && chromaAmount <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // 分离亮度与两个色度分量（YCbCr 式变换，色度对噪声最敏感）
    const bool denoiseLuma = lumaAmount > 1e-4f;
    const bool denoiseChroma = chromaAmount > 1e-4f;

    // 明度不做降噪时不必保存亮度平面：重新合成时可以从原 RGB 重新算回同一个 y
    // （此时 RGB 缓冲除了最后一次合成外没有被动过），省下一整份全尺寸数组。
    std::vector<float> luma;
    if (denoiseLuma) {
        luma.resize(pixels);
    }
    std::vector<float> chromaR(pixels);
    std::vector<float> chromaB(pixels);

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float r = data[index * 3u];
        const float g = data[index * 3u + 1];
        const float b = data[index * 3u + 2];
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        if (denoiseLuma) {
            luma[index] = y;
        }
        chromaR[index] = r - y;
        chromaB[index] = b - y;
    }

    // 三个平面共用同一组全尺寸临时缓冲，避免每个平面每层都重新分配/清零
    WaveletScratch scratch;

    // 色度降噪：色度可以承受更强的收缩，默认 15 也能实际生效。
    if (denoiseChroma) {
        waveletDenoisePlane(chromaR, width, height, chromaAmount, pixelScale, scratch);
        waveletDenoisePlane(chromaB, width, height, chromaAmount, pixelScale, scratch);
    }

    // 明度降噪（D5）：
    //  - 小波：交互预览默认，速度快；
    //  - 非局部均值：利用大片相似纹理的统计特性，纹理与边缘保持更好，
    //    适合最终导出。两者都只作用于亮度平面，色度上面已单独处理。
    if (denoiseLuma) {
        if (method == NoiseReductionMethod::NLMeans) {
            nlMeansDenoisePlane(luma, width, height, lumaAmount,
                                searchRadius, pixelScale);
        } else if (method == NoiseReductionMethod::Bm3d) {
            bm3dDenoisePlane(luma, width, height, lumaAmount,
                             searchRadius, pixelScale);
        } else {
            waveletDenoisePlane(luma, width, height, lumaAmount, pixelScale, scratch);
        }
    }

    // 重新合成 RGB
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        const float cr = chromaR[static_cast<std::size_t>(i)];
        const float cb = chromaB[static_cast<std::size_t>(i)];
        // 明度未做降噪时，原始 RGB 尚未被修改，可以原样重算亮度
        const float y = denoiseLuma
            ? luma[static_cast<std::size_t>(i)]
            : 0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2];

        pixel[0] = y + cr;
        pixel[1] = y - (0.299f / 0.587f) * cr - (0.114f / 0.587f) * cb;
        pixel[2] = y + cb;
    }
}

// ===========================================================================
//  9.5 彩纹抑制（CFA 采样格点偏置均衡）
// ===========================================================================

void suppressChromaMoire(ImageBuffer& image, float strength,
                         int cfaOriginX, int cfaOriginY)
{
    if (image.isEmpty() || strength <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    if (width < 16 || height < 16) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // 感知域色差：以平方根压到接近显示 gamma 的尺度，暗部的色差绝对值
    // 才不会小到失去判断意义；以 G 为基准可避开白平衡通道增益的影响，
    // 而且 G 采样最密、去马赛克最可靠。
    std::vector<float> diff1(pixels);
    std::vector<float> diff2(pixels);

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float r = std::sqrt(std::max(0.0f, data[index * 3u]));
        const float g = std::sqrt(std::max(0.0f, data[index * 3u + 1u]));
        const float b = std::sqrt(std::max(0.0f, data[index * 3u + 2u]));
        diff1[index] = r - g;
        diff2[index] = b - g;
    }

    // 8x8 分块。块内通常覆盖多个纹理周期，块均值可视为该区域的平均色度。
    // 拜耳阵列把像素分成 4 个采样格点（2x2 单元内的四个位置），周期纹理
    // 的拍频误差表现为**格点之间的均值差**；真实色彩细节则在四个格点上
    // 连续一致。因此按格点分别统计色差均值，再把每个格点移到块均值：
    // 彩纹（格点偏置）被抹平，格点内部的逐像素色度波动完整保留。
    constexpr int kFactor = 8;
    const int lowWidth = (width + kFactor - 1) / kFactor;
    const int lowHeight = (height + kFactor - 1) / kFactor;
    const std::size_t lowCount =
        static_cast<std::size_t>(lowWidth) * static_cast<std::size_t>(lowHeight);

    // 四个格点各自的色差和与计数。下标 t = py * 2 + px（拜耳坐标系）。
    std::vector<float> sum1[4];
    std::vector<float> sum2[4];
    std::vector<int> count[4];
    for (int t = 0; t < 4; ++t) {
        sum1[t].assign(lowCount, 0.0f);
        sum2[t].assign(lowCount, 0.0f);
        count[t].assign(lowCount, 0);
    }

    RAWCORE_PARALLEL_FOR(ly, lowHeight)
    {
        const int y0 = ly * kFactor;
        const int y1 = std::min(y0 + kFactor, height);
        for (int lx = 0; lx < lowWidth; ++lx) {
            const int x0 = lx * kFactor;
            const int x1 = std::min(x0 + kFactor, width);
            const std::size_t li =
                static_cast<std::size_t>(ly) * lowWidth + static_cast<std::size_t>(lx);

            for (int y = y0; y < y1; ++y) {
                const std::size_t rowBase = static_cast<std::size_t>(y) * width;
                // 格点编号按拜耳坐标计算，裁剪后仍与原始阵列对齐
                const int py = ((y + cfaOriginY) % 2 + 2) % 2;
                for (int x = x0; x < x1; ++x) {
                    const int px = ((x + cfaOriginX) % 2 + 2) % 2;
                    const int t = py * 2 + px;
                    const std::size_t i = rowBase + static_cast<std::size_t>(x);
                    sum1[t][li] += diff1[i];
                    sum2[t][li] += diff2[i];
                    count[t][li] += 1;
                }
            }
        }
    }

    // 每个格点的修正量 = 块均值 - 格点均值。四个格点合起来块内色度总量
    // 保持不变，因此不会引入整体偏色。
    std::vector<float> shift1[4];
    std::vector<float> shift2[4];
    for (int t = 0; t < 4; ++t) {
        shift1[t].assign(lowCount, 0.0f);
        shift2[t].assign(lowCount, 0.0f);
    }

    RAWCORE_PARALLEL_FOR(li, static_cast<int>(lowCount))
    {
        const std::size_t index = static_cast<std::size_t>(li);
        int total = 0;
        float totalSum1 = 0.0f;
        float totalSum2 = 0.0f;
        for (int t = 0; t < 4; ++t) {
            total += count[t][index];
            totalSum1 += sum1[t][index];
            totalSum2 += sum2[t][index];
        }
        if (total <= 0) {
            continue;
        }

        const float mean1 = totalSum1 / static_cast<float>(total);
        const float mean2 = totalSum2 / static_cast<float>(total);
        for (int t = 0; t < 4; ++t) {
            const int n = count[t][index];
            if (n <= 0) {
                continue;
            }
            shift1[t][index] =
                (mean1 - sum1[t][index] / static_cast<float>(n)) * amount;
            shift2[t][index] =
                (mean2 - sum2[t][index] / static_cast<float>(n)) * amount;
        }
    }

    // 修正量双线性放大回全分辨率。格点偏置本身是缓慢变化的，
    // 8 像素网格插值后足够平滑，不会留下块边界。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        const float fy = (static_cast<float>(y) + 0.5f) / kFactor - 0.5f;
        const int ly0 = math::clamp(static_cast<int>(std::floor(fy)), 0, lowHeight - 1);
        const int ly1 = std::min(ly0 + 1, lowHeight - 1);
        const float wy = math::clamp(fy - static_cast<float>(ly0), 0.0f, 1.0f);

        const int py = ((y + cfaOriginY) % 2 + 2) % 2;
        const std::size_t rowBase = static_cast<std::size_t>(y) * width;

        for (int x = 0; x < width; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / kFactor - 0.5f;
            const int lx0 = math::clamp(static_cast<int>(std::floor(fx)), 0, lowWidth - 1);
            const int lx1 = std::min(lx0 + 1, lowWidth - 1);
            const float wx = math::clamp(fx - static_cast<float>(lx0), 0.0f, 1.0f);

            const std::size_t i00 = static_cast<std::size_t>(ly0) * lowWidth + lx0;
            const std::size_t i01 = static_cast<std::size_t>(ly0) * lowWidth + lx1;
            const std::size_t i10 = static_cast<std::size_t>(ly1) * lowWidth + lx0;
            const std::size_t i11 = static_cast<std::size_t>(ly1) * lowWidth + lx1;

            const int px = ((x + cfaOriginX) % 2 + 2) % 2;
            const int t = py * 2 + px;

            const auto bilinear = [&](const std::vector<float>& plane) {
                const float top = math::lerp(plane[i00], plane[i01], wx);
                const float bottom = math::lerp(plane[i10], plane[i11], wx);
                return math::lerp(top, bottom, wy);
            };

            const float corr1 = bilinear(shift1[t]);
            const float corr2 = bilinear(shift2[t]);
            if (std::fabs(corr1) < 1e-6f && std::fabs(corr2) < 1e-6f) {
                continue;
            }

            const std::size_t index = rowBase + static_cast<std::size_t>(x);
            const float g = std::sqrt(std::max(0.0f, data[index * 3u + 1u]));
            // 感知域换回线性域：sqrt 的逆变换，负数钳到 0。
            // 只写回 R / B，G 通道保持不变，亮度细节完全不受影响。
            const float r = std::max(0.0f, g + diff1[index] + corr1);
            const float b = std::max(0.0f, g + diff2[index] + corr2);
            data[index * 3u]     = r * r;
            data[index * 3u + 2] = b * b;
        }
    }
}

// ===========================================================================
//  9.6 大尺度色纹抑制（低频色度重建）
// ===========================================================================

void suppressLargeChromaMoire(ImageBuffer& image, float strength)
{
    if (image.isEmpty() || strength <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    if (width < 32 || height < 32) {
        return;
    }

    const float amount = math::clamp(strength, 0.0f, 1.0f);
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // 感知域：以 G 为基准的两个色差。G 采样最密，去马赛克最可靠，
    // 而且不受白平衡通道增益影响（与 suppressChromaMoire 同一尺度）。
    // G 通道本身不会被修改，重建时从图像重新开方即可，省下一整份
    // 全尺寸亮度数组（45MP 约 180MB）。
    std::vector<float> diffR(pixels);
    std::vector<float> diffB(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float g = std::sqrt(std::max(0.0f, data[index * 3u + 1u]));
        diffR[index] = std::sqrt(std::max(0.0f, data[index * 3u])) - g;
        diffB[index] = std::sqrt(std::max(0.0f, data[index * 3u + 2u])) - g;
    }

    // 色度降到 1/2 分辨率做低通：2x2 平均先把最细的起伏压掉，
    // 再以 σ=2（半分辨率）= 全分辨率 σ≈4 的高斯平滑。σ≈4 能压掉
    // 周期 8~16 像素的拍频，而真实的大面积颜色变化仍在通带内。
    const int lowWidth = (width + 1) / 2;
    const int lowHeight = (height + 1) / 2;
    const std::size_t lowCount =
        static_cast<std::size_t>(lowWidth) * static_cast<std::size_t>(lowHeight);
    std::vector<float> lowR(lowCount, 0.0f);
    std::vector<float> lowB(lowCount, 0.0f);

    RAWCORE_PARALLEL_FOR(ly, lowHeight)
    {
        const int y0 = ly * 2;
        const int y1 = std::min(y0 + 2, height);
        for (int lx = 0; lx < lowWidth; ++lx) {
            const int x0 = lx * 2;
            const int x1 = std::min(x0 + 2, width);
            float sumR = 0.0f;
            float sumB = 0.0f;
            int count = 0;
            for (int y = y0; y < y1; ++y) {
                const std::size_t rowBase = static_cast<std::size_t>(y) * width;
                for (int x = x0; x < x1; ++x) {
                    sumR += diffR[rowBase + static_cast<std::size_t>(x)];
                    sumB += diffB[rowBase + static_cast<std::size_t>(x)];
                    ++count;
                }
            }
            const float inv = count > 0 ? 1.0f / static_cast<float>(count) : 0.0f;
            const std::size_t li =
                static_cast<std::size_t>(ly) * lowWidth + static_cast<std::size_t>(lx);
            lowR[li] = sumR * inv;
            lowB[li] = sumB * inv;
        }
    }

    // gaussianBlurPlane 的 radius 是 3σ 支撑半径：σ=2 → radius=6
    std::vector<float> blurR;
    std::vector<float> blurB;
    gaussianBlurPlane(lowR, blurR, lowWidth, lowHeight, 6.0f);
    gaussianBlurPlane(lowB, blurB, lowWidth, lowHeight, 6.0f);
    if (blurR.size() != lowCount || blurB.size() != lowCount) {
        return;
    }

    // 双线性放大回全分辨率并重建 R / B。G 通道原样不动，亮度细节
    // （包括纹理、边缘）完全保留，只有色度被替换为低频版本。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        const float fy = (static_cast<float>(y) - 0.5f) * 0.5f;
        const int ly0 = math::clamp(static_cast<int>(std::floor(fy)), 0, lowHeight - 1);
        const int ly1 = std::min(ly0 + 1, lowHeight - 1);
        const float wy = math::clamp(fy - static_cast<float>(ly0), 0.0f, 1.0f);

        const std::size_t rowBase = static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const float fx = (static_cast<float>(x) - 0.5f) * 0.5f;
            const int lx0 = math::clamp(static_cast<int>(std::floor(fx)), 0, lowWidth - 1);
            const int lx1 = std::min(lx0 + 1, lowWidth - 1);
            const float wx = math::clamp(fx - static_cast<float>(lx0), 0.0f, 1.0f);

            const std::size_t i00 =
                static_cast<std::size_t>(ly0) * lowWidth + static_cast<std::size_t>(lx0);
            const std::size_t i01 =
                static_cast<std::size_t>(ly0) * lowWidth + static_cast<std::size_t>(lx1);
            const std::size_t i10 =
                static_cast<std::size_t>(ly1) * lowWidth + static_cast<std::size_t>(lx0);
            const std::size_t i11 =
                static_cast<std::size_t>(ly1) * lowWidth + static_cast<std::size_t>(lx1);

            const auto bilinear = [&](const std::vector<float>& plane) {
                const float top = math::lerp(plane[i00], plane[i01], wx);
                const float bottom = math::lerp(plane[i10], plane[i11], wx);
                return math::lerp(top, bottom, wy);
            };

            const std::size_t index = rowBase + static_cast<std::size_t>(x);
            const float g = std::sqrt(std::max(0.0f, data[index * 3u + 1u]));
            const float r = g + math::lerp(diffR[index], bilinear(blurR), amount);
            const float b = g + math::lerp(diffB[index], bilinear(blurB), amount);

            // 感知域换回线性域，负数钳到 0。
            data[index * 3u] = std::max(0.0f, r) * std::max(0.0f, r);
            data[index * 3u + 2u] = std::max(0.0f, b) * std::max(0.0f, b);
        }
    }
}

// ===========================================================================
//  10. 锐化
// ===========================================================================

namespace {

/// 计算亮度平面，用于锐化的边缘掩码
std::vector<float> extractLumaPlane(const ImageBuffer& image)
{
    const std::size_t pixels = image.pixelCount();
    std::vector<float> luma(pixels);
    const float* data = image.data();
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        luma[index] = luminanceOf(data[index * 3u], data[index * 3u + 1], data[index * 3u + 2]);
    }
    return luma;
}

/// 由感知域亮度生成锐化边缘掩码（USM 与反卷积共用）。
/// 平坦区域接近 0（抑制噪点放大），强边缘接近 1。
std::vector<float> buildSharpenEdgeMask(const std::vector<float>& perceptual,
                                        int width, int height,
                                        float maskThreshold)
{
    const std::size_t pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> edgeMask(pixels, 1.0f);
    if (maskThreshold <= 1e-6f) {
        return edgeMask;
    }

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;

            float gradientX = 0.0f;
            float gradientY = 0.0f;

            const int sobelX[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
            const int sobelY[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};

            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = clampIndex(y + dy, 0, height - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = clampIndex(x + dx, 0, width - 1);
                    const float v = perceptual[static_cast<std::size_t>(ny) * width + nx];
                    gradientX += v * static_cast<float>(sobelX[dy + 1][dx + 1]);
                    gradientY += v * static_cast<float>(sobelY[dy + 1][dx + 1]);
                }
            }

            const float magnitude = 0.25f * std::sqrt(gradientX * gradientX
                                                      + gradientY * gradientY);
            edgeMask[index] = math::saturate(magnitude / maskThreshold);
        }
    }
    return edgeMask;
}

/// Richardson-Lucy 反卷积（D6）。
///
/// 用高斯 PSF 近似镜头 / 低通滤镜 / 去拜耳的模糊，迭代求解
///     estimate ← estimate × blur(I / blur(estimate))
/// 相比 USM，它能恢复被卷积"摊开"的细节，而不是单纯在边缘两侧加过冲，
/// 因此更不容易产生白边。代价是需要多次全图高斯模糊，耗时随迭代次数线性增长，
/// 所以默认关闭、由用户显式选择。
///
/// 仍保留边缘掩码与增量限幅：
///  - 平坦区域的 RL 主要放大噪点，掩码为 0 时完全跳过；
///  - 限幅防止病态解在强边缘产生振铃。
void richardsonLucyLuma(ImageBuffer& image,
                        const std::vector<float>& luma,
                        const std::vector<float>& edgeMask,
                        float strength,
                        float sigma,
                        int iterations)
{
    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = luma.size();
    if (width < 3 || height < 3 || pixels == 0) {
        return;
    }

    const int count = std::clamp(iterations, 3, 30);
    const float support = std::clamp(sigma * 3.0f, 3.0f, 60.0f);

    std::vector<float> estimate(pixels);
    std::vector<float> blurred(pixels);
    std::vector<float> ratio(pixels);
    std::vector<float> correction(pixels);

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        estimate[static_cast<std::size_t>(i)] =
            std::max(0.0f, luma[static_cast<std::size_t>(i)]);
    }

    for (int iteration = 0; iteration < count; ++iteration) {
        gaussianBlurPlane(estimate, blurred, width, height, support);

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            const std::size_t index = static_cast<std::size_t>(i);
            const float observed = std::max(0.0f, luma[index]);
            // 比值限幅：避免除零与某次迭代把局部推爆
            ratio[index] = math::clamp(observed / std::max(blurred[index], 1e-6f),
                                       0.2f, 5.0f);
        }

        gaussianBlurPlane(ratio, correction, width, height, support);

        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            const std::size_t index = static_cast<std::size_t>(i);
            const float observed = std::max(0.0f, luma[index]);
            // 上限取观测值的 8 倍：真实细节恢复不会超过这个量级，
            // 超过通常意味着噪声被当成信号放大。
            estimate[index] = std::clamp(
                estimate[index] * correction[index], 0.0f,
                observed * 8.0f + 1e-5f);
        }
    }

    float* data = image.data();
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float mask = edgeMask[index];
        if (mask <= 1e-4f) {
            continue;
        }

        float delta = estimate[index] - luma[index];
        // 感知域限幅：暗部允许的相对增量更小，避免噪点被强化
        const float maxDelta = 0.5f * std::max(1.0f, std::fabs(luma[index]));
        delta = math::clamp(delta * strength * mask, -maxDelta, maxDelta);
        if (std::fabs(delta) < 1e-7f) {
            continue;
        }

        float* pixel = data + index * 3u;
        pixel[0] += delta;
        pixel[1] += delta;
        pixel[2] += delta;
    }
}

}  // namespace

void applySharpen(ImageBuffer& image, float amount, float radius, float masking,
                  float pixelScale, SharpenMethod method, int iterations)
{
    if (image.isEmpty() || amount <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();

    const float strength = math::clamp(amount / 100.0f, 0.0f, 1.0f) * 2.0f;  // 允许达到 2x 强度
    // USM 的"半径"按惯例指高斯 σ；而 gaussianBlurPlane 的 radius 参数是
    // 3σ 支撑半径（内部 sigma = radius / 3）。这里必须乘 3，否则默认 0.8px
    // 实际只对应 σ≈0.3 的三抽头模糊，与恒等变换几乎没差别，
    // USM 差分近似为 0——锐化默认值形同虚设，1:1 查看就是"糊"的主因之一。
    // 半径按"原始像素"解释：降采样预览里同样的 scene 细节只占更少的像素，
    // 若直接套用原半径，fit 视图会比 1:1 明显更锐，松手导出后又"变软"。
    // 最小保留 0.3σ，保证小半径在低倍率预览下仍有可见作用。
    const float scaledRadius = radius * math::clamp(pixelScale, 0.05f, 4.0f);
    const float fineSigma = math::clamp(scaledRadius, 0.3f, 8.0f);
    // 粗尺度取 2.5 倍：细尺度补微细节，粗尺度补局部对比（纹理、砖缝）。
    // 两者叠加比单尺度 USM 更接近"捕获锐化"，且粗尺度权重更低，避免光晕。
    const float coarseSigma = std::min(fineSigma * 2.5f, 12.0f);
    // 掩码阈值 0~100 映射到感知域亮度差 0~0.10
    const float maskThreshold = math::clamp(masking / 100.0f, 0.0f, 1.0f) * 0.10f;

    // ---- 1) 在感知域计算两级 USM 的低频分量 ----
    //
    // 只在亮度通道上做模糊与锐化差分：
    //  - 三通道各自模糊需要 3 份全尺寸缓冲，40MP 图会额外占用约 480MB；
    //    亮度单通道方案只需少量缓冲，显著降低内存峰值（需求 §5.3、§5.5）。
    //  - 亮度锐化也更符合视觉习惯：人眼对亮度细节最敏感，
    //    而色度锐化容易在彩色边缘产生伪色。
    //
    // 关键：USM 差分必须在**感知域**做。管线在这里仍是线性数据，
    // 而人眼对暗部的相对亮度变化远比对亮部敏感。若直接在线性亮度上取
    // 差分，同样大小的增量在暗部相当于好几个感知级别（噪点被放大），
    // 在亮部却几乎看不出来（细节锐不起来），结果是"亮的糊、暗的脏"。
    // 用 sqrt 近似感知曲线（BT.709 传递函数的主体形状）：在线性化域做
    // USM，再按 d(y)/d(√y) = 2√y 换算回线性增量。
    const std::vector<float> luma = extractLumaPlane(image);
    std::vector<float> perceptual(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        perceptual[static_cast<std::size_t>(i)] =
            std::sqrt(std::max(0.0f, luma[static_cast<std::size_t>(i)]));
    }

    // ---- 2) 计算边缘掩码 ----
    // 需求 §2 明确要求"带边缘掩码，避免噪点放大"。
    // 掩码基于感知域的局部对比度：平坦区域（噪点为主）抑制锐化，
    // 边缘区域（真实细节）保留锐化。在线性域取梯度会让暗部纹理的
    // 掩码值小到几乎不参与锐化，这也是之前暗部"发虚"的来源之一。
    const std::vector<float> edgeMask =
        buildSharpenEdgeMask(perceptual, width, height, maskThreshold);

    // ---- 2.5) 反卷积分支（D6）----
    // PSF 半径沿用锐化半径的 σ 语义；strength 与 USM 共用同一强度映射。
    // 放在两级 USM 模糊之前：反卷积自己会做多次高斯模糊，
    // 提前返回可以省掉两次整幅模糊。
    if (method == SharpenMethod::Deconvolution) {
        richardsonLucyLuma(image, luma, edgeMask, strength, fineSigma, iterations);
        return;
    }

    std::vector<float> blurredFine;
    std::vector<float> blurredCoarse;
    gaussianBlurPlane(perceptual, blurredFine, width, height, fineSigma * 3.0f);
    gaussianBlurPlane(perceptual, blurredCoarse, width, height, coarseSigma * 3.0f);

    // ---- 3) 应用 USM ----
    // 感知域的差分 × 2√y 换算回线性亮度增量，再加到 R/G/B 上——
    // 这样只改变明暗，不引入色偏。
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float mask = edgeMask[index];

        // 掩码为 0 的像素完全跳过锐化（保留噪声原状而非放大它）
        if (mask <= 1e-4f) {
            continue;
        }

        const float root = perceptual[index];
        const float fineDetail = root - blurredFine[index];
        // 粗尺度是带通分量：细模糊减去粗模糊，代表中等尺度的局部对比
        const float coarseDetail = blurredFine[index] - blurredCoarse[index];
        const float detail = fineDetail + 0.45f * coarseDetail;

        // 感知域细节换算回线性域：d(y)/d(√y) = 2√y
        const float linearDetail = 2.0f * root * detail;

        // 限制单个像素的锐化增量，防止产生强过冲（halo）
        const float maxDelta = 0.25f * std::max(1.0f, std::fabs(luma[index]));
        const float clampedDelta = math::clamp(linearDetail * strength * mask,
                                               -maxDelta, maxDelta);

        if (std::fabs(clampedDelta) < 1e-7f) {
            continue;
        }

        float* pixel = data + index * 3u;
        pixel[0] += clampedDelta;
        pixel[1] += clampedDelta;
        pixel[2] += clampedDelta;
    }
}

void applyOutputSharpen(ImageBuffer& image, float amount, float radius)
{
    if (image.isEmpty() || amount <= 1e-4f) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = image.pixelCount();

    const float strength = math::clamp(amount / 100.0f, 0.0f, 1.0f) * 1.5f;
    // 输出锐化作用在最终像素上，半径语义与 USM 相同（σ），不乘场景比例。
    const float sigma = math::clamp(radius, 0.3f, 2.0f);

    // 输入已是 Gamma 编码的显示数据，直接在编码域做 USM。
    const std::vector<float> luma = extractLumaPlane(image);
    std::vector<float> blurred;
    gaussianBlurPlane(luma, blurred, width, height, sigma * 3.0f);

    float* data = image.data();
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float detail = luma[index] - blurred[index];

        // 输出锐化的半径很小，过冲上限取 10% 即可避免白边
        const float maxDelta = 0.10f * std::max(1.0f, std::fabs(luma[index]));
        const float delta = math::clamp(detail * strength, -maxDelta, maxDelta);
        if (std::fabs(delta) < 1e-7f) {
            continue;
        }

        float* pixel = data + index * 3u;
        pixel[0] += delta;
        pixel[1] += delta;
        pixel[2] += delta;
    }
}

// ===========================================================================
//  11. 色域映射与 Gamma 编码
// ===========================================================================

void applyGamutSoftClip(ImageBuffer& image)
{
    if (image.isEmpty()) {
        return;
    }

    // 软膝压缩：对超出 [0,1] 的值用平滑函数压回边界附近，
    // 而不是硬裁切。这样高光层次得以保留，避免生硬的截断边缘。
    //
    // 采用公式：x <= knee 时不变；x > knee 时用指数衰减逼近上限。
    // 两个阈值作为 lambda 内的局部常量定义，避免隐式捕获问题。

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    const auto softClip = [](float value) -> float {
        constexpr float k = 0.8f;  // 软膝起点
        constexpr float m = 1.0f;  // 硬上限

        if (value <= k) {
            // 负值也夹到 0——负的线性光没有物理意义
            return std::max(value, 0.0f);
        }

        // 超出软膝的部分按指数压缩到 (k, m) 区间
        const float excess = value - k;
        const float compressed = m - (m - k) * std::exp(-excess / (m - k));
        return math::clamp(compressed, k, m);
    };

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        pixel[0] = softClip(pixel[0]);
        pixel[1] = softClip(pixel[1]);
        pixel[2] = softClip(pixel[2]);
    }
}

void applyGammaEncode(ImageBuffer& image, ColorSpace space)
{
    if (image.isEmpty()) {
        return;
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    switch (space) {
    case ColorSpace::sRGB:
    case ColorSpace::DisplayP3:
        // Display P3 使用与 sRGB 相同的分段传递函数
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            for (int c = 0; c < 3; ++c) {
                pixel[c] = math::linearToSrgbGamma(pixel[c]);
            }
        }
        break;

    case ColorSpace::AdobeRGB:
        // AdobeRGB 使用纯幂函数，gamma = 563/256
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            for (int c = 0; c < 3; ++c) {
                pixel[c] = math::linearToPowerGamma(pixel[c], 563.0f / 256.0f);
            }
        }
        break;

    case ColorSpace::ProPhotoRGB:
        // ProPhotoRGB 使用 gamma 1.8
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
        {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            for (int c = 0; c < 3; ++c) {
                pixel[c] = math::linearToPowerGamma(pixel[c], 1.8f);
            }
        }
        break;

    case ColorSpace::LinearProPhotoRGB:
    case ColorSpace::XYZ:
        // 线性空间无需编码，保持原样
        break;
    }
}

void applySrgbEncode(ImageBuffer& image)
{
    applyGammaEncode(image, ColorSpace::sRGB);
}

// ===========================================================================
//  辅助：缩放与模糊
// ===========================================================================

ErrorInfo downsampleBayer(const BayerImage& bayer, BayerImage& output, int factor)
{
    if (bayer.isEmpty()) {
        return ErrorInfo::make(ErrorCode::Unknown, "拜耳数据为空，无法降采样。");
    }
    if (factor < 2) {
        output = bayer;  // 无需降采样
        return ErrorInfo::ok();
    }

    // 关键约束：下采样必须**保持拜耳排列的相位**。
    // 做法是把 factor 对齐到偶数（2 或 4 等），这样 2x2 单元的边界
    // 在下采样后位置不变，每个输出像素仍对应原排列中相同颜色的滤镜。
    if (factor % 2 != 0) {
        factor += 1;  // 强制为偶数
    }

    const int newWidth = bayer.width / factor;
    const int newHeight = bayer.height / factor;

    if (newWidth < 8 || newHeight < 8) {
        // 降采样后过小，无法进行 Demosaic 与后续处理。
        // 返回失败，由调用方决定是否改用原始尺寸。
        return ErrorInfo::make(ErrorCode::Unknown,
                               "降采样倍数过大，结果尺寸不足以渲染预览。",
                               "目标尺寸 " + std::to_string(newWidth) + "x"
                               + std::to_string(newHeight));
    }

    output.width = newWidth;
    output.height = newHeight;
    output.pattern = bayer.pattern;  // 排列相位保持
    output.data.resize(static_cast<std::size_t>(newWidth) * newHeight);

    // 每个输出像素只平均块内**同相位**的样本。
    //
    // 拜耳阵列的每个像素只记录一个通道，直接对整块求平均会把 R/G/B
    // 混在一起，输出阵列不再是有效的 CFA，后续 Demosaic 会把它误当成
    // 拜耳数据，产生整幅偏色（典型表现为紫色/品红）。正确做法是：输出
    // 像素 (x,y) 的相位由 (x%2, y%2) 决定，只取块内局部奇偶性与之一致
    // 的输入像素求均值，从而保持拜耳排列与通道构成不变。
    //
    // 块起点是 factor 的整数倍，factor 为偶数，故块起点相位恒为 (0,0)。
    const int samplesPerOutput = (factor / 2) * (factor / 2);
    const float inverseCount = 1.0f / static_cast<float>(samplesPerOutput);

    RAWCORE_PARALLEL_FOR(y, newHeight)
    {
        for (int x = 0; x < newWidth; ++x)
        {
            float sum = 0.0f;
            const int baseX = x * factor;
            const int baseY = y * factor;
            const int parityX = x & 1;
            const int parityY = y & 1;

            for (int dy = parityY; dy < factor; dy += 2) {
                const float* row = bayer.data.data()
                    + static_cast<std::size_t>(baseY + dy) * bayer.width + baseX;
                for (int dx = parityX; dx < factor; dx += 2) {
                    sum += row[dx];
                }
            }

            output.data[static_cast<std::size_t>(y) * newWidth + x] = sum * inverseCount;
        }
    }

    return ErrorInfo::ok();
}

void applyRetouch(ImageBuffer& image, const RetouchSettings& settings)
{
    if (image.isEmpty() || settings.spots.empty()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    const float shortEdge = static_cast<float>(std::min(width, height));

    for (const RetouchSpot& spot : settings.spots) {
        const float radiusPx = std::max(
            1.0f, math::clamp(spot.radius, 0.001f, 0.5f) * shortEdge);
        const float opacity = math::clamp(spot.opacity, 0.0f, 1.0f);
        const float feather = math::clamp(spot.feather, 0.0f, 1.0f);
        if (opacity <= 1e-4f) {
            continue;
        }

        const float targetCx = math::clamp(spot.targetX, 0.0f, 1.0f) * width;
        const float targetCy = math::clamp(spot.targetY, 0.0f, 1.0f) * height;
        const float sourceCx = math::clamp(spot.sourceX, 0.0f, 1.0f) * width;
        const float sourceCy = math::clamp(spot.sourceY, 0.0f, 1.0f) * height;

        // ---- 复制源区域到小缓冲，避免目标区域覆盖源区域时产生反馈 ----
        const int patchX0 = std::clamp(
            static_cast<int>(std::floor(sourceCx - radiusPx)), 0, width - 1);
        const int patchY0 = std::clamp(
            static_cast<int>(std::floor(sourceCy - radiusPx)), 0, height - 1);
        const int patchX1 = std::clamp(
            static_cast<int>(std::ceil(sourceCx + radiusPx)), patchX0, width - 1);
        const int patchY1 = std::clamp(
            static_cast<int>(std::ceil(sourceCy + radiusPx)), patchY0, height - 1);
        const int patchWidth = patchX1 - patchX0 + 1;
        const int patchHeight = patchY1 - patchY0 + 1;
        if (patchWidth <= 0 || patchHeight <= 0) {
            continue;
        }

        std::vector<float> patch(
            static_cast<std::size_t>(patchWidth) * patchHeight * 3u);
        for (int py = 0; py < patchHeight; ++py) {
            for (int px = 0; px < patchWidth; ++px) {
                const std::size_t dstIndex =
                    (static_cast<std::size_t>(py) * patchWidth + px) * 3u;
                for (int c = 0; c < 3; ++c) {
                    patch[dstIndex + static_cast<std::size_t>(c)] =
                        image.at(patchX0 + px, patchY0 + py, c);
                }
            }
        }

        const auto samplePatch = [&](float sampleX, float sampleY, int c) {
            const float px = math::clamp(sampleX - patchX0 - 0.5f,
                                         0.0f, static_cast<float>(patchWidth - 1));
            const float py = math::clamp(sampleY - patchY0 - 0.5f,
                                         0.0f, static_cast<float>(patchHeight - 1));
            const int x0 = std::min(static_cast<int>(std::floor(px)), patchWidth - 1);
            const int y0 = std::min(static_cast<int>(std::floor(py)), patchHeight - 1);
            const int x1 = std::min(x0 + 1, patchWidth - 1);
            const int y1 = std::min(y0 + 1, patchHeight - 1);
            const float fx = px - x0;
            const float fy = py - y0;
            const float v00 = patch[(static_cast<std::size_t>(y0) * patchWidth + x0) * 3u + c];
            const float v10 = patch[(static_cast<std::size_t>(y0) * patchWidth + x1) * 3u + c];
            const float v01 = patch[(static_cast<std::size_t>(y1) * patchWidth + x0) * 3u + c];
            const float v11 = patch[(static_cast<std::size_t>(y1) * patchWidth + x1) * 3u + c];
            const float top = v00 + (v10 - v00) * fx;
            const float bottom = v01 + (v11 - v01) * fx;
            return top + (bottom - top) * fy;
        };

        // ---- 计算目标 / 源区域的颜色均值（仅 Heal 需要）----
        float colorOffset[3] = {0.0f, 0.0f, 0.0f};
        if (spot.mode == RetouchMode::Heal) {
            float targetMean[3] = {0.0f, 0.0f, 0.0f};
            float sourceMean[3] = {0.0f, 0.0f, 0.0f};
            int targetCount = 0;
            int sourceCount = 0;

            const int tx0 = std::clamp(
                static_cast<int>(std::floor(targetCx - radiusPx)), 0, width - 1);
            const int tx1 = std::clamp(
                static_cast<int>(std::ceil(targetCx + radiusPx)), tx0, width - 1);
            const int ty0 = std::clamp(
                static_cast<int>(std::floor(targetCy - radiusPx)), 0, height - 1);
            const int ty1 = std::clamp(
                static_cast<int>(std::ceil(targetCy + radiusPx)), ty0, height - 1);

            for (int y = ty0; y <= ty1; ++y) {
                for (int x = tx0; x <= tx1; ++x) {
                    const float dx = (x + 0.5f) - targetCx;
                    const float dy = (y + 0.5f) - targetCy;
                    if (dx * dx + dy * dy > radiusPx * radiusPx) {
                        continue;
                    }
                    for (int c = 0; c < 3; ++c) {
                        targetMean[c] += image.at(x, y, c);
                    }
                    ++targetCount;
                }
            }

            for (int y = patchY0; y <= patchY1; ++y) {
                for (int x = patchX0; x <= patchX1; ++x) {
                    const float dx = (x + 0.5f) - sourceCx;
                    const float dy = (y + 0.5f) - sourceCy;
                    if (dx * dx + dy * dy > radiusPx * radiusPx) {
                        continue;
                    }
                    for (int c = 0; c < 3; ++c) {
                        sourceMean[c] += image.at(x, y, c);
                    }
                    ++sourceCount;
                }
            }

            if (targetCount > 0 && sourceCount > 0) {
                for (int c = 0; c < 3; ++c) {
                    colorOffset[c] = math::clamp(
                        targetMean[c] / targetCount - sourceMean[c] / sourceCount,
                        -0.75f, 0.75f);
                }
            }
        }

        // ---- 应用修复 / 仿制 ----
        const int tx0 = std::clamp(
            static_cast<int>(std::floor(targetCx - radiusPx)), 0, width - 1);
        const int tx1 = std::clamp(
            static_cast<int>(std::ceil(targetCx + radiusPx)), tx0, width - 1);
        const int ty0 = std::clamp(
            static_cast<int>(std::floor(targetCy - radiusPx)), 0, height - 1);
        const int ty1 = std::clamp(
            static_cast<int>(std::ceil(targetCy + radiusPx)), ty0, height - 1);

        for (int y = ty0; y <= ty1; ++y) {
            for (int x = tx0; x <= tx1; ++x) {
                const float dx = (x + 0.5f) - targetCx;
                const float dy = (y + 0.5f) - targetCy;
                const float normalized =
                    std::sqrt(dx * dx + dy * dy) / radiusPx;
                if (normalized >= 1.0f) {
                    continue;
                }

                const float falloff =
                    1.0f - smoothStep01(1.0f - feather, 1.0f, normalized);
                const float weight = falloff * opacity;
                if (weight <= 1e-5f) {
                    continue;
                }

                float* pixel = &image.at(x, y, 0);
                const float target[3] = {pixel[0], pixel[1], pixel[2]};
                const float sampleX = sourceCx + dx;
                const float sampleY = sourceCy + dy;

                for (int c = 0; c < 3; ++c) {
                    float replacement = samplePatch(sampleX, sampleY, c);
                    if (spot.mode == RetouchMode::Heal) {
                        replacement += colorOffset[c];
                    }
                    replacement = std::max(0.0f, replacement);
                    pixel[c] = target[c] + (replacement - target[c]) * weight;
                }
            }
        }
    }
}

namespace {

/// 解 4 点单应矩阵：dst = H * src（齐次坐标，h33 固定为 1）。
bool solveHomography(const double src[4][2], const double dst[4][2],
                     double H[3][3])
{
    double a[8][9] = {};

    for (int i = 0; i < 4; ++i) {
        const double u = src[i][0];
        const double v = src[i][1];
        const double x = dst[i][0];
        const double y = dst[i][1];

        a[2 * i][0] = u;
        a[2 * i][1] = v;
        a[2 * i][2] = 1.0;
        a[2 * i][6] = -u * x;
        a[2 * i][7] = -v * x;
        a[2 * i][8] = x;

        a[2 * i + 1][3] = u;
        a[2 * i + 1][4] = v;
        a[2 * i + 1][5] = 1.0;
        a[2 * i + 1][6] = -u * y;
        a[2 * i + 1][7] = -v * y;
        a[2 * i + 1][8] = y;
    }

    // 高斯-约当消元（列主元）
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-10) {
            return false;
        }
        if (pivot != col) {
            for (int k = col; k < 9; ++k) {
                std::swap(a[pivot][k], a[col][k]);
            }
        }

        const double divisor = a[col][col];
        for (int k = col; k < 9; ++k) {
            a[col][k] /= divisor;
        }

        for (int row = 0; row < 8; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = a[row][col];
            if (std::fabs(factor) < 1e-14) {
                continue;
            }
            for (int k = col; k < 9; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    H[0][0] = a[0][8];
    H[0][1] = a[1][8];
    H[0][2] = a[2][8];
    H[1][0] = a[3][8];
    H[1][1] = a[4][8];
    H[1][2] = a[5][8];
    H[2][0] = a[6][8];
    H[2][1] = a[7][8];
    H[2][2] = 1.0;
    return true;
}

}  // namespace

bool applyPerspective(ImageBuffer& image, const PerspectiveSettings& settings)
{
    if (image.isEmpty() || !settings.enable || settings.isIdentity()) {
        return false;
    }

    const double vertical = std::clamp(
        static_cast<double>(settings.vertical) / 100.0, -1.0, 1.0);
    const double horizontal = std::clamp(
        static_cast<double>(settings.horizontal) / 100.0, -1.0, 1.0);
    const double zoomAmount = std::clamp(
        static_cast<double>(settings.scale) / 100.0, 0.0, 1.0);

    // 单参数梯形：正值让采样区域上窄下宽，输出拉平后即校正汇聚竖线。
    const double v = vertical * 0.25;
    const double h = horizontal * 0.25;
    const double topWidth = 1.0 - v;
    const double bottomWidth = 1.0 + v;
    const double leftHeight = 1.0 - h;
    const double rightHeight = 1.0 + h;

    double quad[4][2] = {
        {0.5 - 0.5 * topWidth, 0.5 - 0.5 * leftHeight},      // TL
        {0.5 + 0.5 * topWidth, 0.5 - 0.5 * rightHeight},     // TR
        {0.5 + 0.5 * bottomWidth, 0.5 + 0.5 * rightHeight},  // BR
        {0.5 - 0.5 * bottomWidth, 0.5 + 0.5 * leftHeight},   // BL
    };

    // 缩放：正值放大画面（采样区域围绕中心缩小）
    const double zoom = 1.0 / (1.0 + zoomAmount * 0.5);
    for (auto& corner : quad) {
        corner[0] = 0.5 + (corner[0] - 0.5) * zoom;
        corner[1] = 0.5 + (corner[1] - 0.5) * zoom;
    }

    // 自动收窄：四角超出 [0,1] 时围绕中心缩放，避免出现黑边。
    double extent = 0.0;
    for (const auto& corner : quad) {
        extent = std::max(extent, std::fabs(corner[0] - 0.5));
        extent = std::max(extent, std::fabs(corner[1] - 0.5));
    }
    if (extent > 0.5) {
        const double fit = 0.5 / extent;
        for (auto& corner : quad) {
            corner[0] = 0.5 + (corner[0] - 0.5) * fit;
            corner[1] = 0.5 + (corner[1] - 0.5) * fit;
        }
    }

    const double outputCorners[4][2] = {
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0},
    };

    double homography[3][3] = {};
    if (!solveHomography(outputCorners, quad, homography)) {
        return false;
    }

    const int width = image.width();
    const int height = image.height();
    ImageBuffer warped(width, height);

    RAWCORE_PARALLEL_FOR(y, height)
    {
        const double vCoord = (static_cast<double>(y) + 0.5) / height;
        for (int x = 0; x < width; ++x) {
            const double uCoord = (static_cast<double>(x) + 0.5) / width;

            const double denominator = homography[2][0] * uCoord
                + homography[2][1] * vCoord + homography[2][2];
            if (std::fabs(denominator) < 1e-12) {
                warped.at(x, y, 0) = 0.0f;
                warped.at(x, y, 1) = 0.0f;
                warped.at(x, y, 2) = 0.0f;
                continue;
            }

            const double sourceU = std::clamp(
                (homography[0][0] * uCoord + homography[0][1] * vCoord
                 + homography[0][2]) / denominator, 0.0, 1.0);
            const double sourceV = std::clamp(
                (homography[1][0] * uCoord + homography[1][1] * vCoord
                 + homography[1][2]) / denominator, 0.0, 1.0);

            const double px = sourceU * (width - 1);
            const double py = sourceV * (height - 1);
            const int x0 = std::min(static_cast<int>(std::floor(px)), width - 1);
            const int y0 = std::min(static_cast<int>(std::floor(py)), height - 1);
            const int x1 = std::min(x0 + 1, width - 1);
            const int y1 = std::min(y0 + 1, height - 1);
            const float fx = static_cast<float>(px - x0);
            const float fy = static_cast<float>(py - y0);

            for (int c = 0; c < 3; ++c) {
                const float v00 = image.at(x0, y0, c);
                const float v10 = image.at(x1, y0, c);
                const float v01 = image.at(x0, y1, c);
                const float v11 = image.at(x1, y1, c);
                const float top = v00 + (v10 - v00) * fx;
                const float bottom = v01 + (v11 - v01) * fx;
                warped.at(x, y, c) = top + (bottom - top) * fy;
            }
        }
    }

    image.swap(warped);
    return true;
}

bool applyCrop(ImageBuffer& image, const CropSettings& settings,
               int* cropLeft, int* cropTop)
{
    if (cropLeft != nullptr) {
        *cropLeft = 0;
    }
    if (cropTop != nullptr) {
        *cropTop = 0;
    }

    if (image.isEmpty() || !settings.enable) {
        return false;
    }

    if (!std::isfinite(settings.x) || !std::isfinite(settings.y)
        || !std::isfinite(settings.width) || !std::isfinite(settings.height)) {
        return false;
    }

    const float x = math::clamp(settings.x, 0.0f, 1.0f);
    const float y = math::clamp(settings.y, 0.0f, 1.0f);
    const float width = math::clamp(settings.width, 0.0f, 1.0f - x);
    const float height = math::clamp(settings.height, 0.0f, 1.0f - y);
    if (width <= 1e-4f || height <= 1e-4f) {
        return false;
    }

    const int sourceWidth = image.width();
    const int sourceHeight = image.height();

    const int left = std::clamp(
        static_cast<int>(std::floor(x * sourceWidth)), 0, sourceWidth - 1);
    const int top = std::clamp(
        static_cast<int>(std::floor(y * sourceHeight)), 0, sourceHeight - 1);
    const int right = std::clamp(
        static_cast<int>(std::ceil((x + width) * sourceWidth)), left + 1, sourceWidth);
    const int bottom = std::clamp(
        static_cast<int>(std::ceil((y + height) * sourceHeight)), top + 1, sourceHeight);

    // 整幅且无拉直角度才是真正的空操作；
    // 仅设置角度时即使矩形覆盖全图也必须执行旋转。
    if (left == 0 && top == 0 && right == sourceWidth && bottom == sourceHeight
        && std::fabs(settings.angle) <= 1e-4f) {
        return false;
    }

    ImageBuffer cropped;

    if (std::fabs(settings.angle) <= 1e-4f) {
        // 纯裁剪：原样拷贝，不引入任何重采样
        cropImage(image, cropped, left, top, right - left, bottom - top);
    } else {
        // 拉直：以裁剪区域中心旋转，并自动收窄到不出现黑边的最大同比例矩形。
        //
        // 设裁剪区域宽高为 W×H，旋转角 θ，则输出矩形 (a×b, b/a = H/W)
        // 在旋转后的局部坐标中必须满足：
        //   a|cosθ| + b|sinθ| <= W
        //   a|sinθ| + b|cosθ| <= H
        // 取两式允许的最大缩放比例，得到无黑边的最大输出。
        const int cropWidth = right - left;
        const int cropHeight = bottom - top;
        const double theta = std::fabs(settings.angle) * 3.14159265358979323846 / 180.0;
        const double cosTheta = std::cos(theta);
        const double sinTheta = std::sin(theta);
        const double aspect = static_cast<double>(cropHeight) / cropWidth;  // H/W

        const double widthScale =
            1.0 / (cosTheta + aspect * sinTheta);
        const double heightScale =
            aspect / (sinTheta + aspect * cosTheta);
        const double scale = std::clamp(
            std::min(widthScale, heightScale), 0.0, 1.0);

        const int outputWidth = std::max(
            1, static_cast<int>(std::lround(cropWidth * scale)));
        const int outputHeight = std::max(
            1, static_cast<int>(std::lround(cropHeight * scale)));

        cropped.allocate(outputWidth, outputHeight);
        if (cropped.isEmpty()) {
            return false;
        }

        const double direction = (settings.angle >= 0.0f) ? 1.0 : -1.0;
        const double centerX = (left + right) * 0.5;
        const double centerY = (top + bottom) * 0.5;
        const double halfOutW = outputWidth * 0.5;
        const double halfOutH = outputHeight * 0.5;
        const int sourceWidth = image.width();
        const int sourceHeight = image.height();

        RAWCORE_PARALLEL_FOR(y, outputHeight)
        {
            for (int x = 0; x < outputWidth; ++x) {
                const double dx = (x + 0.5) - halfOutW;
                const double dy = (y + 0.5) - halfOutH;

                // 顺时针旋转的逆变换（屏幕上 y 轴向下）
                const double sx = centerX + dx * cosTheta
                    + direction * dy * sinTheta;
                const double sy = centerY - direction * dx * sinTheta
                    + dy * cosTheta;

                const double clampedX = std::clamp(
                    sx, 0.0, static_cast<double>(sourceWidth - 1));
                const double clampedY = std::clamp(
                    sy, 0.0, static_cast<double>(sourceHeight - 1));

                const int x0 = std::min(
                    static_cast<int>(std::floor(clampedX)), sourceWidth - 1);
                const int y0 = std::min(
                    static_cast<int>(std::floor(clampedY)), sourceHeight - 1);
                const int x1 = std::min(x0 + 1, sourceWidth - 1);
                const int y1 = std::min(y0 + 1, sourceHeight - 1);
                const float fx = static_cast<float>(clampedX - x0);
                const float fy = static_cast<float>(clampedY - y0);

                for (int c = 0; c < 3; ++c) {
                    const float v00 = image.at(x0, y0, c);
                    const float v10 = image.at(x1, y0, c);
                    const float v01 = image.at(x0, y1, c);
                    const float v11 = image.at(x1, y1, c);
                    const float topValue = v00 + (v10 - v00) * fx;
                    const float bottomValue = v01 + (v11 - v01) * fx;
                    cropped.at(x, y, c) = topValue + (bottomValue - topValue) * fy;
                }
            }
        }
    }

    if (cropped.isEmpty()) {
        return false;
    }
    image.swap(cropped);

    if (cropLeft != nullptr) {
        *cropLeft = left;
    }
    if (cropTop != nullptr) {
        *cropTop = top;
    }
    return true;
}

void cropImage(const ImageBuffer& source, ImageBuffer& dest,
               int x, int y, int width, int height)
{
    if (source.isEmpty() || width <= 0 || height <= 0) {
        dest.release();
        return;
    }

    // 夹取到源图范围内：调用方给的可能是"可见区域 + 外扩边距"，
    // 靠近图像边缘时会越界。
    const int left = clampIndex(x, 0, source.width() - 1);
    const int top = clampIndex(y, 0, source.height() - 1);
    const int right = clampIndex(x + width, left + 1, source.width());
    const int bottom = clampIndex(y + height, top + 1, source.height());
    const int cropWidth = right - left;
    const int cropHeight = bottom - top;

    dest.allocate(cropWidth, cropHeight);

    const float* src = source.data();
    float* dst = dest.data();
    const std::size_t sourceStride = static_cast<std::size_t>(source.width()) * 3u;
    const std::size_t cropStride = static_cast<std::size_t>(cropWidth) * 3u;

    RAWCORE_PARALLEL_FOR(row, cropHeight)
    {
        const float* srcRow = src
            + static_cast<std::size_t>(top + row) * sourceStride
            + static_cast<std::size_t>(left) * 3u;
        float* dstRow = dst + static_cast<std::size_t>(row) * cropStride;
        std::memcpy(dstRow, srcRow, cropStride * sizeof(float));
    }
}

void resizeImage(const ImageBuffer& source, ImageBuffer& dest, int newWidth, int newHeight)
{
    if (source.isEmpty() || newWidth <= 0 || newHeight <= 0) {
        return;
    }

    if (newWidth == source.width() && newHeight == source.height()) {
        dest = source;
        return;
    }

    dest.allocate(newWidth, newHeight);

    const float scaleX = static_cast<float>(source.width()) / static_cast<float>(newWidth);
    const float scaleY = static_cast<float>(source.height()) / static_cast<float>(newHeight);

    const int srcWidth = source.width();
    const int srcHeight = source.height();
    const float* src = source.data();
    float* dst = dest.data();

    RAWCORE_PARALLEL_FOR(y, newHeight)
    {
        for (int x = 0; x < newWidth; ++x)
        {
            // 双线性插值
            const float srcX = (static_cast<float>(x) + 0.5f) * scaleX - 0.5f;
            const float srcY = (static_cast<float>(y) + 0.5f) * scaleY - 0.5f;

            const int x0 = static_cast<int>(std::floor(srcX));
            const int y0 = static_cast<int>(std::floor(srcY));
            const int x1 = x0 + 1;
            const int y1 = y0 + 1;

            const float fx = srcX - static_cast<float>(x0);
            const float fy = srcY - static_cast<float>(y0);

            const int cx0 = clampIndex(x0, 0, srcWidth - 1);
            const int cx1 = clampIndex(x1, 0, srcWidth - 1);
            const int cy0 = clampIndex(y0, 0, srcHeight - 1);
            const int cy1 = clampIndex(y1, 0, srcHeight - 1);

            float* pixel = dst + (static_cast<std::size_t>(y) * newWidth + x) * 3u;

            for (int c = 0; c < 3; ++c) {
                const float topLeft = src[(static_cast<std::size_t>(cy0) * srcWidth + cx0) * 3u + c];
                const float topRight = src[(static_cast<std::size_t>(cy0) * srcWidth + cx1) * 3u + c];
                const float bottomLeft = src[(static_cast<std::size_t>(cy1) * srcWidth + cx0) * 3u + c];
                const float bottomRight = src[(static_cast<std::size_t>(cy1) * srcWidth + cx1) * 3u + c];

                const float top = math::lerp(topLeft, topRight, fx);
                const float bottom = math::lerp(bottomLeft, bottomRight, fx);
                pixel[c] = math::lerp(top, bottom, fy);
            }
        }
    }
}

void gaussianBlurPlane(const std::vector<float>& src,
                       std::vector<float>& dst,
                       int width, int height,
                       float radius)
{
    if (width <= 0 || height <= 0 || src.size() < static_cast<std::size_t>(width) * height) {
        dst.clear();
        return;
    }

    if (radius <= 0.05f) {
        dst = src;
        return;
    }

    // 分离式高斯：水平一次 + 垂直一次，复杂度从 O(r^2) 降到 O(r)
    // 半径决定核大小，3 sigma 覆盖 99% 能量
    const float sigma = std::max(radius / 3.0f, 0.3f);
    const int kernelRadius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    const int kernelSize = kernelRadius * 2 + 1;

    // 生成一维高斯核
    std::vector<float> kernel(static_cast<std::size_t>(kernelSize));
    float kernelSum = 0.0f;
    const float twoSigmaSquared = 2.0f * sigma * sigma;

    for (int i = 0; i < kernelSize; ++i) {
        const float offset = static_cast<float>(i - kernelRadius);
        const float value = std::exp(-(offset * offset) / twoSigmaSquared);
        kernel[static_cast<std::size_t>(i)] = value;
        kernelSum += value;
    }
    // 归一化，保证模糊不改变整体亮度
    for (float& value : kernel) {
        value /= kernelSum;
    }

    // ---- 水平方向 ----
    std::vector<float> temp(static_cast<std::size_t>(width) * height);

    RAWCORE_PARALLEL_FOR(y, height)
    {
        const float* row = src.data() + static_cast<std::size_t>(y) * width;
        float* outRow = temp.data() + static_cast<std::size_t>(y) * width;

        // 边界段需要夹取坐标；中间段抽头必然落在行内，走无夹取的紧凑循环，
        // 编译器可以把它向量化，这是全分辨率预览里最热的循环之一。
        const int interiorBegin = std::min(kernelRadius, width);
        const int interiorEnd = std::max(interiorBegin, width - kernelRadius);

        for (int x = 0; x < interiorBegin; ++x) {
            float sum = 0.0f;
            for (int k = 0; k < kernelSize; ++k) {
                const int nx = clampIndex(x + k - kernelRadius, 0, width - 1);
                sum += row[nx] * kernel[static_cast<std::size_t>(k)];
            }
            outRow[x] = sum;
        }

        for (int x = interiorBegin; x < interiorEnd; ++x) {
            const float* base = row + (x - kernelRadius);
            float sum = 0.0f;
            for (int k = 0; k < kernelSize; ++k) {
                sum += base[k] * kernel[static_cast<std::size_t>(k)];
            }
            outRow[x] = sum;
        }

        for (int x = interiorEnd; x < width; ++x) {
            float sum = 0.0f;
            for (int k = 0; k < kernelSize; ++k) {
                const int nx = clampIndex(x + k - kernelRadius, 0, width - 1);
                sum += row[nx] * kernel[static_cast<std::size_t>(k)];
            }
            outRow[x] = sum;
        }
    }

    // ---- 垂直方向 ----
    // 按行累加，而不是逐列去跨行取抽头：后者每个抽点都落在不同的行，
    // 37 抽头时等于把整幅图随机访问 37 次，缓存基本失效；
    // 按行累加是顺序读写，相邻输出行复用的源行还停留在缓存里。
    dst.resize(static_cast<std::size_t>(width) * height);

    RAWCORE_PARALLEL_FOR(y, height)
    {
        float* outRow = dst.data() + static_cast<std::size_t>(y) * width;
        std::fill(outRow, outRow + width, 0.0f);
        for (int k = 0; k < kernelSize; ++k) {
            const int ny = clampIndex(y + k - kernelRadius, 0, height - 1);
            const float* srcRow = temp.data() + static_cast<std::size_t>(ny) * width;
            const float weight = kernel[static_cast<std::size_t>(k)];
            for (int x = 0; x < width; ++x) {
                outRow[x] += weight * srcRow[x];
            }
        }
    }
}

void gaussianBlur(const ImageBuffer& source, ImageBuffer& dest, float radius, bool perChannel)
{
    if (source.isEmpty()) {
        dest.release();
        return;
    }

    const int width = source.width();
    const int height = source.height();
    const std::size_t pixels = source.pixelCount();

    dest.allocate(width, height);

    if (perChannel) {
        // 三通道分别模糊
        for (int c = 0; c < 3; ++c) {
            std::vector<float> plane(pixels);
            const float* src = source.data();
            for (std::size_t i = 0; i < pixels; ++i) {
                plane[i] = src[i * 3u + static_cast<std::size_t>(c)];
            }

            std::vector<float> blurred;
            gaussianBlurPlane(plane, blurred, width, height, radius);

            float* dst = dest.data();
            for (std::size_t i = 0; i < pixels; ++i) {
                dst[i * 3u + static_cast<std::size_t>(c)] = blurred[i];
            }
        }
    } else {
        // 按亮度统一模糊（保留色度关系，避免色边）
        std::vector<float> luma(pixels);
        const float* src = source.data();
        for (std::size_t i = 0; i < pixels; ++i) {
            luma[i] = luminanceOf(src[i * 3u], src[i * 3u + 1], src[i * 3u + 2]);
        }

        std::vector<float> blurredLuma;
        gaussianBlurPlane(luma, blurredLuma, width, height, radius);

        float* dst = dest.data();
        for (std::size_t i = 0; i < pixels; ++i) {
            const float originalLuma = luma[i];
            const float scale = (originalLuma > 1e-6f) ? (blurredLuma[i] / originalLuma) : 1.0f;
            dst[i * 3u]     = src[i * 3u]     * scale;
            dst[i * 3u + 1] = src[i * 3u + 1] * scale;
            dst[i * 3u + 2] = src[i * 3u + 2] * scale;
        }
    }
}

void flattenClippedHighlights(ImageBuffer& image,
                              const std::uint8_t* clipMask,
                              float minLevel)
{
    if (image.isEmpty() || clipMask == nullptr) {
        return;
    }

    constexpr std::uint8_t kAllChannels = kClipRed | kClipGreen | kClipBlue;
    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::uint8_t flags = clipMask[i] & kAllChannels;
        if (flags == 0u) {
            continue;
        }

        float* pixel = data + static_cast<std::size_t>(i) * 3u;
        const float peak = std::max({pixel[0], pixel[1], pixel[2]});

        if (flags == kAllChannels) {
            // 三通道都出现过饱和：颜色信息整体不可靠。降采样时掩码按位或，
            // 这里可能是"某个 2x2 单元全部饱和"，也可能是多个单元各自部分饱和，
            // 两种情况都应还原为中性白。亮度本来就够（饱和意味着亮），
            // 仅用 minLevel 排除异常数据。
            if (peak >= minLevel) {
                pixel[0] = peak;
                pixel[1] = peak;
                pixel[2] = peak;
            }
            continue;
        }

        float unclippedMax = 0.0f;
        if ((flags & kClipRed) == 0u) {
            unclippedMax = std::max(unclippedMax, pixel[0]);
        }
        if ((flags & kClipGreen) == 0u) {
            unclippedMax = std::max(unclippedMax, pixel[1]);
        }
        if ((flags & kClipBlue) == 0u) {
            unclippedMax = std::max(unclippedMax, pixel[2]);
        }

        // 未饱和通道很暗说明本体是深色物体，其颜色仍然可信；
        // 只有当未饱和通道也足够亮时，才认定是镜面/玻璃反光这类高光。
        if (unclippedMax < minLevel) {
            continue;
        }

        pixel[0] = peak;
        pixel[1] = peak;
        pixel[2] = peak;
    }
}

void suppressFalseColor(ImageBuffer& image, float relativeThreshold)
{
    if (image.isEmpty()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    if (width < 3 || height < 3) {
        return;
    }

    const float threshold = std::max(0.0f, relativeThreshold);

    // 绿色过小时色比不稳定（暗部噪声会被放大成虚假的"颜色"），直接跳过
    constexpr float kMinGreen = 1e-5f;
    // 色比量级的下限：避免邻域接近 0 时相对阈值退化为 0
    constexpr float kRatioFloor = 0.2f;

    // 读取源、写入副本：邻域读取不能被本像素的修改污染，
    // 同时又避免再来一份完整快照（预览可达数百 MB）。
    ImageBuffer output = image;
    const float* src = image.data();
    float* dst = output.data();

    // 8 个采样位置：四个轴向 + 四个对角，距离均为 2 像素
    static const int sampleOffsets[8][2] = {
        {-2, 0}, {2, 0}, {0, -2}, {0, 2},
        {-2, -2}, {2, -2}, {-2, 2}, {2, 2},
    };

    RAWCORE_PARALLEL_FOR(y, height)
    {
        for (int x = 0; x < width; ++x) {
            const std::size_t center = (static_cast<std::size_t>(y) * width + x) * 3u;
            const float green = src[center + 1];
            if (green <= kMinGreen) {
                continue;
            }

            // 用**色比**而不是色差：色比与白平衡增益无关，因此在白平衡之前
            // 处理也不会被通道增益推偏；同时亮部的镜面高光与暗部背景只要
            // 颜色相同，色比就一致，不会出现"高光处色偏"。
            const float ratioR = src[center] / green;
            const float ratioB = src[center + 2] / green;

            // 在 5x5 范围内按步长 2 采样（8 个邻点）求色比中值。
            //
            // 不用紧邻的 3x3：伪彩常常是 2x2 的簇，紧邻邻域里一半样本本身
            // 就是异常值，中值会被带偏。步长 2 的样本都落在簇外，是干净的背景，
            // 中值才可靠；同时 8 个样本的排序开销也很低。
            float neighborR[8];
            float neighborB[8];
            int count = 0;
            float minR = 0.0f, maxR = 0.0f, minB = 0.0f, maxB = 0.0f;

            for (int step = 0; step < 8; ++step) {
                const int nx = x + sampleOffsets[step][0];
                const int ny = y + sampleOffsets[step][1];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }
                const std::size_t index = (static_cast<std::size_t>(ny) * width + nx) * 3u;
                const float neighborGreen = src[index + 1];
                if (neighborGreen <= kMinGreen) {
                    continue;
                }
                const float r = src[index] / neighborGreen;
                const float b = src[index + 2] / neighborGreen;
                neighborR[count] = r;
                neighborB[count] = b;
                if (count == 0) {
                    minR = maxR = r;
                    minB = maxB = b;
                } else {
                    minR = std::min(minR, r);
                    maxR = std::max(maxR, r);
                    minB = std::min(minB, b);
                    maxB = std::max(maxB, b);
                }
                ++count;
            }

            if (count < 4) {
                continue;
            }

            // 快速筛查：中心色比落在邻域 [min,max] 内说明它不是孤立脉冲，
            // 平场、大面积同色区、粗色彩边缘都会在这里直接跳过，
            // 只有确实离群的点才付出中值排序的代价。
            const float toleranceR = threshold * std::max(maxR, kRatioFloor);
            const float toleranceB = threshold * std::max(maxB, kRatioFloor);
            const bool candidateR = ratioR < minR - toleranceR || ratioR > maxR + toleranceR;
            const bool candidateB = ratioB < minB - toleranceB || ratioB > maxB + toleranceB;
            if (!candidateR && !candidateB) {
                continue;
            }

            std::sort(neighborR, neighborR + count);
            std::sort(neighborB, neighborB + count);
            const float medianR = 0.5f * (neighborR[(count - 1) / 2] + neighborR[count / 2]);
            const float medianB = 0.5f * (neighborB[(count - 1) / 2] + neighborB[count / 2]);

            // 用中值色比重建：亮度沿用本像素绿色，只纠正颜色
            if (candidateR
                && std::fabs(ratioR - medianR) > threshold * std::max(medianR, kRatioFloor)) {
                dst[center] = green * medianR;
            }
            if (candidateB
                && std::fabs(ratioB - medianB) > threshold * std::max(medianB, kRatioFloor)) {
                dst[center + 2] = green * medianB;
            }
        }
    }

    image.swap(output);
}

void antialiasPreviewChroma(ImageBuffer& image)
{
    if (image.isEmpty()) {
        return;
    }

    const int width = image.width();
    const int height = image.height();
    if (width < 3 || height < 3) {
        return;
    }

    const std::size_t pixels = image.pixelCount();
    float* data = image.data();

    // 先拆出色度平面，避免边读边写：每个像素的邻域必须来自平滑前的数据。
    // Cr / Cb 采用与降噪一致的 YCbCr 近似权重，保证两处对"亮度"的定义相同。
    std::vector<float> chromaR(pixels);
    std::vector<float> chromaB(pixels);
    RAWCORE_PARALLEL_FOR(i, static_cast<int>(pixels))
    {
        const std::size_t index = static_cast<std::size_t>(i);
        const float r = data[index * 3u + 0u];
        const float g = data[index * 3u + 1u];
        const float b = data[index * 3u + 2u];
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        chromaR[index] = r - y;
        chromaB[index] = b - y;
    }

    // 十字核：中心 1/2、上下左右各 1/8。与 3x3 全核相比，
    // 它只在轴向借一个像素，横向色度细节保留得更多；
    // 实测色度高频能量与"全分辨率去马赛克 + 2x2 面积平均"基本一致。
    RAWCORE_PARALLEL_FOR(y, height)
    {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        const std::size_t rowUp =
            static_cast<std::size_t>(std::max(0, y - 1)) * width;
        const std::size_t rowDown =
            static_cast<std::size_t>(std::min(height - 1, y + 1)) * width;

        for (int x = 0; x < width; ++x) {
            const std::size_t index = row + static_cast<std::size_t>(x);
            const int xLeft = std::max(0, x - 1);
            const int xRight = std::min(width - 1, x + 1);

            const float filteredR =
                (4.0f * chromaR[index]
                 + chromaR[row + static_cast<std::size_t>(xLeft)]
                 + chromaR[row + static_cast<std::size_t>(xRight)]
                 + chromaR[rowUp + static_cast<std::size_t>(x)]
                 + chromaR[rowDown + static_cast<std::size_t>(x)]) * 0.125f;
            const float filteredB =
                (4.0f * chromaB[index]
                 + chromaB[row + static_cast<std::size_t>(xLeft)]
                 + chromaB[row + static_cast<std::size_t>(xRight)]
                 + chromaB[rowUp + static_cast<std::size_t>(x)]
                 + chromaB[rowDown + static_cast<std::size_t>(x)]) * 0.125f;

            // 亮度沿用本像素原值：色度平滑若同时改变亮度，细节就会被抹平，
            // 那正是用户看到的"糊"。绿通道由 R' / B' 反解，保证 Y 严格不变。
            const float originalR = data[index * 3u + 0u];
            const float originalG = data[index * 3u + 1u];
            const float originalB = data[index * 3u + 2u];
            const float luma = 0.299f * originalR + 0.587f * originalG
                             + 0.114f * originalB;

            const float r = std::max(luma + filteredR, 0.0f);
            const float b = std::max(luma + filteredB, 0.0f);
            float g = (luma - 0.299f * r - 0.114f * b) / 0.587f;
            // 极端互补色边界（例如红蓝细纹）平滑后可能反解出负绿，
            // 按物理下限截断；这种像素的亮度误差远小于可见阈值。
            g = std::max(g, 0.0f);

            data[index * 3u + 0u] = r;
            data[index * 3u + 1u] = g;
            data[index * 3u + 2u] = b;
        }
    }
}

}  // namespace ops
}  // namespace rawcore
