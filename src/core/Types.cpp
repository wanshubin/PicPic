// ============================================================================
//  Types.cpp —— 基础类型的字符串映射与实现
// ============================================================================

#include "Types.h"

#include <algorithm>
#include <cctype>

namespace rawcore {

// ---------------------------------------------------------------------------
//  ImageBuffer
// ---------------------------------------------------------------------------

ImageBuffer::ImageBuffer(int width, int height)
{
    allocate(width, height);
}

void ImageBuffer::allocate(int width, int height)
{
    m_width = width;
    m_height = height;
    const std::size_t needed = pixelCount() * 3u;
    if (m_data.size() != needed) {
        m_data.assign(needed, 0.0f);
    }
    // 尺寸未变时保留原内容，调用方通过 fill() 显式清零即可
}

void ImageBuffer::release()
{
    m_width = 0;
    m_height = 0;
    // 用 swap 强制真正归还内存，而非仅把 size 置 0
    std::vector<float>().swap(m_data);
}

void ImageBuffer::fill(float value)
{
    std::fill(m_data.begin(), m_data.end(), value);
}

void ImageBuffer::swap(ImageBuffer& other) noexcept
{
    std::swap(m_width, other.m_width);
    std::swap(m_height, other.m_height);
    m_data.swap(other.m_data);
}

// ---------------------------------------------------------------------------
//  拜耳模式
// ---------------------------------------------------------------------------

const char* bayerPatternName(BayerPattern pattern)
{
    switch (pattern) {
    case BayerPattern::RGGB: return "RGGB";
    case BayerPattern::BGGR: return "BGGR";
    case BayerPattern::GRBG: return "GRBG";
    case BayerPattern::GBRG: return "GBRG";
    case BayerPattern::XTrans: return "X-Trans";
    case BayerPattern::Unknown: break;
    }
    return "Unknown";
}

BayerPattern bayerPatternFromString(const std::string& text)
{
    std::string upper;
    upper.reserve(text.size());
    for (char ch : text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    if (upper == "RGGB") return BayerPattern::RGGB;
    if (upper == "BGGR") return BayerPattern::BGGR;
    if (upper == "GRBG") return BayerPattern::GRBG;
    if (upper == "GBRG") return BayerPattern::GBRG;
    if (upper == "XTRANS" || upper == "X-TRANS") return BayerPattern::XTrans;
    return BayerPattern::Unknown;
}

int bayerChannelAt(BayerPattern pattern, int x, int y)
{
    // 取坐标在 2x2 单元内的相对位置
    const int px = ((x % 2) + 2) % 2;
    const int py = ((y % 2) + 2) % 2;

    // 把 4 种排列展开成查表，避免索引计算易错
    switch (pattern) {
    case BayerPattern::RGGB: {
        // [0][0]=R [1][0]=G
        // [0][1]=G [1][1]=B
        static const int map[2][2] = {{0, 1}, {1, 2}};
        return map[py][px];
    }
    case BayerPattern::BGGR: {
        static const int map[2][2] = {{2, 1}, {1, 0}};
        return map[py][px];
    }
    case BayerPattern::GRBG: {
        static const int map[2][2] = {{1, 0}, {2, 1}};
        return map[py][px];
    }
    case BayerPattern::GBRG: {
        static const int map[2][2] = {{1, 2}, {0, 1}};
        return map[py][px];
    }
    case BayerPattern::Unknown:
    case BayerPattern::XTrans:
        break;
    }
    return 1;  // 未知排列或 X-Trans（由 cfaMap 描述）时按绿色处理，保证不会越界
}

// ---------------------------------------------------------------------------
//  色彩空间
// ---------------------------------------------------------------------------

const char* colorSpaceName(ColorSpace space)
{
    switch (space) {
    case ColorSpace::ProPhotoRGB: return "ProPhoto RGB";
    case ColorSpace::sRGB: return "sRGB";
    case ColorSpace::AdobeRGB: return "Adobe RGB";
    case ColorSpace::DisplayP3: return "Display P3";
    case ColorSpace::LinearProPhotoRGB: return "Linear ProPhoto RGB";
    case ColorSpace::XYZ: return "CIE XYZ";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
//  Demosaic 算法
// ---------------------------------------------------------------------------

const char* demosaicAlgorithmName(DemosaicAlgorithm algo)
{
    switch (algo) {
    case DemosaicAlgorithm::Bilinear: return "Bilinear";
    case DemosaicAlgorithm::VNG4: return "VNG4";
    case DemosaicAlgorithm::AMAZE: return "AMaZE";
    case DemosaicAlgorithm::LMMSE: return "LMMSE";
    case DemosaicAlgorithm::RCD: return "RCD";
    case DemosaicAlgorithm::Markesteijn: return "Markesteijn";
    }
    return "Unknown";
}

DemosaicAlgorithm demosaicAlgorithmFromString(const std::string& text)
{
    std::string upper;
    upper.reserve(text.size());
    for (char ch : text) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    if (upper == "BILINEAR") return DemosaicAlgorithm::Bilinear;
    if (upper == "VNG4" || upper == "VNG") return DemosaicAlgorithm::VNG4;
    if (upper == "AMAZE") return DemosaicAlgorithm::AMAZE;
    if (upper == "LMMSE") return DemosaicAlgorithm::LMMSE;
    if (upper == "RCD") return DemosaicAlgorithm::RCD;
    if (upper == "MARKESTEIJN") return DemosaicAlgorithm::Markesteijn;
    // 无法识别时回退到质量与速度折中的 VNG4
    return DemosaicAlgorithm::VNG4;
}

// ---------------------------------------------------------------------------
//  渲染阶段
// ---------------------------------------------------------------------------

const char* renderStageName(RenderStage stage)
{
    switch (stage) {
    case RenderStage::Idle: return "空闲";
    case RenderStage::LoadingRaw: return "读取 RAW";
    case RenderStage::BlackWhiteLevel: return "黑白电平";
    case RenderStage::Crosstalk: return "串扰补偿";
    case RenderStage::BayerCcm: return "拜耳域 CCM";
    case RenderStage::BadPixelRepair: return "坏点修复";
    case RenderStage::DarkCurrent: return "暗电流补偿";
    case RenderStage::DarkFrame: return "暗场帧校正";
    case RenderStage::FlatFrame: return "平场帧校正";
    case RenderStage::Demosaic: return "去拜耳";
    case RenderStage::HighlightRolloff: return "高光 Roll-off";
    case RenderStage::Lca: return "横向色差校正";
    case RenderStage::Loca: return "轴向色散校正";
    case RenderStage::EdgeFalseColor: return "边缘伪色抑制";
    case RenderStage::LensCorrection: return "镜头校正";
    case RenderStage::CameraToXyz: return "相机 RGB → XYZ";
    case RenderStage::XyzToWorking: return "XYZ → 工作空间";
    case RenderStage::Perspective: return "透视 / 几何校正";
    case RenderStage::BaseWhiteBalance: return "基准白平衡";
    case RenderStage::WhiteBalance: return "创意白平衡";
    case RenderStage::PreviewTone: return "预览影调";
    case RenderStage::LightAdjust: return "光影调整";
    case RenderStage::Presence: return "色彩与质感";
    case RenderStage::HslAdjust: return "HSL 调色";
    case RenderStage::BlackWhite: return "黑白混色";
    case RenderStage::ColorGrading: return "颜色分级";
    case RenderStage::Curves: return "曲线";
    case RenderStage::Lut: return "3D LUT";
    case RenderStage::LocalAdjust: return "局部调整";
    case RenderStage::Retouch: return "污点修复";
    case RenderStage::Crop: return "裁剪";
    case RenderStage::NoiseReduction: return "降噪";
    case RenderStage::Sharpen: return "锐化";
    case RenderStage::GamutMap: return "色域映射";
    case RenderStage::GammaEncode: return "Gamma 编码";
    case RenderStage::Finished: return "完成";
    case RenderStage::Failed: return "失败";
    }
    return "未知";
}

}  // namespace rawcore
