// ============================================================================
//  Lut3D.h —— .cube 3D LUT 解析与应用（需求 §2 管线第十二项）
//
//  支持：
//   - Adobe .cube 文本格式（LUT_3D_SIZE / DOMAIN_MIN / DOMAIN_MAX / TITLE）
//   - 三线性插值（质量与性能的平衡，也是多数修图软件的做法）
//   - 强度混合（opacity 0~1）
//   - 文件解析结果的线程安全缓存：连续拖动滑块时无需反复读盘解析
//
//  注意：本模块只依赖 STL，不包含任何 Qt 头文件（core 层约束）。
// ============================================================================

#pragma once

#include "Types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rawcore {

/// 已解析的 3D LUT
class Lut3D {
public:
    Lut3D() = default;

    // -----------------------------------------------------------------------
    //  解析
    // -----------------------------------------------------------------------

    /// 从 .cube 文件加载
    static ErrorInfo loadFromFile(const std::string& filePath, Lut3D& out);

    /// 从 .cube 文本解析（便于单元测试与脚本生成）
    static ErrorInfo parseCubeText(const std::string& text, Lut3D& out);

    /// 带缓存的加载：同一路径且文件未被修改时复用上次解析结果。
    /// 解析失败不缓存，便于用户修正文件后立即重试。
    /// 线程安全，可在渲染工作线程中调用。
    static std::shared_ptr<const Lut3D> loadCached(const std::string& filePath,
                                                   ErrorInfo& error);

    // -----------------------------------------------------------------------
    //  状态
    // -----------------------------------------------------------------------

    bool isValid() const { return m_size >= 2 && m_data.size() == expectedSize(); }

    /// 每维格点数（如 33 表示 33³ LUT）
    int size() const { return m_size; }

    const std::array<float, 3>& domainMin() const { return m_domainMin; }
    const std::array<float, 3>& domainMax() const { return m_domainMax; }

    // -----------------------------------------------------------------------
    //  应用
    // -----------------------------------------------------------------------

    /// 对交错 RGB 浮点图像应用 LUT，并与原值按 opacity 混合。
    /// opacity <= 0 或 LUT 无效时保持原图不变。
    void apply(ImageBuffer& image, float opacity) const;

private:
    std::size_t expectedSize() const
    {
        const std::size_t n = static_cast<std::size_t>(m_size);
        return n * n * n * 3u;
    }

    /// 采样单个输出通道（三线性插值）
    float sample(int r, int g, int b, int channel) const;

    int m_size = 0;
    std::array<float, 3> m_domainMin{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> m_domainMax{{1.0f, 1.0f, 1.0f}};

    /// 交错 RGB 的格点数值，顺序与 .cube 文件一致：
    /// R 变化最慢、G 居中、B 变化最快。
    std::vector<float> m_data;
};

}  // namespace rawcore
