// ============================================================================
//  Dither.h —— 8 位量化用的抖动
//
//  16 位转 8 位时无论是 `>> 8` 还是四舍五入，本质都是硬截断：平滑渐变
//  （天空、暗角、弥散光）在 8 位下会留下可见的等亮度色带。叠加三角分布
//  （TPDF）抖动能把量化误差打散成肉眼不可见的细噪点，这是 8 位输出的
//  标准做法。
//
//  预览显示与 8 位导出共用同一套实现，同一像素在两条路径上得到相同的
//  抖动值，不会出现"预览干净、导出却有色带"的不一致。
// ============================================================================
#ifndef RAWCORE_DITHER_H
#define RAWCORE_DITHER_H

#include <cstdint>

namespace rawcore {

/// 位置相关的确定性三角分布抖动，返回值在 [-1, 1]。
///
/// 由两段互不相关的均匀分布相加得到，是与信号无关的零均值噪声：
/// 它把量化误差变成白噪声，而不是让误差跟着信号走。
///
/// 噪声由像素**绝对坐标**哈希得到：局部渲染（ROI）与整幅渲染对同一像素
/// 产生相同的抖动值，平移视口不会闪烁，也不会在拼接处出现色差。
inline float ditherOffset(int x, int y, int channel)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u
                    ^ static_cast<std::uint32_t>(y) * 19349663u
                    ^ static_cast<std::uint32_t>(channel) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    const float u1 = static_cast<float>(h & 0xFFFFu) * (1.0f / 65535.0f);
    const float u2 = static_cast<float>((h >> 16) & 0xFFFFu) * (1.0f / 65535.0f);
    return u1 + u2 - 1.0f;
}

/// 抖动幅度，单位为 8 位级别（LSB）：0.5 即 ±0.5 LSB，峰峰值 1 级。
///
/// 再大虽然能把色带打得更碎，噪声也会开始被看出来；0.5 级是 8 位输出的
/// 常用取值，抖动后的噪声 σ 约 0.2 级，低于任何 RAW 的噪声底。
constexpr float kDitherPeakLevels = 0.5f;

}  // namespace rawcore

#endif  // RAWCORE_DITHER_H
