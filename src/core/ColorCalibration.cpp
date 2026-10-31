// ============================================================================
//  ColorCalibration.cpp —— CCM 矩阵引擎实现
// ============================================================================

#include "ColorCalibration.h"

#include "LinearMath.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rawcore {

namespace {

// ---------------------------------------------------------------------------
//  校验阈值（经验值，来源见各行注释）
// ---------------------------------------------------------------------------

/// 行列式绝对值下限。低于此值矩阵接近奇异，变换会把大片色彩压平。
/// 正常相机矩阵行列式量级在 0.2 ~ 5 之间。
constexpr float kMinAbsDeterminant = 1e-3f;

/// 元素绝对值上限。超过意味着某个通道被极度放大，极易产生过饱和与色噪。
/// 正常相机矩阵元素范围约 -1.5 ~ 2.5。
constexpr float kMaxAbsElement = 4.0f;

/// 触发"警告"的较松阈值
constexpr float kWarnAbsDeterminantLow = 0.05f;
constexpr float kWarnAbsDeterminantHigh = 10.0f;
constexpr float kWarnMaxAbsElement = 2.5f;

/// 判断浮点值是否有效（非 NaN、非 Inf）
bool isFinite(float value)
{
    return std::isfinite(value);
}

}  // namespace

// ---------------------------------------------------------------------------
//  元素存取
// ---------------------------------------------------------------------------

float ColorCalibration::element(int row, int col) const
{
    if (row < 0 || row > 2 || col < 0 || col > 2) {
        return 0.0f;
    }
    return m_matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
}

void ColorCalibration::setElement(int row, int col, float value)
{
    if (row < 0 || row > 2 || col < 0 || col > 2) {
        return;
    }
    // 拒绝写入非有限值，防止 NaN 污染整个矩阵
    if (!isFinite(value)) {
        return;
    }
    m_matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = value;
}

// ---------------------------------------------------------------------------
//  重置
// ---------------------------------------------------------------------------

void ColorCalibration::resetToCameraMatrix(const Matrix3x3& cameraMatrix)
{
    m_matrix = cameraMatrix;
}

void ColorCalibration::resetToIdentity()
{
    m_matrix = identityMatrix3x3();
}

bool ColorCalibration::isIdentity() const
{
    constexpr float kEps = 1e-5f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float expected = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(element(r, c) - expected) > kEps) {
                return false;
            }
        }
    }
    return true;
}

bool ColorCalibration::matchesCameraMatrix(const Matrix3x3& cameraMatrix) const
{
    constexpr float kEps = 1e-5f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(element(r, c)
                          - cameraMatrix[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]) > kEps) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  校验
// ---------------------------------------------------------------------------

MatrixValidationResult ColorCalibration::validate(const Matrix3x3& matrix)
{
    MatrixValidationResult result;

    // ---- 1. 有限性检查：任一元素为 NaN/Inf 直接判定非法 ----
    float maxAbs = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float value = matrix[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (!isFinite(value)) {
                result.level = MatrixValidationLevel::Invalid;
                result.message = "矩阵包含无效数值（NaN 或无穷大），无法用于渲染。";
                result.detail = "位置 [" + std::to_string(r) + "][" + std::to_string(c) + "] 非有限";
                result.determinant = 0.0f;
                result.maxAbsElement = 0.0f;
                return result;
            }
            maxAbs = std::max(maxAbs, std::fabs(value));
        }
    }
    result.maxAbsElement = maxAbs;

    // ---- 2. 行列式检查 ----
    const float det = math::determinant(matrix);
    result.determinant = det;

    if (std::fabs(det) < kMinAbsDeterminant) {
        result.level = MatrixValidationLevel::Invalid;
        result.message = "矩阵接近奇异（行列式接近 0），会导致严重色偏或画面崩坏，"
                         "请不要使用该矩阵。";
        result.detail = "行列式 = " + std::to_string(det)
                      + "（阈值 " + std::to_string(kMinAbsDeterminant) + "）";
        return result;
    }

    if (det < 0.0f) {
        // 负行列式意味着矩阵翻转了某个坐标轴，会把图像色彩反转，物理上不合理
        result.level = MatrixValidationLevel::Invalid;
        result.message = "矩阵行列式为负，会反转色彩通道，属非物理变换，请不要使用。";
        result.detail = "行列式 = " + std::to_string(det);
        return result;
    }

    // ---- 3. 极端数值检查：超过硬上限则非法 ----
    if (maxAbs > kMaxAbsElement) {
        result.level = MatrixValidationLevel::Invalid;
        result.message = "矩阵元素数值过大，会产生严重过饱和与色彩溢出，请不要使用。";
        result.detail = "最大元素绝对值 = " + std::to_string(maxAbs)
                      + "（阈值 " + std::to_string(kMaxAbsElement) + "）";
        return result;
    }

    // ---- 4. 宽松范围：给出警告但不阻止（需求 §6：标红警告但不强制回滚）----
    std::vector<std::string> warnings;
    if (std::fabs(det) < kWarnAbsDeterminantLow) {
        warnings.push_back("行列式偏小（" + std::to_string(det) + "），色彩层次可能被压缩。");
    } else if (std::fabs(det) > kWarnAbsDeterminantHigh) {
        warnings.push_back("行列式偏大（" + std::to_string(det) + "），饱和度可能过强。");
    }
    if (maxAbs > kWarnMaxAbsElement) {
        warnings.push_back("矩阵存在较大元素（" + std::to_string(maxAbs) + "），可能出现色彩溢出。");
    }

    if (!warnings.empty()) {
        result.level = MatrixValidationLevel::Warning;
        std::string combined;
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            if (i > 0) {
                combined += " ";
            }
            combined += warnings[i];
        }
        result.message = "矩阵存在色彩风险：" + combined;
        result.detail = "行列式 = " + std::to_string(det)
                      + "，最大元素 = " + std::to_string(maxAbs);
        return result;
    }

    // ---- 5. 通过 ----
    result.level = MatrixValidationLevel::Valid;
    result.message = "矩阵合法。";
    result.detail = "行列式 = " + std::to_string(det);
    return result;
}

// ---------------------------------------------------------------------------
//  默认矩阵
// ---------------------------------------------------------------------------

Matrix3x3 ColorCalibration::defaultNeutralMatrix()
{
    // 一个轻微提升饱和度的中性矩阵，不含任何厂商色彩偏好（需求 §1.3.6）。
    // 对角线略大于 1、非对角略为负，是典型的"去串扰 + 适度增强"形态。
    return Matrix3x3{{
        {{ 1.05f, -0.04f, -0.01f}},
        {{-0.03f,  1.04f, -0.01f}},
        {{-0.01f, -0.05f,  1.06f}},
    }};
}

// ---------------------------------------------------------------------------
//  CSV
// ---------------------------------------------------------------------------

std::string ColorCalibration::toCsv() const
{
    std::ostringstream out;
    out << "# RAW Color Editor CCM (cam2xyz)\n";
    out << "# 每行对应输出通道 X/Y/Z，每列对应输入通道 CamR/CamG/CamB\n";
    out.precision(8);
    out << std::fixed;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (c > 0) {
                out << ',';
            }
            out << element(r, c);
        }
        out << '\n';
    }
    return out.str();
}

ErrorInfo ColorCalibration::fromCsv(const std::string& csvText)
{
    // 逐行解析，收集所有数值。宽容处理注释、空行与多种分隔符。
    std::vector<float> values;
    std::istringstream input(csvText);
    std::string line;

    while (std::getline(input, line)) {
        // 去掉行内注释
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }

        // 逗号与分号统一替换为空格，简化后续解析
        for (char& ch : line) {
            if (ch == ',' || ch == ';') {
                ch = ' ';
            }
        }

        std::istringstream lineStream(line);
        float value = 0.0f;
        while (lineStream >> value) {
            values.push_back(value);
        }
    }

    if (values.size() != 9) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "CSV 内容不是 3x3 矩阵（应当恰好包含 9 个数值）",
                               "实际解析到 " + std::to_string(values.size()) + " 个数值");
    }

    Matrix3x3 parsed{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            parsed[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] =
                values[static_cast<std::size_t>(r * 3 + c)];
        }
    }

    m_matrix = parsed;

    // 导入后立即校验，把结果反馈给调用方以便 UI 提示（需求 §6）
    const MatrixValidationResult validation = validate(m_matrix);
    if (validation.level == MatrixValidationLevel::Invalid) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "CSV 中的矩阵未通过合法性校验：" + validation.message,
                               validation.detail);
    }

    return ErrorInfo::ok();
}

ErrorInfo ColorCalibration::loadFromCsvFile(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        return ErrorInfo::make(ErrorCode::FileNotFound,
                               "无法打开 CSV 文件",
                               "路径: " + filePath);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return fromCsv(buffer.str());
}

ErrorInfo ColorCalibration::saveToCsvFile(const std::string& filePath) const
{
    std::ofstream file(filePath, std::ios::binary);
    if (!file) {
        return ErrorInfo::make(ErrorCode::ExportFailed,
                               "无法写入 CSV 文件，请检查路径与权限",
                               "路径: " + filePath);
    }

    file << toCsv();
    if (!file) {
        return ErrorInfo::make(ErrorCode::ExportFailed,
                               "CSV 写入过程中发生错误",
                               "路径: " + filePath);
    }

    return ErrorInfo::ok();
}

// ---------------------------------------------------------------------------
//  色卡自动求解 CCM（需求 §4、§7）
//
//  模型：对每个输出通道 k（X/Y/Z）独立求解
//      referenceXyz[i][k] ≈ sampledRgb[i][0] * m[k][0]
//                         + sampledRgb[i][1] * m[k][1]
//                         + sampledRgb[i][2] * m[k][2]
//  这是一个超定线性方程组，用正规方程 (AᵀA)·m = Aᵀb 求最小二乘解。
//
//  为什么不用逐点求解：色卡各色块含有测量噪声与传感器噪声，
//  三点定解会把噪声完全带入矩阵；最小二乘利用全部 24 块的冗余信息，
//  结果更稳健，这也是 x-rite / dcamprof 等工具的通例做法。
// ---------------------------------------------------------------------------

ErrorInfo ColorCalibration::solveFromColorChecker(
    const std::vector<std::array<float, 3>>& sampledRgb,
    const std::vector<std::array<float, 3>>& referenceXyz,
    Matrix3x3& outMatrix,
    const std::vector<float>* weights)
{
    outMatrix = identityMatrix3x3();

    const std::size_t count = sampledRgb.size();
    if (count < 3) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "色卡样本不足，至少需要 3 个色块才能求解矩阵",
                               "实际样本数 " + std::to_string(count));
    }
    if (referenceXyz.size() != count) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "采样值与参考值的色块数量不一致",
                               "采样 " + std::to_string(count)
                               + " 个，参考 " + std::to_string(referenceXyz.size()) + " 个");
    }

    // ---- 1. 输入有限性检查：NaN/Inf 会直接污染正规方程 ----
    for (std::size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (!isFinite(sampledRgb[i][static_cast<std::size_t>(c)])
                || !isFinite(referenceXyz[i][static_cast<std::size_t>(c)])) {
                return ErrorInfo::make(ErrorCode::InvalidMatrix,
                                       "色卡采样数据包含无效数值（NaN 或无穷大）",
                                       "第 " + std::to_string(i + 1) + " 个色块");
            }
        }
    }

    // ---- 2. 组装正规方程 AᵀA 与 Aᵀb ----
    if (weights != nullptr && weights->size() != count) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "色卡权重数量与色块数量不一致");
    }

    Matrix3x3 normal{};
    // rhs[输出通道][输入通道]
    Matrix3x3 rhs{};
    int activeCount = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const float weight = (weights != nullptr)
            ? std::clamp((*weights)[i], 0.0f, 1.0f) : 1.0f;
        if (weight <= 1e-6f) {
            continue;
        }
        ++activeCount;

        const float s0 = sampledRgb[i][0];
        const float s1 = sampledRgb[i][1];
        const float s2 = sampledRgb[i][2];

        normal[0][0] += weight * s0 * s0;
        normal[0][1] += weight * s0 * s1;
        normal[0][2] += weight * s0 * s2;
        normal[1][0] += weight * s1 * s0;
        normal[1][1] += weight * s1 * s1;
        normal[1][2] += weight * s1 * s2;
        normal[2][0] += weight * s2 * s0;
        normal[2][1] += weight * s2 * s1;
        normal[2][2] += weight * s2 * s2;

        for (int k = 0; k < 3; ++k) {
            const float ref = referenceXyz[i][static_cast<std::size_t>(k)];
            rhs[k][0] += weight * s0 * ref;
            rhs[k][1] += weight * s1 * ref;
            rhs[k][2] += weight * s2 * ref;
        }
    }

    if (activeCount < 3) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "启用的色块不足 3 个，无法求解 CCM");
    }

    // ---- 3. 求解：正常数矩阵必须可逆 ----
    Matrix3x3 normalInverse{};
    if (!math::invert(normal, normalInverse)) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "色卡样本退化（各色块采样值近似线性相关），无法求解矩阵",
                               "正规方程行列式 = " + std::to_string(math::determinant(normal)));
    }

    Matrix3x3 solved{};
    for (int k = 0; k < 3; ++k) {
        const float rhsRow[3] = {
            rhs[k][0], rhs[k][1], rhs[k][2],
        };
        float solution[3] = {0.0f, 0.0f, 0.0f};
        math::multiply(normalInverse, rhsRow, solution);
        solved[k][0] = solution[0];
        solved[k][1] = solution[1];
        solved[k][2] = solution[2];
    }

    // ---- 4. 结果校验：沿用统一的矩阵合法性规则（需求 §1.3.7、§6）----
    const MatrixValidationResult validation = validate(solved);
    if (!validation.isUsable()) {
        return ErrorInfo::make(ErrorCode::InvalidMatrix,
                               "色卡求解得到的矩阵未通过合法性校验：" + validation.message,
                               validation.detail + "；原始解 = " + formatMatrix(solved, 4));
    }

    outMatrix = solved;
    return ErrorInfo::ok();
}

// ---------------------------------------------------------------------------
//  格式化
// ---------------------------------------------------------------------------

std::string formatMatrix(const Matrix3x3& matrix, int precision)
{
    std::ostringstream out;
    out.precision(precision);
    out << std::fixed;
    for (int r = 0; r < 3; ++r) {
        out << "[ ";
        for (int c = 0; c < 3; ++c) {
            out << matrix[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (c < 2) {
                out << ", ";
            }
        }
        out << " ]";
        if (r < 2) {
            out << '\n';
        }
    }
    return out.str();
}

}  // namespace rawcore
