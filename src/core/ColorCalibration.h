// ============================================================================
//  ColorCalibration.h —— CCM 矩阵引擎（需求 §3 模块 B，特色核心模块）
//
//  职责：
//   1. 存储 3x3 浮点矩阵 cam2xyz
//   2. 实现拜耳域逐像素线性矩阵乘法
//   3. 校验函数：行列式检测、极端数值检测，非法矩阵给出警告
//   4. 重置原厂矩阵、9 参数读写、导入/导出 CSV、读写 .camcal
//   5. 色卡采样自动求解 CCM（最小二乘，需求 §4）
//
//  重要概念区分（需求 §1.3.3）：
//   拜耳域 CCM 是线性 3x3 变换，作用在**拜耳域**（Demosaic 之前）；
//   它不是 3D LUT。两者在管线中位置独立，不可混淆。
//
//  本类只提供**校验与数据管理**，实际的矩阵乘法在 PipelineOps 中实现，
//  以保持纯函数式的管线结构、便于 OpenMP 并行与单元测试。
// ============================================================================

#pragma once

#include "EditContext.h"
#include "Types.h"

#include <string>
#include <vector>

namespace rawcore {

/// 矩阵校验结果的严重程度
enum class MatrixValidationLevel : std::uint8_t {
    Valid = 0,    ///< 矩阵合法
    Warning,      ///< 可接受但存在风险（数值偏大、轻微非物理）
    Invalid,      ///< 非法（行列式接近零或为负、存在 NaN/Inf）
};

/// 矩阵校验报告
struct MatrixValidationResult {
    MatrixValidationLevel level = MatrixValidationLevel::Valid;

    /// 面向用户的中文提示，直接可显示在 UI 上（需求 §6：标红警告但不强制回滚）
    std::string message;

    /// 技术细节，写入日志面板
    std::string detail;

    float determinant = 0.0f;        ///< 实际行列式值
    float maxAbsElement = 0.0f;      ///< 最大元素绝对值

    bool isUsable() const { return level != MatrixValidationLevel::Invalid; }
};

/// CCM 矩阵引擎
class ColorCalibration {
public:
    ColorCalibration() = default;
    explicit ColorCalibration(const Matrix3x3& matrix) : m_matrix(matrix) {}

    // -----------------------------------------------------------------------
    //  矩阵存取
    // -----------------------------------------------------------------------

    const Matrix3x3& matrix() const { return m_matrix; }
    void setMatrix(const Matrix3x3& matrix) { m_matrix = matrix; }

    /// 按行/列读取单个元素（需求 §3 模块 F2：9 参数滑块 + 输入框）
    float element(int row, int col) const;
    void setElement(int row, int col, float value);

    /// 重置为相机原厂矩阵
    void resetToCameraMatrix(const Matrix3x3& cameraMatrix);

    /// 重置为单位矩阵（"无变换"状态）
    void resetToIdentity();

    /// 当前矩阵是否为单位矩阵
    bool isIdentity() const;

    /// 当前矩阵是否与给定原厂矩阵一致
    bool matchesCameraMatrix(const Matrix3x3& cameraMatrix) const;

    // -----------------------------------------------------------------------
    //  校验（需求 §1.3.7、§3 模块 B 第 3 条、§6）
    // -----------------------------------------------------------------------

    /// 校验任意矩阵的合法性
    static MatrixValidationResult validate(const Matrix3x3& matrix);

    /// 校验当前矩阵
    MatrixValidationResult validate() const { return validate(m_matrix); }

    /// 生成一个色彩中性、物理合理的默认矩阵，供"恢复默认"使用。
    /// 这是一个轻微的饱和度提升矩阵，不含任何厂商偏好（需求 §1.3.6）
    static Matrix3x3 defaultNeutralMatrix();

    // -----------------------------------------------------------------------
    //  CSV 导入导出（需求 §3 模块 B 第 4 条）
    // -----------------------------------------------------------------------

    /// 导出为 CSV 文本。格式为三行，每行三个以逗号分隔的浮点数。
    std::string toCsv() const;

    /// 从 CSV 文本导入。
    /// 解析宽容：允许任意空白、逗号或分号分隔，忽略 # 开头的注释行。
    /// @return 错误信息；成功时 code == Ok
    ErrorInfo fromCsv(const std::string& csvText);

    /// 从 CSV 文件读取
    ErrorInfo loadFromCsvFile(const std::string& filePath);

    /// 写入 CSV 文件
    ErrorInfo saveToCsvFile(const std::string& filePath) const;

    // -----------------------------------------------------------------------
    //  从色卡采样自动求解 CCM（需求 §4、§7）
    //
    //  对每个输出通道独立做最小二乘拟合，利用全部色块的冗余信息抑制噪声。
    // -----------------------------------------------------------------------

    /// @param sampledRgb  色卡各色块在相机 RGB 空间的实测值，每块 3 个分量
    /// @param referenceXyz 对应色块在标准光源下的 XYZ 参考值，每块 3 个分量
    /// @param outMatrix   求解得到的 cam2xyz 矩阵
    /// @param weights     每块拟合权重（可选）；为空表示等权。0 表示不参与拟合。
    /// @return 样本不足、数据退化或结果未通过合法性校验时返回错误
    static ErrorInfo solveFromColorChecker(
        const std::vector<std::array<float, 3>>& sampledRgb,
        const std::vector<std::array<float, 3>>& referenceXyz,
        Matrix3x3& outMatrix,
        const std::vector<float>* weights = nullptr);

    /// 供 UI 判断该功能是否可用
    static constexpr bool isColorCheckerSolvingAvailable() { return true; }

private:
    Matrix3x3 m_matrix = identityMatrix3x3();
};

// ---------------------------------------------------------------------------
//  矩阵的文本格式化（供 UI 显示与日志输出）
// ---------------------------------------------------------------------------

/// 把矩阵格式化为多行文本，元素保留指定小数位
std::string formatMatrix(const Matrix3x3& matrix, int precision = 4);

}  // namespace rawcore
