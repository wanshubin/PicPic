// ============================================================================
//  PipelineRenderer.cpp —— 完整 CPU 渲染管线实现
//
//  本文件是需求 §2 管线顺序的**唯一权威实现**。
//  render() 方法中的步骤顺序与注释编号严格对应文档中的管线图，
//  修改顺序前请先核对需求文档。
// ============================================================================

#include "PipelineRenderer.h"

#include "ColorCalibration.h"
#include "ColorManagement.h"
#include "DcpApply.h"
#include "DcpProfile.h"
#include "FrameCalibration.h"
#include "LensCorrection.h"
#include "LinearMath.h"
#include "PipelineOps.h"
#include "PipelineStages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace rawcore {

// ===========================================================================
//  内部工具
// ===========================================================================

namespace {

/// 把阶段编号转换为 0~1 的进度值
float stageProgress(RenderStage stage)
{
    const int index = static_cast<int>(stage);
    if (index <= 0) {
        return 0.0f;
    }
    if (index >= kRenderStageCount) {
        return 1.0f;
    }
    return static_cast<float>(index) / static_cast<float>(kRenderStageCount);
}

/// 进度上报辅助。封装判空与阶段换算，让管线代码保持整洁。
class ProgressReporter {
public:
    explicit ProgressReporter(ProgressCallback callback) : m_callback(std::move(callback)) {}

    void report(RenderStage stage) const
    {
        if (m_callback) {
            m_callback(stage, stageProgress(stage));
        }
    }

private:
    ProgressCallback m_callback;
};

/// 消息上报辅助
class MessageReporter {
public:
    explicit MessageReporter(MessageCallback callback, std::vector<std::string>* warnings)
        : m_callback(std::move(callback)), m_warnings(warnings) {}

    void warn(const std::string& text) const
    {
        if (m_warnings != nullptr) {
            m_warnings->push_back(text);
        }
        if (m_callback) {
            m_callback(text, false);
        }
    }

    void error(const std::string& text) const
    {
        if (m_callback) {
            m_callback(text, true);
        }
    }

private:
    MessageCallback m_callback;
    std::vector<std::string>* m_warnings = nullptr;
};

// ---------------------------------------------------------------------------
//  拜耳域 3x3 全矩阵变换已移到 PipelineOps::applyBayerLinearMatrix：
//  串扰补偿与自定义 CCM 需要完全相同的"临时插值 → 矩阵 → 抽回"语义，
//  放在同一处可以避免两套实现产生细微差异。
// ---------------------------------------------------------------------------

/// 源阶段缓存：保存"去拜耳 → 镜头校正 → 工作色彩空间"之后、白平衡之前的
/// 线性浮点图像。
///
/// 拖动滑块时绝大多数参数（白平衡、光影、HSL、曲线、降噪、锐化）都不影响
/// 这段结果，因此可以复用，避免每次都重跑去拜耳等昂贵步骤。
/// 缓存键包含所有会影响这段结果的参数，键不匹配时自动失效。
struct BaseImageCacheEntry {
    std::uint64_t key = 0;
    ImageBuffer image;
    /// 2x2 合并时的饱和标记（与 image 同尺寸）。预览合并路径才有；
    /// 全分辨率去马赛克路径为空。白平衡之后据此还原部分饱和高光。
    std::vector<std::uint8_t> clipMask;
    std::size_t bytes = 0;
    std::size_t lastUse = 0;
};

/// 多级源阶段缓存（G2/G3）。
///
/// 不同降采样倍数使用不同的缓存键：fit 预览、1:1 与导出可以各自命中，
/// 来回切换缩放不再反复重跑去马赛克。总内存有上限，超出时按 LRU 淘汰；
/// 单张全尺寸图超过预算时直接不缓存，避免 100MP 机型把内存吃满。
struct BaseImageCache {
    std::vector<BaseImageCacheEntry> entries;
    std::size_t clock = 0;

    /// 768MB：45MP 全尺寸浮点图约 550MB，仍可同时保留一个预览层级。
    static constexpr std::size_t kBudgetBytes = 768ull * 1024ull * 1024ull;

    BaseImageCacheEntry* find(std::uint64_t key)
    {
        for (BaseImageCacheEntry& entry : entries) {
            if (entry.key == key) {
                entry.lastUse = ++clock;
                return &entry;
            }
        }
        return nullptr;
    }

    void store(std::uint64_t key, const ImageBuffer& image,
               const std::vector<std::uint8_t>& clipMask)
    {
        const std::size_t bytes =
            image.sampleCount() * sizeof(float) + clipMask.size();
        if (bytes == 0 || bytes > kBudgetBytes) {
            return;
        }

        for (BaseImageCacheEntry& entry : entries) {
            if (entry.key == key) {
                entry.image = image;
                entry.clipMask = clipMask;
                entry.bytes = bytes;
                entry.lastUse = ++clock;
                evict();
                return;
            }
        }

        BaseImageCacheEntry entry;
        entry.key = key;
        entry.image = image;
        entry.clipMask = clipMask;
        entry.bytes = bytes;
        entry.lastUse = ++clock;
        entries.push_back(std::move(entry));
        evict();
    }

    void clear()
    {
        entries.clear();
        clock = 0;
    }

    std::size_t totalBytes() const
    {
        std::size_t bytes = 0;
        for (const BaseImageCacheEntry& entry : entries) {
            bytes += entry.bytes;
        }
        return bytes;
    }

private:
    void evict()
    {
        while (!entries.empty() && totalBytes() > kBudgetBytes) {
            const auto oldest = std::min_element(
                entries.begin(), entries.end(),
                [](const BaseImageCacheEntry& a, const BaseImageCacheEntry& b) {
                    return a.lastUse < b.lastUse;
                });
            entries.erase(oldest);
        }
    }
};

/// 计算源阶段缓存键。只混入会影响"白平衡之前"结果的参数，
/// 这样改动光影/HSL 等参数时仍能命中缓存。
std::uint64_t computeBaseKey(const RawDecodeResult& raw,
                             const EditContext& context,
                             const RenderOptions& options,
                             int downsampleFactor);

/// 管线主体实现（自由函数）。
///
/// 提取为自由函数的原因：PipelineRenderer::render 与 CpuRenderBackend::render
/// 需要走同一份管线代码，避免逻辑重复。二者只是接口层不同。
///
/// @param cancelFlag 取消标志；由调用方持有，管线在阶段边界检查它
/// @param lensCorrector 镜头校正器（长期资源，复用 lensfun 数据库）
/// @param colorManagement 色彩管理引擎（长期资源，复用 lcms2 transform 缓存）
/// @param baseCache 源阶段缓存；可为 nullptr（表示本次渲染不复用缓存）
/// @param maskCache 蒙版权重图缓存；可为 nullptr（表示每次重新构建）
ErrorInfo runPipeline(const RawDecodeResult& raw,
                      const EditContext& context,
                      const RenderOptions& options,
                      RenderResult& result,
                      const std::atomic<bool>& cancelFlag,
                      LensCorrector* lensCorrector,
                      ColorManagement* colorManagement,
                      BaseImageCache* baseCache,
                      MaskWeightCache* maskCache,
                      ProgressCallback progress,
                      MessageCallback message);

}  // namespace

// ===========================================================================
//  CPU 渲染后端
// ===========================================================================

namespace {

/// CPU 渲染后端：把管线实现包装成 IRenderBackend 接口。
/// 需求 §3 模块 E 要求预留 GPU 接口，本类是 CPU 侧的默认实现。
class CpuRenderBackend : public IRenderBackend {
public:
    const char* name() const override { return "CPU"; }
    bool isAvailable() const override { return true; }

    ErrorInfo render(const RawDecodeResult& raw,
                     const EditContext& context,
                     const RenderOptions& options,
                     RenderResult& result,
                     ProgressCallback progress,
                     MessageCallback message) override
    {
        // 后端自己持有取消标志与资源。通过 IRenderBackend 接口调用时
        // 无法访问 PipelineRenderer 的状态，因此这里用局部实例。
        // 代价是缺少跨次渲染的资源复用（lensfun 数据库需重新加载），
        // 这是直接使用本接口时的已知代价；正常路径请走 PipelineRenderer。
        static const std::atomic<bool> neverCancel{false};
        LensCorrector lensCorrector;
        ColorManagement colorManagement;
        // 后端接口没有跨次渲染的状态，用 thread_local 缓存保证同线程内可复用
        thread_local MaskWeightCache maskCache;
        return runPipeline(raw, context, options, result, neverCancel,
                           &lensCorrector, &colorManagement, nullptr, &maskCache,
                           std::move(progress), std::move(message));
    }
};

}  // namespace

// ===========================================================================
//  PipelineRenderer::Impl
// ===========================================================================

struct PipelineRenderer::Impl {
    std::atomic<bool> cancelRequested{false};
    std::vector<std::unique_ptr<IRenderBackend>> backends;

    /// 源阶段缓存（白平衡之前的线性工作图像）。
    /// 同一渲染器实例只在单个工作线程中使用，因此无需加锁。
    BaseImageCache baseCache;

    /// 蒙版权重图缓存（B5）：预览拖动时复用形状权重，
    /// 画笔新增笔画只栅格化新增部分。
    MaskWeightCache maskWeightCache;

    /// 镜头校正器按渲染器复用。lensfun 数据库加载较慢，
    /// 复用可避免每次预览都重新加载（需求 §5.1 要求 UI 流畅）。
    std::unique_ptr<LensCorrector> lensCorrector;

    /// 色彩管理引擎。注意 lcms2 的 transform 不线程安全，
    /// 因此每个 PipelineRenderer 实例（对应一个渲染线程）各自持有。
    std::unique_ptr<ColorManagement> colorManagement;
};

PipelineRenderer::PipelineRenderer()
    : m_impl(std::make_unique<Impl>())
{
    m_impl->lensCorrector = std::make_unique<LensCorrector>();
    m_impl->colorManagement = std::make_unique<ColorManagement>();
}

PipelineRenderer::~PipelineRenderer() = default;

void PipelineRenderer::requestCancel()
{
    m_impl->cancelRequested.store(true, std::memory_order_relaxed);
}

bool PipelineRenderer::isCancelled() const
{
    return m_impl->cancelRequested.load(std::memory_order_relaxed);
}

void PipelineRenderer::invalidateBaseCache()
{
    m_impl->baseCache.clear();
    m_impl->maskWeightCache.clear();
}

std::size_t PipelineRenderer::baseCacheBytes() const
{
    return m_impl->baseCache.totalBytes();
}

std::size_t PipelineRenderer::baseCacheEntryCount() const
{
    return m_impl->baseCache.entries.size();
}

void PipelineRenderer::registerBackend(std::unique_ptr<IRenderBackend> backend)
{
    if (backend) {
        m_impl->backends.push_back(std::move(backend));
    }
}

std::vector<std::string> PipelineRenderer::availableBackends() const
{
    std::vector<std::string> names;
    names.reserve(m_impl->backends.size());
    for (const auto& backend : m_impl->backends) {
        if (backend && backend->isAvailable()) {
            names.emplace_back(backend->name());
        }
    }
    return names;
}

int PipelineRenderer::computeDownsampleFactor(int sourceWidth, int sourceHeight,
                                              int previewMaxDimension)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || previewMaxDimension <= 0) {
        return 1;
    }

    const int longestSide = std::max(sourceWidth, sourceHeight);
    if (longestSide <= previewMaxDimension) {
        return 1;  // 原图已足够小，无需降采样
    }

    // 计算需要的最小倍数
    const float factor = static_cast<float>(longestSide) / static_cast<float>(previewMaxDimension);

    // 预览先做 2x2 合并（固定降到 1/2），再在 RGB 域按 factor/2 降采样。
    // 倍数取偶数，保证合并与降采样严格对齐，不会出现半像素错位。
    //
    // 取整方向必须**向下取偶**：渲染结果只要比屏幕需要的像素少一点，
    // 画布就得把它放大显示，画面立刻发虚（1:1 附近 1~1.5 倍的放大尤其明显）。
    // 允许 6% 的轻微放大，是为了避免因子刚好卡在 2→4、4→6 之间时
    // 白白多渲 4 倍像素；6% 的双线性放大肉眼不可辨，而向上取偶最多会放大 50%。
    int integerFactor = 1;
    const int evenLimit = static_cast<int>(std::floor(factor * 1.06f));
    if (evenLimit >= 2) {
        integerFactor = evenLimit - (evenLimit % 2);
    }

    // 极端缩小（例如把竖图缩到很矮的窗口）时不要过度降采样：
    // 长边低于 256 像素后去拜耳与降噪缺乏足够邻域，画质会明显劣化，
    // 而这点尺寸的渲染本来就只要几毫秒，没必要省。
    while (integerFactor > 2 && longestSide / integerFactor < 256) {
        integerFactor -= 2;
    }

    // 1 表示走全分辨率去拜耳（不合并）；否则一定是 >= 2 的偶数
    return integerFactor;
}

// ===========================================================================
//  渲染主流程 —— 严格遵循需求 §2 的管线顺序
// ===========================================================================

ErrorInfo PipelineRenderer::render(const RawDecodeResult& raw,
                                   const EditContext& context,
                                   const RenderOptions& options,
                                   RenderResult& result,
                                   ProgressCallback progress,
                                   MessageCallback message)
{
    m_impl->cancelRequested.store(false, std::memory_order_relaxed);

    // 转发到共享的管线实现。m_impl->lensCorrector 与 colorManagement 是
    // 渲染器的长期资源（lensfun 数据库、lcms2 transform 缓存），
    // 因此通过 runPipeline 的资源参数传入而非每次重建。
    return runPipeline(raw, context, options, result, m_impl->cancelRequested,
                       m_impl->lensCorrector.get(), m_impl->colorManagement.get(),
                       &m_impl->baseCache, &m_impl->maskWeightCache,
                       std::move(progress), std::move(message));
}

namespace {

std::uint64_t computeBaseKey(const RawDecodeResult& raw,
                             const EditContext& context,
                             const RenderOptions& options,
                             int downsampleFactor)
{
    // FNV-1a：混合开销极小，且对浮点位模式完全敏感
    std::uint64_t hash = 1469598103934665603ull;
    const auto mixBytes = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    const auto mixInt = [&mixBytes](std::int64_t value) { mixBytes(&value, sizeof(value)); };
    const auto mixFloat = [&mixBytes](float value) { mixBytes(&value, sizeof(value)); };

    // 数据来源与尺寸（切换 RAW 时 RenderController 还会显式清空缓存）
    mixInt(reinterpret_cast<std::intptr_t>(raw.bayer.data.data()));
    mixInt(raw.bayer.width);
    mixInt(raw.bayer.height);
    mixInt(static_cast<int>(raw.bayer.pattern));
    // 普通位图输入：源数据换成线性 RGB，缓存键必须跟着换，
    // 否则切换 JPG 后可能误命中上一张的源阶段结果。
    mixInt(raw.isRgbSource ? 1 : 0);
    mixInt(reinterpret_cast<std::intptr_t>(raw.rgbLinear.data()));
    mixInt(raw.rgbLinear.width());
    mixInt(raw.rgbLinear.height());
    // 排列猜测会改变拜耳域校正是否执行，必须参与缓存键
    mixInt(raw.cfaPatternGuessed ? 1 : 0);
    mixFloat(raw.whiteLevel);
    mixInt(downsampleFactor);

    // 影响去拜耳之前/去拜耳本身的参数
    mixInt(context.pipelineOrder.enable ? 1 : 0);
    for (const std::string& stage : context.pipelineOrder.stages) {
        mixBytes(stage.data(), stage.size());
    }
    mixFloat(context.raw.whiteLevelOverride);
    mixInt(context.raw.enableBadPixelRepair ? 1 : 0);
    mixInt(static_cast<int>(context.raw.demosaicAlgorithm));
    mixFloat(context.raw.demosaicSmooth);
    mixInt(context.raw.disableCameraMatrix ? 1 : 0);

    // FBDD（拜耳域，位于去拜耳之前）
    mixInt(context.raw.enableFbdd ? 1 : 0);
    mixFloat(context.raw.fbddStrength);

    // 暗电流补偿（拜耳域，位于串扰之前）
    mixInt(context.darkCurrent.enable ? 1 : 0);
    mixFloat(context.darkCurrent.strength);
    mixFloat(raw.metadata.iso);
    for (const auto& entry : context.darkCurrent.isoOffsets) {
        mixFloat(entry.first);
        mixFloat(entry.second);
    }

    // 暗场帧 / 平场帧（拜耳域，暗电流之后、串扰之前）
    // 帧文件路径参与键，切换帧或改强度时源阶段缓存必须失效。
    mixInt(context.frameCalibration.enableDarkFrame ? 1 : 0);
    mixInt(context.frameCalibration.enableFlatFrame ? 1 : 0);
    mixFloat(context.frameCalibration.darkFrameStrength);
    mixFloat(context.frameCalibration.flatFrameStrength);
    mixBytes(context.frameCalibration.darkFramePath.data(),
             context.frameCalibration.darkFramePath.size());
    mixBytes(context.frameCalibration.flatFramePath.data(),
             context.frameCalibration.flatFramePath.size());

    // 彩边 / 色散校正：全部位于白平衡之前，必须参与源阶段缓存键，
    // 否则调整这些参数时界面会一直显示旧结果。
    // 标定视图会把它们全部关闭，因此也把该标志混入键中。
    mixInt(options.calibrationView ? 1 : 0);
    {
        const FalseColorSettings& fc = context.falseColor;
        mixInt(fc.crosstalk.enable ? 1 : 0);
        mixFloat(fc.crosstalk.strength);
        for (const auto& row : fc.crosstalk.decouplingMatrix) {
            for (float value : row) {
                mixFloat(value);
            }
        }
        mixInt(fc.highlightRolloff.enable ? 1 : 0);
        mixFloat(fc.highlightRolloff.knee);
        mixFloat(fc.highlightRolloff.slope);
        mixInt(fc.lca.enable ? 1 : 0);
        mixFloat(fc.lca.redScale);
        mixFloat(fc.lca.blueScale);
        mixFloat(fc.lca.radialStrength);
        mixInt(fc.loca.enable ? 1 : 0);
        mixFloat(fc.loca.strength);
        mixFloat(fc.loca.highlightThreshold);
        mixInt(fc.edgeFalseColor.enable ? 1 : 0);
        mixFloat(fc.edgeFalseColor.strength);
        mixFloat(fc.edgeFalseColor.edgeSensitivity);
        mixInt(fc.edgeFalseColor.smoothingRadius);
    }

    mixInt(context.ccm.enable ? 1 : 0);
    // 双光源模式下实际生效的是按 as-shot 色温插值后的矩阵。
    // 直接混入解析结果，既覆盖了两端矩阵与标定温度，也覆盖了
    // 不同 RAW 之间 as-shot 色温的差异。
    for (const auto& row : context.ccm.resolvedCam2xyz(raw.metadata.asShotTemperature)) {
        for (float value : row) {
            mixFloat(value);
        }
    }

    // DCP 的相机矩阵属于源阶段（D1）。路径、开关与 as-shot 色温都会改变
    // 相机 → XYZ 的结果，必须参与缓存键；文件内容变化由加载缓存按修改时间
    // 感知，这里用路径即可。
    mixInt(context.dcp.enable ? 1 : 0);
    mixInt(context.dcp.useForwardMatrix ? 1 : 0);
    mixBytes(context.dcp.profilePath.data(), context.dcp.profilePath.size());
    mixFloat(raw.metadata.asShotTemperature);
    mixInt(raw.hasCameraWhiteBalanceGain ? 1 : 0);
    for (float value : raw.cameraWhiteBalanceGain) {
        mixFloat(value);
    }

    // 镜头校正位于白平衡之前，同样属于源阶段
    mixInt(options.applyLensCorrection ? 1 : 0);
    mixInt(context.lens.enable ? 1 : 0);
    mixInt(context.lens.enableDistortion ? 1 : 0);
    mixInt(context.lens.enableVignetting ? 1 : 0);
    mixInt(context.lens.enableChromaticAberration ? 1 : 0);
    mixFloat(context.lens.distortionScale);
    mixFloat(context.lens.vignettingScale);
    mixFloat(context.lens.chromaticAberrationScale);
    // D7：手动镜头选择与焦距 / 光圈覆盖会改变 lensfun 模型，属于源阶段
    mixInt(context.lens.useManualLens ? 1 : 0);
    mixBytes(context.lens.manualLensName.data(), context.lens.manualLensName.size());
    mixFloat(context.lens.manualFocalLength);
    mixFloat(context.lens.manualAperture);

    return hash;
}

/// 把 binBayerToRgb 产生的饱和标记按 area 块做"按位或"归约。
///
/// 降采样时只要块内有一个像素的某个通道饱和，该通道的颜色信息就已经丢失，
/// 因此归约必须用按位或而不是平均；块尺寸与 downsampleRgbArea 完全一致
/// （向下取整，越界部分忽略）。
std::vector<std::uint8_t> reduceClipMask(const std::vector<std::uint8_t>& source,
                                         int sourceWidth,
                                         int sourceHeight,
                                         int factor)
{
    if (source.empty() || factor <= 1 || sourceWidth <= 0 || sourceHeight <= 0) {
        return source;
    }

    const int outWidth = std::max(1, sourceWidth / factor);
    const int outHeight = std::max(1, sourceHeight / factor);
    std::vector<std::uint8_t> reduced(static_cast<std::size_t>(outWidth) * outHeight, 0u);

    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            std::uint8_t flags = 0u;
            for (int dy = 0; dy < factor; ++dy) {
                const int srcY = y * factor + dy;
                if (srcY >= sourceHeight) {
                    break;
                }
                const std::uint8_t* row =
                    source.data() + static_cast<std::size_t>(srcY) * sourceWidth;
                for (int dx = 0; dx < factor; ++dx) {
                    const int srcX = x * factor + dx;
                    if (srcX >= sourceWidth) {
                        break;
                    }
                    flags |= row[srcX];
                }
            }
            reduced[static_cast<std::size_t>(y) * outWidth + x] = flags;
        }
    }

    return reduced;
}

// ---------------------------------------------------------------------------
//  DCP 配置缓存（D1）
//
//  渲染线程会以每秒数十次的频率重跑管线，每次重新解析几百 KB 的 DCP 会让
//  拖动滑块明显变卡。这里按"路径 + 修改时间"缓存解析结果：
//   - 每个渲染线程一份（thread_local），无需加锁；
//   - 文件被替换后修改时间变化，自动重新解析；
//   - 解析失败也缓存，但保留错误信息供日志提示。
// ---------------------------------------------------------------------------
struct DcpProfileCache {
    std::string path;
    std::filesystem::file_time_type modified{};
    bool hasTimestamp = false;
    bool attempted = false;
    /// 警告 / 错误只在本次配置（路径或修改时间变化后）第一次渲染时上报，
    /// 避免拖动滑块时每秒往日志面板写几十条相同信息。
    bool warningsReported = false;
    bool errorReported = false;
    bool matrixReported = false;
    DcpProfile profile;
};

DcpProfileCache& dcpProfileCache()
{
    static thread_local DcpProfileCache cache;
    return cache;
}

/// 取得缓存的 DCP；文件缺失或解析失败时返回 nullptr，并写入 error。
const DcpProfile* loadDcpProfileCached(const std::string& path, std::string& error)
{
    DcpProfileCache& cache = dcpProfileCache();
    if (path.empty()) {
        error = "DCP 路径为空。";
        return nullptr;
    }

    std::error_code ec;
    const std::filesystem::file_time_type modified =
        std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
    const bool hasTimestamp = !ec;

    if (!cache.attempted || cache.path != path
        || (hasTimestamp && cache.hasTimestamp && cache.modified != modified)
        || (!hasTimestamp && cache.hasTimestamp)) {
        cache.path = path;
        cache.modified = modified;
        cache.hasTimestamp = hasTimestamp;
        cache.attempted = true;
        cache.warningsReported = false;
        cache.errorReported = false;
        cache.matrixReported = false;
        cache.profile = loadDcpProfile(path);
    }

    if (!cache.profile.valid) {
        error = cache.profile.error.empty()
            ? std::string("DCP 配置无效。") : cache.profile.error;
        return nullptr;
    }
    return &cache.profile;
}

ErrorInfo runPipeline(const RawDecodeResult& raw,
                      const EditContext& context,
                      const RenderOptions& options,
                      RenderResult& result,
                      const std::atomic<bool>& cancelFlag,
                      LensCorrector* lensCorrector,
                      ColorManagement* colorManagement,
                      BaseImageCache* baseCache,
                      MaskWeightCache* maskCache,
                      ProgressCallback progress,
                      MessageCallback message)
{
    result = RenderResult{};

    const ProgressReporter report(progress);
    const MessageReporter notify(message, &result.warnings);

    // ---- 入口校验 ----
    if (!raw.isValid()) {
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "没有可用的 RAW 数据，请先成功打开一个 RAW 文件。");
    }

    // 彩边 / 色散校正总开关：线性标定视图要求输出原生解拜耳数据，
    // 因此一次性关闭全部校正（需求 requirement_append_falsecolor.md「视图联动」）。
    const bool correctionsEnabled = !options.calibrationView;
    const FalseColorSettings& falseColor = context.falseColor;

    // ---- 阶段顺序（diy.md §4）----
    // 自定义顺序必须通过注册表校验；非法顺序回退默认并记录警告。
    std::vector<PipelineStageId> effectiveOrder = defaultPipelineOrder();
    if (context.pipelineOrder.enable && !context.pipelineOrder.stages.empty()) {
        std::vector<PipelineStageId> requested;
        requested.reserve(context.pipelineOrder.stages.size());
        for (const std::string& key : context.pipelineOrder.stages) {
            bool found = false;
            const PipelineStageId id = pipelineStageFromKey(key, &found);
            if (!found) {
                notify.warn("管线顺序包含未知阶段：" + key + "，已回退默认顺序。");
                requested.clear();
                break;
            }
            requested.push_back(id);
        }
        std::string orderError;
        if (!requested.empty() && validatePipelineOrder(requested, orderError)) {
            effectiveOrder = std::move(requested);
        } else if (!requested.empty()) {
            notify.warn("管线顺序非法，已回退默认顺序：" + orderError);
        }
    }

    // 从生效顺序中提取某一组阶段（未列出的阶段不会执行，保持默认包含全部）
    const auto orderedGroup = [&effectiveOrder](
        std::initializer_list<PipelineStageId> members) {
        std::vector<PipelineStageId> result;
        for (PipelineStageId id : effectiveOrder) {
            if (std::find(members.begin(), members.end(), id) != members.end()) {
                result.push_back(id);
            }
        }
        return result;
    };

    // 双光源 CCM：按 RAW 的 as-shot 色温解析出本次实际使用的矩阵。
    // 只依赖 RAW 元数据而不依赖用户白平衡，因此结果稳定，
    // 可以安全地参与源阶段缓存键。
    const Matrix3x3 resolvedCcm = context.ccm.resolvedCam2xyz(raw.metadata.asShotTemperature);

    // ---- DCP 相机配置文件（D1）----
    //
    // 标定视图要求输出"原生解拜耳线性数据"，DCP 的矩阵与 Look 表都属于
    // 相机标定链，因此标定视图整体跳过 DCP，回退到原厂矩阵。
    // 普通位图的像素已完成相机色彩处理，也不再叠加 DCP。
    const bool dcpRequested = !raw.isRgbSource && context.dcp.isActive()
        && !options.calibrationView;
    const DcpProfile* dcpProfile = nullptr;
    if (dcpRequested) {
        std::string dcpError;
        dcpProfile = loadDcpProfileCached(context.dcp.profilePath, dcpError);
        DcpProfileCache& cache = dcpProfileCache();
        if (dcpProfile == nullptr) {
            if (!cache.errorReported) {
                notify.warn("DCP 配置加载失败，已回退到相机原厂矩阵：" + dcpError);
                cache.errorReported = true;
            }
        } else if (!cache.warningsReported) {
            for (const std::string& warning : dcpProfile->warnings) {
                notify.warn("DCP：" + warning);
            }
            cache.warningsReported = true;
        }
    }
    const bool dcpActive = (dcpProfile != nullptr);

    // 自定义 CCM 与 DCP 同时存在时，用户显式启用的自定义 CCM 优先；
    // DCP 只保留 Look 表阶段，不再替换相机矩阵。标定视图同样跳过 DCP。
    const bool customCcmActive = correctionsEnabled && context.ccm.enable
        && !raw.cfaPatternGuessed
        && ColorCalibration::validate(resolvedCcm).isUsable();
    const bool dcpMatrixActive = dcpActive && !context.raw.disableCameraMatrix
        && !customCcmActive;

    // DCP 的 ForwardMatrix / ColorMatrix 都定义在"已做 as-shot 白平衡"的
    // 相机 RGB 上。有可靠的相机中性值时，管线会把它折进矩阵；
    // 这时基准白平衡不再重复施加，避免二次白平衡。
    const bool dcpWhiteBalanceHandled = dcpMatrixActive && raw.hasCameraWhiteBalanceGain;

    if (customCcmActive && dcpActive && context.dcp.hasLookStages()) {
        notify.warn("自定义 CCM 与 DCP 同时启用：相机矩阵使用自定义 CCM，"
                    "DCP 仅应用 Hue/Sat、曲线与 Look 阶段。");
    }

    // ---- CCM 矩阵合法性预检（需求 §1.3.7、§6）----
    // 在渲染开始前检查，矩阵非法时给出警告但不中止（需求 §6 明确不强制回滚）
    if (context.ccm.enable) {
        const MatrixValidationResult validation = ColorCalibration::validate(resolvedCcm);
        if (validation.level == MatrixValidationLevel::Invalid) {
            notify.warn("自定义 CCM 矩阵未通过校验：" + validation.message
                        + " 已回退到相机原厂矩阵以避免画面崩坏。");
        } else if (validation.level == MatrixValidationLevel::Warning) {
            notify.warn("自定义 CCM 矩阵提示：" + validation.message);
        }
    }

    // =======================================================================
    //  第一层：RAW 底层标定（差异化能力）
    //
    //  只处理与相机 / 传感器 / 镜头相关的、应可复现的标定步骤：
    //    白电平 → 坏点 / 暗电流 / 暗场 / 平场 → 串扰 → 拜耳域 CCM
    //    → 去拜耳 → 高光 Roll-off → LCA / LoCA / 边缘伪色 → 镜头校正
    //    → CamRGB→XYZ → XYZ→ProPhoto。
    //  这一层不读取白平衡 / 光影 / 风格参数，结果可写入 .camcal / .lenscal。
    // =======================================================================

    // -----------------------------------------------------------------------
    //  源阶段缓存命中判断
    //
    //  源阶段 = 白电平裁剪 → CCM → 坏点修复 → 去拜耳 → 镜头校正 → 工作空间。
    //  这些步骤只依赖 RAW 数据与少数"基础"参数；拖动白平衡、光影、HSL、
    //  曲线、降噪、锐化滑块时结果不变，可直接复用，避免重复 3~4 秒的去拜耳。
    // -----------------------------------------------------------------------
    // 预览降采样以**裁剪后的有效尺寸**为基准：
    // 大幅裁剪后若仍按原始尺寸计算倍数，裁出来的预览会远小于屏幕需要，
    // 画布只能放大显示而发虚。裁剪矩形很小、倍数降到 1 时宁可多算，
    // 也要保证 1:1 与 fit 视图的清晰度。
    int previewSourceWidth = raw.sourceWidth();
    int previewSourceHeight = raw.sourceHeight();
    if (context.crop.enable) {
        const float cropWidth = math::clamp(
            std::isfinite(context.crop.width) ? context.crop.width : 1.0f,
            0.01f, 1.0f);
        const float cropHeight = math::clamp(
            std::isfinite(context.crop.height) ? context.crop.height : 1.0f,
            0.01f, 1.0f);
        previewSourceWidth = std::max(
            1, static_cast<int>(std::lround(previewSourceWidth * cropWidth)));
        previewSourceHeight = std::max(
            1, static_cast<int>(std::lround(previewSourceHeight * cropHeight)));
    }

    int downsampleFactor = 1;
    if (options.scale == RenderScale::Preview && options.previewMaxDimension > 0) {
        if (raw.isRgbSource) {
            // 普通位图没有 CFA 相位约束，按整数倍数缩到目标尺寸即可，
            // 不必像拜耳域那样强制取偶（取偶会让 6000px 的图停在 3000px）。
            const int longestSide = std::max(previewSourceWidth, previewSourceHeight);
            downsampleFactor = (longestSide > options.previewMaxDimension)
                ? (longestSide + options.previewMaxDimension - 1)
                    / options.previewMaxDimension
                : 1;
        } else {
            // 注意：runPipeline 是自由函数，不能直接调用 PipelineRenderer 的静态成员，
            // 因此使用限定名。
            downsampleFactor = PipelineRenderer::computeDownsampleFactor(
                previewSourceWidth, previewSourceHeight, options.previewMaxDimension);
        }
    }

    const std::uint64_t baseKey = computeBaseKey(raw, context, options, downsampleFactor);
    BaseImageCacheEntry* cachedBase =
        (baseCache != nullptr) ? baseCache->find(baseKey) : nullptr;
    const bool baseCacheHit =
        (cachedBase != nullptr) && !cachedBase->image.isEmpty();

    ImageBuffer workingImage;

    // 2x2 合并产生的饱和标记。必须一直带到白平衡之后才能用于高光还原：
    // 相机的原始通道对白色的响应并不相等，在相机域里把三通道"拉平"得到的
    // 是偏色（实测偏蓝），只有在白平衡之后工作空间里等值才真正是中性白。
    std::vector<std::uint8_t> clipMask;

    // 本次预览是否真的走了 2x2 合并。后续的色度抗混叠只对这条路径生效：
    // 合并失败会退回全分辨率去马赛克，半分辨率的低通会把色度抹得过宽。
    bool previewBinningApplied = false;

    if (baseCacheHit) {
        // 深拷贝：后续阶段会原地修改像素，不能直接改动缓存
        workingImage = cachedBase->image;
        clipMask = cachedBase->clipMask;
        result.wasDownsampled = downsampleFactor > 1;
        // 合并失败时不会写缓存，因此能命中缓存的降采样结果都来自 2x2 合并。
        previewBinningApplied = !raw.isRgbSource
            && !raw.bayer.usesGenericCfa() && downsampleFactor > 1;

        if (cancelFlag.load(std::memory_order_relaxed)) {
            return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
        }
    } else {
        // 降采样失败退回全尺寸时，键与图像尺寸不对应，本次不写缓存
        bool cacheBase = true;

    // -----------------------------------------------------------------------
    //  第 0 步：准备拜耳数据
    //
    //  预览降采样不在这里做。拜耳域大块平均会让同一块内 R/G/B 的采样位置
    //  不一致，强边缘处三个通道的混合比例不同，去马赛克后形成成片的
    //  彩色马赛克。改为在去马赛克一步用"2x2 合并 + RGB 域面积平均"，
    //  保证每个输出像素的 R/G/B 来自同一空间块。
    // -----------------------------------------------------------------------
    const bool genericCfa = raw.bayer.usesGenericCfa();
    // 需求 append_v3 §1.1：拜耳排列识别失败时，bayer.pattern 是 RGGB 猜测值。
    // 所有依赖"某像素属于哪个颜色通道"的拜耳域校正都必须跳过，否则会把
    // 错误的通道假设进一步扩散；与通道排列无关的步骤（坏点、暗电流、
    // 去马赛克本身的尽力而为）继续执行，保证画面仍可查看和编辑。
    const bool cfaPatternReliable = !raw.cfaPatternGuessed;
    // X-Trans 不能做 2x2 合并：改为全分辨率去马赛克后再在 RGB 域面积降采样，
    // 首帧成本更高，但结果会写入源阶段缓存，拖动参数时不会重复付出。
    const bool previewBinning = options.scale == RenderScale::Preview
        && downsampleFactor > 1 && !genericCfa;

    BayerImage workingBayer = raw.bayer;
    if (previewBinning) {
        result.wasDownsampled = true;
    }

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

        // -----------------------------------------------------------------------
        //  普通位图（JPG / PNG / TIFF …）的源阶段
        //
        //  相机/软件已经把白平衡、去马赛克、镜头校正烘焙进像素，这里只做：
        //   1. 预览降采样：线性 RGB 域面积平均（没有 CFA 相位约束）；
        //   2. 线性 sRGB(D65) → 线性 ProPhoto(D50)，与 RAW 路径汇合。
        //  之后的源阶段缓存、白平衡、光影、HSL、曲线、LUT、降噪、锐化、
        //  色彩管理与导出全部共用同一套代码。
        // -----------------------------------------------------------------------
        if (raw.isRgbSource) {
            report.report(RenderStage::Demosaic);

            // 预览降采样到临时缓冲；1:1 与导出直接用解码结果，避免整幅深拷贝
            const ImageBuffer* sourceImage = &raw.rgbLinear;
            ImageBuffer downsampled;
            if (downsampleFactor > 1) {
                ops::downsampleRgbArea(raw.rgbLinear, downsampled, downsampleFactor);
                sourceImage = &downsampled;
                result.wasDownsampled = true;
            }

            workingImage.allocate(sourceImage->width(), sourceImage->height());

            // 三个标准矩阵依次相乘：sRGB(D65)→XYZ(D65)→Bradford→XYZ(D50)
            // →ProPhoto(D50)。白点经此变换后仍为中性（1,1,1）。
            const Matrix3x3 srgbToXyzD65{{
                {{ 0.4124564f, 0.3575761f, 0.1804375f}},
                {{ 0.2126729f, 0.7151522f, 0.0721750f}},
                {{ 0.0193339f, 0.1191920f, 0.9503041f}},
            }};
            const Matrix3x3 d65ToD50Bradford{{
                {{ 1.0478112f, 0.0228866f, -0.0501270f}},
                {{ 0.0295424f, 0.9904844f, -0.0170491f}},
                {{-0.0092345f, 0.0150436f, 0.7521316f}},
            }};
            const Matrix3x3 xyzD50ToProPhoto{{
                {{ 1.3459433f, -0.2556075f, -0.0511118f}},
                {{-0.5445989f, 1.5081673f, 0.0205351f}},
                {{ 0.0000000f, 0.0000000f, 1.2118128f}},
            }};
            const Matrix3x3 srgbToWorking = math::multiply(
                math::multiply(xyzD50ToProPhoto, d65ToD50Bradford), srgbToXyzD65);

            report.report(RenderStage::XyzToWorking);
            const std::size_t pixels = workingImage.pixelCount();
            const float* src = sourceImage->data();
            float* dst = workingImage.data();

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
            for (int i = 0; i < static_cast<int>(pixels); ++i) {
                const float* pixel = src + static_cast<std::size_t>(i) * 3u;
                float* out = dst + static_cast<std::size_t>(i) * 3u;
                math::multiply(srgbToWorking, pixel, out);
            }
        } else {

    // -----------------------------------------------------------------------
    //  第 1 步：白电平饱和裁剪
    //
    //  需求 §2 管线第三项。黑电平扣除已在 RawLoader 中完成（管线第二项），
    //  此处把数据归一化到 0~1 线性范围，作为后续所有运算的统一输入基准。
    // -----------------------------------------------------------------------
    report.report(RenderStage::BlackWhiteLevel);
    {
        float whiteLevel = raw.whiteLevel;
        // 用户在进阶模式下可覆盖白电平（需求 §3 模块 A）
        if (context.raw.whiteLevelOverride > 0.0f) {
            whiteLevel = context.raw.whiteLevelOverride;
        }
        const bool perChannelWhite =
            context.raw.channelWhiteLevel[0] > 0.0f
            || context.raw.channelWhiteLevel[1] > 0.0f
            || context.raw.channelWhiteLevel[2] > 0.0f
            || context.raw.channelWhiteLevel[3] > 0.0f;
        if (perChannelWhite) {
            ops::applyWhiteLevelClipPerChannel(
                workingBayer, context.raw.channelWhiteLevel, whiteLevel);
        } else {
            ops::applyWhiteLevelClip(workingBayer, whiteLevel);
        }
    }

    // -----------------------------------------------------------------------
    //  第 1.2 步：基础坏点修复
    //
    //  需求 v2 §2.1：在暗电流补偿与串扰补偿之前先清掉热像素 / 固定坏点，
    //  避免异常单点被后续邻域运算扩散到周围像素。
    // -----------------------------------------------------------------------
    report.report(RenderStage::BadPixelRepair);
    if (context.raw.enableBadPixelRepair) {
        ops::repairBadPixels(workingBayer, 4.0f);
    }

    // -----------------------------------------------------------------------
    //  第 1.3 步：暗电流补偿
    //
    //  需求 v2 §2.2：按 ISO 分段减去传感器暗底偏移，抑制高 ISO / 长曝光
    //  暗部的彩色底噪。偏移量由机型配置提供，缺省时使用内置保守曲线。
    //  补偿在校正链路最前端完成，且不改变黑位定义。
    //  标定视图关闭本步：标定需要未经任何偏移的原始线性数据（需求 v2 核心原则 2）。
    // -----------------------------------------------------------------------
    if (correctionsEnabled && context.darkCurrent.enable
        && context.darkCurrent.strength > 1e-4f) {
        const float darkOffset =
            context.darkCurrent.offsetForIso(raw.metadata.iso);
        if (darkOffset > 1e-7f) {
            report.report(RenderStage::DarkCurrent);
            ops::applyDarkCurrentCompensation(workingBayer,
                                              context.darkCurrent.strength,
                                              darkOffset);
        }
    }

    // -----------------------------------------------------------------------
    //  第 1.4 步：暗场帧 / 平场帧校正（需求 append_v3 §五 1、2）
    //
    //  暗场帧：逐像素减去暗噪声；平场帧：逐像素除以归一化响应。
    //  两者都作用在归一化拜耳域、串扰与 CCM 之前，不改变管线主顺序。
    //  帧按路径 + 修改时间缓存，拖动参数时不会重复解码 RAW。
    //  标定视图关闭本步：标定需要未经校正的原始线性数据。
    // -----------------------------------------------------------------------
    if (correctionsEnabled) {
        const FrameCalibrationSettings& frames = context.frameCalibration;

        if (frames.enableDarkFrame && !frames.darkFramePath.empty()
            && frames.darkFrameStrength > 1e-4f) {
            std::string frameError;
            const std::shared_ptr<const CalibrationFrame> dark =
                loadCalibrationFrame(frames.darkFramePath, frameError);
            if (!dark) {
                notify.warn("暗场帧加载失败，已跳过暗场帧校正：" + frameError);
            } else {
                if (!dark->matches(workingBayer.width, workingBayer.height)) {
                    notify.warn("暗场帧尺寸与主图不一致，已降级为按通道均值扣除"
                                "（只校正整体暗底抬升，不校正热像素）。");
                }
                report.report(RenderStage::DarkFrame);
                applyDarkFrameSubtraction(workingBayer, *dark,
                                          frames.darkFrameStrength);
            }
        }

        if (frames.enableFlatFrame && !frames.flatFramePath.empty()
            && frames.flatFrameStrength > 1e-4f) {
            std::string frameError;
            const std::shared_ptr<const CalibrationFrame> flat =
                loadCalibrationFrame(frames.flatFramePath, frameError);
            if (!flat) {
                notify.warn("平场帧加载失败，已跳过平场帧校正：" + frameError);
            } else {
                if (!flat->matches(workingBayer.width, workingBayer.height)) {
                    notify.warn("平场帧尺寸与主图不一致，已降级为按通道均值校正"
                                "（只消除偏色，不校正渐晕）。");
                }
                report.report(RenderStage::FlatFrame);
                applyFlatFrameCorrection(workingBayer, *flat,
                                         frames.flatFrameStrength);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  第 1.5 步：CMOS 串扰补偿（3×3 解耦矩阵）
    //
    //  需求 requirement_append_falsecolor.md 第 1 节：BSI / 堆栈式 CMOS 的
    //  像素之间存在光子渗漏，表现为通道串扰（尤其蓝通道向绿通道渗透）。
    //  这一步在**线性传感器域**、白平衡与 CCM 之前完成，与机型色彩标定解耦；
    //  标定矩阵随机型校准文件下发，强度滑块用于微调。
    //  线性标定视图会关闭本步（见 correctionsEnabled）。
    // -----------------------------------------------------------------------
    // 拜耳域阶段：串扰 / 拜耳 CCM / FBDD 按用户顺序执行
    Matrix3x3 camToXyz = raw.cameraToXyz;
    if (context.raw.disableCameraMatrix) {
        camToXyz = identityMatrix3x3();
        notify.warn("已按用户设置禁用相机原厂矩阵（CamRGB→XYZ 使用单位矩阵）。");
    }

    const std::vector<PipelineStageId> bayerStages = orderedGroup(
        {PipelineStageId::Crosstalk, PipelineStageId::BayerCcm,
         PipelineStageId::BayerDenoise});

    for (PipelineStageId stage : bayerStages) {
        if (stage == PipelineStageId::Crosstalk) {
            if (correctionsEnabled && cfaPatternReliable
                && falseColor.crosstalk.enable
                && falseColor.crosstalk.strength > 1e-4f) {
                report.report(RenderStage::Crosstalk);
                ops::applyCrosstalkCompensation(
                    workingBayer, falseColor.crosstalk.decouplingMatrix,
                    falseColor.crosstalk.strength);
            }
        } else if (stage == PipelineStageId::BayerCcm) {
            if (correctionsEnabled && context.ccm.enable && cfaPatternReliable) {
                const MatrixValidationResult validation =
                    ColorCalibration::validate(resolvedCcm);
                if (validation.isUsable()) {
                    report.report(RenderStage::BayerCcm);
                    if (ops::applyBayerLinearMatrix(workingBayer, resolvedCcm).isOk()) {
                        camToXyz = identityMatrix3x3();
                        notify.warn("已启用自定义 CCM 矩阵，原厂矩阵已按优先级规则被覆盖。");
                    } else {
                        notify.warn("拜耳域 CCM 变换失败，已跳过该步骤。");
                    }
                } else {
                    notify.warn("自定义 CCM 矩阵非法（" + validation.message
                                + "），本次渲染已改用相机原厂矩阵。");
                }
            }
        } else if (stage == PipelineStageId::BayerDenoise) {
            if (correctionsEnabled && cfaPatternReliable
                && context.raw.enableFbdd
                && context.raw.fbddStrength > 1e-4f) {
                ops::applyFbddDenoise(workingBayer, context.raw.fbddStrength);
            }
        }
        if (cancelFlag.load(std::memory_order_relaxed)) {
            return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
        }
    }

    // -----------------------------------------------------------------------
    //  第 4 步：Demosaic 去拜耳（算法可选 + 摩尔纹平滑）
    //  需求 §2 管线第六项
    // -----------------------------------------------------------------------
    report.report(RenderStage::Demosaic);

    if (previewBinning) {
        // 2x2 合并替代普通去马赛克：每像素的 R/G/B 来自同一 CFA 单元，
        // 边缘处不会出现通道错位，因而不产生彩色马赛克。
        ImageBuffer binned;
        // 饱和标记：记录哪些 2x2 单元的哪些通道顶到了白电平。
        // 该标记要留到白平衡之后才能用于高光还原，因此先按合并分辨率保存。
        const ErrorInfo binError = ops::binBayerToRgb(workingBayer, binned, &clipMask);

        if (binError.isOk()) {
            previewBinningApplied = true;
            // 排列为猜测值时，掩码里"哪个通道饱和"的判断不可信，
            // 直接丢弃，避免后续高光还原按错误通道做插值。
            if (!cfaPatternReliable) {
                clipMask.clear();
            }
            if (downsampleFactor > 2) {
                // 合并后已是 1/2 分辨率，再在 RGB 域用面积平均降到目标尺寸。
                // RGB 域降采样不涉及 CFA 相位，任意整数倍数都不会产生色差。
                ops::downsampleRgbArea(binned, workingImage, downsampleFactor / 2);
                // 掩码同步归约：块内任一像素的某个通道饱和，就保留该位。
                // 高光还原只用到"是否部分饱和"，与下面的白平衡后处理配合。
                if (!clipMask.empty()) {
                    clipMask = reduceClipMask(clipMask, binned.width(), binned.height(),
                                              downsampleFactor / 2);
                }
            } else {
                workingImage = std::move(binned);
            }

            // 合并时 R / B 是单元内的单点采样，遇到像素级细节（高光点、细线、
            // 镜面反光）会合成出随机颜色的孤立斑点。这里按色比中值清除孤立
            // 伪彩；部分饱和高光由白平衡之后的 flattenClippedHighlights 处理。
            // 仅用于预览合并路径；导出与放大查看走全分辨率去马赛克，不经过这里。
            ops::suppressFalseColor(workingImage, 0.08f);
        } else {
            // 排列未知等异常：退回全分辨率普通去马赛克，宁可慢也保证正确
            result.wasDownsampled = false;
            cacheBase = false;
            // 掩码对应的是半分辨率合并图，与本次的全分辨率输出不匹配，必须丢弃
            clipMask.clear();
            notify.warn("预览 2x2 合并失败，已改用原始分辨率渲染：" + binError.message);

            const ErrorInfo demosaicError = ops::demosaic(
                workingBayer,
                workingImage,
                context.raw.demosaicAlgorithm,
                context.raw.demosaicSmooth);

            if (!demosaicError.isOk()) {
                return ErrorInfo::make(demosaicError.code,
                                       "去拜耳运算失败：" + demosaicError.message,
                                       demosaicError.detail);
            }
        }
    } else {
        const ErrorInfo demosaicError = ops::demosaic(
            workingBayer,
            workingImage,
            context.raw.demosaicAlgorithm,
            context.raw.demosaicSmooth);

        if (!demosaicError.isOk()) {
            return ErrorInfo::make(demosaicError.code,
                                   "去拜耳运算失败：" + demosaicError.message,
                                   demosaicError.detail);
        }

        // 全分辨率去马赛克后的孤立伪彩清理。
        //
        // AMaZE 的色差中值能压掉大部分 zipper，但高密度纹理（幕墙网格、
        // 屋顶瓦片、光栅）上的插值误差仍会留下 1~2 像素的红/蓝/绿孤立彩点。
        // 这些点只影响色度、不影响亮度，用色比中值清理既能显著改善观感，
        // 又不会让细节发软——与预览 2x2 合并路径使用的是同一套判据。
        // 阈值略低于预览路径：全分辨率下降噪与去马赛克已经更精细，
        // 过度清理会把远处的真实彩色细节（招牌、衣物）一起抹掉。
        ops::suppressFalseColor(workingImage, 0.06f);

        // X-Trans 预览：先按原始分辨率去马赛克，再在 RGB 域面积降采样。
        // RGB 域不涉及 CFA 相位，因此任意倍数都安全；饱和掩码同理先全分辨率
        // 生成、再按块或归约，保证高光还原与导出路径一致。
        if (genericCfa && options.scale == RenderScale::Preview
            && downsampleFactor > 1) {
            std::vector<std::uint8_t> fullResMask;
            const bool maskOk = cfaPatternReliable
                && ops::buildClipMaskFromBayer(workingBayer, fullResMask);

            ImageBuffer downsampled;
            ops::downsampleRgbArea(workingImage, downsampled, downsampleFactor);

            if (maskOk) {
                clipMask = reduceClipMask(fullResMask,
                                          workingImage.width(),
                                          workingImage.height(),
                                          downsampleFactor);
            }

            workingImage = std::move(downsampled);
            // RGB 域盒式降采样仍可能在高频细节处产生孤立伪彩，
            // 与 2x2 合并路径保持一致做一次色比中值清理。
            ops::suppressFalseColor(workingImage, 0.08f);
            result.wasDownsampled = true;
        }
    }

    // 全分辨率路径没有 2x2 合并步骤，需要在这里从拜耳邻域生成饱和掩码，
    // 否则导出/1:1 查看时部分饱和高光不会被还原（预览有掩码、导出没有，
    // 会出现"预览正常、导出洋红高光"的不一致）。
    // 预览合并路径的掩码已经与 workingImage 同尺寸；只有尺寸不匹配时才补算。
    if (clipMask.size() != static_cast<std::size_t>(workingImage.pixelCount())) {
        std::vector<std::uint8_t> fullResMask;
        if (cfaPatternReliable
            && ops::buildClipMaskFromBayer(workingBayer, fullResMask)
            && fullResMask.size() == static_cast<std::size_t>(workingImage.pixelCount())) {
            clipMask = std::move(fullResMask);
        }
    }

    // 拜耳数据已完成使命，及时释放（需求 §5.3 内存管理）
    workingBayer.data.clear();
    workingBayer.data.shrink_to_fit();

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

    // -----------------------------------------------------------------------
    //  第 4.05 步：Demosaic 空间边缘 / 伪色策略（diy.md §5）
    // -----------------------------------------------------------------------
    if (!raw.isRgbSource && context.demosaicSpatial.enable) {
        const float spatialScale = raw.sourceWidth() > 0
            ? static_cast<float>(workingImage.width())
                / static_cast<float>(raw.sourceWidth())
            : 1.0f;
        ops::applyDemosaicSpatialStrategy(workingImage,
                                          context.demosaicSpatial,
                                          spatialScale);
    }

    // -----------------------------------------------------------------------
    //  第 4.1 步：高光 Roll-off（线性域软饱和压缩）
    //
    //  需求第 3 节：高光处三通道进入饱和的速度不同，若直接硬截断，
    //  未饱和通道保留细节、已饱和通道变成平台，边缘就会出现彩边。
    //  这里在三通道上做同一条指数软膝曲线，让它们以相近的斜率逼近 1.0。
    // -----------------------------------------------------------------------
    // RGB 色彩校正阶段：Roll-off / LCA / LoCA / 边缘伪色按用户顺序执行
    const std::vector<PipelineStageId> colorStages = orderedGroup(
        {PipelineStageId::HighlightRolloff, PipelineStageId::Lca,
         PipelineStageId::Loca, PipelineStageId::EdgeFalseColor});
    for (PipelineStageId stage : colorStages) {
        if (stage == PipelineStageId::HighlightRolloff) {
            if (correctionsEnabled && falseColor.highlightRolloff.enable) {
                report.report(RenderStage::HighlightRolloff);
                ops::applyHighlightRolloff(workingImage,
                                           falseColor.highlightRolloff.knee,
                                           falseColor.highlightRolloff.slope);
            }
        } else if (stage == PipelineStageId::Lca) {
            if (correctionsEnabled && falseColor.lca.enable) {
                report.report(RenderStage::Lca);
                ops::applyLateralChromaticAberration(
                    workingImage, falseColor.lca.redScale,
                    falseColor.lca.blueScale, falseColor.lca.radialStrength);
            }
        } else if (stage == PipelineStageId::Loca) {
            if (correctionsEnabled && falseColor.loca.enable
                && falseColor.loca.strength > 1e-4f) {
                report.report(RenderStage::Loca);
                ops::applyLongitudinalChromaticAberration(
                    workingImage, falseColor.loca.strength,
                    falseColor.loca.highlightThreshold);
            }
        } else if (stage == PipelineStageId::EdgeFalseColor) {
            if (correctionsEnabled && falseColor.edgeFalseColor.enable
                && falseColor.edgeFalseColor.strength > 1e-4f) {
                report.report(RenderStage::EdgeFalseColor);
                ops::suppressEdgeFalseColor(
                    workingImage, falseColor.edgeFalseColor.strength,
                    falseColor.edgeFalseColor.edgeSensitivity,
                    falseColor.edgeFalseColor.smoothingRadius);
            }
        }
    }

    // -----------------------------------------------------------------------
    //  第 5 步：lensfun 镜头校正（畸变、暗角、轴向色差）
    //  需求 §2 管线第七项，位于 Demosaic 之后、色彩转换之前
    // -----------------------------------------------------------------------
    // 标定视图跳过镜头校正：畸变重采样会改变像素位置，
    // 不适合作为 CCM 标定 / 色彩比对的原始线性输入（需求 v2 核心原则 2）。
    // 普通位图已由相机/软件完成镜头校正，且没有 RAW 的镜头元数据可信度，
    // 这里不再叠加一次。
    if (!raw.isRgbSource && correctionsEnabled && options.applyLensCorrection
        && context.lens.enable) {
        report.report(RenderStage::LensCorrection);

        if (!LensCorrector::isSupported()) {
            notify.warn("本次构建未包含 lensfun 支持，镜头校正已跳过。");
        } else if (lensCorrector->findLens(raw.metadata, &context.lens)) {
            const LensCorrectionResult lensResult =
                lensCorrector->apply(workingImage, context.lens);
            if (lensResult.applied) {
                notify.warn(lensResult.message);
            } else {
                // 需求 §6：无匹配镜头数据时自动关闭并提示
                notify.warn(lensResult.message);
            }
        } else {
            notify.warn(lensCorrector->lastMatchMessage());
        }
    }

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

    // -----------------------------------------------------------------------
    //  第 6 步：CamRGB → XYZ 转换
    //  需求 §2 管线第八项
    //
    //  实现方式：3x3 矩阵乘法（线性域）。这里**不用** lcms2，
    //  因为矩阵乘法更快、更可控，且本就是线性变换的定义式。
    //  若未启用自定义 CCM，此处使用 RAW 自带的原厂矩阵。
    // -----------------------------------------------------------------------
    report.report(RenderStage::CameraToXyz);

    // DCP 相机矩阵（D1）：优先 ForwardMatrix，其次 ColorMatrix 的逆。
    // 两种矩阵在 DNG 中都定义在 as-shot 白平衡之后，因此把相机中性值
    // 折进矩阵的列缩放（等价于先逐通道除以中性值，再做矩阵乘法）。
    if (dcpMatrixActive && dcpProfile != nullptr) {
        Matrix3x3 dcpMatrix{};
        bool haveMatrix = false;
        bool usingForwardMatrix = false;
        const char* source = nullptr;
        if (context.dcp.useForwardMatrix
            && ops::dcpInterpolatedMatrix(*dcpProfile, true,
                                          raw.metadata.asShotTemperature, dcpMatrix)) {
            haveMatrix = true;
            usingForwardMatrix = true;
            source = "ForwardMatrix";
        } else if (ops::dcpInterpolatedMatrix(*dcpProfile, false,
                                               raw.metadata.asShotTemperature, dcpMatrix)) {
            haveMatrix = true;
            source = "ColorMatrix";
        }

        if (haveMatrix) {
            if (raw.hasCameraWhiteBalanceGain) {
                for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t col = 0; col < 3; ++col) {
                        dcpMatrix[row][col] *= raw.cameraWhiteBalanceGain[col];
                    }
                }
            }

            // ForwardMatrix 的定义就是“白平衡后的相机 RGB → XYZ(D50)”，
            // 白点已经落在 D50，不能再做色适应。
            // ColorMatrix 的逆只给出相对标定光源（A / D65）的 XYZ；
            // 白平衡后的相机中性 (1,1,1) 经该矩阵就是拍摄白点，
            // 必须用 Bradford 适应到 D50，否则进入 D50 工作空间会整幅偏色
            // （典型是品红 / 蓝紫色）。
            if (!usingForwardMatrix) {
                const float sourceWhiteXyz[3] = {
                    dcpMatrix[0][0] + dcpMatrix[0][1] + dcpMatrix[0][2],
                    dcpMatrix[1][0] + dcpMatrix[1][1] + dcpMatrix[1][2],
                    dcpMatrix[2][0] + dcpMatrix[2][1] + dcpMatrix[2][2],
                };
                constexpr float kD50WhiteXyz[3] = {0.96422f, 1.0f, 0.82521f};
                if (sourceWhiteXyz[1] > 1e-4f && std::isfinite(sourceWhiteXyz[0])
                    && std::isfinite(sourceWhiteXyz[1])
                    && std::isfinite(sourceWhiteXyz[2])) {
                    dcpMatrix = math::multiply(
                        math::bradfordAdaptation(sourceWhiteXyz, kD50WhiteXyz),
                        dcpMatrix);
                }
            }

            camToXyz = dcpMatrix;
            DcpProfileCache& cache = dcpProfileCache();
            if (!cache.matrixReported) {
                notify.warn(std::string("DCP：相机 → XYZ 使用 ") + source
                            + (raw.hasCameraWhiteBalanceGain
                                   ? "（已在相机域应用 as-shot 白平衡）。"
                                   : "（缺少相机中性值，沿用工作空间白平衡）。"));
                cache.matrixReported = true;
            }
        } else {
            // 没有可用矩阵时每次渲染都提示一次：这是需要用户处理的异常。
            notify.warn("DCP 中没有可用的相机矩阵，本次沿用相机原厂矩阵。");
        }
    }

    {
        const std::size_t pixels = workingImage.pixelCount();
        float* data = workingImage.data();

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
        for (int i = 0; i < static_cast<int>(pixels); ++i) {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            float xyz[3];
            math::multiply(camToXyz, pixel, xyz);
            pixel[0] = xyz[0];
            pixel[1] = xyz[1];
            pixel[2] = xyz[2];
        }
    }

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

    // -----------------------------------------------------------------------
    //  第 7 步：XYZ → ProPhotoRGB（lcms2）
    //  需求 §2 管线第九项。内部固定工作色彩空间为 ProPhotoRGB（需求 §1.3.2）。
    //
    //  说明：lcms2 的 XYZ profile 默认带 gamma 曲线处理，为保证进入
    //  工作空间的是**线性**数据，这里改用等价的线性矩阵转换。
    //  该矩阵是 XYZ(D50) → ProPhotoRGB 线性空间的标准变换，
    //  与 lcms2 对线性 ProPhoto profile 的转换结果一致。
    // -----------------------------------------------------------------------
    report.report(RenderStage::XyzToWorking);
    {
        // XYZ(D50) → ProPhotoRGB 线性空间的矩阵。
        // 数值来源：ProPhotoRGB 规范给出的原色矩阵（D50 白点）。
        const Matrix3x3 xyzToProPhoto{{
            {{ 1.3459433f, -0.2556075f, -0.0511118f}},
            {{-0.5445989f,  1.5081673f,  0.0205351f}},
            {{ 0.0000000f,  0.0000000f,  1.2118128f}},
        }};

        const std::size_t pixels = workingImage.pixelCount();
        float* data = workingImage.data();

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
        for (int i = 0; i < static_cast<int>(pixels); ++i) {
            float* pixel = data + static_cast<std::size_t>(i) * 3u;
            // 用局部变量名 colorOut 而非 result，避免与外层参数 result 混淆
            float colorOut[3];
            math::multiply(xyzToProPhoto, pixel, colorOut);
            pixel[0] = colorOut[0];
            pixel[1] = colorOut[1];
            pixel[2] = colorOut[2];
        }
    }

        }  // RAW / 普通位图 源阶段分支结束

        // 源阶段完成，保存供后续参数调整复用
        if (baseCache != nullptr && cacheBase) {
            baseCache->store(baseKey, workingImage, clipMask);
        }
    }  // 源阶段缓存未命中分支结束

    // -----------------------------------------------------------------------
    //  第 7.5 步：透视 / 几何校正（C3）
    //
    //  紧接工作空间转换、位于白平衡与局部调整之前：
    //   - 镜头校正已经完成，几何校正在其之后，顺序固定；
    //   - 后续的灰卡、蒙版、裁剪都作用在校正后的图像上，坐标一致；
    //   - 输出尺寸不变，自动收窄采样区域避免黑边。
    // -----------------------------------------------------------------------
    report.report(RenderStage::Perspective);
    ops::applyPerspective(workingImage, context.perspective);

    // =======================================================================
    //  第二层：上层调色 / 风格（通用能力）
    //
    //  从白平衡开始，全部是跨相机、可预设、可跨图复制的创意调整：
    //    白平衡 → 预览影调 → 光影 → 色彩与质感 → HSL → 黑白
    //    → 颜色分级 / 分离色调 → 曲线 → 3D LUT → 局部调整 → 裁剪 / 拉直
    //    → 细节 → 输出色彩管理。
    //  这一层不得反向修改 RAW 标定参数，也不写入 .camcal / .lenscal。
    // =======================================================================

    // -----------------------------------------------------------------------
    //  第 8.0 步：基准白平衡（RAW 底层标定层）
    //
    //  建立相机中性色彩基准；与 CCM、黑电平同属 .camcal 标定配置。
    //  普通位图的像素已经由相机完成白平衡，跳过本步。
    // -----------------------------------------------------------------------
    const bool baseWbActive = !raw.isRgbSource && context.baseWhiteBalance.enable;
    const float baseWbTemperature = baseWbActive
        ? std::clamp(context.baseWhiteBalance.temperature, 1000.0f, 40000.0f)
        : 0.0f;
    const float baseWbTint = baseWbActive
        ? std::clamp(context.baseWhiteBalance.tint, -100.0f, 100.0f)
        : 0.0f;

    report.report(RenderStage::BaseWhiteBalance);
    if (baseWbActive) {
        if (dcpWhiteBalanceHandled) {
            // DCP 已在相机域完成 as-shot 白平衡。这里把基准滑块解释为
            // **相对 as-shot 的偏移**：默认值与 as-shot 相同时增益严格为 1，
            // 既保留用户手动微调基准的能力，又不会二次白平衡。
            // 灰卡拾取仍然直接对采样点归一化（与基准语义一致）。
            ops::applyWhiteBalance(workingImage,
                                   baseWbTemperature,
                                   baseWbTint,
                                   context.baseWhiteBalance.grayPickX,
                                   context.baseWhiteBalance.grayPickY,
                                   context.baseWhiteBalance.useGrayPick,
                                   raw.metadata.asShotTemperature,
                                   raw.metadata.asShotTint,
                                   false);
        } else {
            ops::applyWhiteBalance(workingImage,
                                   baseWbTemperature,
                                   baseWbTint,
                                   context.baseWhiteBalance.grayPickX,
                                   context.baseWhiteBalance.grayPickY,
                                   context.baseWhiteBalance.useGrayPick,
                                   0.0f, 0.0f);
        }
    }

    // -----------------------------------------------------------------------
    //  第 8.1 步：DCP 色彩塑形阶段（D1）
    //
    //  DNG 规范给定的顺序是 HueSatMap → LookTable → ToneCurve，作用在
    //  **线性 ProPhoto**（本管线的工作空间）上。它们属于相机配置文件，
    //  放在基准白平衡之后、创意白平衡与用户调色之前：
    //   - 基准白平衡完成相机中性化；
    //   - Hue/Sat 表与 Look 表建立厂商的默认色彩风格；
    //   - 色调曲线最后把线性值映射到输出影调；
    //   - 其后的创意偏移 / 曲线 / HSL 仍可覆盖它。
    //
    //  注意：LookTable 必须位于 ToneCurve **之前**。DNG 规范把 2.5D/3D
    //  Look 表定义在曝光之后、色调曲线之前；若放到曲线之后，
    //  表的明度轴会作用在已被曲线重映射的值上，影调与饱和度都会明显偏离。
    // -----------------------------------------------------------------------
    if (dcpActive && dcpProfile != nullptr) {
        if (context.dcp.useHueSatMap) {
            DcpHueSatMap map;
            if (ops::dcpInterpolatedHueSatMap(*dcpProfile,
                                              raw.metadata.asShotTemperature, map)) {
                ops::applyDcpHueSatMap(workingImage, map);
            }
        }
        if (context.dcp.useLookTable && !dcpProfile->lookTable.isEmpty()) {
            // LookTable 属于 step2：负通道先钳制，表后饱和度钳制到 [0,1]，
            // 与 DNG 规范及 RawTherapee 的实现保持一致。
            ops::applyDcpHueSatMap(workingImage, dcpProfile->lookTable, 1.0f, true);
        }
        if (context.dcp.useToneCurve && !dcpProfile->toneCurve.isEmpty()) {
            ops::applyDcpToneCurve(workingImage, dcpProfile->toneCurve);
        }
    }

    // -----------------------------------------------------------------------
    //  第 8 步：创意白平衡偏移（上层调色层）
    //  需求 §2 管线第十项，**明确位于** XYZ→ProPhoto 之后
    //
    //  偏移以基准白点为参考：目标白点 = 基准 + 偏移。
    //  偏移为 0 时保持基准中性；普通位图仍以 5000K 作为中性参考。
    // -----------------------------------------------------------------------
    report.report(RenderStage::WhiteBalance);

    // DCP 接管相机域白平衡后，画面的中性基准就是 as-shot（或用户微调后的基准）；
    // 创意偏移必须相对它计算，否则拖动偏移会先抵消一次 DCP 的白平衡。
    const float dcpNeutralTemperature =
        baseWbActive ? baseWbTemperature : raw.metadata.asShotTemperature;
    const float dcpNeutralTint = baseWbActive ? baseWbTint : raw.metadata.asShotTint;
    const float wbReferenceTemperature = raw.isRgbSource
        ? 5000.0f
        : (dcpWhiteBalanceHandled ? dcpNeutralTemperature : baseWbTemperature);
    const float wbReferenceTint = raw.isRgbSource
        ? 0.0f
        : (dcpWhiteBalanceHandled ? dcpNeutralTint : baseWbTint);
    const float creativeTemperature = std::clamp(
        wbReferenceTemperature + context.whiteBalance.temperatureOffset,
        1000.0f, 40000.0f);
    const float creativeTint = std::clamp(
        wbReferenceTint + context.whiteBalance.tintOffset,
        -100.0f, 100.0f);

    const bool creativeWbApplied = context.whiteBalance.useGrayPick
        || std::fabs(context.whiteBalance.temperatureOffset) > 1e-4f
        || std::fabs(context.whiteBalance.tintOffset) > 1e-4f;

    if (creativeWbApplied) {
        ops::applyWhiteBalance(workingImage,
                               creativeTemperature,
                               creativeTint,
                               context.whiteBalance.grayPickX,
                               context.whiteBalance.grayPickY,
                               context.whiteBalance.useGrayPick,
                               wbReferenceTemperature,
                               wbReferenceTint,
                               // 普通位图沿用旧的相对校正语义；
                               // RAW 已先应用基准增益，需要严格的比例组合。
                               raw.isRgbSource);
    }

    // 局部调整的"相对色温"以实际作用于画面的白点为基准；
    // 没有基准也没有创意偏移时，用 5500K / 5000K 作为中性参考。
    const float neutralWbTemperature = dcpWhiteBalanceHandled
        ? dcpNeutralTemperature
        : (baseWbActive ? baseWbTemperature
                        : (raw.isRgbSource ? 5000.0f : 5500.0f));
    const float neutralWbTint = dcpWhiteBalanceHandled
        ? dcpNeutralTint
        : (baseWbActive ? baseWbTint : 0.0f);
    const float effectiveWbTemperature = creativeWbApplied
        ? creativeTemperature : neutralWbTemperature;
    const float effectiveWbTint = creativeWbApplied
        ? creativeTint : neutralWbTint;

    // 部分饱和高光还原必须放在白平衡**之后**：工作空间里三通道等值才是中性白。
    // 若在相机原始域把通道拉平，由于传感器对白色的原始响应并不相等，
    // 结果会是偏色（实测偏蓝），这也是很多转换器把 clip highlights
    // 放在白平衡之后的原因。
    // 掩码与图像同尺寸；lensfun 与色彩矩阵都是同尺寸就地变换，因此仍然对齐。
    if (!clipMask.empty()
        && clipMask.size() == static_cast<std::size_t>(workingImage.pixelCount())) {
        ops::flattenClippedHighlights(workingImage, clipMask.data());
    }

    // -----------------------------------------------------------------------
    //  第 8.2 步：预览 2x2 合并路径的色度抗混叠
    //
    //  合并时 R / B 各自是 2x2 单元内的**单点采样**（只有 G 做了两点平均），
    //  色度没有抗混叠。幕墙竖肋、百叶窗、织物这类接近 Nyquist 的周期纹理
    //  会让 R / B 的采样格点与纹理周期产生拍频，表现为整片绿 / 洋红彩纹；
    //  suppressFalseColor 只处理孤立伪彩点，命中不了这种成片拍频。
    //  这里对色度做一次半径 1 像素的十字低通，支撑范围与全分辨率路径
    //  "去马赛克 + 2x2 面积平均"的色度响应相当，亮度完全沿用原像素，
    //  因此只去彩纹、不会发糊。
    //
    //  位置：必须在白平衡与高光还原之后——先把被截断的通道拉平，
    //  否则平滑会把饱和高光的单通道值摊到邻域，形成彩色光晕。
    // -----------------------------------------------------------------------
    if (previewBinningApplied) {
        ops::antialiasPreviewChroma(workingImage);
    }

    // -----------------------------------------------------------------------
    //  第 8.5 步：预览影调基准（自动曝光归一 + 可选弱 S 曲线）
    //
    //  放在白平衡之后、用户光影调整之前，含义是：
    //   - 统计基于白平衡后的工作空间亮度，与 CCM / 白平衡标定完全独立；
    //   - 曝光滑块、对比度、曲线随后作用在基准之上，因此可以覆盖自动结果；
    //   - 不改动任何线性原始 RGB 数据，只影响本次渲染输出。
    //
    //  默认只在预览链路开启（RenderOptions::previewTone 由调用方决定），
    //  导出默认 Off，除非用户打开「预览影调同步导出」。
    // -----------------------------------------------------------------------
    if (options.previewTone != PreviewToneMode::Off) {
        report.report(RenderStage::PreviewTone);
        // 统计**必须**基于整幅图：局部渲染时若只统计可见区域，
        // 平移画面会让整体亮度跟着可见内容跳变。
        const float exposureScale = ops::computeAutoExposureScale(workingImage, 0.995f);
        ops::applyPreviewTone(workingImage,
                              exposureScale,
                              options.previewTone == PreviewToneMode::Look);
    }

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

    // -----------------------------------------------------------------------
    //  第 9 步：光影调整（曝光、高光压缩、阴影提升、对比度、去雾）
    //  需求 §2 管线第十一项
    // -----------------------------------------------------------------------
    report.report(RenderStage::LightAdjust);
    ops::applyLightAdjustments(workingImage, context.light);

    // -----------------------------------------------------------------------
    //  第 9.5 步：全局色彩与质感（饱和度 / 自然饱和度 / 清晰度 / 纹理）
    //
    //  放在光影之后、HSL 之前：
    //   - 饱和度 / 自然饱和度是整体色彩基调，HSL 负责在此基础上做分区微调；
    //   - 清晰度 / 纹理只改变亮度结构，HSL 与曲线之后仍可继续调整颜色。
    // -----------------------------------------------------------------------
    report.report(RenderStage::Presence);
    ops::applyPresence(workingImage, context.presence);

    // -----------------------------------------------------------------------
    //  第 10 步：HSL 调色（七通道偏移）
    //  需求 §2 管线第十二项
    // -----------------------------------------------------------------------
    report.report(RenderStage::HslAdjust);
    ops::applyHsl(workingImage, context.hsl);

    // -----------------------------------------------------------------------
    //  第 10.2 步：黑白混色
    //
    //  必须在颜色分级之前：先转成中性灰并按原色相调好各颜色的明度，
    //  再用颜色分级 / 分离色调给黑白画面染色（如棕褐、冷调黑白）。
    // -----------------------------------------------------------------------
    report.report(RenderStage::BlackWhite);
    ops::applyBlackWhite(workingImage, context.blackWhite);

    // -----------------------------------------------------------------------
    //  第 10.5 步：颜色分级 + 分离色调
    //
    //  放在 HSL 之后、曲线之前：
    //   - HSL 先修正各色相对象的本色；
    //   - 颜色分级再按明暗区域建立冷暖分离（阴影偏青、高光偏暖等）；
    //   - 曲线最后统一收口反差。
    // -----------------------------------------------------------------------
    report.report(RenderStage::ColorGrading);
    ops::applyColorGrading(workingImage, context.colorGrading, context.splitToning);

    // -----------------------------------------------------------------------
    //  第 11 步：单通道曲线（R/G/B/L）
    //  需求 §2 管线第十三项
    // -----------------------------------------------------------------------
    report.report(RenderStage::Curves);
    ops::applyCurves(workingImage, context.curve);

    // -----------------------------------------------------------------------
    //  第 12 步：叠加 cube 3D LUT，混合强度控制
    //  需求 §2 管线第十四项
    // -----------------------------------------------------------------------
    if (context.lut.enable) {
        report.report(RenderStage::Lut);
        const ErrorInfo lutError = ops::applyLut(workingImage, context.lut);
        if (!lutError.isOk()) {
            // LUT 无法应用时跳过该阶段，其余管线照常完成
            notify.warn("3D LUT 未应用：" + lutError.message);
        }
    }

    // 细节 / 降噪 / 锐化的半径以原始像素为单位；裁剪只改变输出范围，
    // 不改变像素尺度，因此必须在裁剪前按“当前工作宽度 / 原始宽度”算好。
    const float renderPixelScale = (raw.sourceWidth() > 0)
        ? static_cast<float>(workingImage.width())
            / static_cast<float>(raw.sourceWidth())
        : 1.0f;

    // -----------------------------------------------------------------------
    //  第 12.2 步：蒙版局部调整
    //
    //  放在 LUT 之后、预览裁剪与细节处理之前：
    //   - 局部调整依赖最终色彩结果，颜色分级 / LUT 之后再局部修正更符合直觉；
    //   - 必须在裁剪前执行，直方图采样（第 12.5 步）才能覆盖整幅图；
    //   - 权重图按归一化坐标生成，预览降采样与全尺寸导出一致。
    // -----------------------------------------------------------------------
    report.report(RenderStage::LocalAdjust);
    ops::applyLocalAdjustments(workingImage,
                               context.masks,
                               effectiveWbTemperature,
                               effectiveWbTint,
                               maskCache);

    // -----------------------------------------------------------------------
    //  第 12.2.5 步：污点修复 / 仿制（C4）
    //
    //  放在局部调整之后、裁剪之前：
    //   - 修复取样的是已经过全局调色与局部调整的像素，克隆内容与周围一致；
    //   - 坐标相对透视校正后的图像，裁剪映射由 UI 负责。
    // -----------------------------------------------------------------------
    report.report(RenderStage::Retouch);
    ops::applyRetouch(workingImage, context.retouch);

    // -----------------------------------------------------------------------
    //  第 12.3 步：非破坏性裁剪（C1）
    //
    //  放在局部调整之后、直方图采样与 1:1 预览裁剪之前：
    //   - 蒙版坐标相对原始图像，先应用蒙版再裁剪，裁剪不会移动选区；
    //   - 直方图基于裁剪后的画面统计，与所见一致；
    //   - 后续降噪 / 锐化只处理裁剪后的像素，导出也自然得到裁剪结果。
    // -----------------------------------------------------------------------
    report.report(RenderStage::Crop);
    int userCropLeft = 0;
    int userCropTop = 0;
    const bool cropApplied = ops::applyCrop(
        workingImage, context.crop, &userCropLeft, &userCropTop);

    // -----------------------------------------------------------------------
    //  第 12.5 步：局部预览 —— 1:1 查看时只渲染屏幕可见区域
    //
    //  走到这里，源阶段（去拜耳 → 镜头校正 → 工作色彩空间）已经在**整幅图**
    //  上完成并写入缓存；前面的光影 / HSL / 曲线 / LUT 也都是逐像素运算，
    //  已经算完。剩下的降噪、锐化、色域映射、显示转换占了整幅渲染的大头，
    //  而屏幕上通常只看得到 4600 万像素里的两三百万——
    //  在降噪之前裁一刀，参数调整的每帧耗时降到原来的十分之一左右。
    //
    //  为什么裁在降噪之前而不是更早：直方图的线性采样取自这里，
    //  必须覆盖整幅图，否则拖动曝光滑块时直方图不会跟着变化；而曝光 /
    //  曲线等调整本身都是逐像素运算、速度很快，留在整幅上计算对总耗时
    //  影响很小（实测 4600 万像素约 12ms）。
    //
    //  已知的细微差别：降噪的噪声 σ 取自整个平面的 30% 分位，局部渲染
    //  自然只能用这一块估计，因此与整幅渲染的降噪结果有 ±1（8 位量化）
    //  级别的差异，肉眼不可见。锐化的邻域运算则由下面的外扩边距保证。
    // -----------------------------------------------------------------------
    const int fullRenderWidth = workingImage.width();
    const int fullRenderHeight = workingImage.height();

    // 直方图用的线性域采样（需求 §3 模块 F3：必须是编码前的线性数据）。
    // 在 **1:1 局部裁剪之前**抽取：局部渲染与整幅渲染的直方图由此完全一致，
    // 平移画面或在 fit / 1:1 之间切换时直方图都不会跳动。
    // 用户裁剪（C1）已经在上面完成，因此直方图统计的是裁剪后的最终画面。
    const auto extractLinearSample = [&workingImage, &result]() {
        const int longestSide = std::max(workingImage.width(), workingImage.height());
        if (longestSide > RenderResult::kHistogramSampleSize) {
            const float scale =
                static_cast<float>(RenderResult::kHistogramSampleSize)
                / static_cast<float>(longestSide);
            const int sampleWidth =
                std::max(1, static_cast<int>(workingImage.width() * scale));
            const int sampleHeight =
                std::max(1, static_cast<int>(workingImage.height() * scale));
            ops::resizeImage(workingImage, result.linearSample, sampleWidth, sampleHeight);
        } else {
            // 图不大时直接复制，避免引入插值误差
            result.linearSample = workingImage;
        }
    };

    extractLinearSample();

    if (options.scale == RenderScale::Preview && options.hasPreviewCrop()) {
        // 外扩一圈：降噪的 à trous 三层叠加约 14px、锐化的两级高斯
        // 支撑半径最大 36px，邻域运算需要可见区域之外的像素，
        // 否则边界会出现亮度接缝。
        constexpr int kCropHalo = 48;
        const int left = std::clamp(options.previewCropX - kCropHalo, 0, fullRenderWidth - 1);
        const int top = std::clamp(options.previewCropY - kCropHalo, 0, fullRenderHeight - 1);
        const int right = std::clamp(options.previewCropX + options.previewCropWidth + kCropHalo,
                                     left + 1, fullRenderWidth);
        const int bottom = std::clamp(options.previewCropY + options.previewCropHeight + kCropHalo,
                                      top + 1, fullRenderHeight);

        ImageBuffer cropped;
        ops::cropImage(workingImage, cropped, left, top, right - left, bottom - top);
        workingImage.swap(cropped);

        result.wasCropped = true;
        result.cropX = left;
        result.cropY = top;
        result.cropWidth = workingImage.width();
        result.cropHeight = workingImage.height();
    }

    // -----------------------------------------------------------------------
    //  第 12.9 步：彩纹抑制（CFA 采样格点偏置均衡）
    //
    //  高密度周期纹理（瓦楞板、百叶窗、瓷砖、织物）会让拜耳红 / 绿 / 蓝
    //  采样点分别落在纹理波峰或波谷上，去马赛克后出现成片绿 / 洋红彩纹。
    //  这类误差表现为 2x2 单元四个采样格点之间的色差偏置，而不是逐像素
    //  噪声：按格点统计块均值并摊平就能消除，同时格点内部的真实色彩细节
    //  （树叶、织物的微观纹理主要靠色度承载）原样保留，不会把画面抹糊。
    //
    //  只在图像与拜耳阵列逐像素对齐时启用下面的格点均衡：
    //   - 导出与 1:1 局部渲染都是全分辨率去马赛克，格点语义成立；
    //   - 2x2 合并预览与 RGB 域降采样后，一个像素不再对应单个 CFA 采样点；
    //   - 镜头畸变校正在 RGB 域重采样，同样会打乱采样格点；
    //   - X-Trans 的 6x6 排列不属于 2x2 格点结构。
    //
    //  但后面的低频色度重建只对色差平面做低通，不依赖 CFA 相位，
    //  镜头校正重采样之后依然成立，所以它不受 lensResampled 限制。
    // -----------------------------------------------------------------------
    const bool cropRotated = context.crop.enable
        && std::fabs(context.crop.angle) > 1e-4f;
    const bool cfaAligned = !raw.isRgbSource
        && !raw.bayer.usesGenericCfa()
        && renderPixelScale >= 0.999f && renderPixelScale <= 1.001f
        && !cropRotated;
    const bool lensResampled = (correctionsEnabled && options.applyLensCorrection
                                && context.lens.enable)
        || cropRotated;
    if (cfaAligned) {
        if (!lensResampled) {
            // 裁剪后图像原点相对拜耳阵列有偏移，必须把偏移带进格点判断，
            // 否则 1:1 查看时格点编号会整体错位。
            // 原点 = 用户裁剪偏移 + 1:1 局部渲染偏移（都相对裁剪前工作图）
            const int originX = userCropLeft + (result.wasCropped ? result.cropX : 0);
            const int originY = userCropTop + (result.wasCropped ? result.cropY : 0);
            ops::suppressChromaMoire(workingImage, 1.0f, originX, originY);
        }

        // 格点均衡只能处理与 CFA 相位直接耦合的高频彩纹；比奈奎斯特
        // 稍低的周期纹理（瓦片、幕墙）还会留下周期约 8~16 像素的
        // 绿 / 洋红条纹，需要再做一次低频色度重建。预览合并路径不经过
        // 这里（它有自己的色度抗锯齿）。
        ops::suppressLargeChromaMoire(workingImage, 1.0f);
    }

    // -----------------------------------------------------------------------
    //  第 13 步：降噪（明度降噪、色度降噪）
    //  需求 §2 管线第十五项
    // -----------------------------------------------------------------------
    report.report(RenderStage::NoiseReduction);
    ops::applyNoiseReduction(workingImage,
                             context.detail.noiseLuma,
                             context.detail.noiseChroma,
                             renderPixelScale,
                             context.detail.noiseMethod,
                             context.detail.nlMeansSearchRadius);

    if (cancelFlag.load(std::memory_order_relaxed)) {
        return ErrorInfo::make(ErrorCode::Cancelled, "渲染已取消。");
    }

    // -----------------------------------------------------------------------
    //  第 14 步：USM 锐化，带边缘掩码
    //  需求 §2 管线第十六项
    //  需求 §5.4：所有色彩计算在线性域 —— 锐化确实作用在线性数据上
    // -----------------------------------------------------------------------
    report.report(RenderStage::Sharpen);
    ops::applySharpen(workingImage,
                      context.detail.sharpenAmount,
                      context.detail.sharpenRadius,
                      context.detail.sharpenMasking,
                      renderPixelScale,
                      context.detail.sharpenMethod,
                      context.detail.deconvolutionIterations);

    // 色度平滑只在预览的 2x2 合并路径做过一次（第 8.2 步，亮度保持的
    // 十字低通）。这里不再追加全幅色度平滑：没有针对性的平滑会让颜色发灰。

    // -----------------------------------------------------------------------
    //  第 15 步：色域映射 + 目标色彩空间转换
    //  需求 §2 管线第十七项
    // -----------------------------------------------------------------------
    report.report(RenderStage::GamutMap);

    // 在色域映射与 Gamma 编码**之前**抽取线性域采样，供直方图使用
    // （需求 §3 模块 F3 要求线性域直方图，不能用编码后的数据统计）。
    // 采样已在第 12.5 步、裁剪与降噪之前完成（必须覆盖整幅图）。

    // 色域警告（E4）必须在软压缩之前采样：软膝压缩会把越界值压回 [0,1]，
    // 之后再检测就会漏报。这里只生成 1 字节/像素的掩码，内存开销很小。
    std::vector<std::uint8_t> gamutMask;
    if (options.scale == RenderScale::Preview && options.gamutWarning) {
        const ErrorInfo gamutError = colorManagement->buildGamutMask(
            workingImage, options.gamutSpace, gamutMask);
        if (!gamutError.isOk()) {
            notify.warn("色域警告生成失败：" + gamutError.message);
            gamutMask.clear();
        }
    }

    // 先做软膝色域压缩（把超出范围的值平滑压回 [0,1]）
    ops::applyGamutSoftClip(workingImage);

    // 预览路径：转到显示器空间；导出路径：转到目标色彩空间。
    //
    // 重要：lcms2 的转换**包含目标 profile 的传递函数（TRC）**，
    // 也就是说 Gamma 编码已经在这一步完成。之后再调用 applyGammaEncode
    // 会造成重复编码（画面明显发灰/发亮）。
    bool colorConverted = false;

    if (options.scale == RenderScale::Preview) {
        // 软打样：按需加载打样 ICC。加载失败时退回普通显示器转换，
        // 只在日志里提示，不中断预览。
        SoftProofOptions proofOptions;
        if (options.softProof.isConfigured()) {
            const ColorProfile* proofProfile =
                colorManagement->loadProfileFromFile(options.softProof.profilePath);
            if (proofProfile != nullptr) {
                proofOptions.proofingProfile = proofProfile;
                proofOptions.simulatePaperWhite = options.softProof.simulatePaperWhite;
            } else {
                notify.warn("软打样配置文件加载失败，已退回普通预览："
                            + colorManagement->lastError());
            }
        }

        // 局部渲染时把裁剪偏移传下去：8 位量化抖动用像素绝对坐标生成，
        // 否则平移视口会让同一像素的抖动值变化，出现闪烁。
        const int displayOriginX =
            userCropLeft + (result.wasCropped ? result.cropX : 0);
        const int displayOriginY =
            userCropTop + (result.wasCropped ? result.cropY : 0);

        if (options.highBitDepthDisplay) {
            // E1：16 位显示缓冲，供 10-bit 画布使用
            std::vector<std::uint16_t> display16;
            const ErrorInfo convertError =
                colorManagement->convertWorkingToDisplay16(
                    workingImage, display16, proofOptions,
                    displayOriginX, displayOriginY);
            if (convertError.isOk()) {
                if (!gamutMask.empty()) {
                    colorManagement->overlayGamutMask16(
                        display16, gamutMask,
                        options.gamutWarningR, options.gamutWarningG,
                        options.gamutWarningB);
                }
                result.display16 = std::move(display16);
                colorConverted = true;
            } else {
                notify.warn("显示器高色深转换失败，预览已按 sRGB 处理："
                            + convertError.message);
            }
        } else {
            // 预览直接产出 8 位显示数据：不生成浮点显示图，
            // 界面层拿到即可上屏（省掉约 550MB 的浮点往返）。
            std::vector<std::uint8_t> display;
            const ErrorInfo convertError =
                colorManagement->convertWorkingToDisplay8(
                    workingImage, display, proofOptions,
                    displayOriginX, displayOriginY);
            if (convertError.isOk()) {
                if (!gamutMask.empty()) {
                    colorManagement->overlayGamutMask8(
                        display, gamutMask,
                        options.gamutWarningR, options.gamutWarningG,
                        options.gamutWarningB);
                }
                result.display8 = std::move(display);
                colorConverted = true;
            } else {
                notify.warn("显示器色彩转换失败，预览已按 sRGB 处理："
                            + convertError.message);
            }
        }
    } else {
        ImageBuffer outputImage;
        const ErrorInfo convertError = colorManagement->convertWorkingToOutput(
            workingImage, options.outputSpace, outputImage);

        if (convertError.isOk()) {
            workingImage.swap(outputImage);
            colorConverted = true;
        } else {
            notify.warn("目标色彩空间转换失败，已按 "
                        + std::string(colorSpaceName(options.outputSpace)) + " 编码输出："
                        + convertError.message);
        }
    }

    // -----------------------------------------------------------------------
    //  第 16 步：Gamma 编码
    //  需求 §2 管线第十八项、§1.3.1：这是管线中唯一做 gamma 编码的位置
    //
    //  只有**色彩转换失败**时才走到这里：此时数据仍是线性工作空间的值，
    //  需要手工补一次 Gamma 编码才能写出可用的图像。
    //  转换成功的情况下 lcms2 已经完成编码，不做任何操作。
    // -----------------------------------------------------------------------
    report.report(RenderStage::GammaEncode);

    if (!colorConverted) {
        ops::applyGammaEncode(workingImage, options.scale == RenderScale::Preview
                                                 ? ColorSpace::sRGB
                                                 : options.gammaSpace);

        // 预览的色彩转换失败时，退回到"浮点数据 + 手工量化"的显示通路，
        // 保证界面仍能显示图像，只是少了显示器 profile 适配。
        if (options.scale == RenderScale::Preview) {
            const std::size_t samples = workingImage.sampleCount();
            const float* src = workingImage.data();
            if (options.highBitDepthDisplay) {
                result.display16.resize(samples);
                std::uint16_t* dst = result.display16.data();
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
                for (long long i = 0; i < static_cast<long long>(samples); ++i) {
                    const float v = std::clamp(src[i], 0.0f, 1.0f);
                    dst[i] = static_cast<std::uint16_t>(v * 65535.0f + 0.5f);
                }
            } else {
                result.display8.resize(samples);
                std::uint8_t* dst = result.display8.data();
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
                for (long long i = 0; i < static_cast<long long>(samples); ++i) {
                    const float v = std::clamp(src[i], 0.0f, 1.0f);
                    dst[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    //  收尾
    // -----------------------------------------------------------------------
    report.report(RenderStage::Finished);

    // 局部渲染时 workingImage 只是整幅图的一块，界面仍需要整幅尺寸
    // 来解释缩放比例与贴回区域，因此报裁剪前记录的值。
    result.renderWidth = fullRenderWidth;
    result.renderHeight = fullRenderHeight;

    // 未降采样尺寸用于界面解释"1:1"。
    //  - 未裁剪：拜耳数据尺寸就是未降采样尺寸；
    //  - 用户裁剪：用裁剪后的工作宽度除以渲染像素比例还原到原始像素尺度。
    if (cropApplied) {
        const float scale = std::max(renderPixelScale, 1e-6f);
        result.fullWidth = std::max(
            1, static_cast<int>(std::lround(fullRenderWidth / scale)));
        result.fullHeight = std::max(
            1, static_cast<int>(std::lround(fullRenderHeight / scale)));
    } else {
        result.fullWidth = raw.sourceWidth();
        result.fullHeight = raw.sourceHeight();
    }

    if (options.scale == RenderScale::Preview) {
        // 预览结果已经以 8 位形式交给界面，浮点工作缓冲不再需要，
        // 立即释放以降低内存峰值（4600 万像素约 550MB）
        workingImage.release();
    } else {
        result.image.swap(workingImage);
    }

    return ErrorInfo::ok();
}

}  // namespace

std::unique_ptr<IRenderBackend> createCpuRenderBackend()
{
    return std::make_unique<CpuRenderBackend>();
}

}  // namespace rawcore
