// ============================================================================
//  Types.h —— 管线核心基础类型定义
//
//  设计约束（对应需求 §1.3）：
//   1. 本目录（core/）完全不依赖 Qt，仅使用 C++ 标准库 + 三个 C 库 + OpenMP。
//      目的是让渲染管线核心可独立复用（未来移动端），也便于单元测试。
//   2. 所有色彩与光影运算一律在 32bit 浮点线性光空间进行。
//   3. 像素数据统一用 float32 存储，仅在最终导出阶段降精度。
// ============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rawcore {

// ---------------------------------------------------------------------------
//  维度常量
// ---------------------------------------------------------------------------

/// 3x3 矩阵（行主序），用于 CCM / 色彩空间变换
using Matrix3x3 = std::array<std::array<float, 3>, 3>;

/// 单位矩阵
inline Matrix3x3 identityMatrix3x3()
{
    return Matrix3x3{{
        {{1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}},
    }};
}

// ---------------------------------------------------------------------------
//  拜耳阵列相关
// ---------------------------------------------------------------------------

/// 拜耳滤镜排列方式，用 2x2 单元内 (0,0)(0,1)(1,0)(1,1) 四点的颜色通道描述。
/// 命名中 4 个字母依次代表 左上、右上、左下、右下。
enum class BayerPattern : std::uint8_t {
    Unknown = 0,
    RGGB,   ///< 最常见（多数相机、Sony/Nikon/Canon 部分机型）
    BGGR,   ///< RGGB 的转置
    GRBG,   ///< 水平镜像
    GBRG,   ///< 垂直镜像
    XTrans, ///< 富士 X-Trans：6x6 周期，排列由 cfaMap 描述
};

/// 非 2x2 CFA 的最大周期（X-Trans 为 6）。
inline constexpr int kMaxCfaPeriod = 6;

/// 返回拜耳排列的字符串名，用于日志与 UI 显示
const char* bayerPatternName(BayerPattern pattern);

/// 从 2x2 排列字符串（如 "RGGB"）解析拜耳模式
BayerPattern bayerPatternFromString(const std::string& text);

/// 判断拜耳模式下某个滤波位置是 R / G / B 中的哪一个通道（0=R,1=G,2=B）
int bayerChannelAt(BayerPattern pattern, int x, int y);

// ---------------------------------------------------------------------------
//  浮点图像缓冲
// ---------------------------------------------------------------------------

/// 像素数据排布方式
enum class PixelLayout : std::uint8_t {
    Interleaved,  ///< RGBRGBRGB... 交错存储，管线主体使用此布局
    Planar,       ///< RRR...GGG...BBB... 分平面存储
};

/// 32 位浮点 RGB 图像缓冲。
/// 约定：**裁切后的有效数据总是从 data[0] 开始且紧致排布**，
/// 不保留任何边界填充（Demosaic 所需的边缘处理在算法内部完成）。
class ImageBuffer {
public:
    ImageBuffer() = default;
    ImageBuffer(int width, int height);

    /// 重新分配缓冲。若尺寸未变则保留原有内容（避免重复分配）
    void allocate(int width, int height);

    /// 释放内存（大图渲染结束后应及时调用，避免内存持续上涨）
    void release();

    bool isEmpty() const { return m_width <= 0 || m_height <= 0 || m_data.empty(); }

    int width() const { return m_width; }
    int height() const { return m_height; }
    std::size_t pixelCount() const
    {
        return static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);
    }
    std::size_t sampleCount() const { return pixelCount() * 3u; }

    /// 交错布局下第 index 个采样的访问（index 已含通道）
    float& sample(std::size_t index) { return m_data[index]; }
    float sample(std::size_t index) const { return m_data[index]; }

    /// 按像素坐标访问通道 c（0=R,1=G,2=B），交错布局
    float& at(int x, int y, int c) { return m_data[(static_cast<std::size_t>(y) * m_width + x) * 3u + c]; }
    float at(int x, int y, int c) const
    {
        return m_data[(static_cast<std::size_t>(y) * m_width + x) * 3u + c];
    }

    /// 直接暴露底层数据，供 OpenMP 循环与第三方库（lcms2）直读
    float* data() { return m_data.data(); }
    const float* data() const { return m_data.data(); }

    /// 用标量填充全部采样
    void fill(float value);

    /// 交换内容（用于免拷贝地移交大数据块）
    void swap(ImageBuffer& other) noexcept;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<float> m_data;  ///< 交错 RGB，长度 = width * height * 3
};

// ---------------------------------------------------------------------------
//  拜耳图像（单通道，Demosaic 之前的形态）
// ---------------------------------------------------------------------------

/// 拜耳域浮点数据。管线最初几步（黑电平扣除、白电平裁剪、拜耳域 CCM）
/// 都在这个结构上进行；完成 Demosaic 后才转为 ImageBuffer。
struct BayerImage {
    int width = 0;
    int height = 0;
    BayerPattern pattern = BayerPattern::Unknown;
    std::vector<float> data;  ///< 长度 = width * height，每点存储其对应滤镜通道的值

    /// 通用 CFA 描述（目前用于 X-Trans）。
    /// cfaPeriod == 0 时由 pattern 描述 2x2 拜耳阵列；大于 0 时按 cfaMap
    /// 取模查表。这样所有拜耳域算子都能用同一个 channelAt() 入口，
    /// 不必为每种传感器写一套索引逻辑。
    int cfaPeriod = 0;
    std::array<std::uint8_t, kMaxCfaPeriod * kMaxCfaPeriod> cfaMap{};

    bool isEmpty() const { return width <= 0 || height <= 0 || data.empty(); }
    std::size_t sampleCount() const
    {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    /// 该点属于 R / G / B 中的哪个通道。越界坐标由调用方负责夹取；
    /// 这里只做取模，保证负坐标也能得到合法值。
    int channelAt(int x, int y) const
    {
        if (cfaPeriod > 0) {
            const int px = ((x % cfaPeriod) + cfaPeriod) % cfaPeriod;
            const int py = ((y % cfaPeriod) + cfaPeriod) % cfaPeriod;
            const int value = cfaMap[static_cast<std::size_t>(py) * cfaPeriod + px];
            return (value <= 2) ? value : 1;
        }
        return bayerChannelAt(pattern, x, y);
    }

    /// 是否为非 2x2 CFA（X-Trans 等），需要走通用处理路径。
    bool usesGenericCfa() const { return cfaPeriod > 0; }

    float& at(int x, int y) { return data[static_cast<std::size_t>(y) * width + x]; }
    float at(int x, int y) const { return data[static_cast<std::size_t>(y) * width + x]; }
};

// ---------------------------------------------------------------------------
//  色彩空间标识
// ---------------------------------------------------------------------------

/// 支持的色彩空间。用作 lcms2 profile 查找的键，以及导出目标选择。
enum class ColorSpace : std::uint8_t {
    ProPhotoRGB,  ///< 内部固定工作空间（需求 §1.3.2）
    sRGB,         ///< 导出目标
    AdobeRGB,     ///< 导出目标
    DisplayP3,    ///< Display P3（E5，宽色域显示 / 导出）
    LinearProPhotoRGB,  ///< 线性版 ProPhoto（管线中间态，不做 gamma 编码）
    XYZ,          ///< CIE XYZ（D50 白点，lcms2 内部基准）
};

const char* colorSpaceName(ColorSpace space);

// ---------------------------------------------------------------------------
//  Demosaic 算法
// ---------------------------------------------------------------------------

/// 去拜耳算法枚举，对应需求 §3 模块 A 的 demosaic_algorithm 配置项
enum class DemosaicAlgorithm : std::uint8_t {
    Bilinear = 0,  ///< 双线性插值：最快，质量最低
    VNG4,          ///< 变梯度四点法：速度与质量折中
    AMAZE,         ///< 自适应边缘感知：质量高，速度慢
    LMMSE,         ///< 线性最小均方误差：低噪场景表现好
    RCD,           ///< 比值 / 色差校正的方向自适应（D4）
    Markesteijn,   ///< X-Trans 三遍方向精修（D4，仅对 X-Trans 生效）
};

const char* demosaicAlgorithmName(DemosaicAlgorithm algo);
DemosaicAlgorithm demosaicAlgorithmFromString(const std::string& text);

// ---------------------------------------------------------------------------
//  渲染尺寸规格
// ---------------------------------------------------------------------------

/// 渲染用途。决定输出分辨率与质量策略（需求 §3 模块 E）
enum class RenderScale : std::uint8_t {
    Preview,  ///< 交互预览：拜耳域下采样后跑管线，保证拖动流畅
    FullSize, ///< 全尺寸：导出与最终确认时使用
};

/// 渲染过程中的阶段编号，用于进度回调（0.0~1.0 由阶段索引换算）
enum class RenderStage : std::uint8_t {
    Idle = 0,
    LoadingRaw,
    BlackWhiteLevel,
    Crosstalk,        ///< CMOS 串扰补偿（去拜耳之前）
    BayerCcm,
    BadPixelRepair,
    DarkCurrent,      ///< 暗电流补偿（坏点修复之后）
    DarkFrame,        ///< 暗场帧逐像素扣除（需求 append_v3 §五 1）
    FlatFrame,        ///< 平场帧逐像素校正（需求 append_v3 §五 2）
    Demosaic,
    HighlightRolloff, ///< 高光软饱和 Roll-off
    Lca,              ///< 横向色差校正
    Loca,             ///< 轴向色散校正
    EdgeFalseColor,   ///< 边缘导向伪色抑制
    LensCorrection,
    CameraToXyz,
    XyzToWorking,
    Perspective,       ///< 透视 / 几何校正
    BaseWhiteBalance,  ///< RAW 标定层：相机中性基准
    WhiteBalance,      ///< 上层：创意白平衡偏移
    PreviewTone,   ///< 预览影调基准（自动曝光 + 可选弱 S 曲线）
    LightAdjust,
    Presence,      ///< 全局色彩与质感（饱和度 / 自然饱和度 / 清晰度 / 纹理）
    HslAdjust,
    BlackWhite,    ///< 黑白混色
    ColorGrading,  ///< 颜色分级 + 分离色调
    Curves,
    Lut,           ///< 3D LUT
    LocalAdjust,   ///< 蒙版局部调整
    Retouch,       ///< 污点修复 / 仿制
    Crop,          ///< 非破坏性裁剪
    NoiseReduction,
    Sharpen,
    GamutMap,
    GammaEncode,
    Finished,
    Failed,
};

const char* renderStageName(RenderStage stage);

/// 管线阶段总数，用于进度百分比换算
constexpr int kRenderStageCount = 36;

// ---------------------------------------------------------------------------
//  错误码
// ---------------------------------------------------------------------------

/// 统一错误码（需求 §6 错误处理规范）
enum class ErrorCode : std::uint8_t {
    Ok = 0,
    FileNotFound,
    FileCorrupted,      ///< 文件损坏
    UnsupportedFormat,  ///< 不是 libraw 支持的 RAW 格式
    DecodeFailed,       ///< libraw 内部解码异常
    OutOfMemory,        ///< 内存不足
    InvalidMatrix,      ///< CCM 矩阵非法
    LensDataMissing,    ///< lensfun 无匹配镜头数据
    ExportFailed,
    Cancelled,          ///< 用户取消
    InvalidArgument,    ///< 调用方传入的参数不合法（如色卡框选区域过小）
    Unknown,
};

/// 带错误码与人类可读信息的错误载体
struct ErrorInfo {
    ErrorCode code = ErrorCode::Ok;
    std::string message;   ///< 面向用户的中文提示
    std::string detail;    ///< 面向开发者的技术细节（写入日志面板）

    bool isOk() const { return code == ErrorCode::Ok; }
    static ErrorInfo ok() { return ErrorInfo{}; }
    static ErrorInfo make(ErrorCode c, std::string msg, std::string det = {})
    {
        return ErrorInfo{c, std::move(msg), std::move(det)};
    }
};

// ---------------------------------------------------------------------------
//  图像元数据
// ---------------------------------------------------------------------------

/// RAW 文件携带的元数据，用于 UI 展示、镜头校正匹配与默认白平衡
struct RawMetadata {
    std::string fileName;
    std::string cameraMake;     ///< 厂商，如 "SONY"
    std::string cameraModel;    ///< 机型，如 "ILCE-7M4"
    std::string lensModel;      ///< 镜头型号，用于 lensfun 匹配
    std::string timestamp;

    int width = 0;              ///< 传感器全宽（含边缘）
    int height = 0;

    float iso = 0.0f;
    float shutter = 0.0f;       ///< 快门时间（秒）
    float aperture = 0.0f;      ///< 光圈 F 值
    float focalLength = 0.0f;   ///< 焦距（mm）
    float focusDistance = 0.0f; ///< 对焦距离（米），lensfun 需要

    /// 相机厂商记录的拍摄白平衡（as-shot），仅作为初始值参考
    float asShotTemperature = 0.0f;
    float asShotTint = 0.0f;

    /// as-shot 白平衡的来源，用于日志与诊断：
    /// "camera-cct"（厂商记录的色温）、"as-shot-neutral"（DNG AsShotNeutral /
    /// 厂商增益）、"planckian"（由增益经 Planckian 轨迹反算）、
    /// "default"（兜底日光）。
    std::string whiteBalanceSource;

    /// libraw 解析到的唯一相机型号（DNG UniqueCameraModel / 厂商字段），
    /// 用于相机配置文件库的自动匹配；可能为空。
    std::string uniqueCameraModel;

    /// 文件是否为 DNG（dng_version != 0）
    bool isDng = false;

    /// 是否带相机内嵌的 JPEG 预览。大图预览可直接显示这张图，
    /// 省去软件管线渲染，显示的是相机直出效果。
    bool hasEmbeddedPreview = false;
};

}  // namespace rawcore
