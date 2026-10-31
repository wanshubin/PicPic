// ============================================================================
//  PipelineStages.h —— 解码 / 调色阶段注册表（diy.md §4 地基）
//
//  把之前硬编码在 PipelineRenderer 里的阶段顺序抽象成可查询的描述：
//    - 阶段 id / 稳定键名 / 显示名；
//    - 输入 / 输出数据类型与色彩空间；
//    - 依赖阶段（必须排在它之前）；
//    - 是否可旁路、可重排及所属分组。
//
//  本步骤只建立“描述与校验”，不改变现有执行顺序：
//  默认顺序仍由 PipelineRenderer 按原代码执行，保证逐位一致。
//  后续拖拽重排将基于同一张表做落点校验与执行图调度。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rawcore {

/// 阶段处理的数据类型（用于校验相邻阶段是否衔接）
enum class PipelineDataKind : std::uint8_t {
    RawBayer = 0,  ///< 拜耳单通道
    CameraRgb,     ///< 去拜耳后的相机 RGB
    Xyz,           ///< CIE XYZ(D50)
    WorkingRgb,    ///< 线性 ProPhoto(D50)
};

const char* pipelineDataKindName(PipelineDataKind kind);

/// 阶段分组；只有同一分组内的阶段默认允许重排
enum class PipelineStageGroup : std::uint8_t {
    Sensor = 0,  ///< 拜耳域：黑电平、坏点、暗电流、串扰、CCM、FBDD
    Demosaic,    ///< 去拜耳边界（不可移动）
    Color,       ///< RGB 域色彩校正：Roll-off、LCA、LoCA、伪色、镜头
    Transform,   ///< 相机 → XYZ → 工作空间
    Tone,        ///< 白平衡、光影、HSL、曲线等上层调色
    Detail,      ///< 局部调整、裁剪、降噪、锐化
    Output,      ///< 色域映射 / 输出色彩空间
};

const char* pipelineStageGroupName(PipelineStageGroup group);

/// 阶段 id（稳定枚举；顺序与默认执行顺序一致）
enum class PipelineStageId : std::uint8_t {
    RawBayer = 0,
    BlackLevel,
    BadPixelRepair,
    DarkCurrent,
    DarkFrame,
    FlatFrame,
    Crosstalk,
    BayerCcm,
    BayerDenoise,
    Demosaic,
    HighlightRolloff,
    Lca,
    Loca,
    EdgeFalseColor,
    LensCorrection,
    CameraToXyz,
    XyzToWorking,
    BaseWhiteBalance,
    CreativeWhiteBalance,
    PreviewTone,
    LightAdjust,
    Presence,
    Hsl,
    BlackWhite,
    ColorGrading,
    Curves,
    Lut,
    LocalAdjust,
    Retouch,
    Crop,
    NoiseReduction,
    Sharpen,
    GamutMap,
    Count,
};

constexpr int kPipelineStageCount = static_cast<int>(PipelineStageId::Count);

/// 单个阶段描述
struct PipelineStageDescriptor {
    PipelineStageId id = PipelineStageId::RawBayer;
    std::string key;         ///< 工程 / 预设中的稳定键名
    std::string displayName; ///< 中文显示名

    PipelineDataKind inputKind = PipelineDataKind::RawBayer;
    PipelineDataKind outputKind = PipelineDataKind::RawBayer;

    std::vector<PipelineStageId> dependencies;

    PipelineStageGroup group = PipelineStageGroup::Sensor;
    bool canReorder = false;  ///< 是否允许拖拽重排
    bool canBypass = false;   ///< 是否允许旁路
};

/// 完整阶段表（按默认顺序）
const std::vector<PipelineStageDescriptor>& pipelineStageRegistry();

/// 默认阶段顺序
const std::vector<PipelineStageId>& defaultPipelineOrder();

/// 稳定键名 / 显示名 ↔ id
const char* pipelineStageKey(PipelineStageId id);
const char* pipelineStageDisplayName(PipelineStageId id);
PipelineStageId pipelineStageFromKey(const std::string& key,
                                     bool* found = nullptr);

/// 校验一个阶段顺序。
///
/// 规则：
///  1. 必须恰好包含每个阶段一次；
///  2. 每个阶段的所有依赖必须排在它之前；
///  3. 相邻阶段的输出 / 输入数据类型必须衔接。
///
/// @param error 失败时给出中文原因
bool validatePipelineOrder(const std::vector<PipelineStageId>& order,
                           std::string& error);

}  // namespace rawcore
