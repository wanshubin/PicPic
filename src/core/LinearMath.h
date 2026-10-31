// ============================================================================
//  LinearMath.h —— 线性光域数学工具
//
//  强制约束（需求 §1.3.1）：所有底层光影、矩阵、调色运算必须在 32bit 浮点线性域。
//  本文件明确区分"线性"与"Gamma 编码"两套函数，命名上以 Linear / Gamma 前缀区分，
//  防止误用。Gamma 编码只允许出现在管线最后一步（§2 管线末端的 Gamma 编码）。
// ============================================================================

#pragma once

#include "Types.h"

#include <cmath>
#include <cstddef>

namespace rawcore {
namespace math {

// ---------------------------------------------------------------------------
//  标量工具
// ---------------------------------------------------------------------------

/// 数值区间夹取
template <typename T>
inline T clamp(T value, T low, T high)
{
    return value < low ? low : (value > high ? high : value);
}

/// 限制在 [0, 1]
inline float saturate(float value)
{
    return clamp(value, 0.0f, 1.0f);
}

/// 线性插值
inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
//  矩阵运算（3x3，行主序）
// ---------------------------------------------------------------------------

/// 矩阵乘向量：out = m * v
inline void multiply(const Matrix3x3& m, const float in[3], float out[3])
{
    out[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2];
    out[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2];
    out[2] = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2];
}

/// 矩阵乘矩阵：out = a * b
Matrix3x3 multiply(const Matrix3x3& a, const Matrix3x3& b);

/// 矩阵行列式，用于 CCM 合法性校验（需求 §1.3.7）
float determinant(const Matrix3x3& m);

/// 矩阵求逆。返回 false 表示奇异矩阵（不可逆）
bool invert(const Matrix3x3& m, Matrix3x3& out);

/// 转置
Matrix3x3 transpose(const Matrix3x3& m);

/// Bradford 色适应矩阵：把 XYZ 从 sourceWhiteXyz 白点映射到
/// destinationWhiteXyz 白点（两者均为 XYZ 三刺激值，Y 可取任意非零比例）。
///
/// 用途：DCP 的 ColorMatrix 定义在各自的标定光源下（A / D65），
/// 求逆得到的“相机 RGB → XYZ”仍以该光源为白点；进入 D50 工作空间前
/// 必须做色适应，否则会整幅偏色（典型表现是品红 / 蓝紫色偏）。
Matrix3x3 bradfordAdaptation(const float sourceWhiteXyz[3],
                             const float destinationWhiteXyz[3]);

/// 线性 ProPhoto RGB（D50 工作空间）→ CIE Lab(D50)。
///
/// 颜色范围蒙版要求跨亮度稳定地筛选色相 / 饱和度，必须在 Lab 空间计算。
/// 输入为管线工作空间的线性值，内部先转 XYZ(D50)，再按 CIE 公式转 Lab。
void linearProPhotoToLabD50(float r, float g, float b,
                            float& outL, float& outA, float& outB);

/// CIE Lab 的色相角（度，0~360）与彩度 C = sqrt(a² + b²)
void labToHueChroma(float a, float b, float& hueDegrees, float& chroma);

// ---------------------------------------------------------------------------
//  Gamma 编解码
//
//  重要：这些函数只在管线首尾使用——
//    - 输入端：读取已 gamma 编码的外部资源（如显示器 ICC 曲线）时解码
//    - 输出端：管线末端 Gamma 编码写出（需求 §2 倒数第二步）
//  管线中段的所有调色运算都不得调用这里的 encode 方向。
// ---------------------------------------------------------------------------

/// sRGB 传递函数：线性 → sRGB 编码（IEC 61966-2-1 标准分段函数）
inline float linearToSrgbGamma(float linear)
{
    if (linear <= 0.0f) {
        return 0.0f;
    }
    if (linear <= 0.0031308f) {
        return 12.92f * linear;
    }
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// sRGB 传递函数：sRGB 编码 → 线性
inline float srgbGammaToLinear(float encoded)
{
    if (encoded <= 0.0f) {
        return 0.0f;
    }
    if (encoded <= 0.04045f) {
        return encoded / 12.92f;
    }
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

/// 纯幂函数 Gamma 编码，用于 AdobeRGB（gamma 2.19921875）等
inline float linearToPowerGamma(float linear, float gamma)
{
    if (linear <= 0.0f) {
        return 0.0f;
    }
    return std::pow(linear, 1.0f / gamma);
}

/// 纯幂函数 Gamma 解码
inline float powerGammaToLinear(float encoded, float gamma)
{
    if (encoded <= 0.0f) {
        return 0.0f;
    }
    return std::pow(encoded, gamma);
}

// ---------------------------------------------------------------------------
//  亮度 / 色度辅助
// ---------------------------------------------------------------------------

/// ProPhotoRGB 亮度权重（D50 白点下的 Y 分量系数）
/// 用于降噪时分离明度与色度、以及明度曲线。
inline float lumaProPhoto(const float rgb[3])
{
    return 0.2880402f * rgb[0] + 0.7118741f * rgb[1] + 0.0000857f * rgb[2];
}

/// sRGB / Rec.709 亮度权重，用于需要感知亮度的场景
inline float lumaSrgb(const float rgb[3])
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// ---------------------------------------------------------------------------
//  色温辅助
// ---------------------------------------------------------------------------

/// 把色温（开尔文）转换为 xy 色度坐标（Planckian locus 近似）。
/// 采用 Kang et al. (2002) 的三次拟合，适用于 1667K~25000K。
void temperatureToXy(float kelvin, float& outX, float& outY);

/// 把 xy 色度坐标转换为色温（开尔文）。反解失败时返回 -1。
float xyToTemperature(float x, float y);

/// 由色温 + 色调生成 XYZ 白点坐标。
/// tint 为绿-品红轴上偏移，正值偏品红、负值偏绿（与 Lightroom 手感一致）
void whitePointFromTemperatureTint(float kelvin, float tint, float outXyz[3]);

}  // namespace math
}  // namespace rawcore
