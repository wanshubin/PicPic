// ============================================================================
//  LinearMath.cpp —— 线性光域数学实现
// ============================================================================

#include "LinearMath.h"

namespace rawcore {
namespace math {

// ---------------------------------------------------------------------------
//  矩阵运算
// ---------------------------------------------------------------------------

Matrix3x3 multiply(const Matrix3x3& a, const Matrix3x3& b)
{
    Matrix3x3 out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row][col] = a[row][0] * b[0][col]
                          + a[row][1] * b[1][col]
                          + a[row][2] * b[2][col];
        }
    }
    return out;
}

float determinant(const Matrix3x3& m)
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

bool invert(const Matrix3x3& m, Matrix3x3& out)
{
    const float det = determinant(m);
    // 行列式过小视为奇异矩阵。阈值取 1e-6 是经验值：
    // 正常色彩矩阵行列式量级在 0.1~10 之间，远高于此阈值。
    if (std::fabs(det) < 1e-6f) {
        return false;
    }

    const float invDet = 1.0f / det;

    // 伴随矩阵转置法求逆
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * invDet;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * invDet;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invDet;

    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * invDet;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invDet;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * invDet;

    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * invDet;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * invDet;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * invDet;

    return true;
}

Matrix3x3 transpose(const Matrix3x3& m)
{
    Matrix3x3 out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row][col] = m[col][row];
        }
    }
    return out;
}

Matrix3x3 bradfordAdaptation(const float sourceWhiteXyz[3],
                             const float destinationWhiteXyz[3])
{
    // Bradford 锥体响应矩阵及其逆矩阵（标准常量）
    constexpr Matrix3x3 kBradford{{
        {{ 0.8951f,  0.2664f, -0.1614f}},
        {{-0.7502f,  1.7135f,  0.0367f}},
        {{ 0.0389f, -0.0685f,  1.0296f}},
    }};
    constexpr Matrix3x3 kBradfordInverse{{
        {{ 0.9869929f, -0.1470543f, 0.1599627f}},
        {{ 0.4323053f,  0.5183603f, 0.0492912f}},
        {{-0.0085287f,  0.0400428f, 0.9684867f}},
    }};

    float sourceCone[3];
    float destinationCone[3];
    multiply(kBradford, sourceWhiteXyz, sourceCone);
    multiply(kBradford, destinationWhiteXyz, destinationCone);

    // 锥体响应按通道缩放：diag(dst / src) * M。
    // 分母过小说明白点无效，此时退回单位矩阵，避免产生极端数值。
    Matrix3x3 scaled{};
    for (int row = 0; row < 3; ++row) {
        if (std::fabs(sourceCone[row]) < 1e-6f) {
            return Matrix3x3{{
                {{1.0f, 0.0f, 0.0f}},
                {{0.0f, 1.0f, 0.0f}},
                {{0.0f, 0.0f, 1.0f}},
            }};
        }
        const float scale = destinationCone[row] / sourceCone[row];
        for (int col = 0; col < 3; ++col) {
            scaled[row][col] = kBradford[row][col] * scale;
        }
    }
    return multiply(kBradfordInverse, scaled);
}

// ---------------------------------------------------------------------------
//  色温 ↔ 色度坐标
//
//  采用 Tanner Helland / Kang et al. 常用的近似拟合。
//  精度对修图用途足够（色温主要用于生成白平衡缩放系数，
//  微调由用户在 UI 上继续完成）。
// ---------------------------------------------------------------------------

void temperatureToXy(float kelvin, float& outX, float& outY)
{
    // 限制到拟合公式的有效区间
    const float t = clamp(kelvin, 1000.0f, 40000.0f);
    const float invT = 1000.0f / t;  // 拟合公式以 1000K 为单位的倒数

    // 三次多项式拟合黑色轨迹（Kang et al. 2002）
    float x = 0.0f;
    float y = 0.0f;

    if (t <= 4000.0f) {
        x = -0.2661239f * invT * invT * invT
            - 0.2343589f * invT * invT
            + 0.8776956f * invT
            + 0.179910f;
        y = -0.9549476f * x * x * x
            - 1.37418593f * x * x
            + 2.09137015f * x
            - 0.16748867f;
    } else {
        x = -3.0258469f * invT * invT * invT
            + 2.1070379f * invT * invT
            + 0.2226347f * invT
            + 0.240390f;
        y = 3.0817580f * x * x * x
            - 5.87338670f * x * x
            + 3.75112997f * x
            - 0.37001483f;
    }

    outX = x;
    outY = y;
}

float xyToTemperature(float x, float y)
{
    // 反解采用 McCamy 近似公式，仅在 D 系列白点附近可靠
    if (y <= 1e-6f) {
        return -1.0f;
    }
    const float n = (x - 0.3320f) / (0.1858f - y);
    const float kelvin = 449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f;

    // 超出合理范围时认为反解失败
    if (kelvin < 1000.0f || kelvin > 40000.0f) {
        return -1.0f;
    }
    return kelvin;
}

void whitePointFromTemperatureTint(float kelvin, float tint, float outXyz[3])
{
    float x = 0.0f;
    float y = 0.0f;
    temperatureToXy(kelvin, x, y);

    // 色调（tint）沿垂直于黑体轨迹的方向移动白点。
    // tint 取值 -100~100，正值偏品红（+x 方向），负值偏绿。
    // 0.0002 是经验缩放，使满量程 tint 产生肉眼可辨但不夸张的偏移。
    const float tintOffset = clamp(tint, -100.0f, 100.0f) * 0.0002f;
    x += tintOffset;

    if (y <= 1e-6f) {
        y = 1e-6f;
    }

    // xy 色度 → XYZ（约定 Y = 1）
    outXyz[0] = x / y;
    outXyz[1] = 1.0f;
    outXyz[2] = (1.0f - x - y) / y;
}

void linearProPhotoToLabD50(float r, float g, float b,
                            float& outL, float& outA, float& outB)
{
    // ProPhoto(D50) → XYZ(D50)。第三行正是需求中的 ProPhoto CIE Y 系数。
    const float x = 0.7976749f * r + 0.1351917f * g + 0.0313534f * b;
    const float y = 0.2880402f * r + 0.7118741f * g + 0.0000857f * b;
    const float z = 0.8252100f * b;

    // D50 白点
    constexpr float kXn = 0.96422f;
    constexpr float kYn = 1.0f;
    constexpr float kZn = 0.82521f;

    const auto f = [](float t) {
        constexpr float epsilon = 216.0f / 24389.0f;
        return (t > epsilon) ? std::cbrt(t)
                             : ((24389.0f / 27.0f * t + 16.0f) / 116.0f);
    };

    const float fx = f(x / kXn);
    const float fy = f(y / kYn);
    const float fz = f(z / kZn);

    outL = 116.0f * fy - 16.0f;
    outA = 500.0f * (fx - fy);
    outB = 200.0f * (fy - fz);
}

void labToHueChroma(float a, float b, float& hueDegrees, float& chroma)
{
    chroma = std::sqrt(a * a + b * b);
    if (chroma < 1e-6f) {
        hueDegrees = 0.0f;
        return;
    }
    float hue = std::atan2(b, a) * 180.0f / 3.14159265358979323846f;
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    hueDegrees = hue;
}

}  // namespace math
}  // namespace rawcore
