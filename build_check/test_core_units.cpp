// ============================================================================
//  test_core_units.cpp —— 核心算法单元测试（需求 §7 测试要求）
//
//  覆盖范围：
//    - 色温 ↔ 色度坐标往返（LinearMath）
//    - 白点色度计算与 tint 偏移方向
//    - 曲线 LUT：恒等曲线、单调性、端点、非法输入
//    - CCM 矩阵校验：合法 / 奇异 / 负行列式 / 超限 / 非有限值
//    - CCM 色卡最小二乘求解：精确数据应还原原矩阵、退化数据应报错
//    - CCM CSV 往返
//    - EditContext 恢复默认与“是否为默认状态”判断
//    - 预览 2x2 合并（四种拜耳排列）与 RGB 面积平均降采样
//    - 伪彩抑制：孤立色点被纠正、大面积色彩与粗边缘不受影响
//    - 部分饱和高光还原：亮部转中性白、深色物体不被洗白
//    - 预览影调：99.5% 分位自动曝光、弱 S 曲线、高光不被硬截断
//
//  说明：本测试只依赖 rawcore，不需要 Qt，也不接触真实 RAW 文件，
//  因此可以在任何装了 C++ 编译器的环境下运行（见 build_check/run_test.bat）。
//  通过返回 0，失败返回 1，便于接入 CTest。
// ============================================================================

#include "core/ColorCalibration.h"
#include "core/ColorManagement.h"
#include "core/DcpApply.h"
#include "core/DcpProfile.h"
#include "core/EditContext.h"
#include "core/FrameCalibration.h"
#include "core/LensCorrection.h"
#include "core/LinearMath.h"
#include "core/MaskModel.h"
#include "core/PipelineOps.h"
#include "core/PipelineRenderer.h"
#include "core/PipelineStages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace rawcore;

// compileCurveLut 位于 rawcore::ops 命名空间
using rawcore::ops::compileCurveLut;

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

/// 浮点近似比较
bool near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

/// 矩阵逐元素近似比较
bool matricesNear(const Matrix3x3& a, const Matrix3x3& b, float eps)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (!near(a[r][c], b[r][c], eps)) {
                std::printf("    矩阵元素 [%d][%d] 不一致：%.6f vs %.6f\n",
                            r, c, a[r][c], b[r][c]);
                return false;
            }
        }
    }
    return true;
}

// ===========================================================================
//  1. 色温 ↔ 色度坐标
// ===========================================================================

void testTemperatureRoundTrip()
{
    std::printf("\n[测试] 色温 ↔ 色度坐标往返\n");

    // McCamy 反解在 D 系列白点附近精度较好，两端略差；
    // 这里要求相对误差不超过 5%，超出则说明公式或系数有问题。
    const float temperatures[] = {2000.0f, 3000.0f, 4000.0f,
                                  5500.0f, 6500.0f, 8000.0f,
                                  10000.0f, 12000.0f};

    bool allWithin = true;
    float worstRelativeError = 0.0f;
    for (float kelvin : temperatures) {
        float x = 0.0f;
        float y = 0.0f;
        math::temperatureToXy(kelvin, x, y);

        const float back = math::xyToTemperature(x, y);
        if (back <= 0.0f) {
            std::printf("    %6.0fK 反解失败\n", kelvin);
            allWithin = false;
            continue;
        }

        const float relativeError = std::fabs(back - kelvin) / kelvin;
        worstRelativeError = std::max(worstRelativeError, relativeError);
        if (relativeError > 0.05f) {
            std::printf("    %6.0fK → %.0fK（相对误差 %.1f%%）\n",
                        kelvin, back, relativeError * 100.0f);
            allWithin = false;
        }
    }
    std::printf("    最大相对误差 %.2f%%\n",
                static_cast<double>(worstRelativeError) * 100.0);
    check(allWithin, "各色温往返误差在 5% 以内");

    // 物理合理性：低色温偏红（x 更大），高色温偏蓝
    float xWarm = 0.0f;
    float yWarm = 0.0f;
    float xCool = 0.0f;
    float yCool = 0.0f;
    math::temperatureToXy(3000.0f, xWarm, yWarm);
    math::temperatureToXy(9000.0f, xCool, yCool);
    check(xWarm > xCool, "低色温的色度 x 大于高色温（偏暖方向正确）");
}

void testWhitePointFromTemperatureTint()
{
    std::printf("\n[测试] 白点色度计算\n");

    float xyz[3] = {0.0f, 0.0f, 0.0f};
    math::whitePointFromTemperatureTint(5500.0f, 0.0f, xyz);

    check(near(xyz[1], 1.0f, 1e-5f), "Y 分量归一化为 1");
    check(xyz[2] > 0.0f, "Z 分量为正");

    float x = 0.0f;
    float y = 0.0f;
    math::temperatureToXy(5500.0f, x, y);
    check(near(xyz[0], x / y, 1e-4f), "X = x / y");

    // tint 正值应向 +x（品红）方向偏移
    float xyzMagenta[3] = {0.0f, 0.0f, 0.0f};
    math::whitePointFromTemperatureTint(5500.0f, 50.0f, xyzMagenta);
    check(xyzMagenta[0] > xyz[0], "正 tint 使白点向品红方向移动");

    // 越界 tint 应被钳制，不应产生失控偏移
    float xyzClamped[3] = {0.0f, 0.0f, 0.0f};
    math::whitePointFromTemperatureTint(5500.0f, 1000.0f, xyzClamped);
    check(std::isfinite(xyzClamped[0]) && std::isfinite(xyzClamped[2]),
          "越界 tint 仍产生有限白点");
}

// ===========================================================================
//  2. 曲线 LUT
// ===========================================================================

void testCurveLut()
{
    std::printf("\n[测试] 曲线 LUT\n");

    const int size = 1025;

    // ---- 恒等曲线 ----
    {
        const std::vector<CurvePoint> points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        const std::vector<float> lut = compileCurveLut(points, size);
        check(static_cast<int>(lut.size()) == size, "恒等曲线 LUT 尺寸正确");

        float maxError = 0.0f;
        for (int i = 0; i < size; ++i) {
            const float expected = static_cast<float>(i) / static_cast<float>(size - 1);
            maxError = std::max(maxError, std::fabs(lut[static_cast<std::size_t>(i)] - expected));
        }
        check(maxError < 1e-4f, "恒等曲线输出等于输入");
    }

    // ---- S 形曲线：应单调递增，且端点精确 ----
    {
        const std::vector<CurvePoint> points = {
            {0.0f, 0.0f}, {0.25f, 0.18f}, {0.5f, 0.5f},
            {0.75f, 0.82f}, {1.0f, 1.0f},
        };
        const std::vector<float> lut = compileCurveLut(points, size);
        check(!lut.empty(), "S 形曲线 LUT 非空");

        bool monotonic = true;
        for (std::size_t i = 1; i < lut.size(); ++i) {
            if (lut[i] + 1e-6f < lut[i - 1]) {
                monotonic = false;
                std::printf("    非单调出现在索引 %zu：%.6f → %.6f\n",
                            i - 1, lut[i - 1], lut[i]);
                break;
            }
        }
        check(monotonic, "S 形曲线 LUT 保持单调（PCHIP 无过冲）");
        check(near(lut.front(), 0.0f, 1e-6f), "左端点精确为 0");
        check(near(lut.back(), 1.0f, 1e-6f), "右端点精确为 1");
    }

    // ---- 控制点乱序：内部应先排序，结果仍单调 ----
    {
        const std::vector<CurvePoint> points = {
            {1.0f, 1.0f}, {0.0f, 0.0f}, {0.5f, 0.7f},
        };
        const std::vector<float> lut = compileCurveLut(points, size);
        bool monotonic = !lut.empty();
        for (std::size_t i = 1; i < lut.size(); ++i) {
            if (lut[i] + 1e-6f < lut[i - 1]) {
                monotonic = false;
                break;
            }
        }
        check(monotonic, "乱序控制点排序后 LUT 仍单调");
        check(!lut.empty() && near(lut[static_cast<std::size_t>(size / 2)], 0.7f, 0.02f),
              "中点数值符合控制点定义");
    }

    // ---- 非法输入 ----
    check(compileCurveLut({{0.0f, 0.0f}}, 256).empty(), "单控制点返回空 LUT");
    check(compileCurveLut({{0.0f, 0.0f}, {1.0f, 1.0f}}, 1).empty(),
          "LUT 尺寸过小时返回空");
    check(compileCurveLut({}, 256).empty(), "空控制点返回空 LUT");
}

// ===========================================================================
//  3. CCM 校验
// ===========================================================================

void testParametricCurve()
{
    std::printf("\n[测试] 参数曲线（A6）\n");

    const auto makeGray = [](float value) {
        ImageBuffer image(8, 8);
        image.fill(value);
        return image;
    };

    // 恒等
    {
        ImageBuffer image = makeGray(0.18f);
        ImageBuffer copy = image;
        const ParametricCurveSettings defaults;
        check(defaults.isIdentity(), "默认参数曲线为恒等");
        ops::applyParametricCurve(copy, defaults);
        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "参数曲线恒等时逐位不变");
    }

    // 阴影：提亮暗部，对高光影响很小
    {
        ImageBuffer dark = makeGray(0.03f);
        ImageBuffer bright = makeGray(0.80f);
        ParametricCurveSettings settings;
        settings.shadows = 100.0f;

        ops::applyParametricCurve(dark, settings);
        ops::applyParametricCurve(bright, settings);

        check(dark.at(0, 0, 0) > 0.05f, "阴影 +100 提亮暗部");
        check(std::fabs(bright.at(0, 0, 0) - 0.80f) < 0.01f,
              "阴影调整基本不影响高光");
    }

    // 高光：提亮亮部，对暗部影响很小
    {
        ImageBuffer dark = makeGray(0.03f);
        ImageBuffer bright = makeGray(0.80f);
        ParametricCurveSettings settings;
        settings.highlights = 100.0f;

        ops::applyParametricCurve(dark, settings);
        ops::applyParametricCurve(bright, settings);

        check(bright.at(0, 0, 0) > 0.82f, "高光 +100 提亮亮部");
        check(std::fabs(dark.at(0, 0, 0) - 0.03f) < 0.01f,
              "高光调整基本不影响暗部");
    }

    // 中间调：对中灰影响最大
    {
        ImageBuffer mid = makeGray(0.18f);
        ParametricCurveSettings settings;
        settings.midtones = 100.0f;
        ops::applyParametricCurve(mid, settings);
        check(mid.at(0, 0, 0) > 0.20f, "中间调 +100 提亮中灰");
    }

    // 范围：加宽阴影范围后，中灰获得更多阴影调整
    {
        ImageBuffer narrow = makeGray(0.18f);
        ImageBuffer wide = narrow;

        ParametricCurveSettings settings;
        settings.shadows = 100.0f;
        settings.shadowRange = 0.06f;
        ops::applyParametricCurve(narrow, settings);

        settings.shadowRange = 0.45f;
        ops::applyParametricCurve(wide, settings);

        const float narrowDelta = narrow.at(0, 0, 0) - 0.18f;
        const float wideDelta = wide.at(0, 0, 0) - 0.18f;
        check(wideDelta > narrowDelta, "加宽阴影范围让中灰获得更多调整");
    }
}

void testMatrixValidation()
{
    std::printf("\n[测试] CCM 矩阵校验\n");

    {
        const MatrixValidationResult result =
            ColorCalibration::validate(identityMatrix3x3());
        check(result.level == MatrixValidationLevel::Valid, "单位矩阵判定为合法");
        check(result.isUsable(), "单位矩阵可用");
    }

    {
        const MatrixValidationResult result =
            ColorCalibration::validate(ColorCalibration::defaultNeutralMatrix());
        check(result.level == MatrixValidationLevel::Valid, "默认中性矩阵判定为合法");
    }

    {
        const Matrix3x3 zero{};
        const MatrixValidationResult result = ColorCalibration::validate(zero);
        check(result.level == MatrixValidationLevel::Invalid, "全零矩阵判定为非法");
        check(!result.isUsable(), "全零矩阵不可用");
    }

    {
        // 负行列式：交换两行构造镜像变换
        const Matrix3x3 flip{{
            {{0.0f, 1.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}},
            {{0.0f, 0.0f, 1.0f}},
        }};
        const MatrixValidationResult result = ColorCalibration::validate(flip);
        check(result.level == MatrixValidationLevel::Invalid, "负行列式矩阵判定为非法");
        check(result.determinant < 0.0f, "负行列式的值被正确计算");
    }

    {
        // 元素过大：对角线 5.0
        const Matrix3x3 huge{{
            {{5.0f, 0.0f, 0.0f}},
            {{0.0f, 5.0f, 0.0f}},
            {{0.0f, 0.0f, 5.0f}},
        }};
        const MatrixValidationResult result = ColorCalibration::validate(huge);
        check(result.level == MatrixValidationLevel::Invalid, "元素超限矩阵判定为非法");
    }

    {
        Matrix3x3 nanMatrix = identityMatrix3x3();
        nanMatrix[1][1] = std::nanf("");
        const MatrixValidationResult result = ColorCalibration::validate(nanMatrix);
        check(result.level == MatrixValidationLevel::Invalid, "含 NaN 的矩阵判定为非法");
    }

    {
        // 行列式偏小但可用：应给出警告而不是直接拒绝
        const Matrix3x3 weak{{
            {{0.30f, 0.0f, 0.0f}},
            {{0.0f, 0.30f, 0.0f}},
            {{0.0f, 0.0f, 0.30f}},
        }};
        const MatrixValidationResult result = ColorCalibration::validate(weak);
        check(result.level == MatrixValidationLevel::Warning, "小行列式矩阵给出警告");
        check(result.isUsable(), "警告级别矩阵仍可使用");
        check(!result.message.empty(), "警告包含面向用户的提示文字");
    }
}

// ===========================================================================
//  4. 色卡求解 CCM
// ===========================================================================

void testSolveFromColorChecker()
{
    std::printf("\n[测试] 色卡最小二乘求解 CCM\n");

    // 构造一个物理合理的已知矩阵作为“真实答案”
    const Matrix3x3 truth{{
        {{ 0.90f, 0.06f, 0.02f}},
        {{ 0.04f, 0.92f, 0.03f}},
        {{ 0.02f, 0.05f, 0.88f}},
    }};

    // 24 个色块的相机 RGB 采样（覆盖较宽的取值范围，避免正规方程退化）
    const std::vector<std::array<float, 3>> samples = {
        {0.85f, 0.12f, 0.10f}, {0.20f, 0.60f, 0.25f}, {0.10f, 0.20f, 0.70f},
        {0.75f, 0.75f, 0.70f}, {0.50f, 0.50f, 0.50f}, {0.30f, 0.25f, 0.20f},
        {0.90f, 0.50f, 0.10f}, {0.15f, 0.35f, 0.80f}, {0.80f, 0.15f, 0.65f},
        {0.05f, 0.70f, 0.60f}, {0.95f, 0.90f, 0.30f}, {0.40f, 0.10f, 0.60f},
        {0.60f, 0.80f, 0.20f}, {0.35f, 0.15f, 0.15f}, {0.12f, 0.45f, 0.40f},
        {0.70f, 0.60f, 0.85f}, {0.25f, 0.85f, 0.65f}, {0.55f, 0.35f, 0.75f},
        {0.82f, 0.28f, 0.35f}, {0.18f, 0.55f, 0.90f}, {0.65f, 0.70f, 0.55f},
        {0.45f, 0.65f, 0.35f}, {0.30f, 0.80f, 0.85f}, {0.85f, 0.95f, 0.90f},
    };

    std::vector<std::array<float, 3>> reference;
    reference.reserve(samples.size());
    for (const auto& rgb : samples) {
        std::array<float, 3> xyz{};
        for (int r = 0; r < 3; ++r) {
            float sum = 0.0f;
            for (int c = 0; c < 3; ++c) {
                sum += truth[r][c] * rgb[static_cast<std::size_t>(c)];
            }
            xyz[static_cast<std::size_t>(r)] = sum;
        }
        reference.push_back(xyz);
    }

    Matrix3x3 solved = identityMatrix3x3();
    const ErrorInfo error =
        ColorCalibration::solveFromColorChecker(samples, reference, solved);
    check(error.isOk(), "精确数据求解成功");
    if (error.isOk()) {
        check(matricesNear(truth, solved, 1e-3f), "求解结果还原已知矩阵");
    }

    // ---- 数量不一致 ----
    {
        std::vector<std::array<float, 3>> shortReference(reference.begin(),
                                                          reference.end() - 1);
        Matrix3x3 out = identityMatrix3x3();
        const ErrorInfo err =
            ColorCalibration::solveFromColorChecker(samples, shortReference, out);
        check(!err.isOk(), "采样与参考数量不一致时报错");
    }

    // ---- 样本不足 ----
    {
        std::vector<std::array<float, 3>> fewSamples({{0.5f, 0.5f, 0.5f}});
        std::vector<std::array<float, 3>> fewRef({{0.5f, 0.5f, 0.5f}});
        Matrix3x3 out = identityMatrix3x3();
        const ErrorInfo err =
            ColorCalibration::solveFromColorChecker(fewSamples, fewRef, out);
        check(!err.isOk(), "样本少于 3 个时报错");
    }

    // ---- 退化样本：所有色块采样相同，正规方程奇异 ----
    {
        std::vector<std::array<float, 3>> degenerate(6, {0.4f, 0.4f, 0.4f});
        std::vector<std::array<float, 3>> refs(6, {0.4f, 0.4f, 0.4f});
        Matrix3x3 out = identityMatrix3x3();
        const ErrorInfo err =
            ColorCalibration::solveFromColorChecker(degenerate, refs, out);
        check(!err.isOk(), "退化样本（线性相关）时报错而非给出垃圾矩阵");
    }

    // ---- NaN 输入 ----
    {
        std::vector<std::array<float, 3>> badSamples = samples;
        badSamples[3][1] = std::nanf("");
        Matrix3x3 out = identityMatrix3x3();
        const ErrorInfo err =
            ColorCalibration::solveFromColorChecker(badSamples, reference, out);
        check(!err.isOk(), "含 NaN 的采样数据报错");
    }
}

// ===========================================================================
//  5. CCM CSV 往返
// ===========================================================================

void testCsvRoundTrip()
{
    std::printf("\n[测试] CCM CSV 往返\n");

    const Matrix3x3 matrix{{
        {{ 1.05f, -0.04f, -0.01f}},
        {{-0.03f,  1.04f, -0.01f}},
        {{-0.01f, -0.05f,  1.06f}},
    }};

    ColorCalibration original(matrix);
    const std::string csv = original.toCsv();
    check(!csv.empty(), "CSV 输出非空");
    check(csv.find('#') != std::string::npos, "CSV 带注释行，便于人工阅读");

    ColorCalibration restored;
    const ErrorInfo error = restored.fromCsv(csv);
    check(error.isOk(), "CSV 解析成功");
    if (error.isOk()) {
        check(matricesNear(matrix, restored.matrix(), 1e-5f), "CSV 往返数值一致");
    }

    // 宽容解析：分号分隔 + 无注释
    ColorCalibration loose;
    const ErrorInfo looseError = loose.fromCsv("1.0; 0.0; 0.0\n0.0; 1.0; 0.0\n0.0; 0.0; 1.0\n");
    check(looseError.isOk(), "分号分隔的 CSV 可解析");
    if (looseError.isOk()) {
        check(loose.isIdentity(), "分号分隔解析结果正确");
    }

    // 数值不足 9 个应报错
    ColorCalibration broken;
    const ErrorInfo brokenError = broken.fromCsv("1.0, 0.0, 0.0\n");
    check(!brokenError.isOk(), "数值不足 9 个时报错");
}

// ===========================================================================
//  6. EditContext 默认值
// ===========================================================================

void testEditContextDefaults()
{
    std::printf("\n[测试] 编辑上下文默认状态\n");

    EditContext context;
    check(context.isDefault(), "新建上下文为默认状态");
    check(context.raw.demosaicAlgorithm == DemosaicAlgorithm::AMAZE,
          "默认去拜耳算法为 AMaZE");
    check(context.raw.enableFbdd, "默认启用 FBDD");
    check(context.output.scale == 1.0f, "默认导出缩放为 1.0");
    check(context.curve.isIdentity(), "默认曲线为恒等");
    check(context.falseColor.crosstalk.enable
              && context.falseColor.highlightRolloff.enable
              && context.falseColor.lca.enable
              && context.falseColor.loca.enable
              && context.falseColor.edgeFalseColor.enable,
          "彩边 / 色散校正默认全部启用");

    context.sourceRawPath = "C:/photos/test.CR3";
    context.light.exposure = 1.5f;
    context.whiteBalance.temperatureOffset = 500.0f;
    context.ccm.enable = true;
    context.output.scale = 0.5f;
    check(!context.isDefault(), "修改参数后不再是默认状态");

    context.resetToDefaults();
    check(context.isDefault(), "恢复默认后回到默认状态");
    check(context.sourceRawPath == "C:/photos/test.CR3",
          "恢复默认保留源文件路径");
    check(context.output.scale == 1.0f, "恢复默认重置导出缩放");
}

void testWhiteBalanceLayers()
{
    std::printf("\n[测试] 基准白平衡 + 创意偏移（两层模型）\n");

    {
        EditContext context;
        check(!context.baseWhiteBalance.enable, "默认不启用基准白平衡");
        check(context.whiteBalance.isIdentity(), "默认创意白平衡偏移为零");
    }

    // 旧 RAW 工程：绝对白点迁移为"as-shot 基准 + 偏移"
    {
        EditContext old;
        old.whiteBalance.legacyAbsolute = true;
        old.whiteBalance.legacyTemperature = 4800.0f;
        old.whiteBalance.legacyTint = -12.0f;

        old.resolveLegacyWhiteBalance(false, 5000.0f, 0.0f);

        check(old.baseWhiteBalance.enable
                  && near(old.baseWhiteBalance.temperature, 5000.0f, 1e-5f),
              "旧 RAW 工程用 as-shot 建立基准");
        check(near(old.whiteBalance.temperatureOffset, -200.0f, 1e-5f)
                  && near(old.whiteBalance.tintOffset, -12.0f, 1e-5f),
              "旧绝对白点换算为相对基准的偏移");
        check(!old.whiteBalance.legacyAbsolute, "迁移后清除 legacy 标记");
        check(old.baseWhiteBalance.presentInProject,
              "迁移后的基准标记为已显式确定");
    }

    // 已被 .camcal / 工程显式提供的基准不能被 as-shot 覆盖
    {
        EditContext context;
        context.baseWhiteBalance.presentInProject = true;
        context.baseWhiteBalance.enable = true;
        context.baseWhiteBalance.temperature = 4200.0f;
        context.baseWhiteBalance.tint = 5.0f;

        context.resolveLegacyWhiteBalance(false, 5000.0f, 0.0f);
        check(near(context.baseWhiteBalance.temperature, 4200.0f, 1e-5f)
                  && near(context.baseWhiteBalance.tint, 5.0f, 1e-5f),
              "已有基准不被 as-shot 覆盖");
        check(context.whiteBalance.isIdentity(),
              "没有旧绝对白点时创意偏移保持为零");
    }

    // 普通位图：不启用基准，旧绝对白点相对 5000K 换算
    {
        EditContext rgb;
        rgb.whiteBalance.legacyAbsolute = true;
        rgb.whiteBalance.legacyTemperature = 6000.0f;
        rgb.whiteBalance.legacyTint = 10.0f;

        rgb.resolveLegacyWhiteBalance(true, 0.0f, 0.0f);
        check(!rgb.baseWhiteBalance.enable, "普通位图不启用基准白平衡");
        check(near(rgb.whiteBalance.temperatureOffset, 1000.0f, 1e-5f)
                  && near(rgb.whiteBalance.tintOffset, 10.0f, 1e-5f),
              "普通位图旧白点相对 D50 中性参考换算");
    }

    // 两层算法组合应等价于"目标白点绝对校正"
    {
        ImageBuffer source(8, 8);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                const float u = static_cast<float>(x) / 7.0f;
                const float v = static_cast<float>(y) / 7.0f;
                source.at(x, y, 0) = 0.15f + 0.55f * u;
                source.at(x, y, 1) = 0.20f + 0.45f * v;
                source.at(x, y, 2) = 0.60f - 0.30f * u;
            }
        }

        // 基准 4800K / -10，偏移 +700K / -5 → 目标 5500K / -15
        ImageBuffer layered = source;
        ops::applyWhiteBalance(layered, 4800.0f, -10.0f,
                               -1.0f, -1.0f, false, 0.0f, 0.0f);
        ops::applyWhiteBalance(layered, 5500.0f, -15.0f,
                               -1.0f, -1.0f, false, 4800.0f, -10.0f, false);

        ImageBuffer absolute = source;
        ops::applyWhiteBalance(absolute, 5500.0f, -15.0f,
                               -1.0f, -1.0f, false, 0.0f, 0.0f);

        float maxError = 0.0f;
        for (std::size_t i = 0; i < source.sampleCount(); ++i) {
            maxError = std::max(
                maxError, std::fabs(layered.data()[i] - absolute.data()[i]));
        }
        check(maxError < 1e-4f,
              "基准校正 × 相对偏移等价于目标白点绝对校正");

        // 偏移为 0 时，创意步骤不应改变基准结果
        ImageBuffer baseOnly = source;
        ops::applyWhiteBalance(baseOnly, 4800.0f, -10.0f,
                               -1.0f, -1.0f, false, 0.0f, 0.0f);
        ImageBuffer zeroOffset = source;
        ops::applyWhiteBalance(zeroOffset, 4800.0f, -10.0f,
                               -1.0f, -1.0f, false, 0.0f, 0.0f);
        ops::applyWhiteBalance(zeroOffset, 4800.0f, -10.0f,
                               -1.0f, -1.0f, false, 4800.0f, -10.0f, false);

        maxError = 0.0f;
        for (std::size_t i = 0; i < source.sampleCount(); ++i) {
            maxError = std::max(
                maxError, std::fabs(baseOnly.data()[i] - zeroOffset.data()[i]));
        }
        check(maxError < 1e-5f, "创意偏移为 0 时保持基准白平衡不变");
    }

    // 基准白平衡灰卡拾取：采样点被归一化为中性
    {
        ImageBuffer image(16, 16);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.at(x, y, 0) = 0.40f;
                image.at(x, y, 1) = 0.20f;
                image.at(x, y, 2) = 0.10f;
            }
        }

        ops::applyWhiteBalance(image, 5500.0f, 0.0f,
                               0.5f, 0.5f, true, 0.0f, 0.0f);
        const float r = image.at(8, 8, 0);
        const float g = image.at(8, 8, 1);
        const float b = image.at(8, 8, 2);
        check(std::fabs(r - g) < 1e-4f && std::fabs(b - g) < 1e-4f,
              "基准灰卡拾取把采样点归一化为中性");
    }

    // 灰点拾取也算有效创意调整
    {
        WhiteBalanceSettings wb;
        check(wb.isIdentity(), "零偏移为恒等");
        wb.useGrayPick = true;
        check(!wb.isIdentity(), "灰点拾取不是恒等");
        wb.useGrayPick = false;
        wb.temperatureOffset = 100.0f;
        check(!wb.isIdentity(), "非零偏移不是恒等");
    }
}

void testHslBandMapping()
{
    std::printf("\n[测试] 色相 → HSL 通道映射（A4）\n");

    check(hslBandForHue(0.0f) == HslBand::Red, "0° 映射到红");
    check(hslBandForHue(355.0f) == HslBand::Red, "355° 映射到红");
    check(hslBandForHue(25.0f) == HslBand::Orange, "25° 映射到橙");
    check(hslBandForHue(60.0f) == HslBand::Yellow, "60° 映射到黄");
    check(hslBandForHue(120.0f) == HslBand::Green, "120° 映射到绿");
    check(hslBandForHue(180.0f) == HslBand::Cyan, "180° 映射到青");
    check(hslBandForHue(240.0f) == HslBand::Blue, "240° 映射到蓝");
    check(hslBandForHue(285.0f) == HslBand::Purple, "285° 映射到紫");
    check(hslBandForHue(-30.0f) == HslBand::Red, "负色相自动回绕");
    check(hslBandForHue(360.0f) == HslBand::Red, "360° 回绕到红");
}

// ===========================================================================
//  9. 预览 2x2 合并与 RGB 面积平均
// ===========================================================================

/// 按拜耳排列填充测试数据：通道基准值 + 位置微扰。
/// 位置微扰用于区分"取对了像素"与"通道值恰好相同"（例如把 R/B 平均
/// 误当成 G 时，纯通道常量会掩盖错误）。
float bayerTestValue(BayerPattern pattern, int x, int y, int width)
{
    static const float base[3] = {0.2f, 0.4f, 0.6f};
    return base[bayerChannelAt(pattern, x, y)] + 0.001f * static_cast<float>(y * width + x);
}

void testPreviewBinning()
{
    std::printf("\n[测试] 预览 2x2 合并\n");

    const BayerPattern patterns[4] = {
        BayerPattern::RGGB, BayerPattern::BGGR,
        BayerPattern::GRBG, BayerPattern::GBRG};

    for (BayerPattern pattern : patterns) {
        BayerImage bayer;
        bayer.width = 4;
        bayer.height = 4;
        bayer.pattern = pattern;
        bayer.data.resize(16);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                bayer.at(x, y) = bayerTestValue(pattern, x, y, 4);
            }
        }

        ImageBuffer out;
        const ErrorInfo error = ops::binBayerToRgb(bayer, out);
        char label[160];
        std::snprintf(label, sizeof(label), "%s 合并成功且尺寸减半",
                      bayerPatternName(pattern));
        check(error.isOk() && out.width() == 2 && out.height() == 2, label);

        // 用 bayerChannelAt 独立定位每个 2x2 单元内的通道位置，
        // 不复用被测实现里的坐标推导，避免"同错同对"。
        bool allMatch = true;
        for (int by = 0; by < 2 && allMatch; ++by) {
            for (int bx = 0; bx < 2 && allMatch; ++bx) {
                const int x0 = bx * 2;
                const int y0 = by * 2;
                float expected[3] = {0.0f, 0.0f, 0.0f};
                int greenCount = 0;
                bool hasRed = false;
                bool hasBlue = false;

                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int channel = bayerChannelAt(pattern, x0 + dx, y0 + dy);
                        const float value = bayerTestValue(pattern, x0 + dx, y0 + dy, 4);
                        if (channel == 1) {
                            expected[1] += value;
                            ++greenCount;
                        } else {
                            expected[channel] = value;
                            hasRed = hasRed || (channel == 0);
                            hasBlue = hasBlue || (channel == 2);
                        }
                    }
                }
                allMatch = hasRed && hasBlue && greenCount == 2;
                if (allMatch) {
                    expected[1] /= static_cast<float>(greenCount);
                    for (int c = 0; c < 3 && allMatch; ++c) {
                        allMatch = near(out.at(bx, by, c), expected[c], 1e-5f);
                    }
                }
            }
        }
        std::snprintf(label, sizeof(label), "%s 的 R/G/B 取自正确的空间位置",
                      bayerPatternName(pattern));
        check(allMatch, label);
    }

    // 未知排列应报错，调用方据此回退到普通去马赛克
    BayerImage unknown;
    unknown.width = 4;
    unknown.height = 4;
    unknown.pattern = BayerPattern::Unknown;
    unknown.data.assign(16, 0.5f);
    ImageBuffer unused;
    check(!ops::binBayerToRgb(unknown, unused).isOk(), "未知拜耳排列返回错误");

    // 尺寸不足一个 2x2 单元应报错
    BayerImage tiny;
    tiny.width = 1;
    tiny.height = 1;
    tiny.pattern = BayerPattern::RGGB;
    tiny.data.assign(1, 0.5f);
    check(!ops::binBayerToRgb(tiny, unused).isOk(), "尺寸不足 2x2 返回错误");

    // 饱和掩码：RGGB 的左上单元中 G、B 顶到白电平，R 未饱和
    {
        BayerImage saturated;
        saturated.width = 4;
        saturated.height = 4;
        saturated.pattern = BayerPattern::RGGB;
        saturated.data.assign(16, 0.5f);
        // (1,0) 与 (0,1) 是绿点，(1,1) 是蓝点
        saturated.at(1, 0) = 1.0f;
        saturated.at(0, 1) = 1.0f;
        saturated.at(1, 1) = 1.0f;

        ImageBuffer out;
        std::vector<std::uint8_t> mask;
        const ErrorInfo error = ops::binBayerToRgb(saturated, out, &mask);

        check(error.isOk() && mask.size() == 4u, "饱和掩码与输出同尺寸");
        check(!mask.empty() && mask[0] == (ops::kClipGreen | ops::kClipBlue),
              "G/B 饱和的单元被正确标记");
        check(mask.size() == 4u && mask[1] == 0u && mask[2] == 0u && mask[3] == 0u,
              "未饱和单元的标记为 0");
    }
}

void testDownsampleRgbArea()
{
    std::printf("\n[测试] RGB 面积平均降采样\n");

    // 4x4 输入，四个 2x2 块各自为常量，块间不同
    ImageBuffer input(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int block = (y / 2) * 2 + (x / 2);
            const float value = 0.1f * static_cast<float>(block);
            input.at(x, y, 0) = value;
            input.at(x, y, 1) = value + 0.01f;
            input.at(x, y, 2) = value + 0.02f;
        }
    }

    ImageBuffer output;
    ops::downsampleRgbArea(input, output, 2);
    check(output.width() == 2 && output.height() == 2, "factor=2 输出尺寸减半");

    bool allMatch = true;
    for (int by = 0; by < 2 && allMatch; ++by) {
        for (int bx = 0; bx < 2 && allMatch; ++bx) {
            const float expected = 0.1f * static_cast<float>(by * 2 + bx);
            allMatch = near(output.at(bx, by, 0), expected, 1e-5f)
                && near(output.at(bx, by, 1), expected + 0.01f, 1e-5f)
                && near(output.at(bx, by, 2), expected + 0.02f, 1e-5f);
        }
    }
    check(allMatch, "块内取平均且三通道使用相同核");

    ImageBuffer same;
    ops::downsampleRgbArea(input, same, 1);
    check(same.width() == 4 && same.height() == 4
              && near(same.at(3, 3, 0), input.at(3, 3, 0), 1e-6f),
          "factor=1 原样拷贝");

    // 不能整除时丢弃右侧/底部余数，尺寸向下取整
    ImageBuffer odd(5, 5);
    odd.fill(0.5f);
    ImageBuffer oddOut;
    ops::downsampleRgbArea(odd, oddOut, 2);
    check(oddOut.width() == 2 && oddOut.height() == 2, "非整除尺寸向下取整");

    // 空输入不应崩溃
    ImageBuffer empty;
    ImageBuffer emptyOut;
    ops::downsampleRgbArea(empty, emptyOut, 2);
    check(emptyOut.isEmpty(), "空输入得到空输出");
}

// ===========================================================================
//  9.5 预览降采样倍数选择
// ===========================================================================

void testPreviewDownsampleSelection()
{
    std::printf("\n[测试] 预览降采样倍数选择\n");

    // 关键约束：渲染出的长边不得明显小于屏幕需要的像素。
    // 一旦小于，画布只能放大显示，画面立刻发虚（放大 1~1.5 倍时最明显）。
    struct Case { int width; int height; int maxDimension; };
    const Case cases[] = {
        {8288, 5520, 1700},   // 45MP 缩到普通窗口
        {8288, 5520, 2550},   // 45MP 缩到 4K 高 DPI 窗口
        {8288, 5520, 2000},   // 因子恰好为偶数边界
        {4000, 3000, 2000},   // 因子恰为 2
        {8288, 5520, 6000},   // 因子介于 1 与 2 之间：应走全分辨率
        {1000, 800, 1200},    // 原图小于屏幕：不降采样
    };

    for (const Case& item : cases) {
        const int factor = PipelineRenderer::computeDownsampleFactor(
            item.width, item.height, item.maxDimension);
        const int longest = std::max(item.width, item.height);
        const int rendered = (factor <= 1) ? longest : longest / factor;

        char label[192] = {};
        std::snprintf(label, sizeof(label),
                      "%dx%d 缩到 %d：因子 %d，渲染长边 %d",
                      item.width, item.height, item.maxDimension, factor, rendered);
        // 原图比屏幕需求还小时不降采样，渲染尺寸就等于原图，无需比较
        if (longest > item.maxDimension) {
            check(rendered >= static_cast<int>(item.maxDimension * 0.94), label);
        } else {
            check(factor == 1, label);
        }
        check(factor == 1 || (factor >= 2 && factor % 2 == 0),
              "倍数必须是 1 或 >=2 的偶数（2x2 合并要求）");
    }
}

// ===========================================================================
//  9.6 USM 锐化实际生效
// ===========================================================================

void testSharpenEffect()
{
    std::printf("\n[测试] USM 锐化\n");

    // 竖直台阶边缘：左半 0.2、右半 0.5（灰度，三通道相同）
    ImageBuffer image(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float value = (x < 8) ? 0.2f : 0.5f;
            image.at(x, y, 0) = value;
            image.at(x, y, 1) = value;
            image.at(x, y, 2) = value;
        }
    }

    ImageBuffer sharpened = image;
    // 半径 0.8px 是默认值：若被误当成 3σ 支撑半径传给高斯模糊，
    // 差分近似为 0，这里的结果会几乎不变——该用例正是为了防止这种回归。
    ops::applySharpen(sharpened, 50.0f, 0.8f, 0.0f);

    const float edgeOvershoot = sharpened.at(8, 8, 1) - image.at(8, 8, 1);
    const float flatChange = std::fabs(sharpened.at(1, 8, 1) - image.at(1, 8, 1));

    char label[160] = {};
    std::snprintf(label, sizeof(label), "默认半径下边缘应出现过冲（实测 %.4f）",
                  static_cast<double>(edgeOvershoot));
    check(edgeOvershoot > 0.01f, label);
    check(flatChange < 1e-4f, "平坦区不应被锐化改变");
}

// ===========================================================================
//  10. 伪彩抑制
// ===========================================================================

void testFalseColorSuppression()
{
    std::printf("\n[测试] 伪彩抑制\n");

    // 情况 1：平坦灰底 + 一个红色孤立点，应被邻居的色差中值纠正
    {
        ImageBuffer image(7, 7);
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                image.at(x, y, 0) = 0.30f;
                image.at(x, y, 1) = 0.30f;
                image.at(x, y, 2) = 0.30f;
            }
        }
        image.at(3, 3, 0) = 0.80f;  // 红色孤立点

        ops::suppressFalseColor(image, 0.08f);

        check(near(image.at(3, 3, 0), 0.30f, 1e-5f), "孤立红点被纠正为邻域色");
        check(near(image.at(3, 3, 1), 0.30f, 1e-6f), "纠正时不改动绿色");
        check(near(image.at(3, 3, 2), 0.30f, 1e-6f), "纠正时不改动蓝色");
        check(near(image.at(1, 1, 0), 0.30f, 1e-6f), "远处像素保持不变");
    }

    // 情况 2：大面积同色区域应完全不受影响
    {
        ImageBuffer image(7, 7);
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                image.at(x, y, 0) = 0.60f;
                image.at(x, y, 1) = 0.20f;
                image.at(x, y, 2) = 0.10f;
            }
        }

        ops::suppressFalseColor(image, 0.08f);

        bool unchanged = true;
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                unchanged = unchanged
                    && near(image.at(x, y, 0), 0.60f, 1e-6f)
                    && near(image.at(x, y, 1), 0.20f, 1e-6f)
                    && near(image.at(x, y, 2), 0.10f, 1e-6f);
            }
        }
        check(unchanged, "大面积彩色区域不被削弱");
    }

    // 情况 3：粗色彩边缘两侧的内部像素保持原样
    {
        ImageBuffer image(9, 9);
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 9; ++x) {
                const bool left = x < 4;
                image.at(x, y, 0) = left ? 0.60f : 0.20f;
                image.at(x, y, 1) = 0.20f;
                image.at(x, y, 2) = left ? 0.10f : 0.20f;
            }
        }

        ops::suppressFalseColor(image, 0.08f);

        check(near(image.at(1, 4, 0), 0.60f, 1e-6f)
                  && near(image.at(1, 4, 2), 0.10f, 1e-6f),
              "边缘左侧内部像素保持原色");
        check(near(image.at(7, 4, 0), 0.20f, 1e-6f)
                  && near(image.at(7, 4, 2), 0.20f, 1e-6f),
              "边缘右侧内部像素保持原色");
    }

    // 情况 4：低于阈值的小偏离不动，避免削弱正常细节
    {
        ImageBuffer image(7, 7);
        image.fill(0.30f);
        image.at(3, 3, 0) = 0.31f;  // 色比偏离约 3%，低于 8% 阈值

        ops::suppressFalseColor(image, 0.08f);
        check(near(image.at(3, 3, 0), 0.31f, 1e-6f), "低于阈值的偏离不处理");
    }

    // 情况 5：过小图像不应越界（无 3x3 邻域时直接返回）
    {
        ImageBuffer tiny(2, 2);
        tiny.fill(0.5f);
        ops::suppressFalseColor(tiny, 0.02f);
        check(near(tiny.at(0, 0, 0), 0.5f, 1e-6f), "小尺寸图像安全返回");
    }
}

// ===========================================================================
//  11. 部分饱和高光的还原
// ===========================================================================

void testClipHighlightReconstruction()
{
    std::printf("\n[测试] 部分饱和高光还原\n");

    // 情况 1：G/B 饱和而 R 仍然较亮（玻璃幕墙反光）→ 还原为中性白
    {
        ImageBuffer image(7, 7);
        image.fill(0.30f);
        image.at(3, 3, 0) = 0.61f;  // 未饱和通道仍然很亮 = 高光
        image.at(3, 3, 1) = 1.0f;
        image.at(3, 3, 2) = 1.0f;

        std::vector<std::uint8_t> mask(49u, 0u);
        mask[3u * 7u + 3u] = static_cast<std::uint8_t>(ops::kClipGreen | ops::kClipBlue);

        ops::flattenClippedHighlights(image, mask.data());

        check(near(image.at(3, 3, 0), 1.0f, 1e-5f)
                  && near(image.at(3, 3, 1), 1.0f, 1e-5f)
                  && near(image.at(3, 3, 2), 1.0f, 1e-5f),
              "部分饱和的亮部被还原为中性白");
        check(near(image.at(1, 1, 0), 0.30f, 1e-6f), "周围像素不受影响");
    }

    // 情况 2：未饱和通道很暗（本体是深色物体）→ 颜色可信，不洗白
    {
        ImageBuffer image(7, 7);
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                image.at(x, y, 0) = 0.05f;
                image.at(x, y, 1) = 1.0f;
                image.at(x, y, 2) = 1.0f;
            }
        }

        std::vector<std::uint8_t> mask(49u, 0u);
        mask[3u * 7u + 3u] = static_cast<std::uint8_t>(ops::kClipGreen | ops::kClipBlue);

        ops::flattenClippedHighlights(image, mask.data());

        check(near(image.at(3, 3, 0), 0.05f, 1e-6f), "深色物体的颜色不被洗白");
    }

    // 情况 3：三通道全部饱和（或降采样掩码按位或凑成全饱和）→ 统一到最大通道
    {
        ImageBuffer image(7, 7);
        image.fill(0.30f);
        image.at(3, 3, 0) = 0.60f;
        image.at(3, 3, 1) = 0.90f;
        image.at(3, 3, 2) = 1.0f;

        std::vector<std::uint8_t> mask(49u, 0u);
        mask[3u * 7u + 3u] = static_cast<std::uint8_t>(
            ops::kClipRed | ops::kClipGreen | ops::kClipBlue);

        ops::flattenClippedHighlights(image, mask.data());

        check(near(image.at(3, 3, 0), 1.0f, 1e-5f)
                  && near(image.at(3, 3, 1), 1.0f, 1e-5f)
                  && near(image.at(3, 3, 2), 1.0f, 1e-5f),
              "全饱和标记的像素被拉平为中性白");
        ops::suppressFalseColor(image, 0.08f);
        check(near(image.at(3, 3, 0), 1.0f, 1e-5f), "随后的色比抑制不会破坏中性白");
    }

    // 情况 4：没有掩码时不做高光还原
    {
        ImageBuffer image(7, 7);
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                image.at(x, y, 0) = 0.61f;
                image.at(x, y, 1) = 1.0f;
                image.at(x, y, 2) = 1.0f;
            }
        }

        ops::flattenClippedHighlights(image, nullptr);

        check(near(image.at(3, 3, 0), 0.61f, 1e-6f), "没有掩码时不做高光还原");
    }

    // 情况 5：只有未饱和通道达到亮度门限才还原
    {
        ImageBuffer image(3, 3);
        image.fill(0.20f);
        image.at(1, 1, 0) = 0.40f;  // 低于 0.5 门限
        image.at(1, 1, 1) = 1.0f;
        image.at(1, 1, 2) = 1.0f;

        std::vector<std::uint8_t> mask(9u, 0u);
        mask[1u * 3u + 1u] = static_cast<std::uint8_t>(ops::kClipGreen | ops::kClipBlue);

        ops::flattenClippedHighlights(image, mask.data());

        check(near(image.at(1, 1, 0), 0.40f, 1e-6f), "未饱和通道不够亮时不还原");
    }
}

// ===========================================================================
//  11.1 全分辨率饱和掩码（拜耳域 5x5 邻域）
// ===========================================================================

void testFullResolutionClipMask()
{
    std::printf("\n[测试] 全分辨率饱和掩码\n");

    // RGGB 8x8，只有 (3,3) 一个采样饱和。RGGB 在 (3,3) 是蓝通道。
    {
        BayerImage bayer;
        bayer.width = 8;
        bayer.height = 8;
        bayer.pattern = BayerPattern::RGGB;
        bayer.data.assign(64, 0.2f);
        bayer.data[3 * 8 + 3] = 1.0f;

        std::vector<std::uint8_t> mask;
        check(ops::buildClipMaskFromBayer(bayer, mask), "可生成全分辨率掩码");
        check(mask.size() == 64, "掩码尺寸与图像一致");
        check(mask[3 * 8 + 3] == ops::kClipBlue, "(3,3) 只有蓝通道饱和位");
        // (1,1) 距离 (3,3) 为 2，5x5 邻域内仍能覆盖该饱和采样
        check(mask[1 * 8 + 1] == ops::kClipBlue, "5x5 邻域覆盖被插值摊开的饱和采样");
        check(mask[0] == 0u, "远离饱和点的像素没有标记");
    }

    // 4x4 全饱和块 → 块中心三通道全标记
    {
        BayerImage bayer;
        bayer.width = 12;
        bayer.height = 12;
        bayer.pattern = BayerPattern::RGGB;
        bayer.data.assign(144, 0.3f);
        for (int y = 4; y < 8; ++y) {
            for (int x = 4; x < 8; ++x) {
                bayer.data[y * 12 + x] = 1.0f;
            }
        }

        std::vector<std::uint8_t> mask;
        check(ops::buildClipMaskFromBayer(bayer, mask), "全饱和块生成掩码");
        const std::uint8_t all = ops::kClipRed | ops::kClipGreen | ops::kClipBlue;
        check(mask[6 * 12 + 6] == all, "全饱和块内部三通道全部标记");
        check(mask[0] == 0u, "块外远处无标记");
    }

    // 未知排列 → 失败且不修改 mask
    {
        BayerImage bayer;
        bayer.width = 8;
        bayer.height = 8;
        bayer.pattern = BayerPattern::Unknown;
        bayer.data.assign(64, 1.0f);

        std::vector<std::uint8_t> mask(3, 0xAA);
        check(!ops::buildClipMaskFromBayer(bayer, mask), "未知排列拒绝生成掩码");
        check(mask.size() == 3 && mask[0] == 0xAA, "失败时不修改现有掩码");
    }

    // 与去拜耳 + 高光还原联动：R 饱和、G/B 较亮 → 最终为中性白
    {
        BayerImage bayer;
        bayer.width = 16;
        bayer.height = 16;
        bayer.pattern = BayerPattern::RGGB;
        bayer.data.assign(256, 0.0f);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int channel = bayerChannelAt(BayerPattern::RGGB, x, y);
                bayer.data[y * 16 + x] = (channel == 0) ? 1.0f
                                       : (channel == 1 ? 0.9f : 0.6f);
            }
        }

        std::vector<std::uint8_t> mask;
        check(ops::buildClipMaskFromBayer(bayer, mask), "联动测试生成掩码");

        ImageBuffer image;
        const ErrorInfo error = ops::demosaic(
            bayer, image, DemosaicAlgorithm::Bilinear, 0.0f);
        check(error.isOk() && !image.isEmpty(), "联动测试去拜耳成功");

        const int px = 8;
        const int py = 8;
        check(std::fabs(image.at(px, py, 0) - image.at(px, py, 1)) > 0.05f,
              "还原前高光带有色偏");

        ops::flattenClippedHighlights(image, mask.data());

        const float r = image.at(px, py, 0);
        const float g = image.at(px, py, 1);
        const float b = image.at(px, py, 2);
        check(near(r, g, 1e-5f) && near(g, b, 1e-5f),
              "全分辨率掩码触发高光还原为中性白");
    }
}

// ===========================================================================
//  11.2 摩尔纹 / 伪彩抑制：强度必须单调，亮度必须不变
// ===========================================================================

void testMoireSuppressionStrength()
{
    std::printf("\n[测试] 摩尔纹抑制强度语义\n");

    // 构造强彩色棋盘拜耳图：每 2x2 块 R/B 互换，绿色保持中灰。
    // 去拜耳后会形成明显的色度高频，正是摩尔纹抑制要处理的对象。
    BayerImage bayer;
    bayer.width = 32;
    bayer.height = 32;
    bayer.pattern = BayerPattern::RGGB;
    bayer.data.assign(32u * 32u, 0.5f);

    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool oddBlock = (((x / 2) + (y / 2)) & 1) != 0;
            const int channel = bayerChannelAt(BayerPattern::RGGB, x, y);
            if (channel == 0) {
                bayer.data[y * 32 + x] = oddBlock ? 0.15f : 0.85f;
            } else if (channel == 2) {
                bayer.data[y * 32 + x] = oddBlock ? 0.85f : 0.15f;
            }
        }
    }

    const auto run = [&](float smooth, std::vector<float>& lumaOut) {
        ImageBuffer image;
        ops::demosaic(bayer, image, DemosaicAlgorithm::Bilinear, smooth);

        const std::size_t pixels = image.pixelCount();
        lumaOut.resize(pixels);

        double mean = 0.0;
        double square = 0.0;
        for (std::size_t i = 0; i < pixels; ++i) {
            const float r = image.data()[i * 3u];
            const float g = image.data()[i * 3u + 1];
            const float b = image.data()[i * 3u + 2];
            const float y = 0.299f * r + 0.587f * g + 0.114f * b;
            lumaOut[i] = y;
            const float c = r - y;
            mean += c;
            square += static_cast<double>(c) * c;
        }
        mean /= static_cast<double>(pixels);
        const double variance = square / static_cast<double>(pixels) - mean * mean;
        return static_cast<float>(variance);
    };

    std::vector<float> luma0;
    std::vector<float> luma5;
    std::vector<float> luma10;
    const float var0 = run(0.0f, luma0);
    const float var5 = run(0.5f, luma5);
    const float var10 = run(1.0f, luma10);

    std::printf("  色度方差：0=%.6f  0.5=%.6f  1.0=%.6f\n", var0, var5, var10);
    check(var0 > var5 && var5 > var10, "抑制强度越高色度方差越小");

    float maxLumaDiff = 0.0f;
    for (std::size_t i = 0; i < luma0.size(); ++i) {
        maxLumaDiff = std::max(maxLumaDiff, std::fabs(luma0[i] - luma10[i]));
    }
    check(maxLumaDiff < 1e-5f, "摩尔纹抑制完全不改变亮度通道");
}

// ===========================================================================
//  11.2b 大尺度色纹抑制：周期约 12 像素的色度条纹被衰减，G 通道不变
// ===========================================================================

void testLargeChromaMoireSuppression()
{
    std::printf("\n[测试] 大尺度色纹抑制\n");

    constexpr int width = 96;
    constexpr int height = 96;
    constexpr float kPi = 3.14159265358979f;

    // 亮度恒定、只带色度条纹的合成图：R/G/B 围绕 0.5 做正负偏置，
    // 周期 12 像素，正好落在该算子要处理的 8~20 像素区间。
    const auto build = [&](ImageBuffer& image) {
        image = ImageBuffer(width, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float delta =
                    0.06f * std::sin(2.0f * kPi * static_cast<float>(x) / 12.0f);
                image.at(x, y, 0) = std::max(0.0f, 0.5f + delta);
                image.at(x, y, 1) = 0.5f;
                image.at(x, y, 2) = std::max(0.0f, 0.5f - delta);
            }
        }
    };

    // 只统计内部区域，避开边界处双线性外插带来的边缘效应
    const auto chromaAmplitude = [&](const ImageBuffer& image) {
        double sum = 0.0;
        double square = 0.0;
        int count = 0;
        for (int y = 8; y < height - 8; ++y) {
            for (int x = 8; x < width - 8; ++x) {
                const double c = 0.5 * (static_cast<double>(image.at(x, y, 0))
                                        - static_cast<double>(image.at(x, y, 2)));
                sum += c;
                square += c * c;
                ++count;
            }
        }
        const double mean = sum / static_cast<double>(count);
        return static_cast<double>(std::sqrt(std::max(0.0, square / static_cast<double>(count) - mean * mean)));
    };

    ImageBuffer image;
    build(image);

    std::vector<float> greenBefore(image.pixelCount());
    for (std::size_t i = 0; i < greenBefore.size(); ++i) {
        greenBefore[i] = image.data()[i * 3u + 1u];
    }

    const double before = chromaAmplitude(image);
    ops::suppressLargeChromaMoire(image, 1.0f);
    const double after = chromaAmplitude(image);

    std::printf("  色度幅度：处理前=%.5f  处理后=%.5f（保留 %.1f%%）\n",
                before, after, before > 1e-9 ? after / before * 100.0 : 0.0);
    check(before > 0.03, "合成图本身带有明显色度条纹");
    check(after < before * 0.4, "12 像素周期的色度条纹被显著衰减");

    bool greenSame = true;
    for (std::size_t i = 0; i < greenBefore.size(); ++i) {
        if (image.data()[i * 3u + 1u] != greenBefore[i]) {
            greenSame = false;
            break;
        }
    }
    check(greenSame, "大尺度色纹抑制完全不改变 G 通道");

    // strength = 0 必须等价于不调用：与未经处理的合成图逐像素比较
    ImageBuffer untouched;
    build(untouched);
    ops::suppressLargeChromaMoire(untouched, 0.0f);
    ImageBuffer reference;
    build(reference);
    bool identical = true;
    for (std::size_t i = 0; i < reference.pixelCount() * 3u; ++i) {
        if (untouched.data()[i] != reference.data()[i]) {
            identical = false;
            break;
        }
    }
    check(identical, "强度为 0 时逐像素不变");
}

// ===========================================================================
//  11.3 镜头校正：自动缩放 + 三次重采样不产生黑角/NaN
// ===========================================================================

void testLensCorrectionResampling(LensCorrector& corrector)
{
    std::printf("\n[测试] 镜头校正重采样\n");

    if (!LensCorrector::isSupported()) {
        std::printf("  [skip] 本次构建未包含 lensfun\n");
        return;
    }

    ImageBuffer image(96, 64);
    image.fill(0.5f);

    LensCorrectionSettings settings;
    settings.enable = true;
    settings.enableDistortion = true;
    settings.enableVignetting = false;  // 常数图开启暗角校正会改变亮度
    settings.enableChromaticAberration = true;

    const LensCorrectionResult result = corrector.apply(image, settings);
    check(result.applied, "已匹配镜头的校正可以执行");

    bool allFinite = true;
    float minValue = 1.0f;
    float maxValue = 0.0f;
    for (std::size_t i = 0; i < image.sampleCount(); ++i) {
        const float value = image.data()[i];
        allFinite = allFinite && std::isfinite(value);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }

    check(allFinite, "三次卷积采样结果全部有限");
    check(minValue > 0.499f && maxValue < 0.501f,
          "自动缩放与局部限幅保持常数图均匀（无黑角/振铃）");
    std::printf("  常数图范围：[%.6f, %.6f]\n", minValue, maxValue);
}

// ===========================================================================
//  11.3 去拜耳重建质量（合成高细节图 PSNR）
// ===========================================================================

void testDemosaicQuality()
{
    std::printf("\n[测试] 去拜耳重建质量\n");

    constexpr int kWidth = 96;
    constexpr int kHeight = 96;
    constexpr float kPi = 3.14159265358979323846f;

    // 合成具有水平/垂直/对角高频与彩色细节的 RGB 真值图
    std::vector<float> groundTruth(static_cast<std::size_t>(kWidth) * kHeight * 3u);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            // 真实场景的共同特征是：亮度细节丰富，而色差（R-G / B-G）
            // 相对平滑。这里用慢变底色 + 共用的高频亮度细节合成真值图，
            // 既考验插值分辨率，也符合去拜耳算法的色差平滑假设。
            const float lumaDetail = 0.12f * (
                std::sin(2.0f * kPi * (fx + fy) / 9.0f)
                + std::sin(2.0f * kPi * fx / 13.0f)
                + std::sin(2.0f * kPi * fy / 17.0f)) / 3.0f;

            float r = 0.5f + 0.06f * std::sin(2.0f * kPi * fx / 61.0f) + lumaDetail;
            float g = 0.5f + 0.06f * std::sin(2.0f * kPi * fy / 67.0f) + lumaDetail;
            float b = 0.5f + 0.06f * std::cos(2.0f * kPi * (fx - fy) / 71.0f) + lumaDetail;

            r = std::clamp(r, 0.0f, 1.0f);
            g = std::clamp(g, 0.0f, 1.0f);
            b = std::clamp(b, 0.0f, 1.0f);

            const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 3u;
            groundTruth[index + 0u] = r;
            groundTruth[index + 1u] = g;
            groundTruth[index + 2u] = b;
        }
    }

    BayerImage bayer;
    bayer.width = kWidth;
    bayer.height = kHeight;
    bayer.pattern = BayerPattern::RGGB;
    bayer.data.resize(static_cast<std::size_t>(kWidth) * kHeight);

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int channel = bayerChannelAt(BayerPattern::RGGB, x, y);
            const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;
            bayer.data[index] = groundTruth[index * 3u + static_cast<std::size_t>(channel)];
        }
    }

    const auto psnrOf = [&](DemosaicAlgorithm algorithm) {
        ImageBuffer image;
        const ErrorInfo error = ops::demosaic(bayer, image, algorithm, 0.0f);
        if (!error.isOk() || image.isEmpty()) {
            return -1.0;
        }

        double squaredError = 0.0;
        std::size_t count = 0;
        for (int y = 4; y < kHeight - 4; ++y) {
            for (int x = 4; x < kWidth - 4; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float diff = image.at(x, y, c)
                                     - groundTruth[(static_cast<std::size_t>(y) * kWidth + x) * 3u
                                                   + static_cast<std::size_t>(c)];
                    squaredError += static_cast<double>(diff) * diff;
                    ++count;
                }
            }
        }

        const double mse = squaredError / static_cast<double>(count);
        return 10.0 * std::log10(1.0 / std::max(mse, 1e-12));
    };

    const double amazePsnr = psnrOf(DemosaicAlgorithm::AMAZE);
    const double lmmsePsnr = psnrOf(DemosaicAlgorithm::LMMSE);
    const double rcdPsnr = psnrOf(DemosaicAlgorithm::RCD);
    const double bilinearPsnr = psnrOf(DemosaicAlgorithm::Bilinear);

    std::printf("  PSNR：AMaZE=%.2f dB  LMMSE=%.2f dB  RCD=%.2f dB  Bilinear=%.2f dB\n",
                amazePsnr, lmmsePsnr, rcdPsnr, bilinearPsnr);

    check(amazePsnr > 45.0, "AMaZE 在合成高细节图上 PSNR > 45 dB");
    check(lmmsePsnr > 44.0, "LMMSE 在合成高细节图上 PSNR > 44 dB");
    check(rcdPsnr > 44.0, "RCD 在合成高细节图上 PSNR > 44 dB");
    check(amazePsnr > bilinearPsnr + 3.0, "AMaZE 明显优于双线性基线");
    check(rcdPsnr > bilinearPsnr + 2.0, "RCD 明显优于双线性基线");

    // ---- 伪彩基准：中性高频图不应产生彩色条纹 ----
    //
    // 真值是严格中性灰（R=G=B）的 45° 高频条纹。去马赛克若方向判断失败，
    // 就会在条纹上产生绿 / 洋红伪彩；这里直接用 |R-G| + |B-G| 的均值度量。
    std::vector<float> neutralPattern(static_cast<std::size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            // 45° 正弦条纹，周期约 5.3 px：频率高于 CFA 的奈奎斯特附近，
            // 但又不是 2×2 对齐图案，能真正考验方向判断。
            const float pattern = 0.5f + 0.18f
                * std::sin(2.0f * kPi * static_cast<float>(x + y) / 5.3f);
            neutralPattern[static_cast<std::size_t>(y) * kWidth + x] = pattern;
        }
    }

    BayerImage neutralBayer;
    neutralBayer.width = kWidth;
    neutralBayer.height = kHeight;
    neutralBayer.pattern = BayerPattern::RGGB;
    neutralBayer.data.resize(static_cast<std::size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            neutralBayer.data[static_cast<std::size_t>(y) * kWidth + x] =
                neutralPattern[static_cast<std::size_t>(y) * kWidth + x];
        }
    }

    const auto chromaErrorOf = [&](DemosaicAlgorithm algorithm) {
        ImageBuffer image;
        const ErrorInfo error = ops::demosaic(neutralBayer, image, algorithm, 0.0f);
        if (!error.isOk() || image.isEmpty()) {
            return 1e9;
        }
        double sum = 0.0;
        std::size_t count = 0;
        for (int y = 6; y < kHeight - 6; ++y) {
            for (int x = 6; x < kWidth - 6; ++x) {
                const float r = image.at(x, y, 0);
                const float g = image.at(x, y, 1);
                const float b = image.at(x, y, 2);
                sum += std::fabs(r - g) + std::fabs(b - g);
                ++count;
            }
        }
        return sum / static_cast<double>(std::max<std::size_t>(count, 1u));
    };

    const double bilinearChroma = chromaErrorOf(DemosaicAlgorithm::Bilinear);
    const double amazeChroma = chromaErrorOf(DemosaicAlgorithm::AMAZE);
    const double rcdChroma = chromaErrorOf(DemosaicAlgorithm::RCD);
    std::printf("  中性高频图伪彩：Bilinear=%.5f  AMaZE=%.5f  RCD=%.5f\n",
                bilinearChroma, amazeChroma, rcdChroma);
    check(amazeChroma < bilinearChroma, "AMaZE 的伪彩低于双线性");
    check(rcdChroma < bilinearChroma, "RCD 的伪彩低于双线性");
}

// ===========================================================================
//  11.4 X-Trans（6x6 CFA）去马赛克
// ===========================================================================

void testXTransDemosaic()
{
    std::printf("\n[测试] X-Trans 去马赛克（6x6 CFA）\n");

    // 富士 X-Trans 的标准 6x6 排列（G=1, R=0, B=2）
    static const int kPattern[6][6] = {
        {1, 2, 1, 1, 0, 1},
        {0, 1, 0, 2, 1, 2},
        {1, 2, 1, 1, 0, 1},
        {1, 0, 1, 1, 2, 1},
        {2, 1, 2, 0, 1, 0},
        {1, 0, 1, 1, 2, 1},
    };

    constexpr int kWidth = 96;
    constexpr int kHeight = 96;
    constexpr float kPi = 3.14159265358979323846f;

    std::vector<float> groundTruth(static_cast<std::size_t>(kWidth) * kHeight * 3u);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float lumaDetail = 0.10f * (
                std::sin(2.0f * kPi * (fx + fy) / 11.0f)
                + std::sin(2.0f * kPi * fx / 15.0f)
                + std::sin(2.0f * kPi * fy / 19.0f)) / 3.0f;

            float r = 0.5f + 0.05f * std::sin(2.0f * kPi * fx / 61.0f) + lumaDetail;
            float g = 0.5f + 0.05f * std::sin(2.0f * kPi * fy / 67.0f) + lumaDetail;
            float b = 0.5f + 0.05f * std::cos(2.0f * kPi * (fx - fy) / 71.0f) + lumaDetail;

            const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 3u;
            groundTruth[index + 0u] = std::clamp(r, 0.0f, 1.0f);
            groundTruth[index + 1u] = std::clamp(g, 0.0f, 1.0f);
            groundTruth[index + 2u] = std::clamp(b, 0.0f, 1.0f);
        }
    }

    BayerImage bayer;
    bayer.width = kWidth;
    bayer.height = kHeight;
    bayer.pattern = BayerPattern::XTrans;
    bayer.cfaPeriod = kMaxCfaPeriod;
    for (int cy = 0; cy < kMaxCfaPeriod; ++cy) {
        for (int cx = 0; cx < kMaxCfaPeriod; ++cx) {
            bayer.cfaMap[static_cast<std::size_t>(cy) * kMaxCfaPeriod + cx] =
                static_cast<std::uint8_t>(kPattern[cy][cx]);
        }
    }
    bayer.data.resize(static_cast<std::size_t>(kWidth) * kHeight);

    int colorCounts[3] = {0, 0, 0};
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int channel = bayer.channelAt(x, y);
            ++colorCounts[channel];
            const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;
            bayer.data[index] =
                groundTruth[index * 3u + static_cast<std::size_t>(channel)];
        }
    }

    // 该 6x6 排列共 36 个位置：G 20 个、R 8 个、B 8 个
    check(colorCounts[0] * 9 == static_cast<int>(bayer.sampleCount()) * 2,
          "6x6 排列中 R 占 8/36");
    check(colorCounts[2] * 9 == static_cast<int>(bayer.sampleCount()) * 2,
          "6x6 排列中 B 占 8/36");
    check(colorCounts[1] * 9 == static_cast<int>(bayer.sampleCount()) * 5,
          "6x6 排列中 G 占 20/36");

    // channelAt 必须严格按 6x6 周期取值，且与排列表逐点一致
    bool phaseOk = true;
    for (int y = 0; y < kHeight && phaseOk; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (bayer.channelAt(x, y) != kPattern[y % 6][x % 6]) {
                phaseOk = false;
                break;
            }
            if (bayer.channelAt(x + 6, y + 6) != bayer.channelAt(x, y)) {
                phaseOk = false;
                break;
            }
        }
    }
    check(phaseOk, "channelAt 按 6x6 周期返回正确的通道");

    ImageBuffer image;
    const ErrorInfo error = ops::demosaic(bayer, image, DemosaicAlgorithm::Bilinear, 0.0f);
    check(error.isOk() && !image.isEmpty(), "X-Trans 去马赛克执行成功");

    if (error.isOk() && !image.isEmpty()) {
        // 原始采样点必须逐位保持（不把真实数据再平滑一次）
        bool samplesPreserved = true;
        for (int y = 6; y < kHeight - 6 && samplesPreserved; ++y) {
            for (int x = 6; x < kWidth - 6; ++x) {
                const int channel = bayer.channelAt(x, y);
                if (!near(image.at(x, y, channel), bayer.at(x, y), 1e-6f)) {
                    samplesPreserved = false;
                    break;
                }
            }
        }
        check(samplesPreserved, "原始 CFA 采样点在输出中逐位保留");

        double squaredError = 0.0;
        std::size_t count = 0;
        for (int y = 6; y < kHeight - 6; ++y) {
            for (int x = 6; x < kWidth - 6; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float diff = image.at(x, y, c)
                        - groundTruth[(static_cast<std::size_t>(y) * kWidth + x) * 3u
                                      + static_cast<std::size_t>(c)];
                    squaredError += static_cast<double>(diff) * diff;
                    ++count;
                }
            }
        }
        const double mse = squaredError / static_cast<double>(count);
        const double psnr = 10.0 * std::log10(1.0 / std::max(mse, 1e-12));

        // D4：Markesteijn 选项在通用结果上追加 3 遍方向色差精修
        ImageBuffer markesteijnImage;
        const ErrorInfo markesteijnError = ops::demosaic(
            bayer, markesteijnImage, DemosaicAlgorithm::Markesteijn, 0.0f);
        check(markesteijnError.isOk() && !markesteijnImage.isEmpty(),
              "Markesteijn 去马赛克执行成功");

        double markesteijnPsnr = 0.0;
        bool markesteijnSamplesPreserved = true;
        if (markesteijnError.isOk() && !markesteijnImage.isEmpty()) {
            double markesteijnErrorSum = 0.0;
            std::size_t markesteijnCount = 0;
            for (int y = 6; y < kHeight - 6; ++y) {
                for (int x = 6; x < kWidth - 6; ++x) {
                    const int channel = bayer.channelAt(x, y);
                    if (!near(markesteijnImage.at(x, y, channel), bayer.at(x, y), 1e-6f)) {
                        markesteijnSamplesPreserved = false;
                    }
                    for (int c = 0; c < 3; ++c) {
                        const float diff = markesteijnImage.at(x, y, c)
                            - groundTruth[(static_cast<std::size_t>(y) * kWidth + x) * 3u
                                          + static_cast<std::size_t>(c)];
                        markesteijnErrorSum += static_cast<double>(diff) * diff;
                        ++markesteijnCount;
                    }
                }
            }
            const double markesteijnMse =
                markesteijnErrorSum / static_cast<double>(markesteijnCount);
            markesteijnPsnr = 10.0 * std::log10(1.0 / std::max(markesteijnMse, 1e-12));
        }

        std::printf("  X-Trans PSNR = %.2f dB（通用） / %.2f dB（Markesteijn）\n",
                    psnr, markesteijnPsnr);
        check(psnr > 38.0, "X-Trans 去马赛克在合成图上 PSNR > 38 dB");
        check(markesteijnPsnr > 37.0, "Markesteijn 精修后 PSNR > 37 dB");
        check(markesteijnSamplesPreserved, "Markesteijn 精修保留全部原始采样");
    }

    // 2x2 合并对 X-Trans 必须明确拒绝，渲染层据此改走 RGB 域降采样
    ImageBuffer binned;
    const ErrorInfo binError = ops::binBayerToRgb(bayer, binned);
    check(!binError.isOk(), "X-Trans 调用 2x2 合并会被拒绝");

    // 饱和掩码：把部分采样设为饱和，掩码应记录对应通道
    BayerImage clipped = bayer;
    for (std::size_t i = 0; i < clipped.data.size(); i += 37) {
        clipped.data[i] = 1.0f;
    }
    std::vector<std::uint8_t> mask;
    check(ops::buildClipMaskFromBayer(clipped, mask), "X-Trans 饱和掩码生成成功");
    check(mask.size() == clipped.sampleCount(), "X-Trans 饱和掩码尺寸正确");

    // 拜耳域线性矩阵在通用 CFA 上应可执行且不产生 NaN
    BayerImage matrixBayer = bayer;
    const Matrix3x3 channelSwap{{
        {{0.0f, 1.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}},
    }};
    const ErrorInfo matrixError = ops::applyBayerLinearMatrix(matrixBayer, channelSwap);
    bool finite = true;
    for (float value : matrixBayer.data) {
        if (!std::isfinite(value)) {
            finite = false;
            break;
        }
    }
    check(matrixError.isOk() && finite, "X-Trans 拜耳域矩阵变换可执行且数值有限");
}

// ===========================================================================
//  11.4 小波降噪：噪声下降、边缘保持、强度为 0 时不变
// ===========================================================================

void testWaveletNoiseReduction()
{
    std::printf("\n[测试] 小波降噪\n");

    constexpr int kSize = 64;

    // 确定性伪随机噪声，保证测试可复现
    std::uint32_t seed = 0x12345678u;
    const auto nextNoise = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f - 0.5f) * 0.04f;
    };

    const auto lumaStd = [](const ImageBuffer& image) {
        const std::size_t pixels = image.pixelCount();
        double mean = 0.0;
        double square = 0.0;
        for (std::size_t i = 0; i < pixels; ++i) {
            const float y = 0.299f * image.data()[i * 3u]
                          + 0.587f * image.data()[i * 3u + 1]
                          + 0.114f * image.data()[i * 3u + 2];
            mean += y;
            square += static_cast<double>(y) * y;
        }
        mean /= static_cast<double>(pixels);
        return std::sqrt(std::max(0.0, square / static_cast<double>(pixels) - mean * mean));
    };

    // 情况 1：亮度噪声应随强度下降
    {
        ImageBuffer image(kSize, kSize);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float value = 0.5f + nextNoise();
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        const double before = lumaStd(image);
        ImageBuffer mild = image;
        ImageBuffer strong = image;
        ops::applyNoiseReduction(mild, 40.0f, 0.0f);
        ops::applyNoiseReduction(strong, 80.0f, 0.0f);

        const double afterMild = lumaStd(mild);
        const double afterStrong = lumaStd(strong);
        std::printf("  亮度标准差：原始=%.5f  40=%.5f  80=%.5f\n",
                    before, afterMild, afterStrong);

        check(afterMild < before, "亮度降噪 40 降低噪声");
        check(afterStrong < afterMild, "亮度降噪强度越高噪声越低");
    }

    // 情况 2：色度噪声下降
    {
        ImageBuffer image(kSize, kSize);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                image.at(x, y, 0) = 0.5f + nextNoise();
                image.at(x, y, 1) = 0.5f;
                image.at(x, y, 2) = 0.5f + nextNoise();
            }
        }

        const auto chromaStd = [](const ImageBuffer& img) {
            double square = 0.0;
            for (std::size_t i = 0; i < img.pixelCount(); ++i) {
                const double cr = img.data()[i * 3u] - img.data()[i * 3u + 1];
                square += cr * cr;
            }
            return std::sqrt(square / static_cast<double>(img.pixelCount()));
        };

        const double before = chromaStd(image);
        ImageBuffer denoised = image;
        ops::applyNoiseReduction(denoised, 0.0f, 50.0f);
        const double after = chromaStd(denoised);
        std::printf("  色度标准差：原始=%.5f  50=%.5f\n", before, after);
        check(after < before, "色度降噪 50 降低色度噪声");
    }

    // 情况 3：台阶边缘对比度必须保持
    {
        ImageBuffer image(kSize, kSize);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float base = (x < kSize / 2) ? 0.2f : 0.8f;
                const float value = base + nextNoise();
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        ImageBuffer denoised = image;
        ops::applyNoiseReduction(denoised, 60.0f, 0.0f);

        double leftMean = 0.0;
        double rightMean = 0.0;
        int count = 0;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 4; x < kSize / 2 - 4; ++x) {
                leftMean += denoised.at(x, y, 1);
                rightMean += denoised.at(x + kSize / 2, y, 1);
                ++count;
            }
        }
        leftMean /= count;
        rightMean /= count;
        std::printf("  台阶两侧均值：%.4f / %.4f\n", leftMean, rightMean);
        check(rightMean - leftMean > 0.55, "小波降噪保持台阶边缘对比度");
    }

    // 情况 4：强度为 0 时逐位不变
    {
        ImageBuffer image(kSize, kSize);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                image.at(x, y, 0) = nextNoise() + 0.5f;
                image.at(x, y, 1) = nextNoise() + 0.5f;
                image.at(x, y, 2) = nextNoise() + 0.5f;
            }
        }
        ImageBuffer copy = image;
        ops::applyNoiseReduction(copy, 0.0f, 0.0f);
        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "降噪强度为 0 时不改变任何像素");
    }
}

// ===========================================================================
//  11.5 色彩与质感（A1）
// ===========================================================================

namespace {

/// 感知域（sRGB 编码）下的色度：max - min。
/// 用于验证饱和度 / 自然饱和度是否按预期改变鲜艳程度。
float perceptualChroma(float r, float g, float b)
{
    const float pr = math::linearToSrgbGamma(std::max(0.0f, r));
    const float pg = math::linearToSrgbGamma(std::max(0.0f, g));
    const float pb = math::linearToSrgbGamma(std::max(0.0f, b));
    return std::max({pr, pg, pb}) - std::min({pr, pg, pb});
}

/// 感知域亮度（Rec.709 系数），与 applyPresence 内部一致。
float perceptualLuma(float r, float g, float b)
{
    const float pr = math::linearToSrgbGamma(std::max(0.0f, r));
    const float pg = math::linearToSrgbGamma(std::max(0.0f, g));
    const float pb = math::linearToSrgbGamma(std::max(0.0f, b));
    return 0.2126f * pr + 0.7152f * pg + 0.0722f * pb;
}

}  // namespace

void testPresenceAdjustments()
{
    std::printf("\n[测试] 色彩与质感（A1）\n");

    // ---- 1) 全零时逐位不变 ----
    {
        ImageBuffer image(24, 16);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float u = static_cast<float>(x) / 23.0f;
                const float v = static_cast<float>(y) / 15.0f;
                image.at(x, y, 0) = 0.05f + 0.6f * u;
                image.at(x, y, 1) = 0.10f + 0.5f * v;
                image.at(x, y, 2) = 0.80f - 0.4f * u;
            }
        }

        ImageBuffer copy = image;
        const PresenceSettings defaults;
        check(defaults.isIdentity(), "默认参数被识别为恒等");

        ops::applyPresence(copy, defaults);

        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "四项全为 0 时不改变任何像素");
    }

    // ---- 2) 饱和度 -100：所有像素去色，亮度结构保持 ----
    {
        ImageBuffer image(32, 32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float u = static_cast<float>(x) / 31.0f;
                const float v = static_cast<float>(y) / 31.0f;
                image.at(x, y, 0) = 0.05f + 0.70f * u;
                image.at(x, y, 1) = 0.10f + 0.55f * v;
                image.at(x, y, 2) = 0.90f - 0.50f * u;
            }
        }

        ImageBuffer copy = image;
        PresenceSettings settings;
        settings.saturation = -100.0f;
        ops::applyPresence(copy, settings);

        bool gray = true;
        float maxLumaError = 0.0f;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float r = copy.at(x, y, 0);
                const float g = copy.at(x, y, 1);
                const float b = copy.at(x, y, 2);
                gray = gray && (std::max({r, g, b}) - std::min({r, g, b}) < 1e-4f);
                maxLumaError = std::max(maxLumaError,
                    std::fabs(perceptualLuma(image.at(x, y, 0), image.at(x, y, 1),
                                             image.at(x, y, 2))
                              - perceptualLuma(r, g, b)));
            }
        }
        check(gray, "饱和度 -100 去掉全部色度");
        check(maxLumaError < 1e-4f, "去色时感知亮度保持不变");
    }

    // ---- 3) 饱和度 +100：色度约翻倍 ----
    {
        ImageBuffer image(4, 4);
        image.fill(0.0f);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.at(x, y, 0) = 0.55f;
                image.at(x, y, 1) = 0.25f;
                image.at(x, y, 2) = 0.12f;
            }
        }

        const float before = perceptualChroma(0.55f, 0.25f, 0.12f);
        ImageBuffer copy = image;
        PresenceSettings settings;
        settings.saturation = 100.0f;
        ops::applyPresence(copy, settings);
        const float after = perceptualChroma(copy.at(0, 0, 0), copy.at(0, 0, 1),
                                             copy.at(0, 0, 2));

        check(after > before * 1.8f && after < before * 2.2f,
              "饱和度 +100 使色度约翻倍");
    }

    // ---- 4) 自然饱和度的肤色保护 ----
    {
        ImageBuffer image(32, 8);
        // 左右两半的感知饱和度相同，仅色相不同：左橙（肤色附近）、右蓝
        const auto toLinear = [](float encoded) {
            return math::srgbGammaToLinear(encoded);
        };
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (x < image.width() / 2) {
                    image.at(x, y, 0) = toLinear(0.85f);
                    image.at(x, y, 1) = toLinear(0.55f);
                    image.at(x, y, 2) = toLinear(0.25f);
                } else {
                    image.at(x, y, 0) = toLinear(0.25f);
                    image.at(x, y, 1) = toLinear(0.45f);
                    image.at(x, y, 2) = toLinear(0.85f);
                }
            }
        }

        ImageBuffer copy = image;
        PresenceSettings settings;
        settings.vibrance = 100.0f;
        ops::applyPresence(copy, settings);

        const int orangeX = image.width() / 4;
        const int blueX = image.width() * 3 / 4;
        const float orangeBefore = perceptualChroma(
            image.at(orangeX, 0, 0), image.at(orangeX, 0, 1), image.at(orangeX, 0, 2));
        const float orangeAfter = perceptualChroma(
            copy.at(orangeX, 0, 0), copy.at(orangeX, 0, 1), copy.at(orangeX, 0, 2));
        const float blueBefore = perceptualChroma(
            image.at(blueX, 0, 0), image.at(blueX, 0, 1), image.at(blueX, 0, 2));
        const float blueAfter = perceptualChroma(
            copy.at(blueX, 0, 0), copy.at(blueX, 0, 1), copy.at(blueX, 0, 2));

        const float orangeGain = orangeAfter - orangeBefore;
        const float blueGain = blueAfter - blueBefore;
        check(blueGain > orangeGain * 1.2f,
              "自然饱和度对橙色肤色的提升小于蓝色");
        check(blueGain > 0.0f, "自然饱和度 +100 仍会提升蓝色");
    }

    // ---- 5) 清晰度：台阶边缘局部对比增强 ----
    {
        ImageBuffer image(64, 8);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float value = (x < image.width() / 2) ? 0.22f : 0.72f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        ImageBuffer copy = image;
        PresenceSettings settings;
        settings.clarity = 100.0f;
        ops::applyPresence(copy, settings);

        float maxBefore = 0.0f;
        float maxAfter = 0.0f;
        for (int x = 0; x < image.width(); ++x) {
            maxBefore = std::max(maxBefore, image.at(x, 0, 0));
            maxAfter = std::max(maxAfter, copy.at(x, 0, 0));
        }
        check(maxAfter > maxBefore + 0.01f, "清晰度 +100 在台阶边缘产生过冲（局部对比增强）");

        // 平坦灰图不应被清晰度改变（高通为零）
        ImageBuffer flat(32, 32);
        flat.fill(0.4f);
        ImageBuffer flatCopy = flat;
        ops::applyPresence(flatCopy, settings);
        bool flatUnchanged = true;
        for (std::size_t i = 0; i < flat.sampleCount(); ++i) {
            flatUnchanged = flatUnchanged
                && std::fabs(flat.data()[i] - flatCopy.data()[i]) < 1e-6f;
        }
        check(flatUnchanged, "清晰度不改变平坦区域");

        // 大图会走「降采样 → 小图高斯 → 双线性放大」的快速路径，
        // 需要确认它仍然保留了清晰度的高通语义。
        ImageBuffer large(6000, 32);
        for (int y = 0; y < large.height(); ++y) {
            for (int x = 0; x < large.width(); ++x) {
                const float value = (x < large.width() / 2) ? 0.22f : 0.72f;
                large.at(x, y, 0) = value;
                large.at(x, y, 1) = value;
                large.at(x, y, 2) = value;
            }
        }

        ImageBuffer largeCopy = large;
        ops::applyPresence(largeCopy, settings);

        float largeMaxBefore = 0.0f;
        float largeMaxAfter = 0.0f;
        bool finite = true;
        for (int x = 0; x < large.width(); ++x) {
            largeMaxBefore = std::max(largeMaxBefore, large.at(x, 0, 0));
            largeMaxAfter = std::max(largeMaxAfter, largeCopy.at(x, 0, 0));
            finite = finite && std::isfinite(largeCopy.at(x, 0, 0));
        }
        check(largeMaxAfter > largeMaxBefore + 0.01f,
              "大图清晰度快速路径仍增强台阶边缘");
        check(finite, "大图清晰度快速路径输出有限值");
    }

    // ---- 6) 纹理：细密条纹的局部对比可增可减 ----
    //
    // 用 6000px 长边，让 filterScaleFor 回到 1.0，从而验证全分辨率下
    // 纹理半径（约 3px 支撑域）对 2px 条纹的实际效果；小图会触发
    // 半径下限，高通几乎为零，无法体现参数语义。
    {
        ImageBuffer image(6000, 32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float value = (((x / 2) % 2) == 0) ? 0.35f : 0.65f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        const auto stdDev = [](const ImageBuffer& buffer) {
            double sum = 0.0;
            double sumSq = 0.0;
            const std::size_t count = buffer.pixelCount();
            for (std::size_t i = 0; i < count; ++i) {
                const double value = perceptualLuma(
                    buffer.data()[i * 3], buffer.data()[i * 3 + 1],
                    buffer.data()[i * 3 + 2]);
                sum += value;
                sumSq += value * value;
            }
            const double mean = sum / static_cast<double>(count);
            return std::sqrt(std::max(0.0, sumSq / static_cast<double>(count)
                                               - mean * mean));
        };

        const double base = stdDev(image);

        ImageBuffer increased = image;
        PresenceSettings plus;
        plus.texture = 100.0f;
        ops::applyPresence(increased, plus);

        ImageBuffer decreased = image;
        PresenceSettings minus;
        minus.texture = -100.0f;
        ops::applyPresence(decreased, minus);

        check(stdDev(increased) > base * 1.05, "纹理 +100 增强细密条纹对比");
        check(stdDev(decreased) < base * 0.95, "纹理 -100 减弱细密条纹对比");
    }
}

// ===========================================================================
//  11.6 颜色分级与分离色调（A2）
// ===========================================================================

void testColorGrading()
{
    std::printf("\n[测试] 颜色分级与分离色调（A2）\n");

    // 左半暗、右半亮的灰阶图，便于分别检查阴影 / 高光区域
    const auto makeSplitNeutral = []() {
        ImageBuffer image(8, 8);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float value = (x < image.width() / 2) ? 0.03f : 0.80f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }
        return image;
    };

    // ---- 1) 恒等 ----
    {
        ImageBuffer image = makeSplitNeutral();
        ImageBuffer copy = image;
        const ColorGradingSettings grading;
        const SplitToningSettings split;
        check(grading.isIdentity() && split.isIdentity(), "默认分级 / 分离色调为恒等");

        ops::applyColorGrading(copy, grading, split);

        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "参数为默认时逐位不改变图像");
    }

    // ---- 2) 阴影偏红：只影响暗部 ----
    {
        ImageBuffer image = makeSplitNeutral();
        ImageBuffer copy = image;

        ColorGradingSettings grading;
        grading.shadows.hue = 0.0f;       // 红
        grading.shadows.saturation = 100.0f;
        ops::applyColorGrading(copy, grading, SplitToningSettings{});

        const float darkChroma = perceptualChroma(
            copy.at(1, 1, 0), copy.at(1, 1, 1), copy.at(1, 1, 2));
        const float brightChroma = perceptualChroma(
            copy.at(6, 1, 0), copy.at(6, 1, 1), copy.at(6, 1, 2));

        check(copy.at(1, 1, 0) > copy.at(1, 1, 1), "阴影着色让暗部偏红");
        check(darkChroma > 0.05f, "阴影着色产生可见色偏");
        check(darkChroma > brightChroma * 3.0f, "阴影着色基本不影响高光");
    }

    // ---- 3) 高光偏蓝：只影响亮部 ----
    {
        ImageBuffer image = makeSplitNeutral();
        ImageBuffer copy = image;

        ColorGradingSettings grading;
        grading.highlights.hue = 240.0f;  // 蓝
        grading.highlights.saturation = 100.0f;
        ops::applyColorGrading(copy, grading, SplitToningSettings{});

        const float darkChroma = perceptualChroma(
            copy.at(1, 1, 0), copy.at(1, 1, 1), copy.at(1, 1, 2));
        const float brightChroma = perceptualChroma(
            copy.at(6, 1, 0), copy.at(6, 1, 1), copy.at(6, 1, 2));

        check(copy.at(6, 1, 2) > copy.at(6, 1, 0), "高光着色让亮部偏蓝");
        check(brightChroma > 0.05f, "高光着色产生可见色偏");
        check(brightChroma > darkChroma * 3.0f, "高光着色基本不影响暗部");
    }

    // ---- 4) 中间调偏绿：只在中间调最强 ----
    {
        ImageBuffer image(8, 8);
        image.fill(0.18f);  // 感知中灰附近

        ImageBuffer copy = image;
        ColorGradingSettings grading;
        grading.midtones.hue = 120.0f;  // 绿
        grading.midtones.saturation = 100.0f;
        ops::applyColorGrading(copy, grading, SplitToningSettings{});

        check(copy.at(4, 4, 1) > copy.at(4, 4, 0)
                  && copy.at(4, 4, 1) > copy.at(4, 4, 2),
              "中间调着色让中灰偏绿");
        check(perceptualChroma(copy.at(4, 4, 0), copy.at(4, 4, 1),
                               copy.at(4, 4, 2)) > 0.05f,
              "中间调着色产生可见色偏");
    }

    // ---- 5) 明度偏移只作用于对应区域 ----
    {
        ImageBuffer image = makeSplitNeutral();
        ImageBuffer copy = image;

        ColorGradingSettings grading;
        grading.shadows.saturation = 0.0f;
        grading.shadows.luminance = 100.0f;
        ops::applyColorGrading(copy, grading, SplitToningSettings{});

        const float darkBefore = perceptualLuma(
            image.at(1, 1, 0), image.at(1, 1, 1), image.at(1, 1, 2));
        const float darkAfter = perceptualLuma(
            copy.at(1, 1, 0), copy.at(1, 1, 1), copy.at(1, 1, 2));
        const float brightBefore = perceptualLuma(
            image.at(6, 1, 0), image.at(6, 1, 1), image.at(6, 1, 2));
        const float brightAfter = perceptualLuma(
            copy.at(6, 1, 0), copy.at(6, 1, 1), copy.at(6, 1, 2));

        check(darkAfter > darkBefore + 0.05f, "阴影明度 +100 提亮暗部");
        check(std::fabs(brightAfter - brightBefore) < 0.01f,
              "阴影明度偏移基本不影响高光");
    }

    // ---- 6) 分离色调：阴影红、高光蓝 ----
    {
        ImageBuffer image = makeSplitNeutral();
        ImageBuffer copy = image;

        SplitToningSettings split;
        split.shadowsHue = 0.0f;
        split.shadowsSaturation = 100.0f;
        split.highlightsHue = 240.0f;
        split.highlightsSaturation = 100.0f;
        ops::applyColorGrading(copy, ColorGradingSettings{}, split);

        check(copy.at(1, 1, 0) > copy.at(1, 1, 2), "分离色调让阴影偏红");
        check(copy.at(6, 1, 2) > copy.at(6, 1, 0), "分离色调让高光偏蓝");
    }

    // ---- 7) 平衡：负值更偏阴影，正值更偏高光 ----
    {
        ImageBuffer image(8, 8);
        image.fill(0.18f);

        ColorGradingSettings negative;
        negative.shadows.hue = 0.0f;
        negative.shadows.saturation = 100.0f;
        negative.balance = -100.0f;

        ColorGradingSettings positive = negative;
        positive.balance = 100.0f;

        ImageBuffer negativeCopy = image;
        ImageBuffer positiveCopy = image;
        ops::applyColorGrading(negativeCopy, negative, SplitToningSettings{});
        ops::applyColorGrading(positiveCopy, positive, SplitToningSettings{});

        const float negativeChroma = perceptualChroma(
            negativeCopy.at(4, 4, 0), negativeCopy.at(4, 4, 1),
            negativeCopy.at(4, 4, 2));
        const float positiveChroma = perceptualChroma(
            positiveCopy.at(4, 4, 0), positiveCopy.at(4, 4, 1),
            positiveCopy.at(4, 4, 2));

        check(negativeChroma > positiveChroma,
              "平衡向阴影时中间调获得更多阴影着色");
    }

    // ---- 8) 混合：过渡越宽，阴影越能影响中间调 ----
    {
        ImageBuffer image(8, 8);
        image.fill(0.18f);

        ColorGradingSettings narrow;
        narrow.shadows.hue = 0.0f;
        narrow.shadows.saturation = 100.0f;
        narrow.blending = 0.0f;

        ColorGradingSettings wide = narrow;
        wide.blending = 100.0f;

        ImageBuffer narrowCopy = image;
        ImageBuffer wideCopy = image;
        ops::applyColorGrading(narrowCopy, narrow, SplitToningSettings{});
        ops::applyColorGrading(wideCopy, wide, SplitToningSettings{});

        const float narrowChroma = perceptualChroma(
            narrowCopy.at(4, 4, 0), narrowCopy.at(4, 4, 1), narrowCopy.at(4, 4, 2));
        const float wideChroma = perceptualChroma(
            wideCopy.at(4, 4, 0), wideCopy.at(4, 4, 1), wideCopy.at(4, 4, 2));

        check(wideChroma > narrowChroma,
              "混合越大，阴影着色向中间调过渡越多");
    }
}

// ===========================================================================
//  11.7 黑白混色（A3）
// ===========================================================================

void testBlackWhiteMix()
{
    std::printf("\n[测试] 黑白混色（A3）\n");

    // 左半红、右半蓝；两半的感知亮度接近，便于比较相对变化
    const auto makeRedBlue = []() {
        ImageBuffer image(8, 8);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (x < image.width() / 2) {
                    image.at(x, y, 0) = 0.80f;
                    image.at(x, y, 1) = 0.30f;
                    image.at(x, y, 2) = 0.30f;
                } else {
                    image.at(x, y, 0) = 0.30f;
                    image.at(x, y, 1) = 0.30f;
                    image.at(x, y, 2) = 0.80f;
                }
            }
        }
        return image;
    };

    // ---- 1) 未启用时逐位不变 ----
    {
        ImageBuffer image = makeRedBlue();
        ImageBuffer copy = image;
        const BlackWhiteSettings settings;
        check(settings.isIdentity(), "未启用时黑白混色为恒等");

        ops::applyBlackWhite(copy, settings);

        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "未启用时逐位不改变图像");
    }

    // ---- 2) 启用且七通道全零：标准黑白，亮度保持 ----
    {
        ImageBuffer image = makeRedBlue();
        ImageBuffer copy = image;

        BlackWhiteSettings settings;
        settings.enabled = true;
        ops::applyBlackWhite(copy, settings);

        bool gray = true;
        float maxLumaError = 0.0f;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float r = copy.at(x, y, 0);
                const float g = copy.at(x, y, 1);
                const float b = copy.at(x, y, 2);
                gray = gray && (std::max({r, g, b}) - std::min({r, g, b}) < 1e-5f);
                maxLumaError = std::max(maxLumaError,
                    std::fabs(perceptualLuma(image.at(x, y, 0), image.at(x, y, 1),
                                             image.at(x, y, 2))
                              - perceptualLuma(r, g, b)));
            }
        }
        check(gray, "启用黑白后输出严格中性灰");
        check(maxLumaError < 1e-4f, "标准黑白保持感知亮度");
    }

    // ---- 3) 红色通道提亮：红花变亮，蓝天基本不变 ----
    {
        ImageBuffer image = makeRedBlue();
        ImageBuffer copy = image;

        BlackWhiteSettings settings;
        settings.enabled = true;
        settings.luminance[0] = 100.0f;  // 红
        ops::applyBlackWhite(copy, settings);

        const float redBefore = perceptualLuma(
            image.at(1, 1, 0), image.at(1, 1, 1), image.at(1, 1, 2));
        const float redAfter = perceptualLuma(
            copy.at(1, 1, 0), copy.at(1, 1, 1), copy.at(1, 1, 2));
        const float blueBefore = perceptualLuma(
            image.at(6, 1, 0), image.at(6, 1, 1), image.at(6, 1, 2));
        const float blueAfter = perceptualLuma(
            copy.at(6, 1, 0), copy.at(6, 1, 1), copy.at(6, 1, 2));

        check(redAfter > redBefore + 0.08f, "红色通道 +100 提亮红色物体");
        check(std::fabs(blueAfter - blueBefore) < 0.03f,
              "红色通道调整基本不影响蓝色物体");
    }

    // ---- 4) 蓝色通道压暗：蓝色变暗 ----
    {
        ImageBuffer image = makeRedBlue();
        ImageBuffer copy = image;

        BlackWhiteSettings settings;
        settings.enabled = true;
        settings.luminance[5] = -100.0f;  // 蓝
        ops::applyBlackWhite(copy, settings);

        const float blueBefore = perceptualLuma(
            image.at(6, 1, 0), image.at(6, 1, 1), image.at(6, 1, 2));
        const float blueAfter = perceptualLuma(
            copy.at(6, 1, 0), copy.at(6, 1, 1), copy.at(6, 1, 2));

        check(blueAfter < blueBefore - 0.05f, "蓝色通道 -100 压暗蓝色物体");
    }
}

// ===========================================================================
//  12. 预览影调基准（自动曝光归一 + 弱 S 曲线）
// ===========================================================================

void testPreviewTone()
{
    std::printf("\n[测试] 预览影调基准\n");

    // 情况 1：暗背景 + 少量高光 → 按 99.5% 分位归一
    {
        ImageBuffer image(100, 10);
        image.fill(0.05f);
        // 末尾 5% 的像素是画面中的亮部
        for (int y = 0; y < 10; ++y) {
            for (int x = 95; x < 100; ++x) {
                image.at(x, y, 0) = 0.2f;
                image.at(x, y, 1) = 0.2f;
                image.at(x, y, 2) = 0.2f;
            }
        }

        const float scale = ops::computeAutoExposureScale(image, 0.995f);
        check(near(scale, 5.0f, 0.05f),
              "暗背景按 99.5% 分位归一（0.2 → 白点 1.0）");
    }

    // 情况 1b：大面积均匀高调画面不应被抬成纯白（中位亮度封顶）
    {
        ImageBuffer image(100, 10);
        image.fill(0.5f);

        const float scale = ops::computeAutoExposureScale(image, 0.995f);
        check(near(scale, 1.0f, 1e-4f),
              "大面积高调画面不做提亮（避免整体发灰、丢失层次）");
    }

    // 情况 2：极少数高光点不应拉低整体增益
    {
        ImageBuffer image(1000, 1);
        image.fill(0.05f);
        // 9.5% 的亮部（0.2）决定 99.5% 分位，
        // 末尾 0.5% 是镜面反光级别的超亮点（4.0），不参与白点
        for (int x = 900; x < 995; ++x) {
            image.at(x, 0, 0) = 0.2f;
            image.at(x, 0, 1) = 0.2f;
            image.at(x, 0, 2) = 0.2f;
        }
        for (int x = 995; x < 1000; ++x) {
            image.at(x, 0, 0) = 4.0f;
            image.at(x, 0, 1) = 4.0f;
            image.at(x, 0, 2) = 4.0f;
        }

        const float scale = ops::computeAutoExposureScale(image, 0.995f);
        check(scale > 4.9f && scale < 5.1f,
              "少量极端高光不把增益拉低（分位数而非最大值）");
    }

    // 情况 3：极亮画面按白点归一后整体压暗，并受 ±3 EV 限制
    {
        ImageBuffer image(10, 10);
        image.fill(8.0f);

        const float scale = ops::computeAutoExposureScale(image, 0.995f);
        check(near(scale, 0.125f, 1e-4f), "极亮画面压暗到白点，且增益限制在 -3 EV");
    }

    // 情况 4：空图像返回 1.0，不做任何缩放
    {
        ImageBuffer empty;
        check(near(ops::computeAutoExposureScale(empty, 0.995f), 1.0f, 1e-6f),
              "空图像返回 1.0 增益");
    }

    // 情况 5：仅曝光归一，不开启 S 曲线时只做线性缩放
    {
        ImageBuffer image(4, 4);
        image.fill(0.25f);

        ops::applyPreviewTone(image, 2.0f, false);
        check(near(image.at(0, 0, 0), 0.5f, 1e-5f), "线性基准预览只做曝光缩放");
    }

    // 情况 6：弱 S 曲线围绕感知域中灰不动，暗部略压、亮部略提
    {
        ImageBuffer mid(1, 1);
        mid.fill(0.214f);  // 约等于 sRGB 编码 0.5 的线性值
        ops::applyPreviewTone(mid, 1.0f, true);
        check(near(mid.at(0, 0, 0), 0.214f, 1e-4f), "S 曲线不改变感知中灰");

        ImageBuffer shadows(1, 1);
        shadows.fill(0.02f);
        ops::applyPreviewTone(shadows, 1.0f, true);
        check(shadows.at(0, 0, 0) < 0.02f, "S 曲线压暗暗部");

        ImageBuffer highlights(1, 1);
        highlights.fill(0.8f);
        ops::applyPreviewTone(highlights, 1.0f, true);
        check(highlights.at(0, 0, 0) > 0.8f, "S 曲线提亮亮部");
    }

    // 情况 7：增益把高光推过 1.0 时，S 曲线不得硬截断
    {
        ImageBuffer image(2, 2);
        image.fill(0.8f);

        ops::applyPreviewTone(image, 2.0f, true);
        check(image.at(0, 0, 0) > 1.5f,
              "超过白点的高光原样保留，交给后续软膝压缩");
    }

    // 情况 8：无效增益退化为 1.0，S 关闭时是空操作
    {
        ImageBuffer image(2, 2);
        image.fill(0.3f);

        ops::applyPreviewTone(image, std::nanf(""), false);
        check(near(image.at(0, 0, 0), 0.3f, 1e-6f), "无效增益不改变图像");
    }
}

// ===========================================================================
//  13. 彩边 / 色散校正
// ===========================================================================

/// 构造一张填充指定通道值的 RGGB 拜耳图
BayerImage makeFlatBayer(int width, int height, float red, float green, float blue)
{
    BayerImage bayer;
    bayer.width = width;
    bayer.height = height;
    bayer.pattern = BayerPattern::RGGB;
    bayer.data.assign(static_cast<std::size_t>(width) * height, 0.0f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int channel = bayerChannelAt(bayer.pattern, x, y);
            bayer.at(x, y) = (channel == 0) ? red : (channel == 1 ? green : blue);
        }
    }
    return bayer;
}

void testFalseColorCorrections()
{
    std::printf("\n[测试] 彩边 / 色散校正\n");

    // ---- 串扰补偿 ----
    {
        // 强度 0：完全不补偿，数据保持不变
        BayerImage bayer = makeFlatBayer(8, 8, 0.4f, 0.5f, 0.6f);
        const BayerImage original = bayer;
        Matrix3x3 matrix = identityMatrix3x3();
        matrix[0][2] = -0.1f;

        ops::applyCrosstalkCompensation(bayer, matrix, 0.0f);
        bool unchanged = true;
        for (std::size_t i = 0; i < original.data.size(); ++i) {
            if (!near(bayer.data[i], original.data[i], 1e-6f)) {
                unchanged = false;
            }
        }
        check(unchanged, "串扰补偿强度为 0 时不改变数据");
    }

    {
        // 强度 1：R 应减去 10% 的 B（解耦矩阵第一行第三列为 -0.1）
        BayerImage bayer = makeFlatBayer(8, 8, 0.4f, 0.5f, 0.6f);
        Matrix3x3 matrix = identityMatrix3x3();
        matrix[0][2] = -0.1f;

        ops::applyCrosstalkCompensation(bayer, matrix, 1.0f);

        // (2,2) 是 R 位置，B 的双线性插值在平坦区恒为 0.6
        check(near(bayer.at(2, 2), 0.34f, 1e-4f),
              "串扰补偿按解耦矩阵混合通道");
        check(near(bayer.at(3, 2), 0.5f, 1e-4f),
              "G 位置只保留自身的 G 值（拜耳结构不变）");
    }

    {
        // 拜耳域线性矩阵：单位矩阵应完全等价于原数据
        BayerImage bayer = makeFlatBayer(6, 6, 0.2f, 0.4f, 0.6f);
        const BayerImage original = bayer;
        check(ops::applyBayerLinearMatrix(bayer, identityMatrix3x3()).isOk(),
              "拜耳域单位矩阵变换成功");
        bool unchanged = true;
        for (std::size_t i = 0; i < original.data.size(); ++i) {
            if (!near(bayer.data[i], original.data[i], 1e-5f)) {
                unchanged = false;
            }
        }
        check(unchanged, "单位矩阵不改变拜耳数据");
    }

    // ---- FBDD ----
    {
        BayerImage bayer = makeFlatBayer(9, 9, 0.5f, 0.5f, 0.5f);
        // 在 (4,4)（R 位置）放一个孤立的异常高值
        bayer.at(4, 4) = 0.95f;

        ops::applyFbddDenoise(bayer, 1.0f);

        check(bayer.at(4, 4) < 0.55f, "FBDD 压低孤立异常采样");
        check(near(bayer.at(2, 2), 0.5f, 1e-5f), "FBDD 不改变平坦区域");
        check(near(bayer.at(4, 6), 0.5f, 1e-5f), "FBDD 不误伤正常邻域");
    }

    // ---- 高光 Roll-off ----
    {
        ImageBuffer image(16, 1);
        for (int x = 0; x < 16; ++x) {
            const float v = static_cast<float>(x) / 15.0f;
            image.at(x, 0, 0) = v;
            image.at(x, 0, 1) = v;
            image.at(x, 0, 2) = v;
        }
        const float belowKnee = image.at(8, 0, 0);  // 0.533，低于 0.9 拐点

        ops::applyHighlightRolloff(image, 0.9f, 0.5f);

        check(near(image.at(8, 0, 0), belowKnee, 1e-6f),
              "Roll-off 不改变拐点以下的数值");
        check(image.at(15, 0, 0) <= 1.0f && image.at(15, 0, 0) > 0.9f,
              "Roll-off 把高光压缩到拐点与 1.0 之间");

        bool monotonic = true;
        for (int x = 1; x < 16; ++x) {
            if (image.at(x, 0, 0) < image.at(x - 1, 0, 0) - 1e-6f) {
                monotonic = false;
            }
        }
        check(monotonic, "Roll-off 保持亮度单调");
    }

    // ---- 暗电流补偿（需求 v2 §2.2）----
    {
        // 内置曲线：ISO 越高偏移越大，且设有上限
        DarkCurrentSettings dark;
        check(dark.offsetForIso(100.0f) <= 1e-6f,
              "ISO 100 时内置暗电流偏移约为 0");
        check(dark.offsetForIso(3200.0f) > dark.offsetForIso(800.0f),
              "内置暗电流偏移随 ISO 单调增大");
        check(dark.offsetForIso(102400.0f) <= 0.004f + 1e-6f,
              "内置暗电流偏移不超过上限 0.004");

        // 机型配置的 ISO 偏移表优先于内置曲线，并做线性插值
        DarkCurrentSettings custom;
        custom.isoOffsets = {{100.0f, 0.001f}, {800.0f, 0.005f}};
        check(near(custom.offsetForIso(100.0f), 0.001f, 1e-6f),
              "机型偏移表命中节点值");
        check(near(custom.offsetForIso(450.0f), 0.003f, 1e-6f),
              "机型偏移表在节点之间线性插值");
        check(near(custom.offsetForIso(50.0f), 0.001f, 1e-6f),
              "低于表内最低 ISO 时取首项");
        check(near(custom.offsetForIso(6400.0f), 0.005f, 1e-6f),
              "高于表内最高 ISO 时取末项");
    }

    {
        // 补偿量 = 强度 × 偏移，且不会把采样压成负数
        BayerImage bayer;
        bayer.width = 4;
        bayer.height = 1;
        bayer.pattern = BayerPattern::RGGB;
        bayer.data = {0.5f, 0.02f, 0.001f, 0.3f};

        ops::applyDarkCurrentCompensation(bayer, 0.5f, 0.02f);

        check(near(bayer.at(0, 0), 0.49f, 1e-6f),
              "暗电流按 强度×偏移 从暗部扣除");
        check(bayer.at(1, 0) >= 0.0f && near(bayer.at(1, 0), 0.01f, 1e-6f),
              "暗电流扣除后仍保持非负");
        check(near(bayer.at(2, 0), 0.0f, 1e-6f),
              "低于偏移量的采样被压到 0");
        check(near(bayer.at(3, 0), 0.29f, 1e-6f),
              "高光采样同样只扣除固定偏移");
    }

    // ---- LCA ----
    {
        // 水平渐变，便于观察 R 通道的亚像素缩放
        ImageBuffer image(61, 61);
        for (int y = 0; y < 61; ++y) {
            for (int x = 0; x < 61; ++x) {
                const float v = static_cast<float>(x) / 60.0f;
                image.at(x, y, 0) = v;
                image.at(x, y, 1) = v;
                image.at(x, y, 2) = v;
            }
        }
        const ImageBuffer original = image;

        LcaSettings lca;
        lca.redScale = 1.05f;
        lca.blueScale = 1.0f;
        lca.radialStrength = 0.0f;
        ops::applyLateralChromaticAberration(image, lca.redScale, lca.blueScale,
                                             lca.radialStrength);

        check(near(image.at(30, 30, 0), original.at(30, 30, 0), 1e-5f),
              "LCA 中心点不变化");
        check(image.at(0, 30, 0) > original.at(0, 30, 0) + 1e-3f,
              "LCA 把边缘 R 通道向中心重采样");
        check(near(image.at(0, 30, 1), original.at(0, 30, 1), 1e-6f),
              "LCA 不修改 G 通道");
        check(near(image.at(0, 30, 2), original.at(0, 30, 2), 1e-6f),
              "LCA 蓝色缩放为 1.0 时不修改 B 通道");
    }

    {
        // 径向强度：同一基础缩放下，四角的校正量应大于画面中心附近
        ImageBuffer plain(61, 61);
        ImageBuffer radial(61, 61);
        for (int y = 0; y < 61; ++y) {
            for (int x = 0; x < 61; ++x) {
                const float v = static_cast<float>(x) / 60.0f;
                plain.at(x, y, 0) = v;
                plain.at(x, y, 1) = v;
                plain.at(x, y, 2) = v;
                radial.at(x, y, 0) = v;
                radial.at(x, y, 1) = v;
                radial.at(x, y, 2) = v;
            }
        }
        const ImageBuffer original = plain;

        ops::applyLateralChromaticAberration(plain, 1.02f, 1.0f, 0.0f);
        ops::applyLateralChromaticAberration(radial, 1.02f, 1.0f, 0.008f);

        const float plainDelta = std::fabs(plain.at(0, 0, 0) - original.at(0, 0, 0));
        const float radialDelta = std::fabs(radial.at(0, 0, 0) - original.at(0, 0, 0));
        check(radialDelta > plainDelta + 1e-5f,
              "径向强度让四角的 LCA 校正量更大");
        check(near(radial.at(30, 30, 0), original.at(30, 30, 0), 1e-5f),
              "径向强度不改变画面中心的 R 通道");
    }

    // ---- LoCA ----
    {
        // 平坦中间调：无高光光斑，应保持不变
        ImageBuffer flat(16, 16);
        flat.fill(0.5f);
        const ImageBuffer original = flat;

        ops::applyLongitudinalChromaticAberration(flat, 1.0f, 0.75f);

        bool unchanged = true;
        for (std::size_t i = 0; i < original.sampleCount(); ++i) {
            if (!near(flat.data()[i], original.data()[i], 1e-6f)) {
                unchanged = false;
            }
        }
        check(unchanged, "LoCA 对无高光光斑的画面不做处理");
    }

    // ---- 边缘导向伪色抑制 ----
    {
        // 平坦彩色图：没有 G 梯度，任何通道都不应变化
        ImageBuffer flat(9, 9);
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 9; ++x) {
                flat.at(x, y, 0) = 0.7f;
                flat.at(x, y, 1) = 0.4f;
                flat.at(x, y, 2) = 0.2f;
            }
        }
        const ImageBuffer original = flat;

        ops::suppressEdgeFalseColor(flat, 1.0f, 0.5f, 1);

        bool unchanged = true;
        for (std::size_t i = 0; i < original.sampleCount(); ++i) {
            if (!near(flat.data()[i], original.data()[i], 1e-6f)) {
                unchanged = false;
            }
        }
        check(unchanged, "无边缘时不进行任何平滑");
    }

    {
        // 竖直硬边：左半 G=0.1，右半 G=0.9；在边缘处放一个 R 伪色点
        ImageBuffer image(9, 9);
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 9; ++x) {
                const float g = (x < 4) ? 0.1f : 0.9f;
                image.at(x, y, 0) = 0.5f;
                image.at(x, y, 1) = g;
                image.at(x, y, 2) = 0.5f;
            }
        }
        // 伪色点：R 明显偏离邻域
        image.at(4, 4, 0) = 0.0f;

        const ImageBuffer original = image;
        ops::suppressEdgeFalseColor(image, 1.0f, 1.0f, 1);

        check(image.at(4, 4, 0) > 0.4f, "边缘处的 R 伪色被中值平滑纠正");
        check(near(image.at(4, 4, 0), 0.5f, 1e-4f),
              "R 被替换为邻域中值");

        // G 通道必须严格不变
        bool greenUnchanged = true;
        for (std::size_t i = 0; i < original.pixelCount(); ++i) {
            if (!near(image.data()[i * 3u + 1u],
                      original.data()[i * 3u + 1u], 1e-6f)) {
                greenUnchanged = false;
            }
        }
        check(greenUnchanged, "边缘伪色抑制不修改 G 通道");
    }

    {
        // 真实彩色边缘：R / B 随亮度同步变化，仅在中心叠加一处 R 伪色。
        // 色差域处理应恢复色相，同时保持亮度结构（G）完全不变。
        ImageBuffer image(9, 9);
        for (int y = 0; y < 9; ++y) {
            for (int x = 0; x < 9; ++x) {
                const float g = (x < 4) ? 0.2f : 0.7f;
                image.at(x, y, 0) = g * 1.5f;
                image.at(x, y, 1) = g;
                image.at(x, y, 2) = g * 0.8f;
            }
        }
        image.at(4, 4, 0) += 0.3f;  // 伪色：红色被抬高

        const ImageBuffer original = image;
        ops::suppressEdgeFalseColor(image, 1.0f, 1.0f, 1);

        // 恢复后 R 应回到 0.7 × 1.5 附近，而不是被拉到左半边的暗部
        check(near(image.at(4, 4, 0), 1.05f, 0.06f),
              "色差域抑制恢复边缘处的真实色相");
        check(near(image.at(4, 4, 1), original.at(4, 4, 1), 1e-6f),
              "色差域抑制保持亮度（G）不变");
        // 未受伪色影响的普通边缘像素也不应被改变亮度
        check(near(image.at(3, 3, 1), original.at(3, 3, 1), 1e-6f),
              "普通边缘像素的 G 保持不变");
    }

    // ---- 参数优先级 ----
    {
        FalseColorSettings settings;
        settings.lca.redScale = 1.02f;  // 用户已调整（但与默认不同）

        FalseColorSettings cameraDefaults;
        cameraDefaults.crosstalk.strength = 0.9f;
        cameraDefaults.lca.redScale = 0.95f;

        settings.markUserOverride(CorrectionModule::Lca);
        settings.applyCameraDefaults(cameraDefaults);

        check(near(settings.crosstalk.strength, 0.9f, 1e-6f),
              "机型默认值填充未被覆盖的模块");
        check(near(settings.lca.redScale, 1.02f, 1e-6f),
              "用户覆盖的模块不被机型默认值修改");
    }

    {
        FalseColorSettings settings;
        LcaSettings lensLca;
        lensLca.redScale = 1.03f;
        lensLca.blueScale = 0.97f;

        check(settings.applyLensProfile(lensLca), "镜头配置可应用到 LCA");
        check(near(settings.lca.redScale, 1.03f, 1e-6f), "镜头配置写入 LCA 参数");
        check(settings.isLensOverride(CorrectionModule::Lca),
              "应用镜头配置后标记镜头覆盖");

        // 用户随后手动调整，镜头配置不再覆盖
        settings.markUserOverride(CorrectionModule::Lca);
        LcaSettings otherLens;
        otherLens.redScale = 1.08f;
        check(!settings.applyLensProfile(otherLens),
              "用户覆盖后镜头配置不再生效");
        check(near(settings.lca.redScale, 1.03f, 1e-6f),
              "用户覆盖后 LCA 参数保持不变");
    }

    {
        // 镜头配置中的 LoCA 也应参与覆盖（需求 v2 §2.9）
        FalseColorSettings settings;
        LocaSettings lensLoca;
        lensLoca.strength = 0.8f;
        lensLoca.highlightThreshold = 0.55f;

        check(settings.applyLensProfileLoca(lensLoca), "镜头配置可应用到 LoCA");
        check(near(settings.loca.strength, 0.8f, 1e-6f), "镜头配置写入 LoCA 参数");
        check(settings.isLensOverride(CorrectionModule::Loca),
              "应用镜头配置后标记 LoCA 镜头覆盖");

        // 用户随后手动调整，镜头配置不再覆盖
        settings.markUserOverride(CorrectionModule::Loca);
        LocaSettings otherLens;
        otherLens.strength = 0.2f;
        check(!settings.applyLensProfileLoca(otherLens),
              "用户覆盖后镜头 LoCA 配置不再生效");
        check(near(settings.loca.strength, 0.8f, 1e-6f),
              "用户覆盖后 LoCA 参数保持不变");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  双光源 CCM：按 as-shot 色温做 mired 插值
// ---------------------------------------------------------------------------
void testDualIlluminantCcm()
{
    std::printf("\n[双光源 CCM]\n");

    CcmSettings settings;
    settings.enable = true;
    settings.cam2xyz = Matrix3x3{{
        {{1.0f, 0.2f, 0.0f}},
        {{0.1f, 1.0f, 0.1f}},
        {{0.0f, 0.3f, 1.2f}},
    }};
    settings.cam2xyzTungsten = Matrix3x3{{
        {{0.8f, 0.4f, 0.0f}},
        {{0.2f, 0.9f, 0.2f}},
        {{0.0f, 0.5f, 1.0f}},
    }};
    settings.tungstenTemperature = 2856.0f;
    settings.daylightTemperature = 6504.0f;

    // 关闭双光源时，任何色温都应返回单光源矩阵
    check(matricesNear(settings.resolvedCam2xyz(3200.0f), settings.cam2xyz, 1e-6f),
          "双光源关闭时始终使用 cam2xyz");

    settings.dualIlluminant = true;
    check(matricesNear(settings.resolvedCam2xyz(2856.0f), settings.cam2xyzTungsten, 1e-5f),
          "拍摄色温 = 钨丝灯端点时取 A 矩阵");
    check(matricesNear(settings.resolvedCam2xyz(6504.0f), settings.cam2xyz, 1e-5f),
          "拍摄色温 = 日光端点时取 D65 矩阵");

    // 中间色温：按 mired 线性插值
    const float miredA = 1.0e6f / 2856.0f;
    const float miredD = 1.0e6f / 6504.0f;
    const float miredMid = 1.0e6f / 4000.0f;
    const float t = (miredMid - miredA) / (miredD - miredA);
    Matrix3x3 expected = settings.cam2xyzTungsten;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            expected[r][c] = settings.cam2xyzTungsten[r][c]
                + t * (settings.cam2xyz[r][c] - settings.cam2xyzTungsten[r][c]);
        }
    }
    check(matricesNear(settings.resolvedCam2xyz(4000.0f), expected, 1e-5f),
          "中间色温按 mired 线性插值");

    // 超出标定范围时钳制到端点，不做外插
    check(matricesNear(settings.resolvedCam2xyz(2000.0f), settings.cam2xyzTungsten, 1e-5f),
          "低于钨丝灯端点时钳制到 A 矩阵");
    check(matricesNear(settings.resolvedCam2xyz(9000.0f), settings.cam2xyz, 1e-5f),
          "高于日光端点时钳制到 D65 矩阵");

    // 元数据缺失（色温为 0）时退回单光源矩阵
    check(matricesNear(settings.resolvedCam2xyz(0.0f), settings.cam2xyz, 1e-6f),
          "as-shot 色温缺失时退回 cam2xyz");
}

// ---------------------------------------------------------------------------
//  预览尺度一致性：锐化 / 降噪半径按渲染比例换算
// ---------------------------------------------------------------------------
void testScaleAwareDetailOps()
{
    std::printf("\n[预览尺度一致性]\n");

    constexpr int kSize = 32;

    // ---- 锐化：降采样预览的过冲应弱于全尺寸 ----
    ImageBuffer step(kSize, kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float value = (x < kSize / 2) ? 0.2f : 0.5f;
            step.at(x, y, 0) = value;
            step.at(x, y, 1) = value;
            step.at(x, y, 2) = value;
        }
    }

    ImageBuffer fullScale = step;
    ImageBuffer previewScale = step;
    ops::applySharpen(fullScale, 60.0f, 1.5f, 0.0f, 1.0f);
    ops::applySharpen(previewScale, 60.0f, 1.5f, 0.0f, 0.3f);

    const float fullOvershoot =
        fullScale.at(kSize / 2, kSize / 2, 1) - step.at(kSize / 2, kSize / 2, 1);
    const float previewOvershoot =
        previewScale.at(kSize / 2, kSize / 2, 1) - step.at(kSize / 2, kSize / 2, 1);
    std::printf("  边缘过冲：全尺寸=%.4f  0.3x 预览=%.4f\n",
                static_cast<double>(fullOvershoot),
                static_cast<double>(previewOvershoot));
    check(previewOvershoot > 0.0f, "低倍率预览仍保留可见锐化");
    check(previewOvershoot < fullOvershoot,
          "低倍率预览的锐化过冲小于全尺寸（半径按比例换算）");

    // ---- 降噪：降采样预览的平滑范围应更小 ----
    std::uint32_t seed = 0x89ABCDEFu;
    const auto nextNoise = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f - 0.5f) * 0.05f;
    };

    ImageBuffer noisy(kSize, kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float value = 0.5f + nextNoise();
            noisy.at(x, y, 0) = value;
            noisy.at(x, y, 1) = value;
            noisy.at(x, y, 2) = value;
        }
    }

    const auto lumaStd = [](const ImageBuffer& img) {
        double mean = 0.0;
        double square = 0.0;
        for (std::size_t i = 0; i < img.pixelCount(); ++i) {
            const float y = 0.299f * img.data()[i * 3u]
                          + 0.587f * img.data()[i * 3u + 1]
                          + 0.114f * img.data()[i * 3u + 2];
            mean += y;
            square += static_cast<double>(y) * y;
        }
        mean /= static_cast<double>(img.pixelCount());
        return std::sqrt(std::max(
            0.0, square / static_cast<double>(img.pixelCount()) - mean * mean));
    };

    ImageBuffer nrFull = noisy;
    ImageBuffer nrPreview = noisy;
    ops::applyNoiseReduction(nrFull, 60.0f, 0.0f, 1.0f);
    ops::applyNoiseReduction(nrPreview, 60.0f, 0.0f, 0.3f);

    const double stdBefore = lumaStd(noisy);
    const double stdFull = lumaStd(nrFull);
    const double stdPreview = lumaStd(nrPreview);
    std::printf("  亮度标准差：原始=%.5f  全尺寸=%.5f  0.3x 预览=%.5f\n",
                stdBefore, stdFull, stdPreview);
    check(stdFull < stdBefore, "全尺寸降噪降低噪声");
    check(stdPreview < stdBefore, "低倍率预览降噪仍然有效");
    check(stdPreview > stdFull, "低倍率预览的平滑范围小于全尺寸");
}

// ---------------------------------------------------------------------------
//  镜头数据库：随程序分发的 lensfun 数据应能被加载并匹配到镜头
// ---------------------------------------------------------------------------
void testLensDatabase()
{
    std::printf("\n[镜头数据库]\n");

    // D7：导入接口必须对缺失 / 非法文件给出明确失败，而不是静默成功
    {
        std::string savedPath;
        std::string error;
        const bool imported = LensCorrector::importDatabaseFile(
            "definitely_missing_lensfun.xml", savedPath, error);
        check(!imported && !error.empty(),
              "不存在的 lensfun XML 被拒绝并给出原因");
    }

    if (!LensCorrector::isSupported()) {
        std::printf("  [skip] 本次构建未包含 lensfun\n");
        return;
    }

    // 指向源码树内的数据库，避免测试依赖系统的 lensfun-data 安装
#if defined(_WIN32)
    _putenv_s("RAWEDIT_LENSFUN_DB", RAWEDIT_TEST_LENSFUN_DB);
#else
    setenv("RAWEDIT_LENSFUN_DB", RAWEDIT_TEST_LENSFUN_DB, 1);
#endif

    LensCorrector corrector;

    RawMetadata metadata;
    metadata.cameraMake = "Nikon";
    metadata.cameraModel = "Nikon Z 7_2";
    metadata.lensModel = "NIKKOR Z 24-70mm f/2.8 S";
    metadata.focalLength = 50.0f;
    metadata.aperture = 5.6f;
    metadata.focusDistance = 5.0f;

    const bool matched = corrector.findLens(metadata);

    check(corrector.databaseLoaded(), "镜头数据库成功加载");
    check(!corrector.databasePath().empty(), "记录了实际使用的数据库目录");
    check(matched, "Nikon Z 7_2 + NIKKOR Z 24-70mm f/2.8 S 匹配成功");

    std::printf("  数据库目录：%s\n", corrector.databasePath().c_str());
    std::printf("  匹配结果  ：%s\n", corrector.lastMatchMessage().c_str());

    if (matched) {
        testLensCorrectionResampling(corrector);
    }
}

// ===========================================================================
//  11.5 DCP 解析
// ===========================================================================

namespace {

/// 构造一个小端 TIFF / DCP 测试文件。
/// 只实现测试需要的标签类型（SHORT / LONG / SRATIONAL / FLOAT / ASCII）。
std::vector<std::uint8_t> buildTestDcpBytes()
{
    struct Entry {
        std::uint16_t tag = 0;
        std::uint16_t type = 0;
        std::uint32_t count = 0;
        std::vector<std::uint8_t> data;
        std::uint32_t dataOffset = 0;
    };

    const auto putU16 = [](std::vector<std::uint8_t>& out, std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto putU32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto putI32 = [&putU32](std::vector<std::uint8_t>& out, std::int32_t value) {
        putU32(out, static_cast<std::uint32_t>(value));
    };
    const auto putFloat = [&putU32](std::vector<std::uint8_t>& out, float value) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        putU32(out, raw);
    };
    const auto makeRational = [&putI32](std::int32_t numerator, std::int32_t denominator) {
        std::vector<std::uint8_t> out;
        putI32(out, numerator);
        putI32(out, denominator);
        return out;
    };
    const auto append = [](std::vector<std::uint8_t>& out,
                           const std::vector<std::uint8_t>& part) {
        out.insert(out.end(), part.begin(), part.end());
    };

    std::vector<Entry> entries;

    // ProfileName（ASCII，含结尾 NUL）
    {
        Entry entry;
        entry.tag = 50936;
        entry.type = 2;
        const std::string name = "TestCam";
        entry.count = static_cast<std::uint32_t>(name.size() + 1);
        entry.data.assign(name.begin(), name.end());
        entry.data.push_back(0);
        entries.push_back(entry);
    }

    // CalibrationIlluminant1 = 17 (Standard light A)
    {
        Entry entry;
        entry.tag = 50778;
        entry.type = 3;
        entry.count = 1;
        entry.data = {17, 0};
        entries.push_back(entry);
    }

    // CalibrationIlluminant2 = 21 (D65)
    {
        Entry entry;
        entry.tag = 50779;
        entry.type = 3;
        entry.count = 1;
        entry.data = {21, 0};
        entries.push_back(entry);
    }

    // ColorMatrix1：钨丝灯端
    {
        static const float kMatrix[9] = {
            0.6722f, -0.0635f, -0.0203f,
            -0.4287f, 1.2460f, 0.2028f,
            -0.0908f, 0.2162f, 0.5668f,
        };
        Entry entry;
        entry.tag = 50721;
        entry.type = 10;  // SRATIONAL
        entry.count = 9;
        for (float value : kMatrix) {
            // 用 10000 作分母保留 4 位小数
            append(entry.data,
                   makeRational(static_cast<std::int32_t>(std::lround(value * 10000.0f)), 10000));
        }
        entries.push_back(entry);
    }

    // ColorMatrix2：日光端
    {
        static const float kMatrix[9] = {
            0.6640f, -0.0840f, -0.0230f,
            -0.4320f, 1.2540f, 0.2050f,
            -0.0870f, 0.2100f, 0.5700f,
        };
        Entry entry;
        entry.tag = 50722;
        entry.type = 10;
        entry.count = 9;
        for (float value : kMatrix) {
            append(entry.data,
                   makeRational(static_cast<std::int32_t>(std::lround(value * 10000.0f)), 10000));
        }
        entries.push_back(entry);
    }

    // ForwardMatrix1（仅验证解析）
    {
        Entry entry;
        entry.tag = 50964;
        entry.type = 10;
        entry.count = 9;
        for (int i = 0; i < 9; ++i) {
            append(entry.data, makeRational(i % 4 == 0 ? 1 : 0, 1));
        }
        entries.push_back(entry);
    }

    // HueSatMapDims = {2, 2, 1}
    {
        Entry entry;
        entry.tag = 50937;
        entry.type = 4;  // LONG
        entry.count = 3;
        for (std::uint32_t value : {2u, 2u, 1u}) {
            putU32(entry.data, value);
        }
        entries.push_back(entry);
    }

    // ProfileHueSatMapEncoding = 1（明度轴按 sRGB 编码索引）
    {
        Entry entry;
        entry.tag = 51107;
        entry.type = 4;  // LONG
        entry.count = 1;
        putU32(entry.data, 1);
        entries.push_back(entry);
    }

    // HueSatMapData1：2*2*1*3 = 12 个 float
    {
        Entry entry;
        entry.tag = 50938;
        entry.type = 11;  // FLOAT
        entry.count = 12;
        for (int i = 0; i < 12; ++i) {
            // 每节点第一列是色相偏移，DNG 规范以度为单位；其余两列为倍率
            const float value = (i % 3 == 0)
                ? static_cast<float>(i / 3) * 36.0f
                : static_cast<float>(i) * 0.01f;
            putFloat(entry.data, value);
        }
        entries.push_back(entry);
    }

    // ProfileToneCurve：两点恒等曲线
    {
        Entry entry;
        entry.tag = 50940;
        entry.type = 11;
        entry.count = 4;
        for (float value : {0.0f, 0.0f, 1.0f, 1.0f}) {
            putFloat(entry.data, value);
        }
        entries.push_back(entry);
    }

    // ProfileLookTableDims = {2, 2, 1} + 12 个 float
    {
        Entry dims;
        dims.tag = 50981;
        dims.type = 4;
        dims.count = 3;
        for (std::uint32_t value : {2u, 2u, 1u}) {
            putU32(dims.data, value);
        }
        entries.push_back(dims);

        Entry data;
        data.tag = 50982;
        data.type = 11;
        data.count = 12;
        for (int i = 0; i < 12; ++i) {
            // 与 Hue/Sat 表一致：第一列为以度表示的色相偏移
            const float value = (i % 3 == 0)
                ? static_cast<float>(i / 3) * 36.0f
                : static_cast<float>(i) * 0.02f;
            putFloat(data.data, value);
        }
        entries.push_back(data);

        // ProfileLookTableEncoding = 1（sRGB 编码）
        Entry encoding;
        encoding.tag = 51108;
        encoding.type = 4;
        encoding.count = 1;
        putU32(encoding.data, 1);
        entries.push_back(encoding);
    }

    // ProfileCopyright（ASCII）
    {
        Entry entry;
        entry.tag = 50942;
        entry.type = 2;
        const std::string copyright = "Test Copyright";
        entry.count = static_cast<std::uint32_t>(copyright.size() + 1);
        entry.data.assign(copyright.begin(), copyright.end());
        entry.data.push_back(0);
        entries.push_back(entry);
    }

    // 标签必须按升序排列
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    const std::size_t ifdOffset = 8;
    const std::size_t entriesStart = ifdOffset + 2;
    const std::size_t dataStart = entriesStart + entries.size() * 12 + 4;

    std::uint32_t nextOffset = static_cast<std::uint32_t>(dataStart);
    for (Entry& entry : entries) {
        if (entry.data.size() > 4) {
            entry.dataOffset = nextOffset;
            nextOffset += static_cast<std::uint32_t>(entry.data.size());
        }
    }

    std::vector<std::uint8_t> bytes;
    bytes.push_back('I');
    bytes.push_back('I');
    // DCP 的魔数是 0x4352（"CR"），不是普通 TIFF 的 42
    putU16(bytes, 0x4352);
    putU32(bytes, static_cast<std::uint32_t>(ifdOffset));
    putU16(bytes, static_cast<std::uint16_t>(entries.size()));

    for (const Entry& entry : entries) {
        putU16(bytes, entry.tag);
        putU16(bytes, entry.type);
        putU32(bytes, entry.count);
        if (entry.data.size() <= 4) {
            // 内联值：不足 4 字节补零
            std::vector<std::uint8_t> inlineValue = entry.data;
            inlineValue.resize(4, 0);
            append(bytes, inlineValue);
        } else {
            putU32(bytes, entry.dataOffset);
        }
    }
    putU32(bytes, 0);  // 无下一个 IFD

    for (const Entry& entry : entries) {
        if (entry.data.size() > 4) {
            append(bytes, entry.data);
        }
    }

    return bytes;
}

}  // namespace

void testFrameCalibration()
{
    std::printf("\n[测试] 暗场帧 / 平场帧校正\n");

    constexpr float kWhiteLevel = 16000.0f;
    constexpr int kWidth = 16;
    constexpr int kHeight = 16;

    // ---- 暗场帧：逐像素扣除 ----
    {
        BayerImage main;
        main.width = kWidth;
        main.height = kHeight;
        main.pattern = BayerPattern::RGGB;
        main.data.assign(static_cast<std::size_t>(kWidth) * kHeight, 0.5f);

        BayerImage darkRaw;
        darkRaw.width = kWidth;
        darkRaw.height = kHeight;
        darkRaw.pattern = BayerPattern::RGGB;
        // 原始刻度 1600 → 归一化 0.1
        darkRaw.data.assign(static_cast<std::size_t>(kWidth) * kHeight, 1600.0f);

        CalibrationFrame dark;
        const ErrorInfo darkError = buildCalibrationFrame(
            std::move(darkRaw), kWhiteLevel, dark);
        check(darkError.isOk(), "暗场帧构造成功");
        check(std::fabs(dark.mean - 0.1f) < 1e-4f, "暗场帧均值正确归一化");

        applyDarkFrameSubtraction(main, dark, 1.0f);
        check(std::fabs(main.at(3, 5) - 0.4f) < 1e-4f,
              "暗场帧按 100% 强度逐像素扣除");

        // 50% 强度只扣一半
        BayerImage half = main;
        half.data.assign(half.sampleCount(), 0.5f);
        applyDarkFrameSubtraction(half, dark, 0.5f);
        check(std::fabs(half.at(0, 0) - 0.45f) < 1e-4f,
              "暗场帧 50% 强度按比例扣除");

        // 扣除后不会出现负值
        BayerImage low;
        low.width = kWidth;
        low.height = kHeight;
        low.pattern = BayerPattern::RGGB;
        low.data.assign(low.sampleCount(), 0.05f);
        applyDarkFrameSubtraction(low, dark, 1.0f);
        check(low.at(0, 0) >= 0.0f, "暗场扣除后夹取到非负");
    }

    // ---- 平场帧：左侧半幅只有一半亮度，校正后拉平 ----
    {
        BayerImage main;
        main.width = kWidth;
        main.height = kHeight;
        main.pattern = BayerPattern::RGGB;
        main.data.assign(main.sampleCount(), 0.5f);

        BayerImage flatRaw;
        flatRaw.width = kWidth;
        flatRaw.height = kHeight;
        flatRaw.pattern = BayerPattern::RGGB;
        flatRaw.data.assign(flatRaw.sampleCount(), 16000.0f);
        // 左半幅响应减半（模拟渐晕）
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth / 2; ++x) {
                flatRaw.at(x, y) = 8000.0f;
            }
        }

        CalibrationFrame flat;
        const ErrorInfo flatError = buildCalibrationFrame(
            std::move(flatRaw), kWhiteLevel, flat);
        check(flatError.isOk(), "平场帧构造成功");
        check(std::fabs(flat.mean - 0.75f) < 1e-3f, "平场帧均值正确");

        applyFlatFrameCorrection(main, flat, 1.0f);
        // 参考均值 0.75：左半幅增益 0.75/0.5 = 1.5，右半幅 0.75/1.0 = 0.75
        check(std::fabs(main.at(0, 0) - 0.75f) < 1e-3f, "平场帧抬升暗角区域");
        check(std::fabs(main.at(kWidth - 1, 0) - 0.375f) < 1e-3f,
              "平场帧压低亮区响应");
    }

    // ---- 尺寸不一致时退化为按通道均值校正 ----
    {
        BayerImage main;
        main.width = kWidth;
        main.height = kHeight;
        main.pattern = BayerPattern::RGGB;
        main.data.assign(main.sampleCount(), 0.5f);

        BayerImage smallDark;
        smallDark.width = 4;
        smallDark.height = 4;
        smallDark.pattern = BayerPattern::RGGB;
        smallDark.data.assign(16, 1600.0f);

        CalibrationFrame dark;
        buildCalibrationFrame(std::move(smallDark), kWhiteLevel, dark);
        check(!dark.matches(main.width, main.height), "尺寸不一致能被识别");

        applyDarkFrameSubtraction(main, dark, 1.0f);
        check(std::fabs(main.at(0, 0) - 0.4f) < 1e-4f,
              "尺寸不一致时按通道均值扣除");
    }
}

void testMaskModel()
{
    std::printf("\n[测试] 蒙版数据模型\n");

    // ---- 径向蒙版 ----
    MaskLayer radial;
    radial.shape = MaskShape::Radial;
    radial.centerX = 0.5f;
    radial.centerY = 0.5f;
    radial.radiusX = 0.25f;
    radial.radiusY = 0.25f;
    radial.feather = 0.0f;
    check(std::fabs(radial.weightAt(0.5f, 0.5f) - 1.0f) < 1e-4f,
          "径向蒙版中心权重为 1");
    check(std::fabs(radial.weightAt(0.95f, 0.95f)) < 1e-4f,
          "径向蒙版边界外权重为 0");

    // 羽化：0.2 的过渡带内，距离边界 0.9 处应正好是 0.5
    radial.feather = 0.2f;
    const float featherWeight = radial.weightAt(0.5f + 0.9f * radial.radiusX, 0.5f);
    check(featherWeight > 0.0f && featherWeight < 1.0f,
          "径向蒙版羽化带内权重介于 0~1");
    check(std::fabs(featherWeight - 0.5f) < 1e-3f, "羽化中点权重为 0.5");
    radial.feather = 0.0f;
    check(std::fabs(radial.weightAt(0.5f + 0.9f * radial.radiusX, 0.5f) - 1.0f) < 1e-4f,
          "关闭羽化后边界内为硬边（权重 1）");

    radial.invert = true;
    check(std::fabs(radial.weightAt(0.5f, 0.5f)) < 1e-4f, "反相后中心权重为 0");
    radial.invert = false;

    radial.opacity = 0.5f;
    check(std::fabs(radial.weightAt(0.5f, 0.5f) - 0.5f) < 1e-4f,
          "不透明度按比例缩放权重");
    radial.opacity = 1.0f;

    // ---- 线性渐变 ----
    MaskLayer linear;
    linear.shape = MaskShape::Linear;
    linear.startX = 0.0f;
    linear.startY = 0.5f;
    linear.endX = 1.0f;
    linear.endY = 0.5f;
    check(std::fabs(linear.weightAt(0.0f, 0.5f)) < 1e-4f, "线性蒙版起点权重为 0");
    check(std::fabs(linear.weightAt(1.0f, 0.5f) - 1.0f) < 1e-4f,
          "线性蒙版终点权重为 1");
    check(std::fabs(linear.weightAt(0.5f, 0.5f) - 0.5f) < 1e-4f,
          "线性蒙版中点权重为 0.5");

    // ---- 画笔 ----
    MaskLayer brush;
    brush.shape = MaskShape::Brush;
    brush.feather = 0.0f;
    brush.strokes.push_back(MaskStrokePoint{0.5f, 0.5f, 0.25f, 1.0f});
    check(std::fabs(brush.weightAt(0.5f, 0.5f) - 1.0f) < 1e-4f,
          "画笔蒙版笔画中心权重为 1");
    check(std::fabs(brush.weightAt(0.9f, 0.9f)) < 1e-4f,
          "画笔蒙版笔画范围外权重为 0");

    // ---- 蒙版栈：并集取最大值 ----
    MaskStack stack;
    stack.enable = true;
    stack.layers.push_back(radial);
    MaskLayer second = radial;
    second.centerX = 0.1f;
    second.centerY = 0.1f;
    stack.layers.push_back(second);
    check(std::fabs(stack.weightAt(0.5f, 0.5f) - 1.0f) < 1e-4f,
          "蒙版栈取各层最大值");
    stack.enable = false;
    check(std::fabs(stack.weightAt(0.5f, 0.5f)) < 1e-4f,
          "蒙版栈关闭后权重为 0");

    // ---- 形状名往返（工程文件存储用） ----
    check(std::string(maskShapeName(MaskShape::Linear)) == "linear"
              && maskShapeFromName("linear") == MaskShape::Linear,
          "蒙版形状名往返一致");
    check(maskShapeFromName("unknown-shape") == MaskShape::Radial,
          "未知形状名回退到径向");
    check(std::string(maskShapeName(MaskShape::Range)) == "range"
              && maskShapeFromName("range") == MaskShape::Range,
          "范围蒙版形状名往返一致");
}

// ===========================================================================
//  11.4 蒙版权重图与局部调整（B1）
// ===========================================================================

void testMaskWeightMap()
{
    std::printf("\n[测试] 蒙版权重图（B1）\n");

    std::vector<float> map;

    // ---- 径向 ----
    MaskLayer radial;
    radial.shape = MaskShape::Radial;
    radial.centerX = 0.5f;
    radial.centerY = 0.5f;
    radial.radiusX = 0.25f;
    radial.radiusY = 0.25f;
    radial.feather = 0.0f;

    radial.buildWeightMap(100, 100, map);
    check(map.size() == 10000, "径向权重图尺寸正确");
    check(map[static_cast<std::size_t>(50) * 100 + 50] > 0.999f,
          "径向权重图中心为 1");
    check(map[0] < 0.001f, "径向权重图角落为 0");

    // 羽化中点应与 weightAt 的语义一致
    radial.feather = 0.2f;
    radial.buildWeightMap(100, 100, map);
    const float featherWeight = map[static_cast<std::size_t>(50) * 100 + 72];
    check(featherWeight > 0.2f && featherWeight < 0.8f,
          "径向权重图羽化带产生过渡");

    // 反相
    radial.feather = 0.0f;
    radial.invert = true;
    radial.buildWeightMap(100, 100, map);
    check(map[static_cast<std::size_t>(50) * 100 + 50] < 0.001f,
          "反相后径向中心为 0");
    check(map[0] > 0.999f, "反相后径向角落为 1");
    radial.invert = false;

    // 不透明度
    radial.opacity = 0.5f;
    radial.buildWeightMap(100, 100, map);
    check(std::fabs(map[static_cast<std::size_t>(50) * 100 + 50] - 0.5f) < 1e-4f,
          "不透明度按比例缩放权重图");
    radial.opacity = 1.0f;

    // ---- 线性渐变 ----
    MaskLayer linear;
    linear.shape = MaskShape::Linear;
    linear.startX = 0.25f;
    linear.startY = 0.5f;
    linear.endX = 0.75f;
    linear.endY = 0.5f;
    linear.buildWeightMap(100, 10, map);
    check(map[static_cast<std::size_t>(5) * 100 + 10] < 0.001f,
          "线性权重图起点侧为 0");
    // 像素中心 u = (50+0.5)/100 = 0.505，对应 t = 0.51
    check(std::fabs(map[static_cast<std::size_t>(5) * 100 + 50] - 0.51f) < 1e-3f,
          "线性权重图中点按像素中心计算");
    check(map[static_cast<std::size_t>(5) * 100 + 90] > 0.999f,
          "线性权重图终点侧为 1");

    // ---- 画笔：半径按短边解释 ----
    MaskLayer brush;
    brush.shape = MaskShape::Brush;
    brush.feather = 0.0f;
    brush.strokes.push_back(MaskStrokePoint{0.5f, 0.5f, 0.1f, 1.0f});

    constexpr int kBrushWidth = 200;
    constexpr int kBrushHeight = 100;
    brush.buildWeightMap(kBrushWidth, kBrushHeight, map);

    const auto brushAt = [&map, kBrushWidth](int x, int y) {
        return map[static_cast<std::size_t>(y) * kBrushWidth + x];
    };
    check(brushAt(100, 50) > 0.999f, "画笔笔画中心为 1");
    check(brushAt(109, 50) > 0.999f, "画笔半径内（短边 10px）为 1");
    check(brushAt(100, 58) > 0.999f, "画笔半径在纵向同样有效");
    check(brushAt(111, 50) < 0.001f, "画笔超出像素半径处为 0");

    // ---- 范围蒙版（B3）----
    {
        MaskLayer rangeLayer;
        rangeLayer.shape = MaskShape::Range;
        rangeLayer.buildWeightMap(16, 12, map);
        bool allOne = map.size() == 16u * 12u;
        for (float value : map) {
            allOne = allOne && std::fabs(value - 1.0f) < 1e-6f;
        }
        check(allOne, "范围蒙版的基础权重全幅为 1");

        RangeMaskSettings luminance;
        luminance.enableLuminance = true;
        luminance.luminanceMin = 0.6f;
        luminance.luminanceMax = 0.9f;
        luminance.luminanceFeather = 0.05f;
        check(luminance.weightAt(0.80f, 0.0f, 0.0f) > 0.99f,
              "亮度范围内权重为 1");
        check(luminance.weightAt(0.30f, 0.0f, 0.0f) < 0.01f,
              "亮度范围外权重为 0");
        const float edgeWeight = luminance.weightAt(0.575f, 0.0f, 0.0f);
        check(edgeWeight > 0.0f && edgeWeight < 1.0f,
              "亮度范围羽化带产生过渡");

        RangeMaskSettings color;
        color.enableColor = true;
        color.colorHue = 0.0f;
        color.colorTolerance = 30.0f;
        color.colorSaturationMin = 0.05f;
        check(color.weightAt(0.5f, 0.0f, 0.5f) > 0.99f,
              "颜色范围中心权重为 1");
        check(color.weightAt(0.5f, 120.0f, 0.5f) < 0.01f,
              "颜色范围外权重为 0");
        check(color.weightAt(0.5f, 0.0f, 0.01f) < 0.01f,
              "低饱和中性色被颜色范围排除");

        RangeMaskSettings both;
        both.enableLuminance = true;
        both.luminanceMin = 0.6f;
        both.luminanceMax = 0.9f;
        both.enableColor = true;
        both.colorHue = 0.0f;
        both.colorTolerance = 30.0f;
        check(both.weightAt(0.30f, 0.0f, 0.5f) < 0.01f,
              "亮度 / 颜色范围同时启用时取交集");
        check(both.weightAt(0.80f, 0.0f, 0.5f) > 0.99f,
              "交集内权重为 1");
    }
}

void testMaskWeightCache()
{
    std::printf("\n[测试] 蒙版权重图缓存（B5）\n");

    const auto maxDifference = [](const std::vector<float>& a,
                                  const std::vector<float>& b) {
        if (a.size() != b.size()) {
            return 1.0e9f;
        }
        float diff = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff = std::max(diff, std::fabs(a[i] - b[i]));
        }
        return diff;
    };

    // 径向：缓存结果与直接构建一致，且重复调用命中缓存
    {
        MaskLayer radial;
        radial.shape = MaskShape::Radial;
        radial.centerX = 0.5f;
        radial.centerY = 0.5f;
        radial.radiusX = 0.3f;
        radial.radiusY = 0.2f;
        radial.feather = 0.2f;
        radial.rotation = 12.0f;

        MaskWeightCache cache;
        const auto& firstRef = cache.weightMap(radial, 64, 64);
        const std::vector<float> first(firstRef.begin(), firstRef.end());

        const auto& secondRef = cache.weightMap(radial, 64, 64);
        const std::vector<float> second(secondRef.begin(), secondRef.end());

        std::vector<float> expected;
        radial.buildWeightMap(64, 64, expected);

        check(first.size() == 64u * 64u, "径向缓存尺寸正确");
        check(maxDifference(first, expected) < 1e-6f,
              "径向缓存结果与直接构建一致");
        check(maxDifference(first, second) < 1e-6f, "重复请求命中同一权重图");
        check(cache.cachedBytes() > 0, "缓存占用大于 0");

        cache.clear();
        check(cache.cachedBytes() == 0, "清空缓存后占用为 0");
    }

    // 画笔：新增笔画只增量栅格化，结果仍与全量构建一致
    {
        MaskLayer brush;
        brush.shape = MaskShape::Brush;
        brush.feather = 0.15f;
        brush.strokes.push_back(MaskStrokePoint{0.30f, 0.5f, 0.10f, 1.0f});

        MaskWeightCache cache;
        const auto& firstRef = cache.weightMap(brush, 128, 64);
        const std::vector<float> first(firstRef.begin(), firstRef.end());

        brush.strokes.push_back(MaskStrokePoint{0.70f, 0.5f, 0.10f, 1.0f});
        const auto& secondRef = cache.weightMap(brush, 128, 64);
        const std::vector<float> second(secondRef.begin(), secondRef.end());

        std::vector<float> expected;
        brush.buildWeightMap(128, 64, expected);

        check(maxDifference(second, expected) < 1e-6f,
              "画笔增量栅格化与全量构建一致");
        check(maxDifference(first, second) > 1e-4f,
              "新增笔画确实改变权重图");

        // 只改不透明度：复用原始权重，最终结果仍正确
        brush.opacity = 0.5f;
        const auto& thirdRef = cache.weightMap(brush, 128, 64);
        const std::vector<float> third(thirdRef.begin(), thirdRef.end());
        std::vector<float> expectedHalf;
        brush.buildWeightMap(128, 64, expectedHalf);
        check(maxDifference(third, expectedHalf) < 1e-6f,
              "不透明度变化复用原始权重且结果正确");
    }

    // 范围蒙版：形状权重全幅为 1，缓存同样可用
    {
        MaskLayer rangeLayer;
        rangeLayer.shape = MaskShape::Range;
        rangeLayer.range.enableLuminance = true;
        rangeLayer.range.luminanceMin = 0.4f;

        MaskWeightCache cache;
        const auto& weightsRef = cache.weightMap(rangeLayer, 32, 32);
        bool allOne = weightsRef.size() == 32u * 32u;
        for (float value : weightsRef) {
            allOne = allOne && std::fabs(value - 1.0f) < 1e-6f;
        }
        check(allOne, "范围蒙版缓存的基础权重全幅为 1");
    }
}

void testLocalAdjustments()
{
    std::printf("\n[测试] 局部调整（B1）\n");

    const auto neutralImage = [](int width, int height, float value) {
        ImageBuffer image(width, height);
        image.fill(value);
        return image;
    };

    const auto proPhotoLuma = [](float r, float g, float b) {
        return 0.2880402f * r + 0.7118741f * g + 0.0000857f * b;
    };

    // ---- 蒙版栈关闭 / 参数恒等：逐位不变 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Radial;
        layer.feather = 0.0f;
        layer.adjustments.exposure = 1.0f;

        MaskStack masks;
        masks.enable = false;
        masks.layers.push_back(layer);

        ImageBuffer image = neutralImage(32, 32, 0.2f);
        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "蒙版栈关闭时逐位不变");

        masks.enable = true;
        layer.adjustments = LocalAdjustments{};
        masks.layers = {layer};
        copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);
        identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "局部调整恒等时逐位不变");
    }

    // ---- 径向曝光：中心翻倍，外部不变 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Radial;
        layer.centerX = 0.5f;
        layer.centerY = 0.5f;
        layer.radiusX = 0.25f;
        layer.radiusY = 0.25f;
        layer.feather = 0.0f;
        layer.adjustments.exposure = 1.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image = neutralImage(64, 64, 0.2f);
        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        check(std::fabs(copy.at(32, 32, 0) - 0.4f) < 1e-4f,
              "局部曝光 +1EV 使蒙版中心亮度翻倍");
        check(std::fabs(copy.at(0, 0, 0) - 0.2f) < 1e-6f,
              "蒙版外像素完全不变");

        // 反相：外部翻倍、中心不变
        layer.invert = true;
        masks.layers = {layer};
        copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);
        check(std::fabs(copy.at(0, 0, 0) - 0.4f) < 1e-4f,
              "反相蒙版使外部获得曝光调整");
        check(std::fabs(copy.at(32, 32, 0) - 0.2f) < 1e-6f,
              "反相蒙版中心保持不变");

        // 不透明度 0.5：中心得到一半调整量
        layer.invert = false;
        layer.opacity = 0.5f;
        masks.layers = {layer};
        copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);
        check(std::fabs(copy.at(32, 32, 0) - 0.3f) < 1e-4f,
              "蒙版不透明度 0.5 混合一半调整量");
    }

    // ---- 线性渐变曝光 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Linear;
        layer.startX = 0.25f;
        layer.startY = 0.5f;
        layer.endX = 0.75f;
        layer.endY = 0.5f;
        layer.adjustments.exposure = 1.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image = neutralImage(64, 8, 0.2f);
        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        check(std::fabs(copy.at(8, 4, 0) - 0.2f) < 1e-5f,
              "线性渐变起点侧不改变");
        check(std::fabs(copy.at(32, 4, 0) - 0.3f) < 0.01f,
              "线性渐变中点获得约一半调整");
        check(std::fabs(copy.at(56, 4, 0) - 0.4f) < 1e-3f,
              "线性渐变终点侧获得完整调整");
    }

    // ---- 局部色温：改变颜色但保持亮度 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Radial;
        layer.feather = 0.0f;
        layer.adjustments.temperature = 2000.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image = neutralImage(32, 32, 0.2f);
        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        const float r = copy.at(16, 16, 0);
        const float g = copy.at(16, 16, 1);
        const float b = copy.at(16, 16, 2);
        check(std::fabs(r - g) > 0.005f || std::fabs(b - g) > 0.005f,
              "局部色温产生可见色偏");
        check(std::fabs(proPhotoLuma(r, g, b) - 0.2f) < 0.01f,
              "局部色温保持整体亮度");
    }

    // ---- 局部对比度 / 饱和度 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Radial;
        layer.feather = 0.0f;
        layer.adjustments.contrast = 100.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image = neutralImage(32, 32, 0.18f);
        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);
        check(std::fabs(copy.at(16, 16, 0) - 0.18f) > 0.005f,
              "局部对比度改变中间调");

        layer.adjustments = LocalAdjustments{};
        layer.adjustments.saturation = 100.0f;
        masks.layers = {layer};

        ImageBuffer colored(32, 32);
        for (int y = 0; y < colored.height(); ++y) {
            for (int x = 0; x < colored.width(); ++x) {
                colored.at(x, y, 0) = 0.40f;
                colored.at(x, y, 1) = 0.20f;
                colored.at(x, y, 2) = 0.10f;
            }
        }

        const float before = perceptualChroma(
            colored.at(16, 16, 0), colored.at(16, 16, 1), colored.at(16, 16, 2));
        ImageBuffer coloredCopy = colored;
        ops::applyLocalAdjustments(coloredCopy, masks, 5500.0f, 0.0f);
        const float after = perceptualChroma(
            coloredCopy.at(16, 16, 0), coloredCopy.at(16, 16, 1),
            coloredCopy.at(16, 16, 2));
        check(after > before + 0.01f, "局部饱和度提高色度");
    }

    // ---- 范围蒙版：亮度范围只作用于亮部 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Range;
        layer.range.enableLuminance = true;
        layer.range.luminanceMin = 0.6f;
        layer.range.luminanceMax = 1.0f;
        layer.range.luminanceFeather = 0.02f;
        layer.adjustments.exposure = 1.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image(64, 64);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const float value = (x < image.width() / 2) ? 0.10f : 0.80f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        check(std::fabs(copy.at(8, 8, 0) - 0.10f) < 1e-5f,
              "亮度范围排除暗部");
        check(std::fabs(copy.at(56, 8, 0) - 1.60f) < 1e-4f,
              "亮度范围只提亮亮部");

        // 纯范围蒙版反相：应反转范围，而不是把整幅权重乘成 0
        layer.invert = true;
        masks.layers = {layer};
        copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);
        check(std::fabs(copy.at(8, 8, 0) - 0.20f) < 1e-4f,
              "纯范围蒙版反相后提亮暗部");
        check(std::fabs(copy.at(56, 8, 0) - 0.80f) < 1e-4f,
              "纯范围蒙版反相后不再影响亮部");
    }

    // ---- 范围蒙版：颜色范围只作用于指定色相 ----
    {
        MaskLayer layer;
        layer.shape = MaskShape::Range;
        layer.range.enableColor = true;
        layer.range.colorHue = 0.0f;
        layer.range.colorTolerance = 20.0f;
        layer.range.colorSaturationMin = 0.05f;
        layer.adjustments.exposure = 1.0f;

        MaskStack masks;
        masks.enable = true;
        masks.layers.push_back(layer);

        ImageBuffer image(32, 32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (x < image.width() / 2) {
                    image.at(x, y, 0) = 0.80f;
                    image.at(x, y, 1) = 0.10f;
                    image.at(x, y, 2) = 0.10f;
                } else {
                    image.at(x, y, 0) = 0.10f;
                    image.at(x, y, 1) = 0.10f;
                    image.at(x, y, 2) = 0.80f;
                }
            }
        }

        ImageBuffer copy = image;
        ops::applyLocalAdjustments(copy, masks, 5500.0f, 0.0f);

        check(copy.at(8, 8, 0) > 1.5f, "颜色范围提亮红色区域");
        check(std::fabs(copy.at(24, 8, 2) - 0.80f) < 1e-5f,
              "颜色范围不改变蓝色区域");
    }
}

void testCropOperations()
{
    std::printf("\n[测试] 非破坏性裁剪（C1）\n");

    ImageBuffer image(100, 80);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.at(x, y, 0) = static_cast<float>(x) / 100.0f;
            image.at(x, y, 1) = static_cast<float>(y) / 80.0f;
            image.at(x, y, 2) = 0.5f;
        }
    }

    // 禁用 / 整幅：不改变图像
    {
        CropSettings disabled;
        ImageBuffer copy = image;
        check(!ops::applyCrop(copy, disabled), "未启用裁剪时不执行");
        check(copy.width() == image.width() && copy.height() == image.height(),
              "未启用裁剪时尺寸不变");

        CropSettings full;
        full.enable = true;
        copy = image;
        check(!ops::applyCrop(copy, full), "整幅裁剪不执行");
        check(copy.width() == image.width() && copy.height() == image.height(),
              "整幅裁剪尺寸不变");
    }

    // 中心 50% 裁剪
    {
        CropSettings crop;
        crop.enable = true;
        crop.x = 0.25f;
        crop.y = 0.25f;
        crop.width = 0.5f;
        crop.height = 0.5f;

        ImageBuffer copy = image;
        int left = -1;
        int top = -1;
        check(ops::applyCrop(copy, crop, &left, &top), "中心裁剪执行成功");
        check(copy.width() == 50 && copy.height() == 40, "裁剪尺寸正确");
        check(left == 25 && top == 20, "返回裁剪原点");
        check(std::fabs(copy.at(0, 0, 0) - image.at(25, 20, 0)) < 1e-6f
                  && std::fabs(copy.at(0, 0, 1) - image.at(25, 20, 1)) < 1e-6f,
              "裁剪像素与源图对应位置一致");
        check(std::fabs(copy.at(49, 39, 0) - image.at(74, 59, 0)) < 1e-6f,
              "裁剪右下角对应源图右下角");
    }

    // 拉直 45°：100×80 会自动收窄，且输出全部为有限值
    {
        CropSettings crop;
        crop.enable = true;
        crop.x = 0.0f;
        crop.y = 0.0f;
        crop.width = 1.0f;
        crop.height = 1.0f;
        crop.angle = 45.0f;

        check(!crop.isFullFrame() && crop.isActive(),
              "仅有拉直角度时也视为有效裁剪");

        ImageBuffer copy = image;  // 100×80，中心附近有渐变
        check(ops::applyCrop(copy, crop), "拉直裁剪执行成功");
        check(copy.width() >= 60 && copy.width() <= 80,
              "45° 拉直后宽度自动收窄");
        check(copy.height() >= 40 && copy.height() <= 60,
              "45° 拉直后高度自动收窄");

        bool finite = true;
        for (std::size_t i = 0; i < copy.sampleCount(); ++i) {
            finite = finite && std::isfinite(copy.data()[i]);
        }
        check(finite, "拉直输出全部为有限值");
    }

    // 输出坐标 → 源图坐标映射（灰卡 / 画笔在裁剪前取样）
    {
        CropSettings crop;
        crop.enable = true;
        crop.x = 0.2f;
        crop.y = 0.1f;
        crop.width = 0.5f;
        crop.height = 0.4f;

        float u = 0.0f;
        float v = 0.0f;
        crop.mapOutputToSource(0.0f, 0.0f, 1000, 800, u, v);
        check(std::fabs(u - 0.2f) < 1e-4f && std::fabs(v - 0.1f) < 1e-4f,
              "裁剪映射：输出左上角对应源裁剪左上角");

        crop.mapOutputToSource(0.5f, 0.5f, 1000, 800, u, v);
        check(std::fabs(u - 0.45f) < 1e-4f && std::fabs(v - 0.3f) < 1e-4f,
              "裁剪映射：输出中心对应源裁剪中心");

        // 拉直后输出中心仍应回到裁剪区域中心
        crop.angle = 30.0f;
        crop.mapOutputToSource(0.5f, 0.5f, 1000, 800, u, v);
        check(std::fabs(u - 0.45f) < 1e-3f && std::fabs(v - 0.3f) < 1e-3f,
              "拉直映射：输出中心仍对应源裁剪中心");

        // 画笔半径比例：50% 裁剪为 0.5，拉直后进一步缩小
        crop.angle = 0.0f;
        // 输出 500×320，源短边 800 → 320/800 = 0.4
        check(std::fabs(crop.outputShortEdgeRatio(1000, 800) - 0.4f) < 1e-4f,
              "裁剪输出短边比例正确");
        crop.angle = 45.0f;
        const float rotatedRatio = crop.outputShortEdgeRatio(1000, 800);
        check(rotatedRatio > 0.0f && rotatedRatio < 0.5f,
              "拉直后输出短边比例进一步缩小");
    }

    // 越界矩形被夹取，至少保留 1 像素
    {
        CropSettings crop;
        crop.enable = true;
        crop.x = 0.8f;
        crop.width = 0.5f;  // x + width > 1
        crop.y = 0.9f;
        crop.height = 0.5f;

        ImageBuffer copy = image;
        check(ops::applyCrop(copy, crop), "越界裁剪仍执行");
        check(copy.width() == 20 && copy.height() == 8,
              "越界裁剪被夹取到剩余区域");
        check(copy.width() >= 1 && copy.height() >= 1, "裁剪结果至少 1 像素");
    }
}

void testPerspectiveCorrection()
{
    std::printf("\n[测试] 透视 / 几何校正（C3）\n");

    // 恒等
    {
        ImageBuffer image(32, 32);
        image.fill(0.5f);
        ImageBuffer copy = image;
        PerspectiveSettings settings;
        check(settings.isIdentity(), "默认透视参数为恒等");
        check(!ops::applyPerspective(copy, settings), "恒等透视不执行重采样");
        bool identical = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            identical = identical && (image.data()[i] == copy.data()[i]);
        }
        check(identical, "恒等透视逐位不变");
    }

    // 常数图经过透视后仍应为同一常数（无黑边）
    {
        ImageBuffer image(64, 64);
        image.fill(0.5f);

        PerspectiveSettings settings;
        settings.enable = true;
        settings.vertical = 80.0f;
        settings.horizontal = -60.0f;
        settings.scale = 30.0f;

        check(ops::applyPerspective(image, settings), "透视校正执行成功");
        check(image.width() == 64 && image.height() == 64,
              "透视校正不改变输出尺寸");

        float maxError = 0.0f;
        bool finite = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            finite = finite && std::isfinite(image.data()[i]);
            maxError = std::max(maxError, std::fabs(image.data()[i] - 0.5f));
        }
        check(finite, "透视输出全部为有限值");
        check(maxError < 1e-5f, "透视自动收窄，常数图无黑边");
    }

    // 渐变图：透视确实改变像素，且输出仍在合法范围
    {
        ImageBuffer image(64, 64);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.at(x, y, 0) = static_cast<float>(x) / 63.0f;
                image.at(x, y, 1) = static_cast<float>(y) / 63.0f;
                image.at(x, y, 2) = 0.5f;
            }
        }

        ImageBuffer original = image;
        PerspectiveSettings settings;
        settings.enable = true;
        settings.vertical = 50.0f;
        settings.horizontal = 25.0f;

        check(ops::applyPerspective(image, settings), "渐变图透视执行成功");

        bool changed = false;
        bool inRange = true;
        for (std::size_t i = 0; i < image.sampleCount(); ++i) {
            changed = changed || std::fabs(image.data()[i] - original.data()[i]) > 1e-4f;
            inRange = inRange && image.data()[i] >= 0.0f && image.data()[i] <= 1.0f;
        }
        check(changed, "透视校正确实改变画面几何");
        check(inRange, "透视输出保持在合法范围");
    }
}

void testRetouch()
{
    std::printf("\n[测试] 污点修复 / 仿制（C4）\n");

    // 仿制：目标区域直接复制源像素
    {
        ImageBuffer image(32, 32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (x < image.width() / 2) {
                    image.at(x, y, 0) = 0.80f;
                    image.at(x, y, 1) = 0.10f;
                    image.at(x, y, 2) = 0.10f;
                } else {
                    image.at(x, y, 0) = 0.10f;
                    image.at(x, y, 1) = 0.10f;
                    image.at(x, y, 2) = 0.80f;
                }
            }
        }

        RetouchSpot spot;
        spot.targetX = 0.75f;
        spot.targetY = 0.50f;
        spot.sourceX = 0.25f;
        spot.sourceY = 0.50f;
        spot.radius = 0.15f;
        spot.feather = 0.0f;
        spot.opacity = 1.0f;
        spot.mode = RetouchMode::Clone;

        RetouchSettings settings;
        settings.spots.push_back(spot);

        ImageBuffer copy = image;
        ops::applyRetouch(copy, settings);

        check(copy.at(24, 16, 0) > 0.75f && copy.at(24, 16, 2) < 0.15f,
              "仿制把源像素复制到目标区域");
        check(copy.at(30, 30, 2) > 0.75f, "仿制不影响远处像素");
    }

    // 修复：复制纹理，同时把颜色均值对齐到目标
    {
        ImageBuffer image(64, 64);
        image.fill(0.5f);
        for (int y = 24; y < 40; ++y) {
            for (int x = 8; x < 24; ++x) {
                const float value = ((x / 2) % 2 == 0) ? 0.40f : 0.80f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        RetouchSpot spot;
        spot.targetX = 0.60f;
        spot.targetY = 0.50f;
        spot.sourceX = 0.20f;
        spot.sourceY = 0.50f;
        spot.radius = 0.10f;
        spot.feather = 0.0f;
        spot.opacity = 1.0f;
        spot.mode = RetouchMode::Heal;

        RetouchSettings settings;
        settings.spots.push_back(spot);

        ImageBuffer copy = image;
        ops::applyRetouch(copy, settings);

        const int cx = static_cast<int>(0.60f * 64);
        const int cy = static_cast<int>(0.50f * 64);
        double sum = 0.0;
        double sumSq = 0.0;
        int count = 0;
        for (int y = cy - 3; y <= cy + 3; ++y) {
            for (int x = cx - 3; x <= cx + 3; ++x) {
                const double value = copy.at(x, y, 1);
                sum += value;
                sumSq += value * value;
                ++count;
            }
        }
        const double mean = sum / count;
        const double variance = sumSq / count - mean * mean;
        check(std::fabs(mean - 0.5) < 0.08, "修复保持目标区域的颜色均值");
        check(variance > 1e-4, "修复把源纹理带到了目标区域");
    }

    // 强度 50%：克隆结果与目标混合
    {
        ImageBuffer image(16, 16);
        image.fill(0.2f);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                image.at(x, y, 0) = 0.8f;
                image.at(x, y, 1) = 0.8f;
                image.at(x, y, 2) = 0.8f;
            }
        }

        RetouchSpot spot;
        spot.targetX = 0.25f;
        spot.targetY = 0.25f;
        spot.sourceX = 0.75f;
        spot.sourceY = 0.75f;
        spot.radius = 0.20f;
        spot.feather = 0.0f;
        spot.opacity = 0.5f;
        spot.mode = RetouchMode::Clone;

        RetouchSettings settings;
        settings.spots.push_back(spot);
        ops::applyRetouch(image, settings);

        // 目标处原本 0.8，源处 0.2，50% 混合应约为 0.5
        const float value = image.at(4, 4, 0);
        check(value > 0.45f && value < 0.55f, "修复强度 50% 按比例混合");
    }
}

void testDcpParsing()
{
    std::printf("\n[DCP 解析]\n");

    const std::vector<std::uint8_t> bytes = buildTestDcpBytes();
    const DcpProfile profile = parseDcpProfileFromBytes(bytes);

    check(profile.valid, "合成 DCP 解析成功");
    if (!profile.valid) {
        std::printf("  解析错误：%s\n", profile.error.c_str());
        return;
    }

    check(profile.profileName == "TestCam", "读取 ProfileName");
    check(profile.profileCopyright == "Test Copyright", "读取 ProfileCopyright");
    check(profile.illuminant1Code == 17 && profile.illuminant2Code == 21,
          "读取标定光源编码");
    check(near(profile.illuminant1Temperature, 2856.0f, 0.1f)
              && near(profile.illuminant2Temperature, 6504.0f, 0.1f),
          "光源编码换算为色温");
    check(profile.hasColorMatrix1 && profile.hasColorMatrix2, "两组 ColorMatrix 均已解析");

    check(near(profile.colorMatrix1[0][0], 0.6722f, 1e-3f)
              && near(profile.colorMatrix2[2][2], 0.5700f, 1e-3f),
          "ColorMatrix 数值与文件一致（SRATIONAL）");

    check(profile.hasForwardMatrix1, "ForwardMatrix1 已解析");
    check(profile.hueSatMap1.hueDivisions == 2 && profile.hueSatMap1.satDivisions == 2
              && profile.hueSatMap1.valDivisions == 1,
          "Hue/Sat 表维度正确");
    check(profile.hueSatMap1.data.size() == 12, "Hue/Sat 表数据长度正确");
    // 文件中 36° 应换算为归一化色相 0.1，否则渲染时色相会乱转
    check(near(profile.hueSatMap1.data[3], 0.1f, 1e-5f),
          "Hue/Sat 色相偏移由度换算为归一化值（36° → 0.1）");
    check(profile.hueSatMap1.srgbEncoding, "读取 ProfileHueSatMapEncoding（sRGB 编码）");
    check(profile.toneCurve.coordinates.size() == 4, "色调曲线已解析");
    check(profile.lookTable.hueDivisions == 2 && profile.lookTable.data.size() == 12,
          "ProfileLookTable 已解析");
    check(near(profile.lookTable.data[3], 0.1f, 1e-5f),
          "Look 表色相偏移同样按度换算（36° → 0.1）");
    check(profile.lookTable.srgbEncoding, "读取 ProfileLookTableEncoding（sRGB 编码）");

    Matrix3x3 daylight;
    check(profile.daylightMatrix(daylight) && matricesNear(daylight, profile.colorMatrix2, 1e-6f),
          "日光端矩阵取 ColorMatrix2");

    // DCP 的 ColorMatrix 是 XYZ → 相机，渲染需要其逆矩阵（相机 → XYZ）。
    Matrix3x3 camToXyz1{};
    Matrix3x3 camToXyz2{};
    check(profile.cameraToXyzMatrix(false, camToXyz1), "光源 1 的相机 → XYZ 矩阵可求逆");
    check(profile.cameraToXyzMatrix(true, camToXyz2), "光源 2 的相机 → XYZ 矩阵可求逆");
    check(matricesNear(math::multiply(profile.colorMatrix1, camToXyz1),
                       identityMatrix3x3(), 2e-4f),
          "求逆结果与 ColorMatrix1 相乘为单位阵");
    check(matricesNear(math::multiply(profile.colorMatrix2, camToXyz2),
                       identityMatrix3x3(), 2e-4f),
          "求逆结果与 ColorMatrix2 相乘为单位阵");

    // 损坏 / 缺少矩阵的输入必须被拒绝
    const DcpProfile tooShort = parseDcpProfileFromBytes(std::vector<std::uint8_t>{});
    check(!tooShort.valid, "空数据被拒绝");

    std::vector<std::uint8_t> badMagic = {'X', 'X', 42, 0, 8, 0, 0, 0};
    check(!parseDcpProfileFromBytes(badMagic).valid, "错误字节序标识被拒绝");

    // 只保留 Header + 空 IFD：没有 ColorMatrix，应明确失败
    std::vector<std::uint8_t> noMatrix = {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const DcpProfile noMatrixProfile = parseDcpProfileFromBytes(noMatrix);
    check(!noMatrixProfile.valid, "缺少 ColorMatrix 的 DCP 被拒绝");

    // 单矩阵路径由实现中的“复制另一端点”逻辑覆盖；
    // 这里验证双矩阵文件被正确识别为双光源配置。
    check(profile.isDualIlluminant(), "双矩阵配置被识别为双光源");
}

// ===========================================================================
//  11.6 软打样转换
// ===========================================================================

void testSoftProofTransform()
{
    std::printf("\n[软打样]\n");

    // 构造一张包含高饱和色的线性工作空间图（ProPhoto 线性 0~1）。
    ImageBuffer source(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float fx = static_cast<float>(x) / 15.0f;
            const float fy = static_cast<float>(y) / 15.0f;
            source.at(x, y, 0) = fx;
            source.at(x, y, 1) = 0.5f * (fx + fy);
            source.at(x, y, 2) = fy;
        }
    }

    ColorManagement colorManagement;
    const ColorProfile* proofProfile = colorManagement.profileFor(ColorSpace::sRGB);
    check(proofProfile != nullptr, "取得打样 profile（内置 sRGB）");

    std::vector<std::uint8_t> normal;
    check(colorManagement.convertWorkingToDisplay8(source, normal).isOk(),
          "普通显示器转换执行成功");
    check(normal.size() == source.sampleCount(), "普通显示数据尺寸正确");

    if (proofProfile == nullptr) {
        return;
    }

    SoftProofOptions proof;
    proof.proofingProfile = proofProfile;
    std::vector<std::uint8_t> relative;
    check(colorManagement.convertWorkingToDisplay8(source, relative, proof).isOk(),
          "相对比色软打样执行成功");
    check(relative.size() == normal.size(), "软打样输出尺寸与普通预览一致");

    proof.simulatePaperWhite = true;
    std::vector<std::uint8_t> absolute;
    check(colorManagement.convertWorkingToDisplay8(source, absolute, proof).isOk(),
          "模拟纸白（绝对比色）软打样执行成功");
    check(absolute.size() == normal.size(), "绝对比色软打样输出尺寸正确");
}

// ===========================================================================
//  diy.md §4：管线阶段注册表
// ===========================================================================

void testPipelineStages()
{
    std::printf("\n[管线阶段注册表]\n");

    const auto& registry = pipelineStageRegistry();
    check(registry.size() == static_cast<std::size_t>(kPipelineStageCount),
          "注册表包含全部阶段");

    std::vector<std::string> keys;
    for (const auto& descriptor : registry) {
        keys.push_back(descriptor.key);
    }
    std::sort(keys.begin(), keys.end());
    check(std::adjacent_find(keys.begin(), keys.end()) == keys.end(),
          "阶段键名唯一");

    std::string error;
    check(validatePipelineOrder(defaultPipelineOrder(), error),
          "默认阶段顺序通过校验");
    if (!error.empty()) {
        std::printf("  默认顺序错误：%s\n", error.c_str());
    }

    // 同一分组内、依赖均已满足的两个阶段可以交换
    std::vector<PipelineStageId> swapped = defaultPipelineOrder();
    const auto findIndex = [&swapped](PipelineStageId id) {
        return static_cast<int>(
            std::find(swapped.begin(), swapped.end(), id) - swapped.begin());
    };
    const int lcaIndex = findIndex(PipelineStageId::Lca);
    const int locaIndex = findIndex(PipelineStageId::Loca);
    std::swap(swapped[static_cast<std::size_t>(lcaIndex)],
              swapped[static_cast<std::size_t>(locaIndex)]);
    check(validatePipelineOrder(swapped, error),
          "LCA / LoCA 可以交换顺序");

    // 依赖违例：Demosaic 不能在拜耳降噪之前
    std::vector<PipelineStageId> invalid = defaultPipelineOrder();
    const int demosaicIndex = findIndex(PipelineStageId::Demosaic);
    const int denoiseIndex = findIndex(PipelineStageId::BayerDenoise);
    std::swap(invalid[static_cast<std::size_t>(demosaicIndex)],
              invalid[static_cast<std::size_t>(denoiseIndex)]);
    check(!validatePipelineOrder(invalid, error), "依赖违例被拒绝");

    // 缺少阶段
    std::vector<PipelineStageId> missing = defaultPipelineOrder();
    missing.pop_back();
    check(!validatePipelineOrder(missing, error), "缺少阶段被拒绝");

    bool found = false;
    check(pipelineStageFromKey("demosaic", &found) == PipelineStageId::Demosaic
              && found,
          "键名可还原为阶段 id");
    check(std::string(pipelineStageKey(PipelineStageId::CameraToXyz))
              == "camera-to-xyz",
          "阶段 id 可生成稳定键名");
}

// ===========================================================================
//  append_mask.md：亮度范围 / 颜色范围蒙版
// ===========================================================================

void testAppendMaskModels()
{
    std::printf("\n[亮度范围 / 颜色范围蒙版]\n");

    // 亮度范围：区间内 1，区间外按柔化衰减
    LuminanceMaskSettings luminance;
    luminance.low = 0.2f;
    luminance.high = 0.8f;
    luminance.softness = 0.1f;
    check(std::fabs(luminance.weightAtLinear(0.5f) - 1.0f) < 1e-6f,
          "亮度区间内权重为 1");
    check(luminance.weightAtLinear(0.05f) < 1e-4f,
          "远低于下限时权重趋近 0");
    check(luminance.weightAtLinear(0.95f) < 1e-4f,
          "远高于上限时权重趋近 0");
    check(luminance.weightAtLinear(0.19f) > 0.0f
              && luminance.weightAtLinear(0.19f) < 1.0f,
          "边界柔化产生中间权重");

    // Lab 转换：ProPhoto 中性白应得到 L≈100、a/b≈0
    float labL = 0.0f;
    float labA = 0.0f;
    float labB = 0.0f;
    math::linearProPhotoToLabD50(1.0f, 1.0f, 1.0f, labL, labA, labB);
    check(std::fabs(labL - 100.0f) < 0.5f
              && std::fabs(labA) < 0.5f && std::fabs(labB) < 0.5f,
          "线性 ProPhoto 白点转换到 Lab D50");

    // 颜色范围：同色相 / 彩度权重接近 1
    ColorRangeMaskSettings color;
    color.targets.push_back(LabColorPoint{50.0f, 40.0f, 20.0f});
    color.hueTolerance = 20.0f;
    color.satTolerance = 15.0f;
    color.softness = 0.1f;
    check(color.weightAtLab(50.0f, 40.0f, 20.0f) > 0.999f,
          "目标 Lab 颜色权重为 1");
    check(color.weightAtLab(50.0f, -40.0f, -20.0f) < 1e-4f,
          "相反色相被排除");

    ColorRangeMaskSettings multi = color;
    multi.targets.push_back(LabColorPoint{50.0f, -40.0f, -20.0f});
    check(multi.weightAtLab(50.0f, -40.0f, -20.0f) > 0.999f,
          "多目标颜色取最大权重");

    // 明度约束默认关闭；开启后亮度过高会被排除
    ColorRangeMaskSettings lightness = color;
    check(lightness.weightAtLab(95.0f, 40.0f, 20.0f) > 0.999f,
          "默认色相筛选不受明度影响");
    lightness.lightnessEnabled = true;
    lightness.lightnessLow = 0.2f;
    lightness.lightnessHigh = 0.6f;
    check(lightness.weightAtLab(95.0f, 40.0f, 20.0f) < 1e-4f,
          "开启明度约束后高亮像素被排除");

    // 范围权重缓存：同一层 + 同一图像应命中缓存
    {
        ImageBuffer image(64, 64);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                // 0 ~ 0.8：覆盖 lower 外侧与区间上界
                const float value = 0.8f * static_cast<float>(x) / 63.0f;
                image.at(x, y, 0) = value;
                image.at(x, y, 1) = value;
                image.at(x, y, 2) = value;
            }
        }

        MaskLayer layer;
        layer.shape = MaskShape::Luminance;
        layer.luminance.low = 0.2f;
        layer.luminance.high = 0.8f;

        std::vector<float> direct;
        buildRangeWeightMap(layer, image, direct);
        check(direct.size() == 64u * 64u, "范围权重图尺寸正确");
        check(direct.front() < 0.01f && direct.back() > 0.99f,
              "范围权重随亮度变化");

        MaskWeightCache cache;
        const std::vector<float> first = cache.rangeWeightMap(layer, image, 64, 64);
        check(first == direct, "缓存权重图与直接计算一致");
        const std::vector<float> second = cache.rangeWeightMap(layer, image, 64, 64);
        check(first == second && cache.cachedBytes() > 0,
              "重复请求命中范围权重缓存");
    }

    // 序列化名称往返
    check(std::string(maskShapeName(MaskShape::Luminance)) == "luminance"
              && maskShapeFromName("luminance") == MaskShape::Luminance,
          "亮度蒙版名称往返");
    check(std::string(maskShapeName(MaskShape::ColorRange)) == "colorRange"
              && maskShapeFromName("colorRange") == MaskShape::ColorRange,
          "颜色范围蒙版名称往返");
    check(std::string(luminanceMaskModeName(LuminanceMaskMode::ProPhotoY))
                  == "prophotoY"
              && luminanceMaskModeFromName("prophotoY")
                  == LuminanceMaskMode::ProPhotoY,
          "亮度计算模式名称往返");
}

// ===========================================================================
//  D1：DCP 完整色彩链
// ===========================================================================

void testDcpApplyChain()
{
    std::printf("\n[DCP 色彩链]\n");

    // ---- 恒等 HueSatMap 不应改动任何像素 ----
    DcpHueSatMap identity;
    identity.hueDivisions = 2;
    identity.satDivisions = 2;
    identity.valDivisions = 1;
    identity.data.assign(12, 0.0f);
    for (int i = 0; i < 4; ++i) {
        identity.data[static_cast<std::size_t>(i) * 3u + 1u] = 1.0f;
        identity.data[static_cast<std::size_t>(i) * 3u + 2u] = 1.0f;
    }

    ImageBuffer image(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            image.at(x, y, 0) = 0.2f + 0.1f * static_cast<float>(x);
            image.at(x, y, 1) = 0.3f;
            image.at(x, y, 2) = 0.4f;
        }
    }
    const ImageBuffer original = image;
    ops::applyDcpHueSatMap(image, identity);
    bool unchanged = true;
    for (std::size_t i = 0; i < image.sampleCount(); ++i) {
        if (image.sample(i) != original.sample(i)) {
            unchanged = false;
            break;
        }
    }
    check(unchanged, "恒等 HueSatMap 逐位不改变像素");

    // ---- 色相偏移：纯红向橙色转动 ----
    DcpHueSatMap hueShift = identity;
    for (int i = 0; i < 4; ++i) {
        hueShift.data[static_cast<std::size_t>(i) * 3u] = 0.1f;  // 36°
    }
    ImageBuffer red(2, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            red.at(x, y, 0) = 1.0f;
            red.at(x, y, 1) = 0.0f;
            red.at(x, y, 2) = 0.0f;
        }
    }
    ops::applyDcpHueSatMap(red, hueShift);
    check(red.at(0, 0, 1) > 0.4f, "HueSatMap 色相偏移把纯红转向橙");

    // ---- sRGB 编码：明度轴按编码值索引，只影响 V ----
    // 节点 0 的 valScale=1、节点 1 的 valScale=2。线性 V=0.5 时插值位置 0.5，
    // 输出 V=0.75；sRGB 编码后 V≈0.735，缩放再解码后应明显更亮（≈1.75）。
    DcpHueSatMap encodedIdentity = identity;
    encodedIdentity.hueDivisions = 1;
    encodedIdentity.satDivisions = 1;
    encodedIdentity.valDivisions = 2;
    encodedIdentity.srgbEncoding = true;
    encodedIdentity.data = {0.0f, 1.0f, 1.0f,
                            0.0f, 1.0f, 2.0f};
    ImageBuffer encodedGray(1, 1);
    encodedGray.at(0, 0, 0) = 0.5f;
    encodedGray.at(0, 0, 1) = 0.5f;
    encodedGray.at(0, 0, 2) = 0.5f;
    ops::applyDcpHueSatMap(encodedGray, encodedIdentity);
    check(encodedGray.at(0, 0, 0) > 1.5f,
          "sRGB 编码的明度轴按编码值插值（V 缩放后再解码）");

    // ---- ToneCurve：0.5 映射到 0.25 ----
    ImageBuffer gray(2, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            gray.at(x, y, 0) = 0.5f;
            gray.at(x, y, 1) = 0.5f;
            gray.at(x, y, 2) = 0.5f;
        }
    }
    DcpToneCurve curve;
    curve.coordinates = {0.0f, 0.0f, 0.5f, 0.25f, 1.0f, 1.0f};
    ops::applyDcpToneCurve(gray, curve);
    check(near(gray.at(0, 0, 0), 0.25f, 2e-3f), "DCP 色调曲线按分段线性映射 0.5 → 0.25");

    // ---- ForwardMatrix 按色温插值 ----
    DcpProfile profile;
    profile.valid = true;
    profile.hasForwardMatrix1 = true;
    profile.hasForwardMatrix2 = true;
    profile.forwardMatrix1 = identityMatrix3x3();
    profile.forwardMatrix2 = identityMatrix3x3();
    profile.forwardMatrix1[0][0] = 2.0f;
    profile.forwardMatrix2[0][0] = 4.0f;
    profile.illuminant1Temperature = 2856.0f;
    profile.illuminant2Temperature = 6504.0f;

    Matrix3x3 warm{};
    Matrix3x3 daylight{};
    Matrix3x3 middle{};
    check(ops::dcpInterpolatedMatrix(profile, true, 2856.0f, warm)
              && near(warm[0][0], 2.0f, 1e-4f),
          "as-shot 为钨丝灯时取 ForwardMatrix1");
    check(ops::dcpInterpolatedMatrix(profile, true, 6504.0f, daylight)
              && near(daylight[0][0], 4.0f, 1e-4f),
          "as-shot 为日光时取 ForwardMatrix2");
    check(ops::dcpInterpolatedMatrix(profile, true, 5000.0f, middle)
              && middle[0][0] > 2.0f && middle[0][0] < 4.0f,
          "中间色温在两组矩阵之间插值");
}

// ===========================================================================
//  D5 / D6：非局部均值、反卷积与输出锐化
// ===========================================================================

void testAdvancedDetailOps()
{
    std::printf("\n[高级降噪 / 锐化]\n");

    constexpr int kSize = 32;
    std::uint32_t seed = 0x9e3779b9u;
    const auto nextNoise = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f - 0.5f) * 0.05f;
    };

    ImageBuffer noisy(kSize, kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float value = 0.5f + nextNoise();
            noisy.at(x, y, 0) = value;
            noisy.at(x, y, 1) = value;
            noisy.at(x, y, 2) = value;
        }
    }

    const auto lumaStd = [](const ImageBuffer& image) {
        double mean = 0.0;
        double square = 0.0;
        const std::size_t pixels = image.pixelCount();
        for (std::size_t i = 0; i < pixels; ++i) {
            const double y = 0.299 * image.data()[i * 3u]
                           + 0.587 * image.data()[i * 3u + 1]
                           + 0.114 * image.data()[i * 3u + 2];
            mean += y;
            square += y * y;
        }
        mean /= static_cast<double>(pixels);
        return std::sqrt(std::max(0.0, square / static_cast<double>(pixels) - mean * mean));
    };

    const double before = lumaStd(noisy);
    ImageBuffer nlm = noisy;
    ops::applyNoiseReduction(nlm, 60.0f, 0.0f, 1.0f,
                             NoiseReductionMethod::NLMeans, 3);
    const double after = lumaStd(nlm);
    char label[160] = {};
    std::snprintf(label, sizeof(label),
                  "非局部均值降低平坦区噪声（%.5f → %.5f）", before, after);
    check(after < before, label);

    // D5：BM3D 单阶段近似同样应显著降低平坦区噪声
    ImageBuffer bm3d = noisy;
    ops::applyNoiseReduction(bm3d, 60.0f, 0.0f, 1.0f,
                             NoiseReductionMethod::Bm3d, 3);
    const double bm3dAfter = lumaStd(bm3d);
    std::snprintf(label, sizeof(label),
                  "BM3D 近似降低平坦区噪声（%.5f → %.5f）", before, bm3dAfter);
    check(bm3dAfter < before, label);

    // ---- 反卷积锐化：先模糊一个台阶，再用 RL 恢复边缘陡度 ----
    ImageBuffer step(kSize, kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float value = (x < 16) ? 0.2f : 0.5f;
            step.at(x, y, 0) = value;
            step.at(x, y, 1) = value;
            step.at(x, y, 2) = value;
        }
    }
    ImageBuffer blurred;
    ops::gaussianBlur(step, blurred, 4.5f, true);  // σ≈1.5
    const float blurredGradient = blurred.at(16, 16, 1) - blurred.at(15, 16, 1);

    ImageBuffer deconvolved = blurred;
    ops::applySharpen(deconvolved, 80.0f, 1.5f, 0.0f, 1.0f,
                      SharpenMethod::Deconvolution, 8);
    const float deconvolvedGradient =
        deconvolved.at(16, 16, 1) - deconvolved.at(15, 16, 1);
    std::printf("  边缘梯度：模糊=%.4f  反卷积=%.4f\n",
                static_cast<double>(blurredGradient),
                static_cast<double>(deconvolvedGradient));
    check(deconvolvedGradient > blurredGradient * 1.1f,
          "Richardson-Lucy 反卷积恢复边缘陡度");

    // ---- 输出锐化：在最终像素上增强台阶边缘 ----
    ImageBuffer outputSharpened = step;
    ops::applyOutputSharpen(outputSharpened, 60.0f, 0.6f);
    const float sourceEdgeGradient =
        step.at(16, 16, 1) - step.at(15, 16, 1);
    const float outputEdgeGradient =
        outputSharpened.at(16, 16, 1) - outputSharpened.at(15, 16, 1);
    check(outputEdgeGradient > sourceEdgeGradient,
          "输出锐化增强台阶边缘的对比");
    check(near(outputSharpened.at(24, 24, 1), step.at(24, 24, 1), 1e-6f),
          "输出锐化不改变远离边缘的平坦区");
}

int main()
{
    std::printf("==================== 核心算法单元测试 ====================\n");

    testTemperatureRoundTrip();
    testWhitePointFromTemperatureTint();
    testCurveLut();
    testParametricCurve();
    testMatrixValidation();
    testSolveFromColorChecker();
    testFrameCalibration();
    testMaskModel();
    testMaskWeightMap();
    testMaskWeightCache();
    testLocalAdjustments();
    testCropOperations();
    testPerspectiveCorrection();
    testRetouch();
    testCsvRoundTrip();
    testEditContextDefaults();
    testWhiteBalanceLayers();
    testHslBandMapping();
    testPreviewBinning();
    testDownsampleRgbArea();
    testPreviewDownsampleSelection();
    testSharpenEffect();
    testFalseColorSuppression();
    testClipHighlightReconstruction();
    testFullResolutionClipMask();
    testMoireSuppressionStrength();
    testLargeChromaMoireSuppression();
    testDemosaicQuality();
    testXTransDemosaic();
    testWaveletNoiseReduction();
    testDualIlluminantCcm();
    testPipelineStages();
    testDcpParsing();
    testAppendMaskModels();
    testDcpApplyChain();
    testAdvancedDetailOps();
    testSoftProofTransform();
    testScaleAwareDetailOps();
    testPreviewTone();
    testPresenceAdjustments();
    testColorGrading();
    testBlackWhiteMix();
    testFalseColorCorrections();
    testLensDatabase();

    std::printf("\n====================================================\n");
    if (g_failures == 0) {
        std::printf("全部检查通过\n");
        return 0;
    }
    std::printf("有 %d 项检查失败\n", g_failures);
    return 1;
}
