// ============================================================================
//  ImageMetadata.h —— 选图工作台使用的轻量文件信息（RAW / JPG / 普通位图）
//
//  与 RawLoader::probeMetadata 的区别：
//   - 这里同时支持 RAW 与普通位图，供缩略图网格与信息栏使用；
//   - 只关心"挑片"需要的字段：拍摄时间、机型、镜头型号、尺寸、焦距/光圈/快门/ISO。
//
//  RAW 走 libraw（与主解码器同一套元数据来源），普通位图走 exiv2；
//  exiv2 缺失或读取失败时仍返回文件名与文件尺寸，不影响列表显示。
// ============================================================================

#pragma once

#include <QDateTime>
#include <QString>

namespace rawio {

/// 单个图片文件的基础信息
struct ImageFileInfo {
    QString path;
    QString fileName;
    QString cameraModel;
    QString lensModel;         ///< 镜头型号（RAW 取自 libraw，普通位图取自 EXIF），可能为空
    QDateTime shotTime;        ///< 无效表示文件没有拍摄时间
    int width = 0;
    int height = 0;
    float iso = 0.0f;
    float shutter = 0.0f;      ///< 秒
    float aperture = 0.0f;     ///< F 值
    float focalLength = 0.0f;  ///< mm
    qint64 fileSize = 0;       ///< 字节（排序与信息栏用）
    bool isRaw = false;
    bool hasEmbeddedPreview = false;  ///< RAW 是否带可直接显示的相机内嵌 JPEG

    // ---- 以下为可选字段，文件 / 相机没有记录时保持默认值，信息栏不展示 ----
    float focusDistance = 0.0f;           ///< 对焦距离（米），<= 0 表示未知
    QString whiteBalance;                 ///< 白平衡模式，如"自动"、"日光"；空表示未知
    float whiteBalanceTemperature = 0.0f; ///< as-shot 色温（K），仅 RAW 可由增益反算
    QString meteringMode;                 ///< 测光模式，如"评价测光"；空表示未知
    bool hasGps = false;                  ///< 是否读到了经纬度
    double latitude = 0.0;                ///< 纬度（十进制度，北纬为正）
    double longitude = 0.0;               ///< 经度（十进制度，东经为正）
    bool hasAltitude = false;             ///< 是否读到了海拔
    double altitude = 0.0;                ///< 海拔（米）
};

/// 是否为选图模块支持的文件（RAW 解码器支持的格式 + 普通位图）。
bool isSupportedImageFile(const QString& path);

/// 读取文件信息。
///
/// @return 是否成功读到 EXIF / RAW 元数据。即使返回 false，也会填好
///         path、fileName、isRaw 与文件系统时间等可用信息。
bool probeImageFile(const QString& path, ImageFileInfo& info);

/// 把快门时间格式化为摄影常用写法（1/250s、2.5s）
QString formatShutterSpeed(float seconds);

}  // namespace rawio
