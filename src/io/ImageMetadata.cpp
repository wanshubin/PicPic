// ============================================================================
//  ImageMetadata.cpp —— 选图工作台的元数据读取
// ============================================================================

#include "ImageMetadata.h"

#include "StandardImageLoader.h"
#include "core/RawLoader.h"

#include <QFileInfo>
#include <QImageReader>
#include <QStringList>

#if defined(RAWCore_HAS_EXIV2)
#  include <exiv2/exiv2.hpp>
#endif

#include <cmath>

namespace rawio {

namespace {

#if defined(RAWCore_HAS_EXIV2)

/// EXIF 测光模式枚举 → 中文名称。未知 / 未定义返回空串，信息栏据此不展示。
QString meteringModeName(qint64 mode)
{
    switch (mode) {
    case 1:
        return QStringLiteral("平均测光");
    case 2:
        return QStringLiteral("中央重点测光");
    case 3:
        return QStringLiteral("点测光");
    case 4:
        return QStringLiteral("多区测光");
    case 5:
        // 佳能 / 索尼称"评价测光"，尼康称"矩阵测光"，这里用通用叫法
        return QStringLiteral("评价测光");
    case 6:
        return QStringLiteral("局部测光");
    default:
        return {};
    }
}

/// EXIF LightSource 枚举 → 中文名称（0 / 16 / 255 等无实际含义的值返回空串）
QString lightSourceName(qint64 source)
{
    switch (source) {
    case 1:
        return QStringLiteral("日光");
    case 2:
        return QStringLiteral("荧光灯");
    case 3:
        return QStringLiteral("钨丝灯");
    case 4:
        return QStringLiteral("闪光灯");
    case 9:
        return QStringLiteral("晴天");
    case 10:
        return QStringLiteral("阴天");
    case 11:
        return QStringLiteral("阴影");
    case 12:
        return QStringLiteral("日光荧光灯");
    case 13:
        return QStringLiteral("日光白荧光灯");
    case 14:
        return QStringLiteral("冷白荧光灯");
    case 15:
        return QStringLiteral("暖白荧光灯");
    case 17:
        return QStringLiteral("标准光 A");
    case 18:
        return QStringLiteral("标准光 B");
    case 19:
        return QStringLiteral("标准光 C");
    case 20:
        return QStringLiteral("D55");
    case 21:
        return QStringLiteral("D65");
    case 22:
        return QStringLiteral("D75");
    case 23:
        return QStringLiteral("D50");
    case 24:
        return QStringLiteral("演播室钨丝灯");
    default:
        return {};
    }
}

/// 读取 GPS 的"度 / 分 / 秒"有理数数组并换算为十进制度。
/// 数据不完整或格式异常时返回 false，调用方按"没有 GPS"处理。
bool readGpsCoordinate(const Exiv2::ExifData& exif, const char* valueKey,
                       const char* refKey, double& out)
{
    try {
        const auto valueIt = exif.findKey(Exiv2::ExifKey(valueKey));
        if (valueIt == exif.end() || valueIt->count() < 3) {
            return false;
        }
        const double degrees = valueIt->toFloat(0);
        const double minutes = valueIt->toFloat(1);
        const double seconds = valueIt->toFloat(2);
        if (!std::isfinite(degrees) || !std::isfinite(minutes) || !std::isfinite(seconds)) {
            return false;
        }

        double value = degrees + minutes / 60.0 + seconds / 3600.0;
        const auto refIt = exif.findKey(Exiv2::ExifKey(refKey));
        const QString ref = refIt == exif.end()
                                ? QString()
                                : QString::fromStdString(refIt->toString()).trimmed().toUpper();
        // S / W 表示南纬 / 西经，统一换算为负值
        if (ref == QLatin1String("S") || ref == QLatin1String("W")) {
            value = -value;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

#endif  // RAWCore_HAS_EXIV2

/// 标准 EXIF 元数据读取（exiv2 可用时）。
///
/// 普通位图从这里取得全部基础信息；RAW 的型号 / 曝光参数等已由 libraw
/// 填好，本函数只补它不提供的测光模式、白平衡模式、GPS 等字段。
/// 因此所有基础字段都只在当前值为空 / 无效时才写入。
void fillExifMetadata(const QString& path, ImageFileInfo& info)
{
#if defined(RAWCore_HAS_EXIV2)
    try {
        auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        const Exiv2::ExifData& exif = image->exifData();

        const auto readString = [&exif](const char* key) -> QString {
            const auto it = exif.findKey(Exiv2::ExifKey(key));
            return it == exif.end() ? QString()
                                    : QString::fromStdString(it->toString());
        };
        const auto readFloat = [&exif](const char* key) -> float {
            const auto it = exif.findKey(Exiv2::ExifKey(key));
            if (it == exif.end()) {
                return 0.0f;
            }
            try {
                return it->toFloat();
            } catch (...) {
                return 0.0f;
            }
        };

        // ---- 基础字段 ----
        if (info.cameraModel.isEmpty()) {
            QString model = readString("Exif.Image.Model").trimmed();
            if (model.isEmpty()) {
                model = readString("Exif.Image.Make").trimmed();
            }
            if (!model.isEmpty()) {
                info.cameraModel = model;
            }
        }

        // 镜头型号：标准 EXIF 字段，部分机型 / 转接环可能不写入，缺失时留空
        if (info.lensModel.isEmpty()) {
            const QString lens = readString("Exif.Photo.LensModel").trimmed();
            if (!lens.isEmpty()) {
                info.lensModel = lens;
            }
        }

        // 优先 DateTimeOriginal（拍摄瞬间），缺失时退回 DateTime（文件修改时间）
        if (!info.shotTime.isValid()) {
            QString dateText = readString("Exif.Photo.DateTimeOriginal");
            if (dateText.isEmpty()) {
                dateText = readString("Exif.Image.DateTime");
            }
            const QDateTime shot =
                QDateTime::fromString(dateText, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
            if (shot.isValid()) {
                info.shotTime = shot;
            }
        }

        if (info.iso <= 0.0f) {
            info.iso = readFloat("Exif.Photo.ISOSpeedRatings");
        }
        if (info.aperture <= 0.0f) {
            info.aperture = readFloat("Exif.Photo.FNumber");
        }
        if (info.shutter <= 0.0f) {
            info.shutter = readFloat("Exif.Photo.ExposureTime");
        }
        if (info.focalLength <= 0.0f) {
            info.focalLength = readFloat("Exif.Photo.FocalLength");
        }

        // 尺寸：优先 EXIF 记录，缺失时由 QImageReader 补齐。
        // RAW 的尺寸来自 libraw（传感器全宽），不要被内嵌 JPEG 的尺寸覆盖。
        if (info.width <= 0 || info.height <= 0) {
            const auto widthIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelXDimension"));
            const auto heightIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.PixelYDimension"));
            if (widthIt != exif.end() && heightIt != exif.end()) {
                info.width = static_cast<int>(widthIt->toInt64());
                info.height = static_cast<int>(heightIt->toInt64());
            }
        }

        // ---- 对焦距离：标准 EXIF 单位为米；0 与超大值（无穷远占位）视为未知 ----
        if (info.focusDistance <= 0.0f) {
            const float distance = readFloat("Exif.Photo.SubjectDistance");
            if (distance > 0.0f && distance < 10000.0f) {
                info.focusDistance = distance;
            }
        }

        // ---- 白平衡：WhiteBalance 只区分自动 / 手动，LightSource 补充具体光源 ----
        if (info.whiteBalance.isEmpty()) {
            qint64 wbMode = -1;
            qint64 lightSource = 0;
            try {
                const auto wbIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance"));
                if (wbIt != exif.end()) {
                    wbMode = wbIt->toInt64();
                }
                const auto lightIt = exif.findKey(Exiv2::ExifKey("Exif.Photo.LightSource"));
                if (lightIt != exif.end()) {
                    lightSource = lightIt->toInt64();
                }
            } catch (...) {
                // 枚举字段损坏时按没有白平衡信息处理
            }

            const QString sourceName = lightSourceName(lightSource);
            QString whiteBalance;
            if (wbMode == 0) {
                whiteBalance = sourceName.isEmpty()
                                   ? QStringLiteral("自动")
                                   : QStringLiteral("自动 (%1)").arg(sourceName);
            } else if (wbMode == 1) {
                whiteBalance = sourceName.isEmpty() ? QStringLiteral("手动") : sourceName;
            } else if (!sourceName.isEmpty()) {
                whiteBalance = sourceName;
            }
            if (!whiteBalance.isEmpty()) {
                info.whiteBalance = whiteBalance;
            }
        }

        // ---- 测光模式 ----
        if (info.meteringMode.isEmpty()) {
            try {
                const auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.MeteringMode"));
                if (it != exif.end()) {
                    info.meteringMode = meteringModeName(it->toInt64());
                }
            } catch (...) {
                // 忽略损坏的枚举值
            }
        }

        // ---- GPS：经纬度必须成对出现才算有效，海拔单独判断 ----
        if (!info.hasGps) {
            double latitude = 0.0;
            double longitude = 0.0;
            if (readGpsCoordinate(exif, "Exif.GPSInfo.GPSLatitude",
                                  "Exif.GPSInfo.GPSLatitudeRef", latitude)
                && readGpsCoordinate(exif, "Exif.GPSInfo.GPSLongitude",
                                     "Exif.GPSInfo.GPSLongitudeRef", longitude)) {
                info.hasGps = true;
                info.latitude = latitude;
                info.longitude = longitude;
            }
        }
        if (!info.hasAltitude) {
            try {
                const auto altIt = exif.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitude"));
                if (altIt != exif.end()) {
                    double altitude = altIt->toFloat();
                    // GPSAltitudeRef：0 = 海平面以上，1 = 海平面以下
                    const QString ref = readString("Exif.GPSInfo.GPSAltitudeRef").trimmed();
                    if (ref == QLatin1String("1")) {
                        altitude = -altitude;
                    }
                    if (std::isfinite(altitude)) {
                        info.hasAltitude = true;
                        info.altitude = altitude;
                    }
                }
            } catch (...) {
                // 忽略损坏的海拔字段
            }
        }
    } catch (const Exiv2::Error&) {
        // 元数据损坏 / 格式不支持：保留文件名等基础信息，交给尺寸兜底
    } catch (...) {
        // exiv2 的其它异常同样不应影响列表显示
    }
#else
    Q_UNUSED(path);
#endif
}

/// 文件系统层面的兜底信息
void fillFileFallback(const QString& path, ImageFileInfo& info)
{
    const QFileInfo fileInfo(path);
    info.path = fileInfo.absoluteFilePath();
    info.fileName = fileInfo.fileName();
    info.fileSize = fileInfo.size();
    if (!info.shotTime.isValid()) {
        // 没有 EXIF 时间时用文件修改时间，至少能按时间排序
        info.shotTime = fileInfo.lastModified();
    }
    if (info.width <= 0 || info.height <= 0) {
        QImageReader reader(path);
        const QSize size = reader.size();
        if (size.isValid()) {
            info.width = size.width();
            info.height = size.height();
        }
    }
}

}  // namespace

bool isSupportedImageFile(const QString& path)
{
    if (rawcore::RawLoader::isSupportedExtension(path.toStdString())) {
        return true;
    }
    // 普通位图（JPG / PNG / TIFF / WebP / BMP）与 RAW 共用同一份扩展名定义
    return isStandardImagePath(path);
}

bool probeImageFile(const QString& path, ImageFileInfo& info)
{
    info = ImageFileInfo{};
    info.path = QFileInfo(path).absoluteFilePath();
    info.fileName = QFileInfo(path).fileName();
    info.isRaw = rawcore::RawLoader::isSupportedExtension(path.toStdString());

    bool metadataRead = false;
    if (info.isRaw) {
        rawcore::RawMetadata metadata;
        const rawcore::ErrorInfo error =
            rawcore::RawLoader().probeMetadata(path.toStdString(), metadata);
        if (error.isOk()) {
            metadataRead = true;
            info.hasEmbeddedPreview = metadata.hasEmbeddedPreview;
            info.cameraModel = QString::fromStdString(metadata.cameraModel).trimmed();
            info.lensModel = QString::fromStdString(metadata.lensModel).trimmed();
            info.width = metadata.width;
            info.height = metadata.height;
            info.iso = metadata.iso;
            info.shutter = metadata.shutter;
            info.aperture = metadata.aperture;
            info.focalLength = metadata.focalLength;
            // 部分相机用超大数值表示无穷远，这类占位值不当作对焦距离展示
            if (metadata.focusDistance > 0.0f && metadata.focusDistance < 10000.0f) {
                info.focusDistance = metadata.focusDistance;
            }
            // "default" 表示没有读到厂商白平衡数据、按日光兜底，不是真实信息
            if (metadata.asShotTemperature > 0.0f
                && metadata.whiteBalanceSource != "default") {
                info.whiteBalanceTemperature = metadata.asShotTemperature;
            }
            if (!metadata.timestamp.empty()) {
                const QDateTime shot =
                    QDateTime::fromString(QString::fromStdString(metadata.timestamp),
                                          QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                if (shot.isValid()) {
                    info.shotTime = shot;
                }
            }
        }
    }

    // 标准 EXIF 字段：普通位图从这里取得全部信息；RAW 用它补齐 libraw
    // 不提供的测光模式、白平衡模式与 GPS。只读元数据，额外开销很小。
    fillExifMetadata(path, info);

    if (!info.isRaw) {
        metadataRead = !info.cameraModel.isEmpty() || info.shotTime.isValid();
    }

    fillFileFallback(path, info);
    return metadataRead;
}

QString formatShutterSpeed(float seconds)
{
    if (seconds <= 0.0f) {
        return QStringLiteral("—");
    }
    if (seconds >= 1.0f) {
        return QString::number(seconds, 'g', 3) + QStringLiteral("s");
    }
    // 相机常见的 1/8000 ~ 1/2 秒，四舍五入到整数分母
    const int denominator = static_cast<int>(std::lround(1.0 / static_cast<double>(seconds)));
    if (denominator <= 1) {
        return QString::number(seconds, 'g', 2) + QStringLiteral("s");
    }
    return QStringLiteral("1/%1s").arg(denominator);
}

}  // namespace rawio
