// ============================================================================
//  MaskModel.h —— 蒙版数据模型、几何求值与局部调整参数（B1/B2）
//
//  - 蒙版由若干图层组成，图层支持径向、线性渐变、画笔三种形状；
//  - 每个图层有启用、反相、不透明度、羽化参数；
//  - 每个图层带一组**局部调整**参数（曝光、对比、高光、阴影、色温、色调、饱和度）；
//  - buildWeightMap() 生成与图像同尺寸的权重图，供管线逐层混合；
//
//  所有坐标均为图像归一化坐标（0~1），与分辨率、裁剪无关。
//  weightAt() 是逐点几何求值（适合单点采样与测试）；
//  buildWeightMap() 是面向整幅渲染的批量版本，画笔按像素半径栅格化。
// ============================================================================

#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rawcore {

/// 蒙版图层形状
enum class MaskShape : std::uint8_t {
    Radial = 0,  ///< 椭圆径向，中心 + 半径 + 旋转
    Linear,      ///< 线性渐变，起点 → 终点
    Brush,       ///< 画笔涂抹，由笔画点序列组成
    Range,       ///< 旧版范围蒙版（兼容 B3 工程）
    Luminance,   ///< 亮度范围蒙版（append_mask.md）
    ColorRange,  ///< 颜色范围蒙版（append_mask.md，Lab 空间）
};

/// 形状的稳定英文名（工程文件存储用，不要随意改动）
const char* maskShapeName(MaskShape shape);

/// 从工程文件中的字符串还原形状；无法识别时回退到 Radial
MaskShape maskShapeFromName(const std::string& name);

/// 画笔笔画上的一个点
struct MaskStrokePoint {
    float x = 0.0f;        ///< 归一化坐标
    float y = 0.0f;        ///< 归一化坐标
    float radius = 0.05f;  ///< 归一化半径（相对图像短边）
    float flow = 1.0f;     ///< 单点覆盖强度 0~1
};

/// 范围蒙版设置（B3）。
///
/// 两种范围可单独启用或同时启用；同时启用时取交集。
/// 可以作用在径向 / 线性 / 画笔图层上作为二次筛选，
/// 也可以配合 MaskShape::Range 作为纯范围蒙版。
struct RangeMaskSettings {
    /// 亮度范围（感知域 0~1）
    bool enableLuminance = false;
    float luminanceMin = 0.0f;
    float luminanceMax = 1.0f;
    float luminanceFeather = 0.10f;

    /// 颜色范围（感知域 HSV 色相 0~360）
    bool enableColor = false;
    float colorHue = 30.0f;
    float colorTolerance = 30.0f;
    float colorSaturationMin = 0.05f;

    bool isActive() const;
    bool isIdentity() const { return !isActive(); }

    /// 计算范围权重 0~1。
    /// @param luma        感知域亮度 0~1
    /// @param hue         HSV 色相 0~360
    /// @param saturation  HSV 饱和度 0~1
    float weightAt(float luma, float hue, float saturation) const;
};

/// 亮度范围蒙版的亮度计算模式
enum class LuminanceMaskMode : std::uint8_t {
    Bt709 = 0,     ///< 线性 BT.709 感知亮度（默认）
    ProPhotoY = 1, ///< ProPhoto RGB 原色的 CIE Y 物理亮度
};

const char* luminanceMaskModeName(LuminanceMaskMode mode);
LuminanceMaskMode luminanceMaskModeFromName(const std::string& name);

/// 亮度范围蒙版（append_mask.md §一）
///
/// 在线性工作空间按明暗筛选：区间内权重 1，区间外按 softness 做 S 型衰减。
struct LuminanceMaskSettings {
    float low = 0.04f;
    float high = 0.88f;
    float softness = 0.11f;
    LuminanceMaskMode mode = LuminanceMaskMode::Bt709;

    /// 由线性亮度计算权重（不含反相 / 密度）
    float weightAtLinear(float linearLuma) const;
};

/// Lab 基准色点（颜色范围蒙版的多点拾取）
struct LabColorPoint {
    float l = 62.0f;
    float a = 18.0f;
    float b = 4.0f;
};

/// 颜色范围蒙版（append_mask.md §二）
///
/// 在 CIE Lab(D50) 空间按色相 / 彩度筛选，明度默认不参与。
struct ColorRangeMaskSettings {
    std::vector<LabColorPoint> targets;
    float hueTolerance = 22.0f;   ///< 色相容差（度）
    float satTolerance = 18.0f;   ///< 彩度容差（Lab C）
    float softness = 0.08f;       ///< 边界柔化 0~1
    bool lightnessEnabled = false;
    float lightnessLow = 0.0f;    ///< Lab L / 100
    float lightnessHigh = 1.0f;
    float softnessRange = 0.10f;  ///< 明度约束的柔化宽度

    /// 由 Lab 计算权重；多目标取最大值
    float weightAtLab(float l, float a, float b) const;
};

/// 单个蒙版的局部调整参数。
///
/// 与全局调整的区别：这些参数只作用于该蒙版权重覆盖的区域，
/// 并按权重与原像素混合。色温 / 色调是**相对全局白平衡的偏移**，
/// 因此不会破坏已经完成的白平衡基准。
struct LocalAdjustments {
    float exposure = 0.0f;     ///< 曝光补偿 EV，-5 ~ 5
    float contrast = 0.0f;     ///< 对比度 -100 ~ 100
    float highlights = 0.0f;   ///< 高光 -100 ~ 100
    float shadows = 0.0f;      ///< 阴影 -100 ~ 100
    float temperature = 0.0f;  ///< 相对全局白平衡的色温偏移（K），-3000 ~ 3000
    float tint = 0.0f;         ///< 相对全局白平衡的色调偏移，-100 ~ 100
    float saturation = 0.0f;   ///< 饱和度 -100 ~ 100

    /// 是否全部为零（管线可跳过该层的逐像素运算）
    bool isIdentity() const;
};

/// 单个蒙版图层
struct MaskLayer {
    std::string name;                    ///< 图层名（界面展示）
    MaskShape shape = MaskShape::Radial;
    bool enable = true;
    bool invert = false;                 ///< 反相：蒙版权重取 1 - w
    float opacity = 1.0f;                ///< 不透明度 0~1
    float feather = 0.10f;               ///< 羽化宽度 0~1（径向 / 画笔的边界过渡；线性渐变本身即为过渡）

    /// 该蒙版覆盖区域的局部调整参数
    LocalAdjustments adjustments;

    /// 旧版亮度 / 颜色范围筛选（兼容 B3 工程）
    RangeMaskSettings range;

    /// 亮度范围蒙版参数（shape == Luminance）
    LuminanceMaskSettings luminance;

    /// 颜色范围蒙版参数（shape == ColorRange）
    ColorRangeMaskSettings colorRange;

    // ---- 径向形状 ----
    float centerX = 0.5f;
    float centerY = 0.5f;
    float radiusX = 0.25f;
    float radiusY = 0.25f;
    float rotation = 0.0f;               ///< 旋转角度（度，顺时针）

    // ---- 线性渐变 ----
    float startX = 0.50f;
    float startY = 0.25f;
    float endX = 0.50f;
    float endY = 0.75f;

    // ---- 画笔 ----
    std::vector<MaskStrokePoint> strokes;

    /// 求指定归一化坐标处的权重（0~1，已计入反相与不透明度）
    float weightAt(float u, float v) const;

    /// 生成与图像同尺寸的权重图（行优先，长度 width*height）。
    ///
    /// 与逐点 weightAt() 相比：
    ///  - 径向 / 线性走 OpenMP 并行逐像素求值；
    ///  - 画笔把每个笔画点按**像素半径**栅格化（radius 相对图像短边），
    ///    避免每个像素遍历全部笔画点；
    ///  - 已计入 feather / invert / opacity。
    ///
    /// @param width/height 工作图尺寸
    /// @param out          输出权重图；内部会 resize 到 width*height
    void buildWeightMap(int width, int height, std::vector<float>& out) const;

    /// 只生成形状权重（不含 invert / opacity）。
    /// 供 MaskWeightCache 复用：反相 / 不透明度变化不需要重算形状。
    void buildShapeWeightMap(int width, int height, std::vector<float>& out) const;
};

/// 蒙版权重图缓存（B5）。
///
/// 缓存键由形状几何 + 图像尺寸 + 画笔笔画前缀组成：
///  - 拖动全局参数（曝光 / HSL 等）时直接复用权重图；
///  - 画笔新增笔画时只栅格化新增部分，不重算全部笔画；
///  - 反相 / 不透明度变化只重做最后一遍变换；
///  - 只缓存预览尺寸（≤ 4MP），并限制总内存，避免全尺寸导出时占用过大。
class MaskWeightCache {
public:
    /// 取得权重图。返回值在下次调用前有效，调用方应立即使用。
    const std::vector<float>& weightMap(const MaskLayer& layer,
                                        int width, int height);

    /// 取得亮度和 / 颜色范围的**原始**权重（未计反相 / 不透明度）。
    ///
    /// 按 256×256 tile 计算并缓存，平移 / ROI 预览时重叠 tile 直接命中；
    /// 只改局部调整参数不会使该缓存失效。
    const std::vector<float>& rangeWeightMap(const MaskLayer& layer,
                                             const ImageBuffer& image,
                                             int width, int height);

    void clear();
    std::size_t cachedBytes() const;

private:
    struct RangeTile {
        std::uint64_t key = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<float> weights;
        std::size_t lastUse = 0;
    };

    struct Entry {
        std::uint64_t shapeKey = 0;
        int width = 0;
        int height = 0;
        std::size_t strokeCount = 0;
        std::uint64_t strokeHash = 0;
        std::vector<float> rawWeights;
        std::size_t lastUse = 0;
    };

    std::vector<Entry> m_entries;
    std::vector<RangeTile> m_rangeTiles;
    std::vector<float> m_scratch;
    std::vector<float> m_rangeScratch;
    std::size_t m_clock = 0;
};

/// 计算亮度 / 颜色范围蒙版的原始逐像素权重（不含反相 / 不透明度）。
///
/// 供没有缓存可用时（例如全尺寸导出）直接调用。
void buildRangeWeightMap(const MaskLayer& layer,
                         const ImageBuffer& image,
                         std::vector<float>& out);

/// 蒙版栈：所有图层按添加顺序保存，求值时取并集（最大值）
struct MaskStack {
    bool enable = false;
    std::vector<MaskLayer> layers;
    int activeLayer = -1;  ///< 当前选中图层，-1 表示无

    bool isEmpty() const { return layers.empty(); }

    /// 所有启用图层的并集权重（逐层求值后取最大值）
    float weightAt(float u, float v) const;
};

}  // namespace rawcore
