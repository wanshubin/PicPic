// ============================================================================
//  ColorManagement.h —— 色彩管理引擎（需求 §3 模块 C）
//
//  职责（封装 lcms2）：
//   - 内置 ICC：ProPhotoRGB、sRGB、AdobeRGB
//   - 转换接口：XYZ ↔ ProPhotoRGB；工作空间 ↔ 显示器空间；工作空间 ↔ 导出目标
//   - 读取系统显示器 ICC，预览适配屏幕色域
//   - 渲染意图：相对比色优先
//
//  设计决策：**内置 profile 全部由代码生成**，不依赖外部 .icc 文件。
//  理由：便于单文件打包，且避免许可与文件缺失问题。生成所用的白点与
//  基色坐标均为公开标准值（见 cpp 中的常量表）。
//
//  本文件不暴露 lcms2 的任何类型，第三方库完全隔离在 cpp 内部。
// ============================================================================

#pragma once

#include "Types.h"

#include <memory>
#include <string>
#include <vector>

namespace rawcore {

/// 渲染意图（需求 §3 模块 C：相对比色优先）
enum class RenderIntent : std::uint8_t {
    Perceptual = 0,
    RelativeColorimetric,  ///< 默认
    Saturation,
    AbsoluteColorimetric,
};

/// 不透明的 profile 句柄。外部代码只能传递，不能访问内部结构。
class ColorProfile;

/// 软打样参数。
///
/// 软打样把“打印后在纸上呈现的效果”模拟到屏幕上：先在 proofingProfile
/// 的描述空间内做色域压缩，再转换到显示器空间。它只影响预览显示，
/// 不会改变导出的像素数据。
struct SoftProofOptions {
    /// 打样目标 profile（通常为打印机 / 纸张的 ICC）。为空表示关闭软打样。
    const ColorProfile* proofingProfile = nullptr;

    /// 是否模拟纸白。
    ///
    /// lcms2 没有独立的“纸白”开关，业界通行做法是把打样意图设成
    /// 绝对比色：白点直接被映射到纸张白，纸白的偏色与亮度得以保留。
    /// 关闭时使用相对比色，纸张白会被适配回显示器白。
    bool simulatePaperWhite = false;
};

/// 色彩管理引擎。
///
/// 线程安全说明：lcms2 的 transform **不是**线程安全的，且创建开销较大。
/// 因此本类按"每线程持有自己的实例"使用：渲染工作线程各自创建
/// ColorManagement 实例，不要在多个线程间共享同一个对象。
class ColorManagement {
public:
    ColorManagement();
    ~ColorManagement();

    ColorManagement(const ColorManagement&) = delete;
    ColorManagement& operator=(const ColorManagement&) = delete;

    // -----------------------------------------------------------------------
    //  Profile 获取
    // -----------------------------------------------------------------------

    /// 获取内置色彩空间的 profile（惰性创建并缓存）
    const ColorProfile* profileFor(ColorSpace space);

    /// 获取系统显示器 ICC profile。
    /// 读取失败时回退到 sRGB，并把原因写入 lastError()。
    const ColorProfile* displayProfile();

    /// 从文件加载任意 ICC profile（用于导出时嵌入自定义 profile）
    const ColorProfile* loadProfileFromFile(const std::string& filePath);

    // -----------------------------------------------------------------------
    //  像素转换
    //
    //  所有转换都以 float32 交错 RGB 缓冲为输入输出。
    //  注意：lcms2 的 float 格式化器要求数据在 0.0~1.0 范围之外也能处理，
    //  但超出范围的部分会被裁剪，因此调用方应确保输入在合理范围内。
    // -----------------------------------------------------------------------

    /// 把 source 缓冲从 fromSpace 转换到 toSpace，结果写入 dest。
    /// dest 会自动按 source 尺寸分配。失败时 dest 内容未定义。
    ErrorInfo convert(const ImageBuffer& source,
                      ColorSpace fromSpace,
                      ColorSpace toSpace,
                      ImageBuffer& dest,
                      RenderIntent intent = RenderIntent::RelativeColorimetric);

    /// 工作空间（ProPhotoRGB）→ 显示器空间，用于预览显示。
    /// 内部自动处理目标为 sRGB 编码（显示器需要 gamma 编码数据）。
    ErrorInfo convertWorkingToDisplay(const ImageBuffer& source, ImageBuffer& dest);

    /// 工作空间 → 显示器空间，直接产出 8 位 RGB888 显示数据（预览专用）。
    ///
    /// 与 convertWorkingToDisplay 的区别：屏幕只看得到 8 位，没必要先生成
    /// 一份 4600 万像素 ×3 通道的浮点显示图，再由界面层逐像素量化。
    /// 这里在 lcms2 的 16 位整数流水线输出后直接量化成 8 位，省掉一次
    /// 约 550MB 的浮点写入和一次全幅读取。
    ///
    /// @param proof 软打样参数。proofingProfile 为空时与普通预览完全一致。
    /// @param originX 本块数据在整幅图中的左上角坐标（像素）。
    /// @param originY 局部渲染（ROI）时传入裁剪偏移，抖动噪声才能与整幅
    ///                渲染保持一致，平移画面时不会出现闪烁或接缝。
    ///
    /// dest 会被调整为 width*height*3 字节。
    ErrorInfo convertWorkingToDisplay8(const ImageBuffer& source,
                                       std::vector<std::uint8_t>& dest,
                                       const SoftProofOptions& proof = SoftProofOptions{},
                                       int originX = 0,
                                       int originY = 0);

    /// 工作空间 → 显示器空间，输出 16 位 RGB 缓冲（E1）。
    /// 用于 10-bit 高色深预览；不做抖动，保留全部 16 位精度。
    ErrorInfo convertWorkingToDisplay16(const ImageBuffer& source,
                                        std::vector<std::uint16_t>& dest,
                                        const SoftProofOptions& proof = SoftProofOptions{},
                                        int originX = 0,
                                        int originY = 0);

    /// 色域警告（E4）：生成“超出 targetSpace 色域”的逐像素掩码。
    ///
    /// 必须在色域软压缩**之前**调用，否则越界值已经被压回 [0,1]，
    /// 警告会漏报。掩码每像素 1 字节，1 表示越界。
    ErrorInfo buildGamutMask(const ImageBuffer& source,
                             ColorSpace targetSpace,
                             std::vector<std::uint8_t>& mask);

    /// 把色域掩码叠加到 8 / 16 位显示缓冲（只影响预览，不影响导出）。
    void overlayGamutMask8(std::vector<std::uint8_t>& display,
                           const std::vector<std::uint8_t>& mask,
                           float warnR, float warnG, float warnB,
                           float blend = 0.85f) const;
    void overlayGamutMask16(std::vector<std::uint16_t>& display,
                            const std::vector<std::uint8_t>& mask,
                            float warnR, float warnG, float warnB,
                            float blend = 0.85f) const;

    /// 工作空间（ProPhotoRGB）→ 指定的导出色彩空间（sRGB / AdobeRGB）。
    ErrorInfo convertWorkingToOutput(const ImageBuffer& source,
                                     ColorSpace targetSpace,
                                     ImageBuffer& dest,
                                     RenderIntent intent = RenderIntent::RelativeColorimetric);

    /// 把 XYZ(D50) 数据转换为 ProPhotoRGB 线性（管线中的 XYZ → 工作空间步骤）。
    ErrorInfo convertXyzToWorking(const ImageBuffer& source, ImageBuffer& dest);

    /// 把相机 RGB 数据转换到 XYZ(D50)。
    /// 注意：管线中该步骤优先用自带的 3x3 矩阵乘法（更快、可控），
    /// 本方法用于需要 lcms2 精确处理的场景与校验对照。
    ErrorInfo convertCameraToXyz(const ImageBuffer& source,
                                 const ColorProfile* cameraProfile,
                                 ImageBuffer& dest);

    // -----------------------------------------------------------------------
    //  ICC 数据导出（用于把 profile 嵌入导出文件，需求 §3 模块 G）
    // -----------------------------------------------------------------------

    /// 把内置 profile 序列化为 ICC 字节流，供 TIFF/JPEG/WebP 嵌入
    std::vector<std::uint8_t> profileBytesFor(ColorSpace space);

    /// 把任意 profile 序列化为字节流
    std::vector<std::uint8_t> profileBytes(const ColorProfile* profile);

    // -----------------------------------------------------------------------
    //  诊断
    // -----------------------------------------------------------------------

    /// 最近一次失败的原因（中文描述）
    const std::string& lastError() const { return m_lastError; }

    /// lcms2 版本字符串
    static std::string lcmsVersion();

private:
    /// 在给定 profile 之间创建（并缓存）transform。
    /// 返回 nullptr 有两种含义：创建失败（详见 lastError）或"源与目标相同无需转换"。
    ///
    /// proof 非空时创建带软打样的 proofing transform；simulatePaperWhite
    /// 会把打样意图切到绝对比色，以保留纸张白。
    void* acquireTransform(const ColorProfile* src,
                           const ColorProfile* dst,
                           RenderIntent intent,
                           std::uint32_t format,
                           const ColorProfile* proof = nullptr,
                           bool simulatePaperWhite = false);

    /// 核心转换实现：接受已解析的 profile 句柄，供上述各公开接口复用。
    ///
    /// prefer16Bit = true 时走 lcms2 的 16 位整数流水线（精度仍远高于 8 位显示，
    /// 但吞吐接近浮点路径的十倍），仅用于屏幕预览；导出仍走浮点路径保住精度。
    ErrorInfo convertInternal(const ImageBuffer& source,
                              const ColorProfile* srcProfile,
                              const ColorProfile* dstProfile,
                              ImageBuffer& dest,
                              RenderIntent intent,
                              bool prefer16Bit = false);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_lastError;
};

}  // namespace rawcore
