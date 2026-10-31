// ============================================================================
//  PipelineStages.cpp —— 阶段注册表实现（diy.md §4 地基）
// ============================================================================

#include "PipelineStages.h"

#include <algorithm>
#include <array>

namespace rawcore {

const char* pipelineDataKindName(PipelineDataKind kind)
{
    switch (kind) {
    case PipelineDataKind::RawBayer: return "RawBayer";
    case PipelineDataKind::CameraRgb: return "CameraRgb";
    case PipelineDataKind::Xyz: return "XYZ";
    case PipelineDataKind::WorkingRgb: return "WorkingRgb";
    }
    return "Unknown";
}

const char* pipelineStageGroupName(PipelineStageGroup group)
{
    switch (group) {
    case PipelineStageGroup::Sensor: return "Sensor";
    case PipelineStageGroup::Demosaic: return "Demosaic";
    case PipelineStageGroup::Color: return "Color";
    case PipelineStageGroup::Transform: return "Transform";
    case PipelineStageGroup::Tone: return "Tone";
    case PipelineStageGroup::Detail: return "Detail";
    case PipelineStageGroup::Output: return "Output";
    }
    return "Unknown";
}

namespace {

PipelineStageDescriptor stage(PipelineStageId id, const char* key,
                              const char* name,
                              PipelineDataKind input,
                              PipelineDataKind output,
                              PipelineStageGroup group,
                              std::vector<PipelineStageId> dependencies,
                              bool canReorder = false,
                              bool canBypass = false)
{
    PipelineStageDescriptor descriptor;
    descriptor.id = id;
    descriptor.key = key;
    descriptor.displayName = name;
    descriptor.inputKind = input;
    descriptor.outputKind = output;
    descriptor.group = group;
    descriptor.dependencies = std::move(dependencies);
    descriptor.canReorder = canReorder;
    descriptor.canBypass = canBypass;
    return descriptor;
}

const std::vector<PipelineStageDescriptor>& buildRegistry()
{
    using Id = PipelineStageId;
    using Kind = PipelineDataKind;
    using Group = PipelineStageGroup;

    static const std::vector<PipelineStageDescriptor> registry = {
        stage(Id::RawBayer, "raw-bayer", "原始拜耳", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {}, false, false),
        stage(Id::BlackLevel, "black-level", "黑电平校正", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::RawBayer}, false, false),
        stage(Id::BadPixelRepair, "bad-pixel", "坏点修复", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::BlackLevel}, false, true),
        stage(Id::DarkCurrent, "dark-current", "暗电流补偿", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::BadPixelRepair}, false, true),
        stage(Id::DarkFrame, "dark-frame", "暗场帧", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::DarkCurrent}, false, true),
        stage(Id::FlatFrame, "flat-frame", "平场帧", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::DarkFrame}, false, true),
        stage(Id::Crosstalk, "crosstalk", "串扰补偿", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::FlatFrame}, true, true),
        stage(Id::BayerCcm, "bayer-ccm", "拜耳域 CCM", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::FlatFrame}, true, true),
        stage(Id::BayerDenoise, "bayer-denoise", "拜耳降噪 FBDD", Kind::RawBayer,
              Kind::RawBayer, Group::Sensor, {Id::FlatFrame}, true, true),
        stage(Id::Demosaic, "demosaic", "Demosaic", Kind::RawBayer,
              Kind::CameraRgb, Group::Demosaic, {Id::BayerDenoise, Id::BayerCcm, Id::Crosstalk}, false, false),
        stage(Id::HighlightRolloff, "highlight-rolloff", "高光 Roll-off",
              Kind::CameraRgb, Kind::CameraRgb, Group::Color, {Id::Demosaic},
              true, true),
        stage(Id::Lca, "lca", "LCA 横向色差", Kind::CameraRgb, Kind::CameraRgb,
              Group::Color, {Id::Demosaic}, true, true),
        stage(Id::Loca, "loca", "LoCA 轴向色散", Kind::CameraRgb, Kind::CameraRgb,
              Group::Color, {Id::Demosaic}, true, true),
        stage(Id::EdgeFalseColor, "edge-false-color", "边缘伪色抑制",
              Kind::CameraRgb, Kind::CameraRgb, Group::Color, {Id::Demosaic},
              true, true),
        stage(Id::LensCorrection, "lens-correction", "镜头校正", Kind::CameraRgb,
              Kind::CameraRgb, Group::Color, {Id::EdgeFalseColor}, false, true),
        stage(Id::CameraToXyz, "camera-to-xyz", "相机 → XYZ", Kind::CameraRgb,
              Kind::Xyz, Group::Transform, {Id::LensCorrection}, false, false),
        stage(Id::XyzToWorking, "xyz-to-working", "XYZ → 工作空间", Kind::Xyz,
              Kind::WorkingRgb, Group::Transform, {Id::CameraToXyz}, false, false),
        stage(Id::BaseWhiteBalance, "base-white-balance", "基准白平衡",
              Kind::WorkingRgb, Kind::WorkingRgb, Group::Tone,
              {Id::XyzToWorking}, false, true),
        stage(Id::CreativeWhiteBalance, "creative-white-balance", "创意白平衡",
              Kind::WorkingRgb, Kind::WorkingRgb, Group::Tone,
              {Id::BaseWhiteBalance}, false, true),
        stage(Id::PreviewTone, "preview-tone", "预览影调", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Tone, {Id::CreativeWhiteBalance}, false, true),
        stage(Id::LightAdjust, "light", "光影", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Tone, {Id::PreviewTone}, false, true),
        stage(Id::Presence, "presence", "色彩与质感", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Tone, {Id::LightAdjust}, false, true),
        stage(Id::Hsl, "hsl", "HSL", Kind::WorkingRgb, Kind::WorkingRgb,
              Group::Tone, {Id::Presence}, false, true),
        stage(Id::BlackWhite, "black-white", "黑白混色", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Tone, {Id::Hsl}, false, true),
        stage(Id::ColorGrading, "color-grading", "颜色分级", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Tone, {Id::BlackWhite}, false, true),
        stage(Id::Curves, "curves", "曲线", Kind::WorkingRgb, Kind::WorkingRgb,
              Group::Tone, {Id::ColorGrading}, false, true),
        stage(Id::Lut, "lut", "3D LUT", Kind::WorkingRgb, Kind::WorkingRgb,
              Group::Tone, {Id::Curves}, false, true),
        stage(Id::LocalAdjust, "local-adjust", "局部调整", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Detail, {Id::Lut}, false, true),
        stage(Id::Retouch, "retouch", "污点修复", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Detail, {Id::LocalAdjust}, false, true),
        stage(Id::Crop, "crop", "裁剪 / 拉直", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Detail, {Id::Retouch}, false, true),
        stage(Id::NoiseReduction, "noise-reduction", "降噪", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Detail, {Id::Crop}, false, true),
        stage(Id::Sharpen, "sharpen", "锐化", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Detail, {Id::NoiseReduction}, false, true),
        stage(Id::GamutMap, "gamut-map", "色域映射 / 输出", Kind::WorkingRgb,
              Kind::WorkingRgb, Group::Output, {Id::Sharpen}, false, false),
    };
    return registry;
}

}  // namespace

const std::vector<PipelineStageDescriptor>& pipelineStageRegistry()
{
    return buildRegistry();
}

const std::vector<PipelineStageId>& defaultPipelineOrder()
{
    static const std::vector<PipelineStageId> order = []() {
        std::vector<PipelineStageId> result;
        result.reserve(pipelineStageRegistry().size());
        for (const PipelineStageDescriptor& descriptor : pipelineStageRegistry()) {
            result.push_back(descriptor.id);
        }
        return result;
    }();
    return order;
}

const char* pipelineStageKey(PipelineStageId id)
{
    for (const PipelineStageDescriptor& descriptor : pipelineStageRegistry()) {
        if (descriptor.id == id) {
            return descriptor.key.c_str();
        }
    }
    return "unknown";
}

const char* pipelineStageDisplayName(PipelineStageId id)
{
    for (const PipelineStageDescriptor& descriptor : pipelineStageRegistry()) {
        if (descriptor.id == id) {
            return descriptor.displayName.c_str();
        }
    }
    return "未知阶段";
}

PipelineStageId pipelineStageFromKey(const std::string& key, bool* found)
{
    for (const PipelineStageDescriptor& descriptor : pipelineStageRegistry()) {
        if (descriptor.key == key) {
            if (found != nullptr) {
                *found = true;
            }
            return descriptor.id;
        }
    }
    if (found != nullptr) {
        *found = false;
    }
    return PipelineStageId::RawBayer;
}

bool validatePipelineOrder(const std::vector<PipelineStageId>& order,
                           std::string& error)
{
    error.clear();
    const auto& registry = pipelineStageRegistry();
    if (order.size() != registry.size()) {
        error = "阶段数量不完整：需要 " + std::to_string(registry.size())
            + " 个，实际 " + std::to_string(order.size()) + " 个。";
        return false;
    }

    std::array<int, kPipelineStageCount> position{};
    position.fill(-1);
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        const int id = static_cast<int>(order[static_cast<std::size_t>(i)]);
        if (id < 0 || id >= kPipelineStageCount) {
            error = "存在未知阶段 id。";
            return false;
        }
        if (position[static_cast<std::size_t>(id)] >= 0) {
            error = std::string("阶段重复：") + pipelineStageKey(order[static_cast<std::size_t>(i)]);
            return false;
        }
        position[static_cast<std::size_t>(id)] = i;
    }

    for (const PipelineStageDescriptor& descriptor : registry) {
        const int current = position[static_cast<std::size_t>(descriptor.id)];
        if (current < 0) {
            error = std::string("缺少阶段：") + descriptor.key;
            return false;
        }
        for (PipelineStageId dependency : descriptor.dependencies) {
            const int required = position[static_cast<std::size_t>(dependency)];
            if (required < 0 || required >= current) {
                error = std::string("阶段 \"") + descriptor.displayName
                    + "\" 必须排在 \"" + pipelineStageDisplayName(dependency)
                    + "\" 之后。";
                return false;
            }
        }
    }

    for (std::size_t i = 1; i < order.size(); ++i) {
        const PipelineStageDescriptor& previous = registry[static_cast<std::size_t>(
            position[static_cast<std::size_t>(order[i - 1])])];
        const PipelineStageDescriptor& current = registry[static_cast<std::size_t>(
            position[static_cast<std::size_t>(order[i])])];
        if (previous.outputKind != current.inputKind) {
            error = std::string("数据格式不衔接：\"") + previous.displayName
                + "\" 输出 " + pipelineDataKindName(previous.outputKind)
                + "，而 \"" + current.displayName + "\" 需要 "
                + pipelineDataKindName(current.inputKind) + "。";
            return false;
        }
    }
    return true;
}

}  // namespace rawcore
