// ============================================================================
//  FrameCalibration.h —— 暗场帧 / 平场帧校正（需求 append_v3 §五 1、2）
//
//  设计原则：
//   1. 两帧都在**拜耳域**、**白电平归一化之后**、串扰补偿与 CCM 之前介入，
//      因此不改变现有管线主顺序，只是在既有"暗电流补偿"一步之后插入
//      与通道排列无关的逐像素运算。
//   2. 暗场帧：逐像素减去暗噪声（含热像素底噪、放大器偏移）。
//   3. 平场帧：逐像素除以归一化响应，补偿镜头渐晕与像素响应不均（PRNU）。
//   4. 帧文件本身可以是任意支持的 RAW；加载后按自身白电平归一化，
//      尺寸与主图不一致时退化为"按通道均值校正"，并给出一条警告。
//   5. 帧解码成本较高（一次完整 RAW 解码），因此提供按路径 + 修改时间的
//      小型缓存，渲染线程与 CLI 共用。
// ============================================================================

#pragma once

#include "Types.h"

#include <array>
#include <memory>
#include <string>

namespace rawcore {

/// 已解码并归一化到 0~1 的标定帧（拜耳域）。
struct CalibrationFrame {
    /// 归一化后的拜耳数据。保留原文件的排列信息，便于按通道退化为均值校正。
    BayerImage bayer;

    /// 全帧均值（含全部像素），用于平场帧的参考亮度
    float mean = 0.0f;

    /// R / G / B 三通道均值，尺寸不匹配时用于"按通道均值校正"的退化路径
    std::array<float, 3> channelMean{{0.0f, 0.0f, 0.0f}};

    bool isEmpty() const { return bayer.isEmpty(); }

    /// 帧尺寸是否与待校正图像一致（一致时走逐像素路径）
    bool matches(int width, int height) const
    {
        return bayer.width == width && bayer.height == height;
    }
};

/// 由已解码的拜耳数据构造标定帧：按白电平归一化并统计均值。
/// @param bayer      输入拜耳数据（黑电平已扣除、尚未白电平裁剪，与 RawLoader 一致）
/// @param whiteLevel 饱和白电平，<= 0 时视为数据已在 0~1 范围
/// @param out        输出标定帧
ErrorInfo buildCalibrationFrame(BayerImage bayer,
                                float whiteLevel,
                                CalibrationFrame& out);

/// 加载（并缓存）一个标定帧 RAW 文件。
/// 缓存按"绝对路径 + 文件修改时间"命中，最多保留 4 帧。
/// @param path         帧文件路径
/// @param errorMessage 失败时写入中文原因
/// @return 成功时返回有效帧指针，失败返回 nullptr
std::shared_ptr<const CalibrationFrame> loadCalibrationFrame(
    const std::string& path,
    std::string& errorMessage);

/// 清空帧缓存（切换工程、退出前释放内存时调用）
void clearCalibrationFrameCache();

/// 逐像素暗场帧扣除：pixel = max(0, pixel − strength × dark)。
/// 尺寸不一致时改为按通道均值扣除（只校正整体暗底抬升，不校正热像素）。
void applyDarkFrameSubtraction(BayerImage& bayer,
                               const CalibrationFrame& dark,
                               float strength);

/// 逐像素平场帧校正：pixel = pixel × (1 + strength × (参考均值 / 帧像素 − 1))。
/// 单像素增益夹取在 [0.25, 4.0]，避免帧上的尘埃或坏点造成过度放大。
/// 尺寸不一致时改为按通道均值校正（只能消除平场自身的偏色，不校正渐晕）。
void applyFlatFrameCorrection(BayerImage& bayer,
                              const CalibrationFrame& flat,
                              float strength);

}  // namespace rawcore
