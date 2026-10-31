// ============================================================================
//  DcpProfile.h —— DNG 相机配置文件（DCP）解析
//
//  DCP 本质上是 TIFF 文件，相机厂商 / 用户用它描述“相机原生 RGB → XYZ”
//  的标定矩阵、Hue/Sat 查找表与色调曲线。本模块只负责**解析**，
//  不依赖 Qt，也不直接改动渲染参数，方便单元测试与复用。
//
//  支持的主要 DNG 标签（IFD0）：
//    50721/50722 ColorMatrix1/2           XYZ → 相机 RGB（光源 1/2）
//    50778/50779 CalibrationIlluminant1/2 标定光源
//    50936 ProfileName                  配置名称
//    50937 ProfileHueSatMapDims         Hue/Sat 表维度 [hue, sat, val]
//    50938/50939 ProfileHueSatMapData1/2 Hue/Sat 表数据
//    50940 ProfileToneCurve             色调曲线（成对 x/y）
//    50942 ProfileCopyright             版权信息
//    50964/50965 ForwardMatrix1/2        白平衡后的相机 RGB → XYZ(D50)
//    50981/50982 ProfileLookTableDims/Data  默认 Look 表
//
//  说明：ColorMatrix 按 DNG 规范保存为**行优先的 XYZ → 相机 RGB** 矩阵；
//  渲染层需要的是反向的“相机 RGB → XYZ”，必须通过
//  cameraToXyzMatrix() 求逆，不能直接把 ColorMatrix 当作 cam2xyz 使用。
// ============================================================================

#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rawcore {

/// DCP 中的 Hue/Sat 查找表。
///
/// 数据按 **[value][hue][saturation]** 排列（saturation 变化最快），
/// 每个节点 3 个浮点值，依次是色相偏移、饱和度缩放、明度缩放（DNG 规范）。
/// 注意：DCP 文件中的色相偏移以**度**（约 -180..180）存储，
/// 解析时已统一除以 360，因此内存中的偏移量是 [0,1) 归一化色相单位。
struct DcpHueSatMap {
    int hueDivisions = 0;  ///< 色相方向节点数（通常 90）
    int satDivisions = 0;  ///< 饱和度方向节点数（通常 30）
    int valDivisions = 0;  ///< 明度方向节点数（1 或 16）
    /// 明度（V）轴是否按 sRGB 编码查表。
    /// 对应 ProfileHueSatMapEncoding / ProfileLookTableEncoding：
    /// 0 = 线性，1 = sRGB。sRGB 编码让暗部获得更多索引精度，
    /// 查表时先把 V 编码、查完再解码回线性（仅作用于 V 轴）。
    bool srgbEncoding = false;
    std::vector<float> data;

    bool isEmpty() const
    {
        return hueDivisions <= 0 || satDivisions <= 0 || valDivisions <= 0 || data.empty();
    }
};

/// DCP 色调曲线。coordinates 按 [x0, y0, x1, y1, ...] 顺序保存。
struct DcpToneCurve {
    std::vector<float> coordinates;

    bool isEmpty() const { return coordinates.size() < 4; }
};

/// 解析结果。valid 为 false 时 error 给出中文原因。
struct DcpProfile {
    bool valid = false;
    std::string profileName;
    std::string profileCopyright;

    /// DNG UniqueCameraModel（标签 50708），用于配置文件库的自动匹配。
    /// 不是所有 DCP 都提供，缺失时为空。
    std::string uniqueCameraModel;

    // ---- 相机 RGB → XYZ 标定矩阵（DCP 原始数据为反向的 XYZ → 相机）----
    bool hasColorMatrix1 = false;
    bool hasColorMatrix2 = false;
    Matrix3x3 colorMatrix1 = identityMatrix3x3();
    Matrix3x3 colorMatrix2 = identityMatrix3x3();

    // ---- 正向矩阵（白平衡后的相机 RGB → XYZ D50），仅解析、供诊断显示 ----
    bool hasForwardMatrix1 = false;
    bool hasForwardMatrix2 = false;
    Matrix3x3 forwardMatrix1 = identityMatrix3x3();
    Matrix3x3 forwardMatrix2 = identityMatrix3x3();

    // ---- 标定光源 ----
    int illuminant1Code = 0;  ///< EXIF LightSource 编码，0 表示未提供
    int illuminant2Code = 0;
    float illuminant1Temperature = 0.0f;  ///< 由编码换算的色温（K），0 表示未知
    float illuminant2Temperature = 0.0f;

    // ---- 附加色彩塑形数据 ----
    DcpHueSatMap hueSatMap1;
    DcpHueSatMap hueSatMap2;
    DcpToneCurve toneCurve;
    DcpHueSatMap lookTable;

    /// 解析过程中的非致命提醒（例如数据缺失、矩阵接近奇异）
    std::vector<std::string> warnings;

    /// 解析失败的原因（valid == false 时有效）
    std::string error;

    /// 是否为双光源配置（两组标定矩阵 + 两个光源）
    bool isDualIlluminant() const
    {
        return hasColorMatrix1 && hasColorMatrix2;
    }

    /// 取日光端矩阵：优先 ColorMatrix2（D65），缺失时退回 ColorMatrix1。
    /// 注意：返回的是 DCP 原始的 XYZ → 相机矩阵，供诊断 / 测试使用。
    bool daylightMatrix(Matrix3x3& output) const;

    /// 取“相机 RGB → XYZ”矩阵（对 ColorMatrix 求逆），可直接填入
    /// EditContext::ccm.cam2xyz。
    /// @param second true 取光源 2（通常 D65），false 取光源 1（通常 A）。
    /// @param output 求逆结果
    /// @return 矩阵存在且可逆时返回 true
    bool cameraToXyzMatrix(bool second, Matrix3x3& output) const;
};

/// 把 EXIF / DNG 的 CalibrationIlluminant 编码换算为标称色温（K）。
/// 未知编码返回 0。
float dcpIlluminantTemperature(int code);

/// 从内存字节流解析 DCP（便于单元测试，无需真实文件）。
DcpProfile parseDcpProfileFromBytes(const std::vector<std::uint8_t>& bytes);

/// 从文件加载并解析 DCP。
DcpProfile loadDcpProfile(const std::string& filePath);

}  // namespace rawcore
