// ============================================================================
//  PipelineRenderer.h —— 完整 CPU 渲染管线（需求 §3 模块 E）
//
//  职责：把 RawLoader 的解码结果 + EditContext 参数，按需求 §2 规定的
//  **固定顺序**渲染为浮点 RGB 图像。
//
//  需求 §2 管线顺序（代码严格遵循，见 PipelineRenderer.cpp 的 render()）：
//    原始RAW拜耳数据
//    → [RawLoader：libraw读取]          （由调用方预先完成，传入 RawDecodeResult）
//    → 黑电平扣除（用户可叠加偏移）      （RawLoader 内完成）
//    → 白电平饱和裁剪
//    → CMOS串扰补偿（3×3解耦矩阵，线性传感器域）
//    → 【可选分支：拜耳域CCM矩阵线性变换】
//    → 基础坏点修复
//    → FBDD拜耳域降噪（伪色抑制，Demosaic之前）
//    → Demosaic去拜耳（算法可选+摩尔纹平滑）
//    → 高光Roll-off（线性域软饱和压缩，避免通道不同步截断彩边）
//    → LCA横向色差校正（以G为基准，径向增强）
//    → LoCA轴向色散校正（高光光斑掩码）
//    → 边缘导向伪色抑制（G通道梯度掩码，只平滑R/B）
//    → lensfun镜头校正：畸变、暗角、轴向色差
//    → CamRGB → XYZ转换
//    → XYZ → ProPhotoRGB（lcms2）
//    → 白平衡白点缩放：色温+色调、灰点拾取
//    → 【预览专用分支：自动曝光归一 + 弱 S 曲线】  （默认只作用于屏幕预览）
//    → 光影调整：曝光、高光压缩、阴影提升、对比度、去雾
//    → 色彩与质感：自然饱和度、饱和度、清晰度、纹理
//    → HSL调色：七通道偏移
//    → 黑白混色：按原色相调整黑白明度
//    → 颜色分级 + 分离色调：按明暗区域着色
//    → 单通道曲线（R/G/B/L）
//    → 3D LUT（.cube，可选）
//    → 局部调整（蒙版）
//    → 非破坏性裁剪 / 拉直
//    → 降噪：明度降噪、色度降噪
//    → USM锐化，带边缘掩码
//    → 色域映射 + 目标色彩空间转换
//    → Gamma编码
//    → 预览画布 / 导出文件
//
//  优先级规则（需求 §2）：拜耳域自定义 CCM > RAW 内置原厂 cam2xyz 矩阵；
//  白平衡在 XYZ 转 ProPhoto 之后。
//
//  设计约束：
//   - 支持 preview（小图交互）与 full_size（原图导出）两种尺寸
//   - OpenMP 多线程并行（在 PipelineOps 中实现）
//   - 预留 GPU 渲染接口，MVP 仅 CPU 实现
//   - 进度回调、错误回调
// ============================================================================

#pragma once

#include "EditContext.h"
#include "RawLoader.h"
#include "Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rawcore {

// ---------------------------------------------------------------------------
//  渲染选项
// ---------------------------------------------------------------------------

/// 预览影调基准模式。
///
/// 自动曝光按亮度 99.5% 分位归一（白点），可选叠加一条低幅度 S 曲线。
/// 该影调只作用于渲染链路，不修改白平衡 / CCM 产生的线性数据。
enum class PreviewToneMode {
    Off = 0,           ///< 不施加自动影调（导出默认值）
    AutoExposure = 1,  ///< 仅自动曝光归一（线性基准预览）
    Look = 2,          ///< 自动曝光 + 弱 S 曲线（观感预览，默认）
};

struct RenderOptions {
    /// 渲染用途，决定分辨率策略
    RenderScale scale = RenderScale::Preview;

    /// 预览影调基准。默认 Off，由调用方按用途显式开启：
    ///  - 屏幕预览：Look（观感预览）或 AutoExposure（线性基准预览）
    ///  - 导出：默认 Off；用户打开「预览影调同步导出」后与预览一致
    ///
    /// 之所以不在这里默认 Look，是为了避免任何导出路径意外把预览影调
    /// 写进最终图像（需求明确要求默认不写入导出）。
    PreviewToneMode previewTone = PreviewToneMode::Off;

    /// 预览模式下的目标长边像素数。
    /// 实际使用的拜耳域下采样倍数会据此自动计算（需求 §1.3.4）。
    int previewMaxDimension = 2048;

    /// 预览局部渲染区域（源图像素坐标），宽或高 <= 0 表示渲染整幅图。
    ///
    /// 1:1 查看时整幅图有四五千万像素，而屏幕上只看得到其中一小块。
    /// 源阶段缓存（去拜耳 → 镜头校正 → 工作色彩空间）之后的所有阶段
    /// 都是逐像素或局部邻域运算，因此可以只对可见区域计算，
    /// 界面再把结果贴回已有的全分辨率显示图。
    ///
    /// 只对 RenderScale::Preview 生效：导出必须始终是完整图像。
    /// 传入的区域会在管线内部按**去噪 / 锐化邻域半径**外扩一圈并夹取到
    /// 图像范围内（实际渲染区域见 RenderResult::cropX/cropWidth）。
    int previewCropX = 0;
    int previewCropY = 0;
    int previewCropWidth = 0;
    int previewCropHeight = 0;

    bool hasPreviewCrop() const { return previewCropWidth > 0 && previewCropHeight > 0; }

    /// 是否执行镜头校正（lensfun 可用且参数启用时才真正生效）
    bool applyLensCorrection = true;

    /// 色彩转换失败时的兜底 Gamma 编码目标空间。
    ///
    /// 正常情况下不会用到：lcms2 的目标 profile 自带传递函数（TRC），
    /// 色彩空间转换本身就完成了 Gamma 编码。只有在 lcms2 转换失败
    /// （例如显示器 profile 损坏）时，才对线性数据做一次手工编码，
    /// 至少保证能显示出可用的图像。
    ColorSpace gammaSpace = ColorSpace::sRGB;

    /// 输出色彩空间。Preview 时由 ColorManagement 转到显示器空间，
    /// FullSize 时按此值转到导出目标空间。
    ColorSpace outputSpace = ColorSpace::sRGB;

    /// 线性标定视图。
    ///
    /// 打开后**关闭全部彩边 / 色散校正**（串扰补偿、FBDD、高光 Roll-off、
    /// LCA、LoCA、边缘导向伪色抑制），输出原生解拜耳线性数据，
    /// 用于 CCM 比对与色彩标定。这是需求
    /// requirements/requirement_append_falsecolor.md 的"视图联动"要求。
    ///
    /// 与 previewTone 相互独立：标定视图通常配合 AutoExposure（只归一曝光、
    /// 不加 S 曲线）使用，但本标志只负责关闭校正，不改变影调。
    bool calibrationView = false;

    /// 高色深显示（E1）。
    ///
    /// 为 true 时预览输出 16 位显示缓冲（RenderResult::display16），
    /// 画布据此上传 10-bit 纹理；配合启动时设置的 10-bit QSurfaceFormat，
    /// 可在支持的显示器 / 驱动上看到更平滑的渐变。
    bool highBitDepthDisplay = false;

    /// 色域警告（E4）。
    ///
    /// 开启后，预览会把超出 `gamutSpace` 色域的像素叠加成警告色。
    /// 目标空间通常取导出设置（sRGB / AdobeRGB / Display P3 / ProPhoto）。
    bool gamutWarning = false;
    float gamutWarningR = 1.0f;
    float gamutWarningG = 0.0f;
    float gamutWarningB = 1.0f;
    ColorSpace gamutSpace = ColorSpace::sRGB;

    /// 软打样（打印模拟）。
    ///
    /// 只用于屏幕预览：渲染的最后一步会经证明变换转到打样设备的色域，
    /// 从而在屏幕上看到打印后的颜色与纸白。导出路径完全忽略本项，
    /// 保证输出文件仍然是干净的工作空间 → 目标空间的转换结果。
    SoftProofSettings softProof;
};

// ---------------------------------------------------------------------------
//  渲染结果
// ---------------------------------------------------------------------------

struct RenderResult {
    /// 渲染后的图像。
    /// 注意其色彩空间取决于 RenderOptions：
    ///  - Preview: 显示器空间（已 gamma 编码，可直接显示）
    ///  - FullSize: outputSpace（已 gamma 编码，可写出文件）
    ImageBuffer image;

    /// 供直方图使用的**线性域**采样数据（需求 §3 模块 F3）。
    ///
    /// 为什么单独保存：管线最终输出的是 gamma 编码后的数据，
    /// 而需求明确要求直方图基于线性域统计。
    /// 这里在色域映射与 Gamma 编码**之前**抽取一份降采样副本，
    /// 既保证直方图反映真实的线性光分布，又不会带来明显的内存与耗时开销。
    ///
    /// 采样规则：长边不超过 kHistogramSampleSize 像素；原图本身更小时按原尺寸。
    ImageBuffer linearSample;

    /// 预览专用的 8 位显示数据（RGB888，显示器色彩空间，已完成 gamma 编码）。
    ///
    /// 屏幕只有 8 位，再保留一份 4600 万像素的浮点显示图纯属浪费：
    /// 它要多占约 550MB 内存，界面层还得逐像素量化一遍。
    /// 预览结果只填这个缓冲（image 为空）；全尺寸结果仍走 image。
    std::vector<std::uint8_t> display8;

    /// 高色深预览缓冲（E1）：RGB 交错 16 位，显示器色彩空间。
    /// 与 display8 二选一，由 RenderOptions::highBitDepthDisplay 决定。
    std::vector<std::uint16_t> display16;

    /// 本次渲染实际使用的渲染尺寸（用于 UI 提示当前是预览还是全尺寸）。
    /// 局部渲染时这是**整幅**图的尺寸，不是 display8 的尺寸——
    /// 界面解释缩放比例、贴回局部区域都以它和 cropX/cropY 为准。
    int renderWidth = 0;
    int renderHeight = 0;

    /// 本次预览是否为整幅图的局部（1:1 只渲染可见区域）。
    ///
    /// 为 true 时 display8 只是 cropWidth × cropHeight 的一块，
    /// 界面必须把它贴回已有的全分辨率显示图（cropX/cropY 为左上角），
    /// 而不能当作整幅图替换显示缓冲。
    bool wasCropped = false;
    int cropX = 0;
    int cropY = 0;
    int cropWidth = 0;
    int cropHeight = 0;

    /// 未降采样时的原始尺寸，即"1:1 显示"的基准。
    ///
    /// 预览会按屏幕显示尺寸做拜耳域降采样，此时 renderWidth/renderHeight
    /// 小于原图。界面必须用 fullWidth/fullHeight 解释缩放比例，否则会把
    /// 降采样图当成原始像素，放大查看时误判清晰度。
    int fullWidth = 0;
    int fullHeight = 0;

    /// 是否为降分辨率预览
    bool wasDownsampled = false;

    /// 渲染期间产生的警告（矩阵校验、镜头数据缺失、LUT 未实现等）
    std::vector<std::string> warnings;

    bool isValid() const
    {
        return !image.isEmpty() || !display8.empty() || !display16.empty();
    }

    /// 直方图采样缓冲的长边上限。
    /// 512 已足够让 256 分箱的直方图稳定，再大只是浪费内存。
    static constexpr int kHistogramSampleSize = 512;
};

// ---------------------------------------------------------------------------
//  回调
// ---------------------------------------------------------------------------

/// 进度回调。stage 为当前阶段，progress 为 0.0~1.0 总进度。
using ProgressCallback = std::function<void(RenderStage stage, float progress)>;

/// 错误/警告回调。用于把非致命信息送到 UI 日志面板。
using MessageCallback = std::function<void(const std::string& message, bool isError)>;

// ---------------------------------------------------------------------------
//  渲染后端接口（需求 §3 模块 E：预留 GPU 渲染接口）
//
//  MVP 只实现 CPU 后端。未来接入 GPU 时实现本接口并在 RendererFactory
//  中注册即可，UI 与参数模型无需任何改动。
// ---------------------------------------------------------------------------

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    /// 后端名称，用于日志与调试
    virtual const char* name() const = 0;

    /// 该后端是否可在当前硬件上使用
    virtual bool isAvailable() const = 0;

    /// 执行渲染管线
    virtual ErrorInfo render(const RawDecodeResult& raw,
                             const EditContext& context,
                             const RenderOptions& options,
                             RenderResult& result,
                             ProgressCallback progress,
                             MessageCallback message) = 0;
};

// ---------------------------------------------------------------------------
//  PipelineRenderer —— CPU 渲染管线实现
// ---------------------------------------------------------------------------

class PipelineRenderer {
public:
    PipelineRenderer();
    ~PipelineRenderer();

    PipelineRenderer(const PipelineRenderer&) = delete;
    PipelineRenderer& operator=(const PipelineRenderer&) = delete;

    // -----------------------------------------------------------------------
    //  渲染入口
    // -----------------------------------------------------------------------

    /// 执行完整渲染管线。
    /// @param raw       RawLoader 的解码结果
    /// @param context   编辑参数
    /// @param options   渲染选项（尺寸、输出空间等）
    /// @param result    输出图像
    /// @param progress  进度回调（可为空）
    /// @param message   消息回调（可为空）
    /// @return 错误信息；成功时 code == ErrorCode::Ok
    ErrorInfo render(const RawDecodeResult& raw,
                     const EditContext& context,
                     const RenderOptions& options,
                     RenderResult& result,
                     ProgressCallback progress = nullptr,
                     MessageCallback message = nullptr);

    /// 请求取消当前渲染。渲染会在下一个阶段边界处返回 ErrorCode::Cancelled。
    void requestCancel();

    /// 是否已被请求取消
    bool isCancelled() const;

    /// 清空"源阶段"缓存（去拜耳 → 镜头校正 → 工作色彩空间 的中间结果）。
    /// 切换 RAW 文件时必须调用，避免旧图的中间结果被误用。
    void invalidateBaseCache();

    /// 源阶段缓存当前占用的字节数 / 层级数（用于性能日志与测试）
    std::size_t baseCacheBytes() const;
    std::size_t baseCacheEntryCount() const;

    // -----------------------------------------------------------------------
    //  预览降采样辅助
    // -----------------------------------------------------------------------

    /// 根据目标长边计算合适的预览下采样倍数。
    /// 需求 §1.3.4：拖动滑块时渲染低分辨率预览。
    ///
    /// 倍数一定是偶数（2x2 CFA 合并的相位要求），且**向下取偶**：
    /// 渲染结果不小于屏幕需要的像素，避免画布放大预览导致画面发虚。
    /// @return 下采样倍数，1 表示走全分辨率去拜耳
    static int computeDownsampleFactor(int sourceWidth, int sourceHeight,
                                       int previewMaxDimension);

    // -----------------------------------------------------------------------
    //  后端管理（预留 GPU）
    // -----------------------------------------------------------------------

    /// 注册一个渲染后端。MVP 只注册 CPU 后端。
    void registerBackend(std::unique_ptr<IRenderBackend> backend);

    /// 列出所有可用后端名，供 UI 展示
    std::vector<std::string> availableBackends() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// 创建 CPU 渲染后端（需求 §3 模块 E：MVP 仅 CPU 实现）
std::unique_ptr<IRenderBackend> createCpuRenderBackend();

}  // namespace rawcore
