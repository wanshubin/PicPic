// ============================================================================
//  DcpApply.h —— DCP 完整色彩链的渲染阶段（D1）
//
//  DcpProfile 只负责解析；本模块把解析结果真正作用到像素上：
//    1. ForwardMatrix / ColorMatrix 的按光源插值（相机 RGB → XYZ D50）；
//    2. ProfileHueSatMap（线性 ProPhoto 域的 HSV 三线性查找）；
//    3. ProfileToneCurve（逐通道分段线性曲线）；
//    4. ProfileLookTable（默认 Look 的 HSV 查找）。
//
//  阶段顺序遵循 DNG 规范：HueSatMap → ToneCurve → LookTable，
//  且全部定义在**线性 ProPhoto RGB**（即管线的工作空间）上。
//
//  为什么把这几步拆成独立自由函数：它们不依赖 Qt / lcms2，输入输出明确，
//  可以用合成的小表在单元测试里精确验证插值与边界行为。
// ============================================================================

#pragma once

#include "DcpProfile.h"
#include "EditContext.h"
#include "Types.h"

namespace rawcore {
namespace ops {

/// 按拍摄色温在 DCP 的两组矩阵之间插值（mired 线性，超出范围钳制）。
///
/// @param forward true 使用 ForwardMatrix1/2，false 使用 ColorMatrix1/2
/// @param asShotTemperature 拍摄色温（K），<= 0 时取光源 2 / 单矩阵
/// @param output 输出矩阵（ForwardMatrix 直接输出；ColorMatrix 会求逆成
///        “相机 RGB → XYZ”后输出，与 DcpProfile::cameraToXyzMatrix 一致）
/// @return 配置中有所需矩阵时 true
bool dcpInterpolatedMatrix(const DcpProfile& profile,
                           bool forward,
                           float asShotTemperature,
                           Matrix3x3& output);

/// 按拍摄色温选择 / 插值 HueSatMap。
/// 两组表的维度必须一致才能逐元素插值；否则取更接近拍摄色温的一组。
/// 没有 HueSatMap 时返回 false。
bool dcpInterpolatedHueSatMap(const DcpProfile& profile,
                              float asShotTemperature,
                              DcpHueSatMap& output);

/// 在**线性 ProPhoto** 图像上应用 Hue/Sat/Look 表。
///
/// 表节点的色相偏移已由解析层从度换算为归一化值；
/// 若表声明了 sRGB 编码（ProfileHueSatMapEncoding / ProfileLookTableEncoding），
/// 明度轴会先按 sRGB 曲线编码后查表、缩放后再解码回线性。
///
/// @param strength 0~1 混合强度，0 不改变像素，1 完全应用表中的缩放 / 偏移
/// @param lookTablePath true 表示应用 ProfileLookTable（step2 阶段）：
///        负通道先钳制为 0，表后饱和度钳制到 [0,1]（与 DNG / RawTherapee 一致）；
///        false 表示 Hue/Sat 表阶段，保持原行为。
void applyDcpHueSatMap(ImageBuffer& image, const DcpHueSatMap& map,
                       float strength = 1.0f, bool lookTablePath = false);

/// 应用 ProfileToneCurve（逐通道、分段线性）。
/// 坐标外推采用端点保持：x < 0 取首点 y，x > 1 取末点 y 对应的斜率外推会被钳制。
void applyDcpToneCurve(ImageBuffer& image, const DcpToneCurve& curve);

}  // namespace ops
}  // namespace rawcore
