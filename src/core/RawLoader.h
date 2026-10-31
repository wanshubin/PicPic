// ============================================================================
//  RawLoader.h —— RAW 解码封装层（需求 §3 模块 A）
//
//  职责：封装 libraw，向上层隔离第三方库细节。
//  输出：拜耳浮点数组、元数据、原厂 cam2xyz 矩阵、黑白电平、拜耳模式。
//
//  设计要点：
//   - 此处**只负责解码**，不做任何色彩变换或图像处理。
//   - 输出的是拜耳域单通道浮点数据，Demosaic 由管线负责（需求 §2 顺序）。
//     这样拜耳域 CCM 才有作用的位置，符合需求 §1.3.3 的约束。
//   - 错误按需求 §6 分类：文件损坏 / 不支持格式 / libraw 解码异常。
// ============================================================================

#pragma once

#include "EditContext.h"
#include "Types.h"

#include <memory>
#include <string>
#include <vector>

namespace rawcore {

/// RAW 解码结果：一次性产出管线起步所需的全部数据
struct RawDecodeResult {
    /// 拜耳域浮点数据（黑电平已扣除到 0 基准，但**尚未**做白电平裁剪，
    /// 白电平裁剪属管线步骤，见需求 §2）
    BayerImage bayer;

    /// 文件元数据
    RawMetadata metadata;

    /// 相机原厂 CamRGB → XYZ 矩阵（D50 白点基准）。
    /// 优先对 libraw 的 cam_xyz（原厂 XYZ→CamRGB）求逆得到；缺失时回退到
    /// rgb_cam 并补上 pre_mul 通道缩放。若相机未提供有效矩阵，则为单位矩阵，
    /// 上层可据 hasCameraMatrix 判断是否需要警告。
    Matrix3x3 cameraToXyz = identityMatrix3x3();
    bool hasCameraMatrix = false;

    /// 相机记录的黑电平（每通道，顺序 R/G/B/G2）
    std::array<float, 4> blackLevel{{0.0f, 0.0f, 0.0f, 0.0f}};

    /// 相机记录的饱和白电平（归一化后的原始刻度值）
    float whiteLevel = 0.0f;

    /// 相机域 as-shot 白平衡**增益**（已按绿通道归一化）。
    ///
    /// DCP ForwardMatrix 按 DNG 规范要求输入已经过 as-shot 白平衡的相机 RGB，
    /// 因此管线在启用 DCP 正向矩阵时先把这组增益乘到相机通道上。
    /// 值为 {R/G, 1, B/G}（增益，不是白点）；没有可靠增益时保持 {1,1,1}。
    std::array<float, 3> cameraWhiteBalanceGain{{1.0f, 1.0f, 1.0f}};
    bool hasCameraWhiteBalanceGain = false;

    /// 自动识别到的拜耳排列（未被用户覆盖时使用）
    BayerPattern detectedPattern = BayerPattern::Unknown;

    /// 自动识别失败、按 RGGB 猜测渲染。为 true 时拜耳域的颜色相关校正
    /// （串扰补偿、拜耳域 CCM、FBDD、饱和掩码）都建立在错误的通道假设上，
    /// 管线会禁用它们，只保留与通道排列无关的步骤。
    bool cfaPatternGuessed = false;

    /// 解码期间 libraw 产生的警告信息，转发到 UI 日志面板
    std::vector<std::string> warnings;

    /// 普通位图（JPG / PNG / TIFF …）解码后的**线性 sRGB（D65）**像素。
    ///
    /// 非空时表示本次输入不是拜耳数据：相机已经把白平衡、去马赛克、镜头
    /// 校正烘焙进像素，管线会跳过黑电平、白电平、串扰、拜耳域 CCM、
    /// 去马赛克、镜头校正等 RAW 专属阶段，直接转到工作色彩空间，之后
    /// 的光影 / HSL / 曲线 / 降噪 / 锐化 / 导出与 RAW 完全共用。
    ImageBuffer rgbLinear;
    bool isRgbSource = false;

    /// 源图像尺寸：RAW 为拜耳尺寸，普通位图为 RGB 尺寸
    int sourceWidth() const { return isRgbSource ? rgbLinear.width() : bayer.width; }
    int sourceHeight() const { return isRgbSource ? rgbLinear.height() : bayer.height; }

    bool isValid() const
    {
        return isRgbSource ? !rgbLinear.isEmpty() : !bayer.isEmpty();
    }
};

/// RAW 解码器。每次加载创建一个实例，不持有跨文件的长期状态。
class RawLoader {
public:
    RawLoader();
    ~RawLoader();

    // 禁止拷贝（内部持有 libraw 句柄），允许移动
    RawLoader(const RawLoader&) = delete;
    RawLoader& operator=(const RawLoader&) = delete;

    /// 打开并解码 RAW 文件。
    /// @param filePath  RAW 文件路径
    /// @param settings  解码参数（黑电平偏移、强制拜耳模式、裁剪等）
    /// @param result    输出解码结果
    /// @return 错误信息；成功时 code == ErrorCode::Ok
    ErrorInfo load(const std::string& filePath,
                   const RawSettings& settings,
                   RawDecodeResult& result);

    /// 仅读取元数据，不解码像素。用于文件列表快速预览与镜头匹配。
    ErrorInfo probeMetadata(const std::string& filePath, RawMetadata& metadata);

    /// 读取 RAW 内嵌的预览图（相机生成的 JPEG）。
    ///
    /// 选图工作台需要在短时间内列出整个目录的缩略图：走完整解码 + 渲染
    /// 每张要数百毫秒，目录稍大就无法接受。内嵌预览由相机直接生成，
    /// 读取只需几毫秒。返回数据通常是 JPEG，可直接交给 QImage 解码；
    /// 没有内嵌预览或格式不是 JPEG 时返回失败，调用方回退到预览管线。
    ///
    /// 只读文件，不修改任何跨文件状态，可在工作线程中并发调用。
    /// @param jpegData 输出：内嵌 JPEG 的字节流
    ErrorInfo loadEmbeddedPreview(const std::string& filePath,
                                  std::vector<std::uint8_t>& jpegData);

    /// 取消当前解码（由工作线程从 UI 线程触发）。
    /// 会尽快中断解码过程并返回 ErrorCode::Cancelled。
    void requestCancel();
    bool isCancelled() const;

    /// 判断给定扩展名是否为支持的 RAW 格式（需求 §1.4）
    static bool isSupportedExtension(const std::string& filePath);

    /// 返回支持的扩展名列表，用于文件对话框过滤器
    static std::vector<std::string> supportedExtensions();

private:
    struct Impl;                       ///< Pimpl：把 libraw 头文件隔离在本 cpp 内
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawcore
