// ============================================================================
//  test_pipeline.cpp —— 管线冒烟测试（仅用于开发期验证，不参与正式构建）
//
//  目的：在没有 rawloader/lcms2 真实实现的环境下，构造合成拜耳数据，
//  跑通整条渲染管线，验证：
//    - 各阶段不会崩溃、不会产生 NaN/Inf
//    - 输出尺寸正确
//    - 预览降采样与全尺寸两条路径都可用
//    - 各种参数组合（CCM、白平衡、曲线、降噪、锐化）都能正常执行
//
//  说明：lcms2 为桩实现，色彩转换不会真正发生，因此这里只做"数值合法性"
//  检查，不做色彩正确性断言。
// ============================================================================

#include "core/EditContext.h"
#include "core/PipelineRenderer.h"
#include "core/RawLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace rawcore;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    } else {
        std::printf("  [ ok ] %s\n", what);
    }
}

/// 构造一张合成的拜耳图。
///
/// 关键：每个像素的取值必须**按它所属的滤镜通道**生成，
/// 否则整幅图接近灰度、各去拜耳算法的差异会被抹平，测试也就失去意义。
/// 这里构造一个"红→绿→蓝"的水平渐变场景：
///   R 通道：左侧强
///   G 通道：中间强
///   B 通道：右侧强
/// 再叠加细条纹用于检验插值细节。
BayerImage makeSyntheticBayer(int width, int height, BayerPattern pattern)
{
    BayerImage bayer;
    bayer.width = width;
    bayer.height = height;
    bayer.pattern = pattern;
    bayer.data.resize(static_cast<std::size_t>(width) * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);

            // 每个通道在不同位置达到峰值，形成彩色渐变
            float channelValue = 0.0f;
            const int channel = bayerChannelAt(pattern, x, y);
            switch (channel) {
            case 0:  // R：左侧强
                channelValue = 0.80f * (1.0f - u) + 0.05f;
                break;
            case 1:  // G：中间强
                channelValue = 0.75f * (1.0f - std::fabs(u - 0.5f) * 2.0f) + 0.10f;
                break;
            default: // B：右侧强
                channelValue = 0.80f * u + 0.05f;
                break;
            }

            // 叠加细条纹与轻微纵向渐变，制造高频细节
            const float stripe = ((x / 3) % 2 == 0) ? 0.05f : -0.05f;
            const float value = channelValue * (0.9f + 0.1f * v) + stripe;

            bayer.data[static_cast<std::size_t>(y) * width + x] =
                std::max(0.0f, std::min(1.0f, value));
        }
    }
    return bayer;
}

RawDecodeResult makeRawResult(int width, int height)
{
    RawDecodeResult raw;
    raw.bayer = makeSyntheticBayer(width, height, BayerPattern::RGGB);
    raw.detectedPattern = BayerPattern::RGGB;
    raw.whiteLevel = 1.0f;   // 数据已在 0~1，白电平设为 1
    raw.blackLevel = {{0.0f, 0.0f, 0.0f, 0.0f}};
    raw.hasCameraMatrix = true;

    // 一个合理的相机 RGB → XYZ(D50) 矩阵
    raw.cameraToXyz = Matrix3x3{{
        {{ 0.4124f, 0.3576f, 0.1805f}},
        {{ 0.2126f, 0.7152f, 0.0722f}},
        {{ 0.0193f, 0.1192f, 0.9505f}},
    }};

    raw.metadata.width = width;
    raw.metadata.height = height;
    raw.metadata.cameraMake = "TestMake";
    raw.metadata.cameraModel = "TestModel";
    raw.metadata.lensModel = "TestLens";
    raw.metadata.focalLength = 50.0f;
    raw.metadata.aperture = 5.6f;
    return raw;
}

/// 检查图像中所有采样值是否有限
bool allFinite(const ImageBuffer& image)
{
    if (image.isEmpty()) {
        return false;
    }
    const float* data = image.data();
    for (std::size_t i = 0; i < image.sampleCount(); ++i) {
        if (!std::isfinite(data[i])) {
            std::printf("    非有限值出现在索引 %zu\n", i);
            return false;
        }
    }
    return true;
}

void printStats(const char* label, const ImageBuffer& image)
{
    if (image.isEmpty()) {
        std::printf("    %s: 空图像\n", label);
        return;
    }
    double sum = 0.0;
    float minValue = 1e9f;
    float maxValue = -1e9f;
    const float* data = image.data();
    for (std::size_t i = 0; i < image.sampleCount(); ++i) {
        sum += data[i];
        minValue = std::min(minValue, data[i]);
        maxValue = std::max(maxValue, data[i]);
    }
    const double mean = sum / static_cast<double>(image.sampleCount());
    std::printf("    %s: %dx%d  均值=%.4f  范围=[%.4f, %.4f]\n",
                label, image.width(), image.height(), mean, minValue, maxValue);
}

/// 跑一次渲染并检查基本不变量
void runCase(const char* name, const RawDecodeResult& raw,
             const EditContext& context, const RenderOptions& options)
{
    std::printf("\n[用例] %s\n", name);

    PipelineRenderer renderer;
    RenderResult result;

    const auto start = std::chrono::steady_clock::now();
    const ErrorInfo error = renderer.render(raw, context, options, result);
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start).count();

    check(error.isOk(), "渲染返回成功");
    if (!error.isOk()) {
        std::printf("    错误: %s | %s\n", error.message.c_str(), error.detail.c_str());
        return;
    }

    check(result.isValid(), "输出图像非空");

    // 预览走"8 位显示数据直出"通路（省去一份全幅浮点显示缓冲），
    // 全尺寸仍输出浮点图像供导出使用
    if (options.scale == RenderScale::Preview) {
        check(!result.display8.empty(), "预览输出 8 位显示数据");
        check(result.display8.size()
                  == static_cast<std::size_t>(result.renderWidth)
                         * static_cast<std::size_t>(result.renderHeight) * 3u,
              "预览 8 位数据尺寸与渲染尺寸一致");
        check(result.renderWidth > 0 && result.renderHeight > 0,
              "预览渲染尺寸有效");
    } else {
        check(allFinite(result.image), "输出全部为有限值");
    }

    // 尺寸检查：预览允许降采样，全尺寸必须与原图一致
    if (options.scale == RenderScale::FullSize) {
        check(result.image.width() == raw.bayer.width,
              "全尺寸宽度与原图一致");
        check(result.image.height() == raw.bayer.height,
              "全尺寸高度与原图一致");
    }

    check(!result.linearSample.isEmpty(),
          "直方图线性采样存在（需求 §3 模块 F3）");
    check(allFinite(result.linearSample), "直方图采样全部为有限值");

    printStats("输出", result.image);
    if (!result.display8.empty()) {
        std::printf("    预览 8 位输出: %dx%d (%zu 字节)\n", result.renderWidth,
                    result.renderHeight, result.display8.size());
    }
    printStats("直方图采样", result.linearSample);
    std::printf("    耗时: %.1f ms\n", ms);
}

}  // namespace

int main()
{
    std::printf("==================== 管线冒烟测试 ====================\n");

    // 使用较小尺寸以加快测试；同时验证非偶数尺寸的鲁棒性
    const int width = 257;
    const int height = 193;
    const RawDecodeResult raw = makeRawResult(width, height);

    // ---- 用例 1：默认参数 + 全尺寸 ----
    {
        EditContext context;
        RenderOptions options;
        options.scale = RenderScale::FullSize;
        options.outputSpace = ColorSpace::sRGB;
        runCase("默认参数（全尺寸）", raw, context, options);
    }

    // ---- 用例 1.5：蒙版局部调整真正进入管线（B1）----
    {
        RenderOptions options;
        options.scale = RenderScale::FullSize;
        options.outputSpace = ColorSpace::sRGB;

        EditContext baseContext;
        PipelineRenderer baseRenderer;
        RenderResult baseResult;
        const ErrorInfo baseError =
            baseRenderer.render(raw, baseContext, options, baseResult);
        check(baseError.isOk() && !baseResult.image.isEmpty(),
              "基线渲染成功（用于蒙版对比）");

        EditContext maskedContext;
        maskedContext.masks.enable = true;
        MaskLayer layer;
        layer.shape = MaskShape::Radial;
        layer.centerX = 0.5f;
        layer.centerY = 0.5f;
        layer.radiusX = 0.25f;
        layer.radiusY = 0.25f;
        layer.feather = 0.0f;
        layer.adjustments.exposure = 2.0f;
        maskedContext.masks.layers.push_back(layer);

        PipelineRenderer maskedRenderer;
        RenderResult maskedResult;
        const ErrorInfo maskedError =
            maskedRenderer.render(raw, maskedContext, options, maskedResult);
        check(maskedError.isOk() && !maskedResult.image.isEmpty(),
              "带蒙版局部调整的渲染成功");

        if (baseError.isOk() && maskedError.isOk()
            && !baseResult.image.isEmpty() && !maskedResult.image.isEmpty()) {
            const auto luminanceAt = [](const ImageBuffer& image, int x, int y) {
                return (image.at(x, y, 0) + image.at(x, y, 1) + image.at(x, y, 2))
                    / 3.0f;
            };

            const int centerX = width / 2;
            const int centerY = height / 2;
            const float baseCenter =
                luminanceAt(baseResult.image, centerX, centerY);
            const float maskedCenter =
                luminanceAt(maskedResult.image, centerX, centerY);
            const float baseCorner = luminanceAt(baseResult.image, 2, 2);
            const float maskedCorner = luminanceAt(maskedResult.image, 2, 2);

            check(maskedCenter > baseCenter * 1.2f,
                  "蒙版局部曝光在完整管线中生效");
            check(std::fabs(maskedCorner - baseCorner) < 0.01f,
                  "蒙版外像素在完整管线中保持不变");
        }
    }

    // ---- 用例 1.6：非破坏性裁剪（C1）----
    {
        EditContext context;
        context.crop.enable = true;
        context.crop.x = 0.25f;
        context.crop.y = 0.25f;
        context.crop.width = 0.50f;
        context.crop.height = 0.50f;

        RenderOptions options;
        options.scale = RenderScale::FullSize;
        options.outputSpace = ColorSpace::sRGB;

        PipelineRenderer renderer;
        RenderResult result;
        const ErrorInfo error = renderer.render(raw, context, options, result);
        check(error.isOk() && !result.image.isEmpty(), "裁剪渲染成功");

        // 与 applyCrop 相同的 floor / ceil 取整规则
        const int expectedWidth = static_cast<int>(
            std::ceil(0.75 * width)) - static_cast<int>(std::floor(0.25 * width));
        const int expectedHeight = static_cast<int>(
            std::ceil(0.75 * height)) - static_cast<int>(std::floor(0.25 * height));

        check(result.image.width() == expectedWidth
                  && result.image.height() == expectedHeight,
              "裁剪输出尺寸正确");
        check(result.fullWidth == expectedWidth
                  && result.fullHeight == expectedHeight,
              "裁剪后 1:1 基准尺寸为裁剪尺寸");
    }

    // ---- 用例 1.7：多级源阶段缓存（G2/G3）----
    //
    // 用一个更大的合成图，让 256 / 512 两种预览尺寸落在不同的降采样倍数
    // （4 与 2）。257px 的小图受"长边低于 256 不再继续降采样"规则影响，
    // 所有预览尺寸都会收敛到倍数 2，无法验证多级缓存。
    {
        const RawDecodeResult bigRaw = makeRawResult(1024, 768);

        PipelineRenderer renderer;
        RenderOptions options;
        options.scale = RenderScale::Preview;
        options.previewMaxDimension = 256;

        RenderResult smallResult;
        check(renderer.render(bigRaw, EditContext{}, options, smallResult).isOk(),
              "多级缓存：256px 预览渲染成功");

        options.previewMaxDimension = 512;
        RenderResult mediumResult;
        check(renderer.render(bigRaw, EditContext{}, options, mediumResult).isOk(),
              "多级缓存：512px 预览渲染成功");

        check(renderer.baseCacheEntryCount() >= 2,
              "多级缓存保留不同降采样层级");
        check(renderer.baseCacheBytes() <= 768ull * 1024ull * 1024ull,
              "源阶段缓存不超过内存预算");

        renderer.invalidateBaseCache();
        check(renderer.baseCacheEntryCount() == 0
                  && renderer.baseCacheBytes() == 0,
              "清理后源阶段缓存为空");
    }

    // ---- 用例 2：预览降采样 ----
    {
        EditContext context;
        RenderOptions options;
        options.scale = RenderScale::Preview;
        options.previewMaxDimension = 64;  // 强制降采样
        runCase("预览降采样", raw, context, options);
    }

    // ---- 用例 3：全功能参数组合 ----
    {
        EditContext context;
        context.baseWhiteBalance.enable = true;
        context.baseWhiteBalance.temperature = 4200.0f;
        context.baseWhiteBalance.tint = 12.0f;
        context.whiteBalance.temperatureOffset = 300.0f;
        context.whiteBalance.tintOffset = -5.0f;

        context.light.exposure = 0.7f;
        context.light.highlight = -40.0f;
        context.light.shadow = 55.0f;
        context.light.contrast = 30.0f;
        context.light.dehaze = 25.0f;

        context.hsl.bands[0].hue = 15.0f;         // 红
        context.hsl.bands[3].saturation = -30.0f; // 绿
        context.hsl.bands[5].luminance = 20.0f;   // 蓝

        context.curve.red.points = {{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}};
        context.curve.luminance.points = {{0.0f, 0.02f}, {0.85f, 0.9f}, {1.0f, 1.0f}};

        context.detail.noiseLuma = 30.0f;
        context.detail.noiseChroma = 45.0f;
        context.detail.sharpenAmount = 50.0f;
        context.detail.sharpenRadius = 1.5f;
        context.detail.sharpenMasking = 30.0f;

        context.raw.demosaicAlgorithm = DemosaicAlgorithm::VNG4;
        context.raw.demosaicSmooth = 0.3f;

        RenderOptions options;
        options.scale = RenderScale::FullSize;
        runCase("全功能参数组合", raw, context, options);
    }

    // ---- 用例 4：启用拜耳域 CCM ----
    {
        EditContext context;
        context.ccm.enable = true;
        context.ccm.cam2xyz = Matrix3x3{{
            {{ 1.10f, -0.06f, -0.04f}},
            {{-0.05f,  1.08f, -0.03f}},
            {{-0.02f, -0.07f,  1.09f}},
        }};

        RenderOptions options;
        options.scale = RenderScale::FullSize;
        runCase("启用拜耳域自定义 CCM", raw, context, options);
    }

    // ---- 用例 5：非法 CCM（应回退并给出警告）----
    {
        EditContext context;
        context.ccm.enable = true;
        context.ccm.cam2xyz = Matrix3x3{{
            {{ 0.0f, 0.0f, 0.0f}},
            {{ 0.0f, 0.0f, 0.0f}},
            {{ 0.0f, 0.0f, 0.0f}},
        }};

        RenderOptions options;
        options.scale = RenderScale::FullSize;
        runCase("非法 CCM 矩阵（应回退）", raw, context, options);

        PipelineRenderer renderer;
        RenderResult result;
        EditContext fallbackContext = context;
        renderer.render(raw, fallbackContext, options, result);
        check(!result.warnings.empty(), "非法矩阵产生了警告（需求 §6）");
        for (const auto& warning : result.warnings) {
            std::printf("    警告: %s\n", warning.c_str());
        }
    }

    // ---- 用例 6：灰点拾取 ----
    {
        EditContext context;
        context.whiteBalance.useGrayPick = true;
        context.whiteBalance.grayPickX = 0.5f;
        context.whiteBalance.grayPickY = 0.5f;

        RenderOptions options;
        options.scale = RenderScale::FullSize;
        runCase("灰点拾取白平衡", raw, context, options);
    }

    // ---- 用例 7：各种去拜耳算法 ----
    {
        const DemosaicAlgorithm algorithms[] = {
            DemosaicAlgorithm::Bilinear,
            DemosaicAlgorithm::VNG4,
            DemosaicAlgorithm::AMAZE,
            DemosaicAlgorithm::LMMSE,
        };
        for (DemosaicAlgorithm algorithm : algorithms) {
            EditContext context;
            context.raw.demosaicAlgorithm = algorithm;

            RenderOptions options;
            options.scale = RenderScale::FullSize;

            char label[128];
            std::snprintf(label, sizeof(label), "去拜耳算法：%s",
                          demosaicAlgorithmName(algorithm));
            runCase(label, raw, context, options);
        }
    }

    // ---- 用例 8：取消与错误路径 ----
    {
        std::printf("\n[用例] 空 RAW 数据应返回错误\n");
        PipelineRenderer renderer;
        RenderResult result;
        RawDecodeResult emptyRaw;
        EditContext context;
        RenderOptions options;
        const ErrorInfo error = renderer.render(emptyRaw, context, options, result);
        check(!error.isOk(), "空 RAW 返回错误而非崩溃");
        std::printf("    错误信息: %s\n", error.message.c_str());
    }

    // ---- 用例 9：普通位图（线性 sRGB）作为源 ----
    //
    // 验证 RGB 源分支：sRGB → ProPhoto(D50) 之后中性灰仍保持中性，
    // 高饱和色不会串通道；预览降采样走 RGB 域面积平均。
    {
        std::printf("\n[用例] 普通位图源（线性 sRGB）\n");

        const int rgbWidth = 64;
        const int rgbHeight = 48;
        RawDecodeResult rgbRaw;
        rgbRaw.isRgbSource = true;
        rgbRaw.rgbLinear.allocate(rgbWidth, rgbHeight);
        for (int y = 0; y < rgbHeight; ++y) {
            for (int x = 0; x < rgbWidth; ++x) {
                float* pixel = rgbRaw.rgbLinear.data()
                    + (static_cast<std::size_t>(y) * rgbWidth + x) * 3u;
                if (x < rgbWidth / 2) {
                    // 左半：中性灰（线性 0.2）
                    pixel[0] = pixel[1] = pixel[2] = 0.2f;
                } else {
                    // 右半：饱和红
                    pixel[0] = 1.0f;
                    pixel[1] = 0.0f;
                    pixel[2] = 0.0f;
                }
            }
        }
        rgbRaw.metadata.width = rgbWidth;
        rgbRaw.metadata.height = rgbHeight;

        EditContext rgbContext;
        // 普通位图的白平衡初值：D50 中性位置，增益约等于 1
        rgbContext.whiteBalance.temperatureOffset = 0.0f;
        rgbContext.whiteBalance.tintOffset = 0.0f;

        RenderOptions rgbOptions;
        rgbOptions.scale = RenderScale::FullSize;
        rgbOptions.outputSpace = ColorSpace::sRGB;
        rgbOptions.applyLensCorrection = false;

        PipelineRenderer rgbRenderer;
        RenderResult rgbResult;
        const ErrorInfo rgbError =
            rgbRenderer.render(rgbRaw, rgbContext, rgbOptions, rgbResult);
        check(rgbError.isOk(), "RGB 源渲染成功");
        check(rgbResult.image.width() == rgbWidth
                  && rgbResult.image.height() == rgbHeight,
              "RGB 源全尺寸尺寸正确");
        check(rgbResult.fullWidth == rgbWidth && rgbResult.fullHeight == rgbHeight,
              "RGB 源报告的原图尺寸正确");
        check(allFinite(rgbResult.image), "RGB 源输出全部为有限值");

        if (rgbError.isOk() && !rgbResult.image.isEmpty()) {
            const float neutralA = rgbResult.image.at(8, 8, 0);
            const float neutralB = rgbResult.image.at(8, 8, 1);
            const float neutralC = rgbResult.image.at(8, 8, 2);
            const float spread =
                std::max({neutralA, neutralB, neutralC})
                - std::min({neutralA, neutralB, neutralC});
            std::printf("    中性灰输出: %.4f %.4f %.4f（通道差 %.5f）\n",
                        neutralA, neutralB, neutralC, spread);
            check(spread < 0.01f, "中性灰经 sRGB→ProPhoto 转换后保持中性");

            const float redR = rgbResult.image.at(rgbWidth - 9, 8, 0);
            const float redG = rgbResult.image.at(rgbWidth - 9, 8, 1);
            const float redB = rgbResult.image.at(rgbWidth - 9, 8, 2);
            check(redR > redG && redR > redB, "饱和红仍以红通道为主");
        }

        // 预览降采样：64 长边缩到 16，倍数 4 → 16×12
        RenderOptions previewOptions;
        previewOptions.scale = RenderScale::Preview;
        previewOptions.previewMaxDimension = 16;
        previewOptions.applyLensCorrection = false;

        RenderResult rgbPreview;
        const ErrorInfo previewError =
            rgbRenderer.render(rgbRaw, rgbContext, previewOptions, rgbPreview);
        check(previewError.isOk(), "RGB 源预览渲染成功");
        check(rgbPreview.wasDownsampled, "RGB 源预览发生了降采样");
        check(rgbPreview.renderWidth == 16 && rgbPreview.renderHeight == 12,
              "RGB 源预览尺寸按整数倍数缩小");
        check(rgbPreview.fullWidth == rgbWidth && rgbPreview.fullHeight == rgbHeight,
              "RGB 源预览仍报告原图尺寸");
    }

    std::printf("\n====================================================\n");
    if (g_failures == 0) {
        std::printf("全部检查通过\n");
        return 0;
    }
    std::printf("有 %d 项检查失败\n", g_failures);
    return 1;
}
