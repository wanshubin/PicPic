// ============================================================================
//  StandardImageLoader.cpp —— 普通位图解码实现
// ============================================================================

#include "StandardImageLoader.h"

#include "ImageMetadata.h"

#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <array>
#include <cmath>

namespace rawio {

namespace {

/// 支持的扩展名。与 CullWindow / 主窗口的目录扫描共用同一份定义，
/// 新增格式时只需要改这里。
const QStringList& imageSuffixes()
{
    static const QStringList kSuffixes = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("webp"),
        QStringLiteral("bmp"),
    };
    return kSuffixes;
}

/// sRGB 8 位 → 线性光。查表避免每像素两次 pow。
const std::array<float, 256>& srgbToLinearTable()
{
    static const std::array<float, 256> kTable = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) {
            const float value = static_cast<float>(i) / 255.0f;
            table[static_cast<std::size_t>(i)] =
                (value <= 0.04045f)
                    ? value / 12.92f
                    : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        return table;
    }();
    return kTable;
}

/// sRGB 16 位 → 线性光。16 位 TIFF / PNG 走这张表，避免先压成 8 位
/// 再线性化（那会把 16 位源白白丢掉 8 位精度）。
const std::array<float, 65536>& srgbToLinearTable16()
{
    static const std::array<float, 65536> kTable = [] {
        std::array<float, 65536> table{};
        for (int i = 0; i < 65536; ++i) {
            const float value = static_cast<float>(i) / 65535.0f;
            table[static_cast<std::size_t>(i)] =
                (value <= 0.04045f)
                    ? value / 12.92f
                    : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        return table;
    }();
    return kTable;
}

}  // namespace

bool isStandardImagePath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return !suffix.isEmpty() && imageSuffixes().contains(suffix);
}

QStringList standardImageSuffixes()
{
    return imageSuffixes();
}

rawcore::ErrorInfo loadStandardImage(const QString& path,
                                     rawcore::RawDecodeResult& result)
{
    result = rawcore::RawDecodeResult{};

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return rawcore::ErrorInfo::make(rawcore::ErrorCode::FileNotFound,
                                        "文件不存在：" + path.toStdString());
    }
    if (!isStandardImagePath(path)) {
        return rawcore::ErrorInfo::make(rawcore::ErrorCode::UnsupportedFormat,
                                        "不支持的图片格式：" + fileInfo.suffix().toStdString());
    }

    // 解码交由 Qt 完成：JPG/PNG/TIFF/WebP/BMP 由图像插件处理。
    // setAutoTransform 让竖拍照片按 EXIF 方向转正，与 RAW 解码后的方向一致。
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        return rawcore::ErrorInfo::make(
            rawcore::ErrorCode::FileCorrupted,
            "无法解码图片：" + reader.errorString().toStdString(),
            path.toStdString());
    }

    const int width = image.width();
    const int height = image.height();
    result.rgbLinear.allocate(width, height);
    float* dst = result.rgbLinear.data();

    // 16 位源（TIFF / PNG）保留原始位深：压成 8 位再线性化会损失精度，
    // 尤其是暗部——线性域里 8 位量化误差会放大成明显的断层。
    // 判据看实际位深：RGB888 = 24，ARGB32 = 32，RGBX64 = 64，Grayscale16 = 16。
    const bool source16Bit = image.depth() > 32
        || image.format() == QImage::Format_Grayscale16;

    if (source16Bit) {
        // 统一成 RGBA64：RGB 三通道 + 一个未使用的 alpha 槽
        if (image.format() != QImage::Format_RGBA64) {
            image = image.convertToFormat(QImage::Format_RGBA64);
        }
        if (image.isNull()) {
            return rawcore::ErrorInfo::make(rawcore::ErrorCode::DecodeFailed,
                                            "图片像素格式转换失败。");
        }

        const auto& table = srgbToLinearTable16();
        for (int y = 0; y < height; ++y) {
            const quint16* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
            float* outRow = dst + static_cast<std::size_t>(y) * width * 3u;
            for (int x = 0; x < width; ++x) {
                outRow[x * 3 + 0] = table[static_cast<std::size_t>(row[x * 4 + 0])];
                outRow[x * 3 + 1] = table[static_cast<std::size_t>(row[x * 4 + 1])];
                outRow[x * 3 + 2] = table[static_cast<std::size_t>(row[x * 4 + 2])];
            }
        }
    } else {
        // 8 位源。带透明通道的 PNG 直接丢弃 alpha（编辑与导出都是 RGB）。
        if (image.format() != QImage::Format_RGB888) {
            image = image.convertToFormat(QImage::Format_RGB888);
        }
        if (image.isNull()) {
            return rawcore::ErrorInfo::make(rawcore::ErrorCode::DecodeFailed,
                                            "图片像素格式转换失败。");
        }

        const auto& table = srgbToLinearTable();
        for (int y = 0; y < height; ++y) {
            const uchar* row = image.constScanLine(y);
            float* outRow = dst + static_cast<std::size_t>(y) * width * 3u;
            for (int x = 0; x < width; ++x) {
                outRow[x * 3 + 0] = table[static_cast<std::size_t>(row[x * 3 + 0])];
                outRow[x * 3 + 1] = table[static_cast<std::size_t>(row[x * 3 + 1])];
                outRow[x * 3 + 2] = table[static_cast<std::size_t>(row[x * 3 + 2])];
            }
        }
    }

    // ---- 管线需要的基础字段 ----
    result.isRgbSource = true;
    // 普通位图没有拜耳阵列、黑白电平与相机矩阵，管线会跳过相应阶段。
    result.bayer = rawcore::BayerImage{};
    result.blackLevel = {{0.0f, 0.0f, 0.0f, 0.0f}};
    result.whiteLevel = 0.0f;
    result.detectedPattern = rawcore::BayerPattern::Unknown;
    result.cfaPatternGuessed = false;
    result.cameraToXyz = rawcore::identityMatrix3x3();
    result.hasCameraMatrix = false;

    // ---- 元数据（用于信息栏 / 状态栏 / 工程记录）----
    rawio::ImageFileInfo info;
    rawio::probeImageFile(path, info);

    auto& metadata = result.metadata;
    metadata.fileName = fileInfo.fileName().toStdString();
    metadata.cameraModel = info.cameraModel.toStdString();
    metadata.lensModel.clear();  // 普通位图不做镜头匹配（见管线镜头阶段）
    metadata.width = width;
    metadata.height = height;
    if (info.shotTime.isValid()) {
        metadata.timestamp =
            info.shotTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")).toStdString();
    } else {
        metadata.timestamp =
            fileInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")).toStdString();
    }
    metadata.iso = info.iso;
    metadata.shutter = info.shutter;
    metadata.aperture = info.aperture;
    metadata.focalLength = info.focalLength;

    // 关键：**不要**填 asShotTemperature。
    // 普通位图的像素已经过相机白平衡，管线再按 as-shot 补偿一次会产生偏色。
    // 主窗口会为这类文件把白平衡初始化为 D50 中性值（5000K / 0）。
    metadata.asShotTemperature = 0.0f;
    metadata.asShotTint = 0.0f;

    return rawcore::ErrorInfo::ok();
}

}  // namespace rawio
