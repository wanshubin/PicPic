// ============================================================================
//  RawLoader.cpp —— libraw 封装实现
// ============================================================================

#include "RawLoader.h"

#include "LinearMath.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>

// libraw 头文件位置随安装渠道而变，由 CMake 注入宏统一处理
#include RAWCore_LIBRAW_HEADER

// libraw 0.22 把 LIBRAW_INSUFFICIENT_MEMORY 更名为 LIBRAW_UNSUFFICIENT_MEMORY。
// 两者都是枚举常量而非宏，无法用 #ifndef 探测，因此按版本号做预处理选择。
#if LIBRAW_MINOR_VERSION >= 22
#  define RAW_LIBRAW_INSUFFICIENT_MEMORY LIBRAW_UNSUFFICIENT_MEMORY
#else
#  define RAW_LIBRAW_INSUFFICIENT_MEMORY LIBRAW_INSUFFICIENT_MEMORY
#endif

namespace rawcore {

namespace {

// ---------------------------------------------------------------------------
//  以 UTF-8 路径打开 RAW 文件
//
//  调用方传入的 std::string 来自 QString::toStdString()，编码是 UTF-8。
//  libraw 的窄字符 open_file() 在 Windows 上内部走 fopen，会按系统 ANSI
//  代码页（中文系统为 GBK）解释路径，含中文等非 ASCII 字符的路径必然打开失败，
//  并被 libraw 归为 LIBRAW_IO_ERROR（表现为"文件无法读取，可能已损坏"的误报）。
//  libraw 在 MSVC 下提供了宽字符重载，这里在 Windows 上统一改用宽路径；
//  其它平台保持 UTF-8 窄路径即可。
// ---------------------------------------------------------------------------
int openRawFile(LibRaw& raw, const std::string& utf8Path)
{
#if defined(_WIN32)
    // C++17 的 u8path 会把 UTF-8 字节串正确转换为平台本地路径（Windows 上为宽字符）
    const std::wstring widePath = std::filesystem::u8path(utf8Path).wstring();
    return raw.open_file(widePath.c_str());
#else
    return raw.open_file(utf8Path.c_str());
#endif
}

// ---------------------------------------------------------------------------
//  拍摄时间格式化
//
//  libraw 提供的是 time_t（相机记录的本地时间）。选图的「按拍摄时间排序」
//  与信息栏都按 "yyyy-MM-dd HH:mm:ss" 显示，这里统一转成该格式，
//  避免 UI 层再处理平台相关的本地时间转换。
// ---------------------------------------------------------------------------
std::string formatCaptureTimestamp(std::time_t timestamp)
{
    if (timestamp <= 0) {
        return {};
    }
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &timestamp);
#else
    localtime_r(&timestamp, &local);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

// ---------------------------------------------------------------------------
//  sRGB(D65) → XYZ(D50) 常量矩阵
//
//  用途：cam_xyz 不可用时回退到 libraw 的 rgb_cam（CamRGB → sRGB D65）。
//  本软件的内部工作空间是 ProPhotoRGB(D50)，管线中间以 XYZ(D50) 为枢纽，
//  因此需要把 rgb_cam 右乘这个 sRGB→XYZ(D50) 矩阵，
//  才能得到"相机原厂 CamRGB → XYZ(D50)"矩阵。
//
//  该矩阵为 sRGB 基色结合 Bradford 色适应(D65→D50)的合成结果，
//  数值取自标准色彩管理资料，小数点后 6 位足够修图精度。
// ---------------------------------------------------------------------------
constexpr float kSrgbD65ToXyzD50[3][3] = {
    {0.4360747f, 0.3850649f, 0.1430804f},
    {0.2225045f, 0.7168786f, 0.0606169f},
    {0.0139322f, 0.0971045f, 0.7141733f},
};

Matrix3x3 srgbToXyzD50Matrix()
{
    Matrix3x3 m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r][c] = kSrgbD65ToXyzD50[r][c];
        }
    }
    return m;
}

/// 把 libraw 的 rgb_cam（float[4][3]，行=相机通道，列=sRGB）转为 Matrix3x3。
/// libraw 0.21 及更早版本的布局：4 行（多一个第二绿通道），取前 3 行。
Matrix3x3 rgbCamToMatrix3x3(const float (&rgbCam)[4][3])
{
    Matrix3x3 m{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r][c] = rgbCam[r][c];
        }
    }
    return m;
}

/// libraw 0.22 起 rgb_cam 改为 float[3][4]（行=sRGB 输出，列=相机通道），
/// 与旧布局互为转置；这里转置回"相机通道 → sRGB"的 3x3。
Matrix3x3 rgbCamToMatrix3x3(const float (&rgbCam)[3][4])
{
    Matrix3x3 m{};
    for (int out = 0; out < 3; ++out) {
        for (int cam = 0; cam < 3; ++cam) {
            m[cam][out] = rgbCam[out][cam];
        }
    }
    return m;
}

/// 相机原厂 CamRGB → XYZ(D50) 矩阵求解结果
struct CameraMatrixSolution
{
    Matrix3x3 camToXyz{};  ///< 相机原始通道 → XYZ(D50)
    bool valid = false;    ///< 是否得到物理可信的原厂矩阵
};

/// 求解 CamRGB → XYZ(D50) 矩阵。
///
/// 首选 libraw 的 cam_xyz：它是"XYZ → 相机通道"的原厂矩阵（DNG ColorMatrix），
/// 直接求逆即得到物理正确的 CamRGB → XYZ，未引入任何逐通道归一化。
///
/// 兜底才用 rgb_cam。libraw 生成 rgb_cam 的流程（utils_dcraw.cpp: cam_xyz_coeff）
/// 是先把 cam_rgb 每一行除以行和做归一化，被除掉的缩放记录在 pre_mul 中：
///
///     CamRGB → sRGB 的实际矩阵 = rgb_cam^T * diag(pre_mul)
///
/// 因此单独使用 rgb_cam 会丢失通道缩放（表现为中灰与肤色严重偏色），
/// 必须把 pre_mul 作为列缩放补回去，再右乘 sRGB(D65) → XYZ(D50)。
CameraMatrixSolution buildCameraToXyz(const LibRaw& raw)
{
    const auto& color = raw.imgdata.color;
    CameraMatrixSolution solution;

    // ---- 首选：对原厂 XYZ→CamRGB 矩阵求逆 ----
    Matrix3x3 xyzToCam{};
    bool hasCamXyz = false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            xyzToCam[r][c] = color.cam_xyz[r][c];
            if (std::fabs(color.cam_xyz[r][c]) > 1e-6f) {
                hasCamXyz = true;
            }
        }
    }
    if (hasCamXyz && std::fabs(math::determinant(xyzToCam)) > 1e-6f
        && math::invert(xyzToCam, solution.camToXyz)) {
        solution.valid = true;
        return solution;
    }

    // ---- 兜底：rgb_cam 补 pre_mul 通道缩放 ----
    const Matrix3x3 camToSrgb = rgbCamToMatrix3x3(color.rgb_cam);
    if (std::fabs(math::determinant(camToSrgb)) < 1e-6f) {
        return solution;
    }

    // 先转到 sRGB(D65) 参考系，再经色适应转到 XYZ(D50)
    Matrix3x3 m = math::multiply(srgbToXyzD50Matrix(), camToSrgb);

    // 补回 libraw 归一化时除掉的通道缩放（右乘对角矩阵 = 缩放各列）。
    // pre_mul 为 0 通常表示该通道未被使用，此时保持 1 以免整列塌成 0。
    for (int c = 0; c < 3; ++c) {
        const float scale = (color.pre_mul[c] > 1e-6f) ? color.pre_mul[c] : 1.0f;
        for (int r = 0; r < 3; ++r) {
            m[r][c] *= scale;
        }
    }

    solution.camToXyz = m;
    solution.valid = true;
    return solution;
}

// libraw 0.22 移除了 imgother.focus_distance 字段。
// 这里用编译期检测判断当前头文件是否仍提供该字段，缺失时保持默认值 0。
template <typename T, typename = void>
struct HasFocusDistance : std::false_type {};

template <typename T>
struct HasFocusDistance<T, std::void_t<decltype(std::declval<T>().focus_distance)>>
    : std::true_type {};

template <typename ImgOther>
void copyFocusDistance(const ImgOther& other, RawMetadata& metadata)
{
    if constexpr (HasFocusDistance<ImgOther>::value) {
        metadata.focusDistance = other.focus_distance;
    }
}

/// 由 Planckian 轨迹上的 y 坐标反解色温。
/// 与 math::temperatureToXy 使用同一套拟合公式做数值反演（二分法），
/// 保证"反算 → 正向重建"是自洽的，不会因正反公式不同引入色调偏差。
/// 轨迹上的 y 随色温单调递减，超出 1000K~40000K 覆盖范围时返回 false。
bool planckianTemperatureFromY(float y, float& outKelvin)
{
    float trackX = 0.0f;
    float yLow = 0.0f;
    float yHigh = 0.0f;
    math::temperatureToXy(1000.0f, trackX, yLow);
    math::temperatureToXy(40000.0f, trackX, yHigh);

    if (y > yLow || y < yHigh) {
        return false;
    }

    float low = 1000.0f;
    float high = 40000.0f;
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (low + high);
        float x = 0.0f;
        float yMid = 0.0f;
        math::temperatureToXy(mid, x, yMid);
        // y 随色温升高而减小：中点偏暖（y 更大）说明目标色温更高
        if (yMid > y) {
            low = mid;
        } else {
            high = mid;
        }
    }

    outKelvin = 0.5f * (low + high);
    return true;
}

/// 由相机白平衡增益反算 as-shot 光源色温与色调。
///
/// 原理：libraw 的 cam_mul 是"乘到原始通道上"的增益，对场景中性灰有
///   rawR * gainR = rawG * gainG = rawB * gainB
/// 所以相机空间中光源白点的方向是增益的**倒数** (1/gainR, 1, 1/gainB)
/// （以绿通道归一化）。注意不能用增益本身——那正好是白点的反方向，
/// 会得到色温约 9000K~10000K、tint 顶格的错误结果，画面严重偏黄。
///
/// 白点向量经 camToXyz（相机 → XYZ D50）变换后取 xy，即光源色度；
/// 再由 xy 反解色温，并按 UI 的 tint 定义换算。这里必须使用与管线
/// 完全相同的 camToXyz，才能保证"反算 → applyWhiteBalance 补偿"
/// 在数学上互相抵消，初始画面恢复中性。
///
/// @param camMul     libraw 的 cam_mul（顺序 R/G1/B/G2）
/// @param camToXyz   相机原始通道 → XYZ(D50) 矩阵
bool computeAsShotWhiteBalance(const float* camMul,
                               const Matrix3x3& camToXyz,
                               float& outTemperature,
                               float& outTint)
{
    if (camMul == nullptr) {
        return false;
    }

    // 相机矩阵必须是可用的非奇异矩阵，否则白点方向无意义
    if (std::fabs(math::determinant(camToXyz)) < 1e-6f) {
        return false;
    }

    float gainR = camMul[0];
    float gainG = camMul[1];
    float gainB = camMul[2];
    // 部分文件只记录三项，或 G2 与 G1 相同；G1 无效时退回 G2
    if (!(gainG > 1e-6f)) {
        gainG = (camMul[3] > 1e-6f) ? camMul[3] : 0.0f;
    }
    if (!(gainR > 1e-6f) || !(gainG > 1e-6f) || !(gainB > 1e-6f)) {
        return false;
    }

    // 归一化到绿通道：相机空间中的光源白点（增益的倒数）
    const float camWhite[3] = {gainG / gainR, 1.0f, gainG / gainB};

    float xyz[3];
    math::multiply(camToXyz, camWhite, xyz);
    const float sum = xyz[0] + xyz[1] + xyz[2];
    if (!(sum > 1e-9f)) {
        return false;
    }

    const float x = xyz[0] / sum;
    const float y = xyz[1] / sum;

    // 先在 UI 允许的色温范围内反解；失败再退回 McCamy 近似
    float kelvin = 0.0f;
    if (!planckianTemperatureFromY(y, kelvin)) {
        kelvin = math::xyToTemperature(x, y);
        if (kelvin <= 0.0f) {
            return false;
        }
    }

    // 色温夹取到 UI 滑块范围（2000K~12000K），保证面板可以直接载入
    kelvin = math::clamp(kelvin, 2000.0f, 12000.0f);

    // 按 whitePointFromTemperatureTint 的正向定义反解 tint：
    // 正向是 x = x_track(T) + 0.0002 * tint，y 保持不变
    float trackX = 0.0f;
    float trackY = 0.0f;
    math::temperatureToXy(kelvin, trackX, trackY);

    outTemperature = kelvin;
    outTint = math::clamp((x - trackX) / 0.0002f, -100.0f, 100.0f);
    return true;
}

/// 由白点 xy 与色温反解 tint（与 computeAsShotWhiteBalance 的定义一致）
float tintFromWhiteXy(float x, float y, float kelvin)
{
    (void)y;
    float trackX = 0.0f;
    float trackY = 0.0f;
    math::temperatureToXy(kelvin, trackX, trackY);
    return math::clamp((x - trackX) / 0.0002f, -100.0f, 100.0f);
}

/// 尝试使用相机 / 厂商记录的 as-shot 色温（libraw WBCT_Coeffs 第一行）。
///
/// D3：DNG 的 AsShotNeutral、厂商 MakerNote 的色温预设都被 libraw 收进
/// WBCT_Coeffs（第一行即 as-shot）。直接采用相机记录的 CCT 比"由增益经
/// Planckian 轨迹反算"更可靠，尤其是荧光灯 / LED 等非黑体光源。
///
/// tint 仍由增益换算出的白点色度决定：色温给出黑体轨迹上的位置，
/// 实际白点偏离轨迹的方向即为 tint。
bool computeAsShotFromCameraCct(const LibRaw& raw,
                                const Matrix3x3& camToXyz,
                                float& outTemperature,
                                float& outTint)
{
    const auto& color = raw.imgdata.color;
    const float cct = color.WBCT_Coeffs[0][0];
    if (!(cct >= 1500.0f && cct <= 20000.0f)) {
        return false;
    }

    float gainR = color.WBCT_Coeffs[0][1];
    float gainG = color.WBCT_Coeffs[0][2];
    float gainB = color.WBCT_Coeffs[0][3];
    if (!(gainG > 1e-6f) && color.WBCT_Coeffs[0][4] > 1e-6f) {
        gainG = color.WBCT_Coeffs[0][4];
    }
    if (!(gainR > 1e-6f) || !(gainG > 1e-6f) || !(gainB > 1e-6f)) {
        return false;
    }

    // 一致性校验：不同 libraw 版本 / 机型的 WBCT_Coeffs[0] 未必就是 as-shot。
    // 只有它与 cam_mul 的通道比例一致时才采用记录色温，否则退回
    // "由 as-shot 增益经 Planckian 反算"，避免把某个预设当成拍摄白平衡。
    if (!(color.cam_mul[0] > 1e-6f) || !(color.cam_mul[1] > 1e-6f)
        || !(color.cam_mul[2] > 1e-6f)) {
        return false;
    }
    const auto relativeDifference = [](float a, float b) {
        const float scale = std::max(std::fabs(b), 1e-6f);
        return std::fabs(a - b) / scale;
    };
    const float wbRedRatio = gainR / gainG;
    const float wbBlueRatio = gainB / gainG;
    const float camRedRatio = color.cam_mul[0] / color.cam_mul[1];
    const float camBlueRatio = color.cam_mul[2] / color.cam_mul[1];
    if (relativeDifference(wbRedRatio, camRedRatio) > 0.25f
        || relativeDifference(wbBlueRatio, camBlueRatio) > 0.25f) {
        return false;
    }

    float tint = 0.0f;
    const bool haveGains = gainR > 1e-6f && gainG > 1e-6f && gainB > 1e-6f
        && std::fabs(math::determinant(camToXyz)) > 1e-6f;
    if (haveGains) {
        const float camWhite[3] = {gainG / gainR, 1.0f, gainG / gainB};
        float xyz[3];
        math::multiply(camToXyz, camWhite, xyz);
        const float sum = xyz[0] + xyz[1] + xyz[2];
        if (sum > 1e-9f) {
            tint = tintFromWhiteXy(xyz[0] / sum, xyz[1] / sum,
                                   math::clamp(cct, 2000.0f, 12000.0f));
        }
    }

    outTemperature = math::clamp(cct, 2000.0f, 12000.0f);
    outTint = tint;
    return true;
}

/// 把反算结果写入元数据；无法反算时退回标准日光，保证上层始终有可用初值
void fillAsShotWhiteBalance(const LibRaw& raw,
                            const CameraMatrixSolution& matrix,
                            RawMetadata& metadata)
{
    const auto& color = raw.imgdata.color;

    float temperature = 0.0f;
    float tint = 0.0f;

    // D3 优先级：
    //   1. 厂商 / DNG 记录的色温（WBCT_Coeffs）；
    //   2. 相机记录的 as-shot 增益（DNG AsShotNeutral 会被 libraw 转成 cam_mul）；
    //   3. libraw 推算的 pre_mul；
    //   4. 标准日光兜底。
    // 只有前三项都不可用时才走 Planckian 反算，避免在非黑体光源下偏色。
    bool solved = matrix.valid
        && computeAsShotFromCameraCct(raw, matrix.camToXyz, temperature, tint);
    std::string source = "camera-cct";

    if (!solved && matrix.valid) {
        solved = computeAsShotWhiteBalance(color.cam_mul, matrix.camToXyz,
                                           temperature, tint);
        source = raw.imgdata.idata.dng_version != 0 ? "as-shot-neutral" : "planckian";
    }
    if (!solved && matrix.valid) {
        solved = computeAsShotWhiteBalance(color.pre_mul, matrix.camToXyz,
                                           temperature, tint);
        source = "planckian";
    }

    if (solved) {
        metadata.asShotTemperature = temperature;
        metadata.asShotTint = tint;
        metadata.whiteBalanceSource = source;
    } else {
        metadata.asShotTemperature = 5500.0f;
        metadata.asShotTint = 0.0f;
        metadata.whiteBalanceSource = "default";
    }
}

/// 提取相机域 as-shot 白平衡**增益**（以绿通道归一化）。
///
/// DCP ForwardMatrix 的输入定义是"已按 AsShotNeutral 白平衡的相机 RGB"，
/// 即 raw × (1/AsShotNeutral)；libraw 的 cam_mul 正是这组乘数。
/// 注意这里保存的是"乘上去"的增益，而不是白点本身（白点是增益的倒数）。
void fillCameraNeutral(const LibRaw& raw, RawDecodeResult& result)
{
    const auto& color = raw.imgdata.color;

    const float* gains = nullptr;
    if (color.cam_mul[0] > 1e-6f && color.cam_mul[1] > 1e-6f
        && color.cam_mul[2] > 1e-6f) {
        gains = color.cam_mul;
    } else if (color.pre_mul[0] > 1e-6f && color.pre_mul[1] > 1e-6f
               && color.pre_mul[2] > 1e-6f) {
        gains = color.pre_mul;
    }
    if (gains == nullptr) {
        return;
    }

    float gainG = gains[1];
    if (!(gainG > 1e-6f)) {
        gainG = (gains[3] > 1e-6f) ? gains[3] : 0.0f;
    }
    if (!(gainG > 1e-6f)) {
        return;
    }

    // 归一化到绿通道：绿通道增益记为 1，白平衡保持整体亮度
    result.cameraWhiteBalanceGain = {gains[0] / gainG, 1.0f, gains[2] / gainG};
    result.hasCameraWhiteBalanceGain = true;
}

// ---------------------------------------------------------------------------
//  拜耳排列解析
//
//  重要：cdesc 不是拜耳排列！
//  libraw 的 cdesc 只是"通道代码 → 颜色字符"的查找表，三色相机几乎恒为
//  "RGBG"（代码 0=R、1=G、2=B、3=G）。把它直接当成排列字符串会导致所有
//  相机都被判成 RGGB，实际为 BGGR/GRBG/GBRG 的机器会整幅偏色。
//  真正的排列存放在 filters 位域中：2x2 单元每个位点占 2 bit，
//  取出的代码再经 cdesc 映射为颜色字符。
// ---------------------------------------------------------------------------

BayerPattern patternFromFilters(unsigned filters, const char* cdesc)
{
    // filters == 9 是 libraw 对 X-Trans 传感器的约定：没有 2x2 的 filters
    // 位域，真实排列由 idata.xtrans[6][6] 描述。
    if (filters == 9) {
        return BayerPattern::XTrans;
    }
    // filters == 0 表示不是标准拜耳阵列（线性 DNG、Foveon 等）；
    // 小于 1000 的其它取值是 libraw 内部的紧凑编码，本管线不支持。
    if (filters == 0 || filters < 1000) {
        return BayerPattern::Unknown;
    }

    const char* table = (cdesc != nullptr) ? cdesc : "RGBG";
    char pattern[5] = {0, 0, 0, 0, 0};
    int index = 0;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            // 与 libraw 的 FC 宏一致：(row, col) 决定代码在 filters 中的偏移
            const int shift = ((((row << 1) & 14) + (col & 1)) << 1);
            const int code = static_cast<int>((filters >> shift) & 3u);
            const char ch = table[code];
            pattern[index++] = (ch != '\0') ? ch : "RGBG"[code];
        }
    }

    // 三色拜耳应恰好得到 R/G/B 各字符，映射到枚举
    return bayerPatternFromString(pattern);
}

BayerPattern patternFromCdesc(const char* cdesc)
{
    if (cdesc == nullptr) {
        return BayerPattern::Unknown;
    }
    // 少数文件（如部分 DNG）会直接给出 "RGGB" 这类排列字符串
    BayerPattern p = bayerPatternFromString(cdesc);
    if (p != BayerPattern::Unknown) {
        return p;
    }
    // 4 字符形式：cdesc 若恰好按 2x2 顺序书写，可据此推断
    const std::string s(cdesc);
    if (s == "RGBG") return BayerPattern::RGGB;
    if (s == "GRGB") return BayerPattern::GRBG;
    if (s == "GBGR") return BayerPattern::GBRG;
    if (s == "BGRG") return BayerPattern::BGGR;
    return BayerPattern::Unknown;
}

// ---------------------------------------------------------------------------
//  扩展名判断
// ---------------------------------------------------------------------------

std::string toLowerExtension(const std::string& filePath)
{
    const std::size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = filePath.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Pimpl
//
//  把 LibRaw 对象与所有 libraw 类型都收在这里，公共头文件保持干净，
//  上层代码无需包含 libraw 头文件（模块解耦要求）。
// ---------------------------------------------------------------------------

struct RawLoader::Impl {
    LibRaw raw;
    std::atomic<bool> cancelRequested{false};

    /// libraw 进度回调需要访问本对象，用静态函数 + user_data 传回
    static int progressCallback(void* context, enum LibRaw_progress stage, int iteration, int expected)
    {
        auto* self = static_cast<Impl*>(context);
        if (self == nullptr) {
            return 0;
        }
        if (self->cancelRequested.load(std::memory_order_relaxed)) {
            return 1;  // 非零值请求 libraw 中止
        }
        (void)stage;
        (void)iteration;
        (void)expected;
        return 0;
    }
};

// ---------------------------------------------------------------------------
//  构造 / 析构
// ---------------------------------------------------------------------------

RawLoader::RawLoader()
    : m_impl(std::make_unique<Impl>())
{
}

RawLoader::~RawLoader() = default;

void RawLoader::requestCancel()
{
    m_impl->cancelRequested.store(true, std::memory_order_relaxed);
}

bool RawLoader::isCancelled() const
{
    return m_impl->cancelRequested.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
//  扩展名支持
// ---------------------------------------------------------------------------

std::vector<std::string> RawLoader::supportedExtensions()
{
    // 均为 libraw 原生支持的格式，按厂商分组便于核对：
    //   佳能：CR2 / CR3 / CRW（早期 CIFF）
    //   尼康：NEF / NRW（Coolpix 压缩 RAW）
    //   索尼：ARW / SR2 / SRF（早期机型）
    //   富士：RAF；松下：RW2；奥林巴斯 / OM SYSTEM：ORF
    //   宾得：PEF；适马：X3F（Foveon）；徕卡：RWL
    //   哈苏：3FR / FFF；飞思：IIQ
    // DNG 为通用格式，单独列出
    return {"dng",
            "cr2", "cr3", "crw",
            "nef", "nrw",
            "arw", "sr2", "srf",
            "raf", "rw2", "orf", "pef",
            "x3f", "rwl",
            "3fr", "fff", "iiq"};
}

bool RawLoader::isSupportedExtension(const std::string& filePath)
{
    const std::string ext = toLowerExtension(filePath);
    if (ext.empty()) {
        return false;
    }
    const auto supported = supportedExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

// ---------------------------------------------------------------------------
//  元数据探测
// ---------------------------------------------------------------------------

ErrorInfo RawLoader::probeMetadata(const std::string& filePath, RawMetadata& metadata)
{
    // LibRaw 对象体积很大（数百 KB），必须放在堆上，否则可能栈溢出
    auto raw = std::make_unique<LibRaw>();

    // 只读元数据，不解析图像数据，速度快
    const int ret = openRawFile(*raw, filePath);
    if (ret != LIBRAW_SUCCESS) {
        if (ret == LIBRAW_FILE_UNSUPPORTED) {
            return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                                   "不支持的文件格式",
                                   "libraw 返回 LIBRAW_FILE_UNSUPPORTED: " + filePath);
        }
        if (ret == LIBRAW_IO_ERROR || ret == LIBRAW_TOO_BIG) {
            return ErrorInfo::make(ErrorCode::FileCorrupted,
                                   "文件无法读取，可能已损坏",
                                   "libraw 返回 IO 错误，码 " + std::to_string(ret));
        }
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "读取文件元数据失败",
                               "libraw open_file 返回 " + std::to_string(ret));
    }

    const auto& img = raw->imgdata;
    metadata.fileName = filePath;
    metadata.cameraMake = img.idata.make;
    metadata.cameraModel = img.idata.model;
    metadata.uniqueCameraModel = img.color.UniqueCameraModel;
    metadata.isDng = img.idata.dng_version != 0;
    metadata.lensModel = img.lens.Lens;
    metadata.width = img.sizes.width;
    metadata.height = img.sizes.height;
    metadata.timestamp = formatCaptureTimestamp(img.other.timestamp);

    // 是否存在可直接显示的内嵌 JPEG。只统计 JPEG：位图 / H.265 / JPEG XL
    // 等格式无法直接交给 QImageReader，按"不可用"处理并回退软件管线。
    {
        const auto& thumbs = img.thumbs_list;
        const int thumbCount =
            std::min<int>(thumbs.thumbcount, LIBRAW_THUMBNAIL_MAXCOUNT);
        for (int i = 0; i < thumbCount; ++i) {
            if (thumbs.thumblist[i].tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG) {
                metadata.hasEmbeddedPreview = true;
                break;
            }
        }
    }

    metadata.iso = img.other.iso_speed;
    metadata.shutter = img.other.shutter;
    metadata.aperture = img.other.aperture;
    metadata.focalLength = img.other.focal_len;
    copyFocusDistance(img.other, metadata);

    // as-shot 白平衡：libraw 只给出 R/G/B 三个增益系数（cam_mul），
    // 并未直接提供色温，这里结合相机原色矩阵做色度反算。
    fillAsShotWhiteBalance(*raw, buildCameraToXyz(*raw), metadata);

    raw->recycle();
    return ErrorInfo::ok();
}

// ---------------------------------------------------------------------------
//  内嵌预览读取（选图缩略图）
// ---------------------------------------------------------------------------

ErrorInfo RawLoader::loadEmbeddedPreview(const std::string& filePath,
                                         std::vector<std::uint8_t>& jpegData)
{
    jpegData.clear();

    // LibRaw 对象较大，放堆上（与 probeMetadata 一致）
    auto raw = std::make_unique<LibRaw>();
    int ret = openRawFile(*raw, filePath);
    if (ret != LIBRAW_SUCCESS) {
        return ErrorInfo::make(ErrorCode::FileCorrupted,
                               "无法读取文件，可能已损坏",
                               "libraw open_file 返回 " + std::to_string(ret));
    }

    // 部分机型同时带小缩略图与整幅 JPEG 预览，取面积最大的 JPEG：
    // 缩略图网格用大图缩放的观感明显更好。跳过位图 / H.265 等格式，
    // 否则会选到无法解码的条目，白白放弃可用的 JPEG。
    const auto& thumbs = raw->imgdata.thumbs_list;
    int bestIndex = -1;
    unsigned bestArea = 0;
    const int count = std::min<int>(thumbs.thumbcount, LIBRAW_THUMBNAIL_MAXCOUNT);
    for (int i = 0; i < count; ++i) {
        if (thumbs.thumblist[i].tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG) {
            continue;
        }
        const unsigned area = static_cast<unsigned>(thumbs.thumblist[i].twidth)
                            * static_cast<unsigned>(thumbs.thumblist[i].theight);
        if (area > bestArea) {
            bestArea = area;
            bestIndex = i;
        }
    }

    // thumbcount 为 0 的旧式文件仍可尝试经典 unpack_thumb()
    ret = (bestIndex >= 0) ? raw->unpack_thumb_ex(bestIndex) : raw->unpack_thumb();
    if (ret != LIBRAW_SUCCESS) {
        raw->recycle();
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "文件没有内嵌预览图",
                               "libraw unpack_thumb 返回 " + std::to_string(ret));
    }

    int error = LIBRAW_SUCCESS;
    libraw_processed_image_t* image = raw->dcraw_make_mem_thumb(&error);
    if (image == nullptr || error != LIBRAW_SUCCESS) {
        if (image != nullptr) {
            raw->dcraw_clear_mem(image);
        }
        raw->recycle();
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "内嵌预览解析失败",
                               "dcraw_make_mem_thumb 返回 " + std::to_string(error));
    }

    // 绝大多数相机把预览存成 JPEG；少数格式（如部分 DNG）是未压缩位图。
    // 位图需要上层再做编码，收益不大，这里统一按"不可用"处理并回退管线。
    if (image->type == LIBRAW_IMAGE_JPEG) {
        jpegData.assign(image->data, image->data + image->data_size);
    }
    raw->dcraw_clear_mem(image);
    raw->recycle();

    if (jpegData.empty()) {
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "内嵌预览不是 JPEG 格式",
                               "libraw thumbnail type != LIBRAW_IMAGE_JPEG");
    }
    return ErrorInfo::ok();
}

// ---------------------------------------------------------------------------
//  主解码流程
// ---------------------------------------------------------------------------

ErrorInfo RawLoader::load(const std::string& filePath,
                          const RawSettings& settings,
                          RawDecodeResult& result)
{
    result = RawDecodeResult{};
    m_impl->cancelRequested.store(false, std::memory_order_relaxed);

    // ---- 1. 文件存在性与扩展名检查 ----
    if (!isSupportedExtension(filePath)) {
        return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                               "不支持的文件格式，请选择受支持的 RAW 文件"
                               "（DNG/CR2/CR3/CRW/NEF/NRW/ARW/SR2/SRF/RAF/RW2/ORF/PEF/X3F/RWL/3FR/FFF/IIQ）",
                               "扩展名不在支持列表: " + filePath);
    }

    // 重新构造 LibRaw，保证多次加载之间没有状态残留
    m_impl->raw.recycle();
    LibRaw& raw = m_impl->raw;

    // 设置进度回调，使长时间解码可以响应取消请求
    raw.set_progress_handler(&Impl::progressCallback, m_impl.get());

    // ---- 2. 打开文件 ----
    int ret = openRawFile(raw, filePath);
    if (ret != LIBRAW_SUCCESS) {
        if (ret == LIBRAW_FILE_UNSUPPORTED) {
            return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                                   "该相机的 RAW 格式暂不受支持",
                                   "libraw open_file -> LIBRAW_FILE_UNSUPPORTED");
        }
        if (ret == LIBRAW_IO_ERROR) {
            return ErrorInfo::make(ErrorCode::FileCorrupted,
                                   "文件无法读取，可能已损坏或无访问权限",
                                   "libraw open_file -> LIBRAW_IO_ERROR");
        }
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "打开 RAW 文件失败",
                               "libraw open_file 返回错误码 " + std::to_string(ret));
    }

    // ---- 3. 配置解码参数（需求 §3 模块 A 的可配置项）----

    // 不生成缩略图与预览图，只取原始拜耳数据（节省内存与时间）
    raw.imgdata.params.no_auto_bright = 1;   // 关闭 libraw 的自动亮度，保持中性
    raw.imgdata.params.use_camera_wb = 0;    // 白平衡由本软件管线负责，不用相机预设
    raw.imgdata.params.use_auto_wb = 0;
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.output_color = 1;     // libraw 输出 sRGB 空间基准
    raw.imgdata.params.gamm[0] = 1.0;        // 线性输出，不做 gamma 编码
    raw.imgdata.params.gamm[1] = 1.0;
    raw.imgdata.params.no_auto_scale = 1;    // 不做自动缩放，保留原始数值范围

    // 去拜耳算法映射到 libraw 的枚举。
    // 注意：本软件在管线中自行实现 Demosaic（需求 §2 要求 Demosaic 位于
    // 拜耳域 CCM 之后），因此 libraw 端固定用最简单的双线性，
    // 真正的算法选择由管线的 Demosaic 环节执行。
    raw.imgdata.params.user_qual = 0;        // 0 = 双线性

    // 强制拜耳模式（自动识别优先，需求 §3 模块 A）
    raw.imgdata.params.user_sat = static_cast<int>(
        settings.whiteLevelOverride > 0.0f ? settings.whiteLevelOverride : 0);

    // 不裁剪、不旋转，保留完整传感器数据，由本软件自行处理
    raw.imgdata.params.cropbox[0] = 0;
    raw.imgdata.params.cropbox[1] = 0;
    raw.imgdata.params.cropbox[2] = 0;
    raw.imgdata.params.cropbox[3] = 0;

    // 关闭 libraw 的自动去噪与亮度调整，保持"中性、低美化"取向（需求 §1.1）
    raw.imgdata.params.threshold = 0;

    // ---- 4. 解包（读取原始拜耳数据）----
    ret = raw.unpack();
    if (ret != LIBRAW_SUCCESS) {
        if (ret == LIBRAW_OUT_OF_ORDER_CALL || ret == LIBRAW_NO_THUMBNAIL) {
            return ErrorInfo::make(ErrorCode::DecodeFailed,
                                   "RAW 数据解包失败，文件可能不完整",
                                   "libraw unpack 返回 " + std::to_string(ret));
        }
        if (ret == RAW_LIBRAW_INSUFFICIENT_MEMORY || ret == LIBRAW_TOO_BIG) {
            return ErrorInfo::make(ErrorCode::OutOfMemory,
                                   "内存不足，无法解码该 RAW 文件",
                                   "libraw unpack 内存错误，码 " + std::to_string(ret));
        }
        if (ret == LIBRAW_CANCELLED_BY_CALLBACK) {
            return ErrorInfo::make(ErrorCode::Cancelled, "解码已取消。");
        }
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "不支持或无法解码的 RAW 数据",
                               "libraw unpack 返回 " + std::to_string(ret));
    }

    if (isCancelled()) {
        return ErrorInfo::make(ErrorCode::Cancelled, "解码已取消", "用户请求取消");
    }

    // ---- 5. 收集元数据 ----
    const auto& img = raw.imgdata;
    result.metadata.fileName = filePath;
    result.metadata.cameraMake = img.idata.make;
    result.metadata.cameraModel = img.idata.model;
    result.metadata.uniqueCameraModel = img.color.UniqueCameraModel;
    result.metadata.isDng = img.idata.dng_version != 0;
    result.metadata.lensModel = img.lens.Lens;
    result.metadata.width = img.sizes.width;
    result.metadata.height = img.sizes.height;
    result.metadata.timestamp = formatCaptureTimestamp(img.other.timestamp);
    result.metadata.iso = img.other.iso_speed;
    result.metadata.shutter = img.other.shutter;
    result.metadata.aperture = img.other.aperture;
    result.metadata.focalLength = img.other.focal_len;
    copyFocusDistance(img.other, result.metadata);

    // ---- 5.1 推导相机原厂 CamRGB → XYZ(D50) 矩阵 ----
    // 优先对 cam_xyz 求逆；只有缺少 cam_xyz 时才回退到补了 pre_mul 缩放的
    // rgb_cam（详见 buildCameraToXyz 的说明）。
    const CameraMatrixSolution cameraMatrix = buildCameraToXyz(raw);
    result.hasCameraMatrix = cameraMatrix.valid;
    if (cameraMatrix.valid) {
        result.cameraToXyz = cameraMatrix.camToXyz;
    } else {
        // RawDecodeResult 的默认值就是单位矩阵，保持即可并提示用户
        result.warnings.push_back("相机未提供有效的原厂色彩矩阵，已回退为单位矩阵。");
    }

    // as-shot 白平衡反算（与 probeMetadata 使用同一套公式），
    // 必须使用同一份 cameraToXyz，保证反算与管线补偿能互相抵消。
    fillAsShotWhiteBalance(raw, cameraMatrix, result.metadata);

    // DCP ForwardMatrix 需要的相机域白点
    fillCameraNeutral(raw, result);
    if (!result.hasCameraWhiteBalanceGain) {
        result.warnings.push_back(
            "未读到可靠的 as-shot 白平衡增益，DCP ForwardMatrix 将按中性输入处理。");
    }

    // ---- 6. 解析黑电平与白电平 ----
    //
    // libraw 的黑电平由三部分组成（见 LibRaw::adjust_bl / subtract_black_internal）：
    //   color.black        —— 公共黑电平（单值）
    //   color.cblack[0..3] —— 分通道黑电平，按 CFA 码索引；
    //                         libraw 默认 cdesc 为 "RGBG"，即依次为 R、G1、B、G2
    //   color.cblack[4..5] —— DNG 的 BlackLevelRepeatDim（重复区域的行数、列数）
    //   color.cblack[6..]  —— 重复区域内逐位置的黑电平（行优先）
    //
    // 关键：unpack() 之后 libraw 还没有执行 adjust_bl()，
    // 此时 cblack[0..3] 是相对公共黑电平的**偏移**，而 cblack[6..] 是**绝对值**。
    // 因此每个像素的实际黑电平为：
    //       color.black + cblack[CFA码] + cblack[6 + 重复区域内位置]
    //
    // 这一点对 Adobe 转换的 DNG 尤其重要：这类文件把 BlackLevel 写成 2x2
    // 重复区域，libraw 只把它存进 cblack[6..]，color.black 仍为 0。
    // 若只读 color.black，就会完全不扣黑，低信号处通道比例被破坏，
    // 画面表现为发灰、暗部严重偏紫（低光区域 R、B 通道相对被抬高）。
    //
    // 未使用的 cblack 项被填为 0xffffffff（LIBRAW_NO_BLACK），按 0 处理。
    const float commonBlack = static_cast<float>(img.color.black);
    for (int i = 0; i < 4; ++i) {
        const unsigned extra = img.color.cblack[i];
        result.blackLevel[static_cast<std::size_t>(i)] =
            commonBlack + (extra == 0xffffffffu ? 0.0f : static_cast<float>(extra));
    }

    // 逐位置黑电平（重复区域）。常见相机为 2x2 且四格同值；这里保留通用实现，
    // 兼容 DNG 规范允许的更大重复区域（手机、工业传感器上可能出现）。
    std::vector<float> blackRepeat;
    int blackRepeatRows = 0;
    int blackRepeatCols = 0;
    {
        const int rows = static_cast<int>(img.color.cblack[4]);
        const int cols = static_cast<int>(img.color.cblack[5]);
        // cblack 末位被 libraw 用作"黑电平项数"记录，数据从下标 6 开始，
        // 可用项数上限为 SIZE-7（与 libraw 解析时使用的上限一致）
        const int maxItems = static_cast<int>(LIBRAW_CBLACK_SIZE) - 7;
        if (rows > 0 && cols > 0 && rows * cols <= maxItems) {
            blackRepeatRows = rows;
            blackRepeatCols = cols;
            blackRepeat.resize(static_cast<std::size_t>(rows) * cols);
            for (int i = 0; i < rows * cols; ++i) {
                blackRepeat[static_cast<std::size_t>(i)] =
                    static_cast<float>(img.color.cblack[6 + i]);
            }
        }
    }

    // 与 libraw 的 adjust_bl 保持一致：把重复区域黑电平的最小值作为公共部分
    // 并入通道黑电平，剩余差值保留在逐位置表中。
    // 这样 result.blackLevel 反映的就是每个通道的实际黑电平（界面显示也正确），
    // 提取像素时只需再叠加逐位置的相对差值。
    if (!blackRepeat.empty()) {
        const float repeatCommon =
            *std::min_element(blackRepeat.begin(), blackRepeat.end());
        for (float& value : blackRepeat) {
            value -= repeatCommon;
        }
        for (int i = 0; i < 4; ++i) {
            result.blackLevel[static_cast<std::size_t>(i)] += repeatCommon;
        }
    }

    // 扣黑后的有效公共黑电平（各通道黑电平的最小值），用于把白电平
    // 换算到扣黑后的刻度。
    float effectiveCommonBlack = result.blackLevel[0];
    for (int i = 1; i < 4; ++i) {
        effectiveCommonBlack =
            std::min(effectiveCommonBlack, result.blackLevel[static_cast<std::size_t>(i)]);
    }

    result.whiteLevel = static_cast<float>(img.color.maximum);
    if (result.whiteLevel <= 0.0f) {
        // 极少数文件未记录 maximum。按常见 14 位传感器的满量程推算，
        // 保证后续管线不会因除零产生 NaN。
        result.whiteLevel = 16383.0f;
        result.warnings.push_back("文件未记录白电平，已按 14 位满量程 16383 推算。");
        if (effectiveCommonBlack > 0.0f) {
            result.whiteLevel -= effectiveCommonBlack;
        }
    } else if (effectiveCommonBlack > 0.0f && result.whiteLevel > effectiveCommonBlack) {
        // 归一化发生在扣黑之后，饱和点必须同样换成扣黑后的刻度
        // （libraw 的 subtract_black_internal 也是这么做的：maximum -= black）。
        // 否则白电平会被高估，整幅画面偏暗、高光提前失去层次。
        result.whiteLevel -= effectiveCommonBlack;
    }

    // ---- 7. 解析拜耳排列 ----
    // 线性 DNG（LinearRaw）里像素已经是 RGB，没有 CFA 数据。
    // 本管线从拜耳域起步（黑电平、串扰、拜耳域 CCM 都以 CFA 为前提），
    // 无法把已经去拜耳的 RGB 当作拜耳数据处理——那样只会得到严重错误的图像。
    // 因此在这里明确拒绝并给出可执行的说明，而不是退化成"按 RGGB 猜测"。
    const bool hasBayerPixels = img.rawdata.raw_image != nullptr;
    const bool hasRgbPixels = img.rawdata.color3_image != nullptr
                              || img.rawdata.color4_image != nullptr;
    if (!hasBayerPixels && hasRgbPixels) {
        // X3F 是 Sigma 的 Foveon 三层传感器，天然没有 CFA；
        // 与线性 DNG 一样需要独立的 RGB 域处理管线，当前版本无法编辑。
        const bool isFoveon = toLowerExtension(filePath) == "x3f";
        return ErrorInfo::make(
            ErrorCode::UnsupportedFormat,
            isFoveon ? "适马 X3F（Foveon 传感器）当前版本不支持编辑"
                     : "检测到线性 DNG（像素已完成去拜耳），当前版本不支持",
            "libraw 提供的是 RGB 像素（color3/color4_image）而非 CFA 数据。"
            "本管线需要拜耳原始数据才能执行黑电平、串扰与拜耳域 CCM。"
            "X3F 请先在 SPP 中导出 TIFF / DNG；线性 DNG 建议用 DNG Converter "
            "转出原始（非 Linear）DNG，或直接使用相机原生 RAW。");
    }

    // 优先从 filters 位域解析（真实排列），无法解析时再退回 cdesc
    result.detectedPattern = patternFromFilters(img.idata.filters, img.idata.cdesc);
    // 少数 libraw 版本对 X-Trans 不把 filters 设为 9，而是保留紧凑编码；
    // xtrans 数组非空是更直接的证据。
    if (result.detectedPattern == BayerPattern::Unknown
        && img.idata.filters < 1000 && img.idata.xtrans[0][0] > 0) {
        result.detectedPattern = BayerPattern::XTrans;
    }
    if (result.detectedPattern == BayerPattern::Unknown) {
        result.detectedPattern = patternFromCdesc(img.idata.cdesc);
    }

    // 用户在进阶模式下可强制指定排列，自动识别优先（需求 §3 模块 A）
    BayerPattern effectivePattern = result.detectedPattern;
    if (settings.forceBayerPattern != BayerPattern::Unknown) {
        effectivePattern = settings.forceBayerPattern;
        if (result.detectedPattern != BayerPattern::Unknown
            && result.detectedPattern != settings.forceBayerPattern) {
            result.warnings.push_back(
                std::string("用户强制拜耳模式 ") + bayerPatternName(settings.forceBayerPattern)
                + "，覆盖自动识别结果 " + bayerPatternName(result.detectedPattern));
        }
    }
    if (effectivePattern == BayerPattern::Unknown) {
        // 无法识别时按最常见的 RGGB 处理并警告，避免直接失败。
        // 同时标记"猜测"状态：拜耳域的颜色相关校正会因此被管线禁用
        // （需求 append_v3 §1.1），否则错误通道假设会被串扰/CCM 放大。
        effectivePattern = BayerPattern::RGGB;
        result.cfaPatternGuessed = true;

        // 自定义 CFA（非 2x2）的 DNG 在 libraw 里以紧凑编码表示，
        // 本管线不解析这种编码，只能靠用户手动指定拜耳模式。
        const bool customCfaDng = img.idata.dng_version > 0
                                  && img.idata.filters < 1000
                                  && img.idata.filters != 0;
        result.warnings.push_back(
            customCfaDng
                ? "该 DNG 使用了非 2x2 的自定义 CFA 排列，本版本无法自动解析，"
                  "已按 RGGB 猜测渲染并禁用拜耳域颜色校正。"
                  "如色彩异常，请在“传感器”页手动指定拜耳模式。"
                : "无法识别拜耳排列，已按 RGGB 猜测渲染；本次已禁用串扰补偿、"
                  "拜耳域 CCM、FBDD 与高光饱和掩码。如色彩异常请手动指定拜耳模式。");
    }
    if (effectivePattern == BayerPattern::XTrans) {
        result.warnings.push_back(
            "检测到 X-Trans 传感器：使用通用 CFA 去马赛克（非 2x2 合并），"
            "拜耳域预览合并不可用，预览首帧会比 Bayer 机型略慢。");
    }

    // ---- 8. 提取拜耳数据为浮点数组 ----
    //
    // 坐标系说明（这是 libraw 里最容易出错的地方）：
    //   raw_width / raw_height  : 传感器原始尺寸，raw_image 的索引范围
    //   width / height          : 可见区域尺寸（去掉光学黑边与伪影边）
    //   left_margin / top_margin: 可见区域相对原始数据的偏移
    // raw_image[y][x] 使用**原始坐标**，因此裁剪起点必须加上 margin。
    //
    // 另一点：拜耳排列（idata.filters）是相对**可见区原点**定义的，
    // 所以判断某点属于哪个滤镜通道时，要用"原始坐标减掉边距"后的可见区坐标，
    // 既不能用原始绝对坐标，也不能用用户裁剪后的局部坐标。
    const int rawWidth = img.sizes.raw_width;
    const int rawHeight = img.sizes.raw_height;
    const int visibleWidth = img.sizes.width;
    const int visibleHeight = img.sizes.height;
    const int topMargin = img.sizes.top_margin;
    const int leftMargin = img.sizes.left_margin;

    // 计算裁剪后的有效区域（应用用户的 raw_crop 设置，需求 §3 模块 A）
    //
    // 注意：拜耳排列以 2x2 为周期，且 filters 是相对**可见区原点**定义的
    // （见 libraw 的 raw2image/FC 用法）。因此对齐到偶数必须作用在
    // "相对可见区的偏移"上，而不能直接对绝对坐标取整：
    // 可见区边距为奇数时，绝对坐标取整会让相位翻转，Demosaic 后出现彩色摩尔纹。
    // X-Trans 的周期是 6，必须按 6 对齐；2x2 拜耳按 2 对齐。
    const int cfaAlign = (effectivePattern == BayerPattern::XTrans) ? kMaxCfaPeriod : 2;
    const int cropRelX = settings.cropLeft / cfaAlign * cfaAlign;
    const int cropRelY = settings.cropTop / cfaAlign * cfaAlign;
    int cropX = leftMargin + cropRelX;
    int cropY = topMargin + cropRelY;
    int cropW = visibleWidth - cropRelX - settings.cropRight;
    int cropH = visibleHeight - cropRelY - settings.cropBottom;

    // 防御性边界检查，避免越界读取
    cropX = std::clamp(cropX, 0, std::max(0, rawWidth - 2));
    cropY = std::clamp(cropY, 0, std::max(0, rawHeight - 2));
    cropW = std::clamp(cropW, 1, rawWidth - cropX);
    cropH = std::clamp(cropH, 1, rawHeight - cropY);

    // ---- 8.1 应用 EXIF 方向（libraw 的 sizes.flip）----
    //
    // libraw 的 flip 是 dcraw 的位域约定：
    //   bit0 (1): 水平翻转（在源坐标中左右翻）
    //   bit1 (2): 垂直翻转（在源坐标中上下翻）
    //   bit2 (4): 转置（交换行/列），在翻转之后执行
    // 由此得到的取值与 LibRaw 文档一致：3 = 180°，5 = 逆时针 90°，
    // 6 = 顺时针 90°（已用竖构图 NEF 样张与 libraw 参考渲染比对确认）。
    // 注意顺序：翻转使用**源**尺寸，最后再做转置；先转置再翻转会得到
    // 相差 180° 的结果（竖构图照片会上下颠倒）。
    // 不处理 flip 会让竖构图照片横躺显示，预览/导出方向与拍摄时不一致。
    const int flip = img.sizes.flip & 7;
    const bool transpose = (flip & 4) != 0;
    const bool flipVertical = (flip & 2) != 0;
    const bool flipHorizontal = (flip & 1) != 0;

    const int outWidth = transpose ? cropH : cropW;
    const int outHeight = transpose ? cropW : cropH;

    // 输出坐标（相对输出图左上角）→ 源坐标（相对裁剪区左上角）
    auto outputToSource = [&](int ox, int oy, int& sx, int& sy) {
        // 先撤销转置：转置后输出的 (x,y) 对应翻转后的源 (y,x)
        const int flippedX = transpose ? oy : ox;
        const int flippedY = transpose ? ox : oy;
        // 再撤销源坐标系中的水平/垂直翻转（使用源尺寸）
        sx = flipHorizontal ? (cropW - 1 - flippedX) : flippedX;
        sy = flipVertical ? (cropH - 1 - flippedY) : flippedY;
    };

    result.bayer.width = outWidth;
    result.bayer.height = outHeight;

    // 旋转/翻转会改变 2x2 单元的相位（奇数尺寸时尤其明显），
    // 因此输出拜耳排列必须由实际映射关系重新推导，不能沿用源排列。
    // 做法是取输出 2x2 的首单元，逐点反查源像素所属滤镜通道。
    BayerPattern outputPattern = effectivePattern;
    if (effectivePattern != BayerPattern::XTrans
        && flip != 0 && outWidth >= 2 && outHeight >= 2) {
        static const char kChannelChars[3] = {'R', 'G', 'B'};
        char patternChars[5] = {0, 0, 0, 0, 0};
        int index = 0;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                int sx = 0;
                int sy = 0;
                outputToSource(dx, dy, sx, sy);
                // 排列以可见区原点为基准（与 libraw 的 FC 约定一致），需减掉边距
                const int channel = bayerChannelAt(effectivePattern,
                                                   cropX + sx - leftMargin,
                                                   cropY + sy - topMargin);
                patternChars[index++] = kChannelChars[channel];
            }
        }
        const BayerPattern derived = bayerPatternFromString(patternChars);
        if (derived != BayerPattern::Unknown) {
            outputPattern = derived;
        }
    }
    result.bayer.pattern = outputPattern;
    result.bayer.data.resize(static_cast<std::size_t>(outWidth) * outHeight);

    // X-Trans 没有 2x2 相位，改为在**输出坐标**上建立 6x6 通道表。
    // 坐标映射与像素提取完全一致（含转置与翻转），因此旋转后的竖构图
    // 也能得到正确的 CFA 相位；直接沿用源排列会在翻转后错位。
    if (effectivePattern == BayerPattern::XTrans) {
        if (outWidth >= kMaxCfaPeriod && outHeight >= kMaxCfaPeriod) {
            result.bayer.cfaPeriod = kMaxCfaPeriod;
            for (int cy = 0; cy < kMaxCfaPeriod; ++cy) {
                for (int cx = 0; cx < kMaxCfaPeriod; ++cx) {
                    int sx = 0;
                    int sy = 0;
                    outputToSource(cx, cy, sx, sy);
                    // COLOR 使用原始绝对坐标，libraw 内部已处理可见区边距
                    const int code = raw.COLOR(cropY + sy, cropX + sx);
                    const std::uint8_t channel =
                        (code == 0) ? 0u : ((code == 2) ? 2u : 1u);
                    result.bayer.cfaMap[static_cast<std::size_t>(cy) * kMaxCfaPeriod + cx]
                        = channel;
                }
            }
        } else {
            result.bayer.pattern = BayerPattern::RGGB;
            result.warnings.push_back(
                "X-Trans 图像尺寸过小（不足 6 像素），无法建立 CFA 相位，已按 RGGB 处理。");
        }
    }

    // 用户可叠加的黑电平偏移（需求 §3 模块 A 的 black_level）
    const float blackOffset = settings.blackLevelOffset;

    // libraw 的 raw_image 是**一维扁平缓冲**（长度 raw_width × raw_height），
    // 按行优先排布，而不是行指针数组。取像素须自行计算偏移。
    const ushort* rawImage = img.rawdata.raw_image;
    if (rawImage == nullptr) {
        return ErrorInfo::make(ErrorCode::DecodeFailed,
                               "RAW 像素数据不可用",
                               "raw_image 为空，可能已调用 dcraw_process");
    }

    try {
        for (int y = 0; y < outHeight; ++y) {
            float* dstRow = result.bayer.data.data() + static_cast<std::size_t>(y) * outWidth;
            // 与 outputToSource 保持同一套映射：
            // 先由输出坐标撤销转置，再按源尺寸撤销水平/垂直翻转。
            for (int x = 0; x < outWidth; ++x) {
                int sxRel = transpose ? y : x;
                int syRel = transpose ? x : y;
                if (flipHorizontal) {
                    sxRel = cropW - 1 - sxRel;
                }
                if (flipVertical) {
                    syRel = cropH - 1 - syRel;
                }

                const int srcX = cropX + sxRel;
                const int srcY = cropY + syRel;
                const float rawValue = static_cast<float>(
                    rawImage[static_cast<std::size_t>(srcY) * rawWidth + srcX]);

                // 用**可见区相对坐标**判断滤镜通道与黑电平位置：
                // libraw 的 filters 和 BlackLevelRepeatDim 都以可见区原点为基准。
                const int relX = srcX - leftMargin;
                const int relY = srcY - topMargin;

                // CFA 码 0..3 在默认 cdesc "RGBG" 下依次为 R、G1、B、G2，
                // 与 blackLevel 的排列一致；按码索引才能区分数值不同的 G1/G2。
                int code;
                if (img.idata.filters > 1000) {
                    const int shift = (((relY << 1) & 14) | (relX & 1)) << 1;
                    code = static_cast<int>((img.idata.filters >> shift) & 3u);
                } else if (effectivePattern == BayerPattern::XTrans) {
                    // X-Trans：COLOR() 返回原始通道码（0=R、1=G、2=B、3=G2），
                    // 与 blackLevel 的按码索引一致。
                    code = raw.COLOR(srcY, srcX);
                    code = std::clamp(code, 0, 3);
                } else {
                    // 非标准拜耳没有可用的 filters 位域，
                    // 退回按排列判断通道，G1/G2 统一使用前一个绿通道的黑电平。
                    code = bayerChannelAt(effectivePattern, relX, relY);
                }

                float black = result.blackLevel[static_cast<std::size_t>(code)];
                if (blackRepeatRows > 0 && blackRepeatCols > 0) {
                    const int pr = relY % blackRepeatRows;
                    const int pc = relX % blackRepeatCols;
                    black += blackRepeat[static_cast<std::size_t>(pr) * blackRepeatCols + pc];
                }

                // 扣除黑电平并叠加用户偏移（需求 §2 管线第二步）
                const float channelOffset =
                    settings.channelBlackOffset[static_cast<std::size_t>(
                        std::clamp(code, 0, 3))];
                float value = rawValue - black - blackOffset - channelOffset;

                // 注意：这里**不做**白电平裁剪与归一化。
                // 白电平饱和裁剪是管线的独立步骤（需求 §2 第三步），
                // 归一化到 0~1 也放在管线里统一处理，保持 RawLoader 职责单一。
                dstRow[x] = value;
            }
        }
    } catch (const std::bad_alloc&) {
        result.bayer.data.clear();
        result.bayer.data.shrink_to_fit();
        return ErrorInfo::make(ErrorCode::OutOfMemory,
                               "内存不足，无法为该 RAW 分配像素缓冲",
                               "提取拜耳数据时 std::bad_alloc");
    }

    if (isCancelled()) {
        result = RawDecodeResult{};
        return ErrorInfo::make(ErrorCode::Cancelled, "解码已取消", "用户请求取消");
    }

    // ---- 10. 收尾 ----
    // 及时释放 libraw 内部的大块缓冲，避免内存持续上涨（需求 §5.3）
    raw.recycle();

    return ErrorInfo::ok();
}

}  // namespace rawcore
