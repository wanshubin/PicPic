// ============================================================================
//  StandardImageLoader.h —— 普通位图（JPG / PNG / TIFF / WebP / BMP）解码
//
//  与 RawLoader 的关系：
//   - RawLoader 输出拜耳域单通道数据，交给管线去马赛克；
//   - 本模块输出**线性 sRGB（D65）**三通道浮点数据，相机/软件已把白平衡、
//     去马赛克、镜头校正烘焙进像素，因此管线会跳过全部 RAW 专属阶段，
//     直接从"线性 sRGB → ProPhoto 工作空间"开始。
//
//  EXIF 方向在这里直接应用（QImageReader::setAutoTransform），
//  像素方向与相机竖拍一致，后续管线不再旋转。
// ============================================================================

#pragma once

#include "core/RawLoader.h"

#include <QString>
#include <QStringList>

namespace rawio {

/// 是否为普通位图（非 RAW）且由本模块负责解码
bool isStandardImagePath(const QString& path);

/// 支持的扩展名（不含点），用于文件对话框与目录扫描
QStringList standardImageSuffixes();

/// 把普通位图解码为 RawDecodeResult::rgbLinear（线性 sRGB），
/// 并填写可供界面显示的元数据。
/// @return 错误信息；成功时 code == ErrorCode::Ok
rawcore::ErrorInfo loadStandardImage(const QString& path,
                                     rawcore::RawDecodeResult& result);

}  // namespace rawio
