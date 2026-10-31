// ============================================================================
//  EditContext.h —— 非破坏性编辑参数模型（需求 §3 模块 D）
//
//  职责：承载全部可调参数，作为管线输入的唯一数据源。
//  约束：
//   - 绝不修改原始 RAW 文件，所有调整都在此模型中表达。
//   - 本文件不依赖 Qt，保持 core 的纯净可复用性。
// ============================================================================

#pragma once

#include "MaskModel.h"
#include "Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rawcore {

// ---------------------------------------------------------------------------
//  RAW 解码参数（需求 §3 模块 A 的"可配置项"）
// ---------------------------------------------------------------------------

/// 对应 .camcal 与 .rawedit.json 中的 raw_settings 段
struct RawSettings {
    /// 黑电平叠加偏移。实际使用值 = 相机记录黑电平 + 此偏移（可为负）
    float blackLevelOffset = 0.0f;

    /// 分通道黑电平附加偏移（解码值刻度，顺序 R/G1/B/G2）。
    /// 与 blackLevelOffset 叠加，用于 diy.md 的拜耳直方图游标。
    std::array<float, 4> channelBlackOffset{{0.0f, 0.0f, 0.0f, 0.0f}};

    /// 白电平饱和上限。0 表示使用相机记录的默认值
    float whiteLevelOverride = 0.0f;

    /// 分通道白电平（解码值刻度，0 表示沿用全局 whiteLevelOverride）。
    std::array<float, 4> channelWhiteLevel{{0.0f, 0.0f, 0.0f, 0.0f}};

    /// 强制指定拜耳排列。Unknown 表示任由 libraw 自动识别（自动识别优先）
    BayerPattern forceBayerPattern = BayerPattern::Unknown;

    /// 是否禁用相机原厂矩阵。
    /// 置 true 时，管线跳过"RAW 自带原厂矩阵"这条路径——适用于用户
    /// 已提供完整自定义 CCM 的场景（需求 §2 优先级规则）。
    bool disableCameraMatrix = false;

    /// 去拜耳算法。默认 AMaZE：边缘感知插值，伪色与拉链伪影较少，
    /// 高质量导出可改用 LMMSE；VNG4 / Bilinear 保留作测试与快速预览对照。
    DemosaicAlgorithm demosaicAlgorithm = DemosaicAlgorithm::AMAZE;

    /// 摩尔纹抑制强度 0.0 ~ 1.0。
    /// 默认 0.15：只压明显的高频伪彩，避免色度被过度模糊后 1:1 细节发软。
    float demosaicSmooth = 0.15f;

    /// FBDD（四方向拜耳域降噪）开关。
    /// 在去拜耳之前抑制孤立采样异常，可显著减少插值伪色。
    bool enableFbdd = true;

    /// FBDD 强度 0.0 ~ 1.0。默认 0.5，越高去伪色越强、细节越软。
    float fbddStrength = 0.5f;

    /// 传感器边缘裁剪，去除光学黑边与伪影边缘
    int cropLeft = 0;
    int cropTop = 0;
    int cropRight = 0;   ///< 从右侧裁掉的列数
    int cropBottom = 0;  ///< 从底部裁掉的行数

    /// 开启基础坏点修复（管线中的固定步骤，此处仅控制是否执行）
    bool enableBadPixelRepair = true;
};

// ---------------------------------------------------------------------------
//  自定义 CCM 参数（需求 §3 模块 B）
// ---------------------------------------------------------------------------

struct CcmSettings {
    /// 是否启用拜耳域自定义 CCM 矩阵。
    /// 启用后按需求 §2 优先级规则覆盖相机原厂矩阵。
    bool enable = false;

    /// CamRGB → XYZ 的 3x3 矩阵（D50 白点基准）
    ///
    /// 双光源模式下，此矩阵表示**日光端**（D65）的标定结果。
    Matrix3x3 cam2xyz = identityMatrix3x3();

    /// 是否已从原厂矩阵初始化过。用于 UI 上区分"未改动"与"用户自定义"
    bool initializedFromCamera = false;

    // ---- 双光源标定（需求 §1.4 相机校准）----
    //
    // 传感器在不同色温光源下的光谱响应差异无法用单组矩阵描述。
    // 开启双光源后，管线按拍摄时的 as-shot 色温在两组矩阵之间插值：
    //   - cam2xyz            → 日光端（D65）
    //   - cam2xyzTungsten    → 钨丝灯端（A）
    //
    // 插值依据是 RAW 元数据中的 as-shot 色温，而不是用户的白平衡设置。
    // 原因是 CCM 位于源阶段缓存内：若随白平衡变化，拖动色温滑块会导致
    // 去拜耳等昂贵步骤反复重算。
    bool dualIlluminant = false;

    /// 钨丝灯端（A）的 CamRGB → XYZ 矩阵
    Matrix3x3 cam2xyzTungsten = identityMatrix3x3();

    /// 钨丝灯端标定色温（K），标准 A 光源为 2856K
    float tungstenTemperature = 2856.0f;

    /// 日光端标定色温（K），标准 D65 为 6504K
    float daylightTemperature = 6504.0f;

    /// 按 as-shot 色温解析出实际使用的矩阵。
    ///
    /// 色温的倒数是 mired，摄影上按 mired 线性插值比按开尔文线性插值
    /// 更接近感知均匀。拍摄色温缺失（<= 0）或两端温度非法时退回 cam2xyz。
    Matrix3x3 resolvedCam2xyz(float asShotTemperature) const
    {
        if (!dualIlluminant || asShotTemperature <= 0.0f
            || tungstenTemperature <= 0.0f || daylightTemperature <= 0.0f
            || std::fabs(tungstenTemperature - daylightTemperature) < 1e-3f) {
            return cam2xyz;
        }

        const float miredTungsten = 1.0e6f / tungstenTemperature;
        const float miredDaylight = 1.0e6f / daylightTemperature;
        const float miredShot = 1.0e6f / asShotTemperature;
        const float span = miredDaylight - miredTungsten;
        if (std::fabs(span) < 1e-3f) {
            return cam2xyz;
        }

        // t = 0 → 钨丝灯端，t = 1 → 日光端；超出标定范围时钳制在端点，
        // 不对外插（外插会让矩阵迅速失真）。
        const float t = std::clamp((miredShot - miredTungsten) / span, 0.0f, 1.0f);

        Matrix3x3 result = cam2xyzTungsten;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 3; ++col) {
                result[row][col] = cam2xyzTungsten[row][col]
                    + t * (cam2xyz[row][col] - cam2xyzTungsten[row][col]);
            }
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
//  DCP 相机配置文件（D1）
//
//  DCP 不只是一组 ColorMatrix：完整链路还包含 ForwardMatrix、HueSatMap、
//  ProfileToneCurve 与 LookTable。这里保存配置路径与各阶段的开关，
//  解析结果由管线按路径加载并缓存（不写入工程，避免工程文件膨胀）。
//
//  管线归属：全部在 RAW 底层标定层，属于源阶段；与上层创意调色无关。
// ---------------------------------------------------------------------------
struct DcpSettings {
    /// 是否使用 DCP 配置
    bool enable = false;

    /// .dcp 文件路径（工程保存该路径，重开时重新解析）
    std::string profilePath;

    /// 配置名称，仅用于界面显示与日志
    std::string profileName;

    /// 使用 ForwardMatrix 替代 ColorMatrix 作为相机 → XYZ 转换。
    /// ForwardMatrix 按 DNG 规范定义在**白平衡之后**，管线会用相机记录的
    /// as-shot 中性值先做相机域白平衡，再应用该矩阵。
    bool useForwardMatrix = true;

    /// 应用 ProfileHueSatMap（线性 ProPhoto 域的 HSV 三线性查找）
    bool useHueSatMap = true;

    /// 应用 ProfileToneCurve
    bool useToneCurve = true;

    /// 应用 ProfileLookTable
    bool useLookTable = true;

    bool isActive() const { return enable && !profilePath.empty(); }

    /// 是否有任何色彩塑形阶段需要执行
    bool hasLookStages() const
    {
        return useHueSatMap || useToneCurve || useLookTable;
    }
};

// ---------------------------------------------------------------------------
//  镜头校正参数（需求 §3 模块 F2 第 4 组）
// ---------------------------------------------------------------------------

struct LensCorrectionSettings {
    /// lensfun 自动校正总开关。找不到匹配镜头数据时会被自动关闭（需求 §6）
    bool enable = false;

    /// 三个分项的独立开关
    bool enableDistortion = true;
    bool enableVignetting = true;
    bool enableChromaticAberration = true;

    /// 手动覆盖系数，1.0 表示完全采用 lensfun 计算结果。
    /// 用户可在自动校正基础上做强度增减。
    float distortionScale = 1.0f;
    float vignettingScale = 1.0f;
    float chromaticAberrationScale = 1.0f;

    // ---- 手动镜头选择（D7）----
    //
    // EXIF 里的镜头型号经常是厂商内部名称（例如 "24-70mm F2.8 DG DN"），
    // lensfun 的模型库未必能精确匹配。此时允许用户手动指定数据库中的镜头；
    // 手动选择优先于 EXIF 自动匹配，且可以选择是否用 EXIF 的焦距 / 光圈
    // 覆盖镜头本身的记录（转接环、增距镜常需要手动修正）。
    bool useManualLens = false;
    std::string manualLensName;      ///< lensfun 数据库中的镜头名
    float manualFocalLength = 0.0f;  ///< 0 表示沿用 EXIF 焦距
    float manualAperture = 0.0f;     ///< 0 表示沿用 EXIF 光圈
};

// ---------------------------------------------------------------------------
//  基准白平衡（RAW 底层标定层）
//
//  作用：建立**相机中性色彩基准**，与 CCM、黑电平一起组成 .camcal
//  相机标定配置。它描述"这台相机在什么白点下应该是中性"，与创作意图无关。
//
//  来源：
//   - DNG AsShotNeutral / 厂商 as-shot（fromAsShot=true）；
//   - 灰卡 / 手动标定（fromAsShot=false）。
//
//  推荐在标定完成后锁定（locked=true），避免上层调色误改基准。
// ---------------------------------------------------------------------------
struct BaseWhiteBalanceSettings {
    /// 是否应用基准白平衡校正。普通位图不使用（像素已完成白平衡）。
    bool enable = false;

    /// 标定锁定：锁定后 UI 不允许修改温度 / 色调。
    bool locked = false;

    /// 基准白点色温（开尔文）
    float temperature = 5500.0f;

    /// 基准白点色调 -100 ~ 100
    float tint = 0.0f;

    /// 来源是否为相机 as-shot（true）或手动 / 灰卡标定（false）
    bool fromAsShot = true;

    /// 是否使用灰卡拾取结果作为基准。
    /// 置 true 时色温 / 色调滑块不参与运算，管线直接对采样点做白点归一化。
    bool useGrayPick = false;
    float grayPickX = -1.0f;
    float grayPickY = -1.0f;

    /// 工程 / .camcal 是否显式提供过该字段。
    /// 仅用于载入旧工程时的迁移，不写入文件。
    bool presentInProject = false;
};

// ---------------------------------------------------------------------------
//  创意白平衡偏移（上层调色层）
//
//  在基准白点之上叠加的二次色平衡缩放。参数是**偏移量**，不是绝对白点：
//    目标白点 = 基准白点 + 偏移
//  因此：
//   - 偏移为 0 时，画面停在相机标定的中性基准；
//   - 移动偏移不会改写基准白平衡，也不会写入 .camcal；
//   - 参数随调色预设 / 工程保存，可跨图复制。
// ---------------------------------------------------------------------------
struct WhiteBalanceSettings {
    /// 色温偏移（开尔文），-4000 ~ +4000
    float temperatureOffset = 0.0f;

    /// 色调偏移，-100 ~ 100，正值偏品红、负值偏绿
    float tintOffset = 0.0f;

    /// 灰点拾取坐标（归一化 0~1）。x < 0 表示未拾取
    float grayPickX = -1.0f;
    float grayPickY = -1.0f;

    /// 是否使用灰点拾取结果（相对基准的创意偏移）。
    /// 置 true 时以采样点为准，偏移滑块暂不生效。
    bool useGrayPick = false;

    // ---- 旧工程迁移（运行期状态，不写入新文件）----
    /// 旧版工程把 white_balance.temperature/tint 存成绝对白点。
    /// 载入后由主窗口结合 as-shot / 基准白平衡换算成偏移。
    bool legacyAbsolute = false;
    float legacyTemperature = 5500.0f;
    float legacyTint = 0.0f;

    /// 是否没有创意偏移（灰点拾取也算有效调整）
    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  光影调整（需求 §3 模块 D light）
// ---------------------------------------------------------------------------

struct LightSettings {
    /// 曝光补偿，单位 EV。-5 ~ +5
    float exposure = 0.0f;

    /// 高光压缩，-100 ~ 100。负值压暗高光、正值提亮高光
    float highlight = 0.0f;

    /// 阴影提升，-100 ~ 100。正值提亮暗部
    float shadow = 0.0f;

    /// 对比度，-100 ~ 100
    float contrast = 0.0f;

    /// 去雾，-100 ~ 100。正值增加通透度、负值模拟雾感
    float dehaze = 0.0f;
};

// ---------------------------------------------------------------------------
//  色彩与质感（全局 Presence 调整，A1）
//
//  这四项是专业修图软件「基本面板」里的常用控制：
//    - 饱和度：整体提高 / 降低所有颜色的鲜艳程度；
//    - 自然饱和度：优先提升低饱和颜色，并保护肤色，避免整体过艳；
//    - 清晰度：中大半径的局部对比，增强中间调的立体感；
//    - 纹理：小半径的细节对比，强化纹理而较少影响轮廓。
//
//  饱和度 / 自然饱和度在感知域计算；清晰度 / 纹理只作用于亮度结构，
//  不改变色相，半径按渲染尺寸自适应（见 ops::applyPresence）。
// ---------------------------------------------------------------------------
struct PresenceSettings {
    /// 自然饱和度 -100 ~ 100
    float vibrance = 0.0f;

    /// 整体饱和度 -100 ~ 100
    float saturation = 0.0f;

    /// 清晰度 -100 ~ 100
    float clarity = 0.0f;

    /// 纹理 -100 ~ 100
    float texture = 0.0f;

    /// 四项是否全部为零（管线可据此跳过整个阶段）
    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  HSL 调色（需求 §3 模块 D hsl）
// ---------------------------------------------------------------------------

/// HSL 的 7 个色相通道（红橙黄绿青蓝紫）
enum class HslBand : std::uint8_t {
    Red = 0,
    Orange,
    Yellow,
    Green,
    Cyan,
    Blue,
    Purple,
    Count,  ///< 哨兵，表示通道数量
};

constexpr int kHslBandCount = static_cast<int>(HslBand::Count);

/// 七个 HSL 通道的色相中心角（度），顺序与 HslBand 枚举一致。
/// HSL 调色与目标调整工具共用这一张表，保证"点在哪个颜色上"与
/// "滑块调整哪个通道"完全对应。
inline constexpr float kHslBandHueCenters[static_cast<std::size_t>(HslBand::Count)] = {
    0.0f,    // 红
    30.0f,   // 橙
    60.0f,   // 黄
    120.0f,  // 绿
    180.0f,  // 青
    240.0f,  // 蓝
    285.0f,  // 紫
};

const char* hslBandName(HslBand band);

/// 返回色相最接近的 HSL 通道（0~360 度，超出范围自动回绕）。
/// 目标调整工具用它把画布上点中的颜色映射到对应的 HSL 滑块组。
HslBand hslBandForHue(float hue);

/// 单个色相通道的三项偏移，取值均为 -100 ~ 100
struct HslBandAdjust {
    float hue = 0.0f;        ///< 色相偏移
    float saturation = 0.0f; ///< 饱和度偏移
    float luminance = 0.0f;  ///< 明亮度偏移
};

struct HslSettings {
    /// 7 个通道的调整量，索引对应 HslBand
    std::array<HslBandAdjust, static_cast<std::size_t>(HslBand::Count)> bands{};

    /// 判断是否全部为零（管线可据此跳过整个 HSL 环节以节省时间）
    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  黑白混色（A3）
//
//  先把画面转为黑白，再按**原色相**调整该颜色在黑白中的明度：
//  例如把「红」提亮可以让红花在黑白里更突出，把「蓝」压暗可以让天空更深。
//  索引与 HSL 七通道一致（红橙黄绿青蓝紫）。
// ---------------------------------------------------------------------------
struct BlackWhiteSettings {
    /// 是否转换为黑白
    bool enabled = false;

    /// 各色相通道的明度偏移 -100 ~ 100，索引对应 HslBand
    std::array<float, static_cast<std::size_t>(kHslBandCount)> luminance{};

    /// 未启用或全部通道为零时，不改变画面（启用且全零仍会转黑白）
    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  颜色分级（A2）
//
//  三段式调色：分别给阴影、中间调、高光叠加一个带亮度保持的色偏，
//  再用「平衡」移动三段的分界、用「混合」控制三段的过渡宽度。
//  与 HSL 的区别：HSL 按**色相**选色，颜色分级按**明暗区域**选色，
//  因此可以在不改变物体固有色相的前提下给画面建立冷暖分离。
// ---------------------------------------------------------------------------
struct ColorGradeWheel {
    /// 色相 0~360（0 = 红，120 = 绿，240 = 蓝）
    float hue = 0.0f;

    /// 着色强度 0~100
    float saturation = 0.0f;

    /// 该区域的明度偏移 -100~100
    float luminance = 0.0f;

    /// 是否真正参与运算
    bool isActive() const;
};

struct ColorGradingSettings {
    ColorGradeWheel shadows;
    ColorGradeWheel midtones;
    ColorGradeWheel highlights;

    /// 三段分界的平衡 -100~100。正值更偏重高光，负值更偏重阴影。
    float balance = 0.0f;

    /// 三段之间的过渡宽度 0~100。越大过渡越柔和、重叠越多。
    float blending = 50.0f;

    /// 仅当三段都没有着色 / 明度调整时才算恒等（平衡与混合单独不产生效果）
    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  分离色调（A2）
//
//  给高光与阴影分别染上不同色相，是黑白与低饱和画面的常用手法。
//  与颜色分级可以叠加使用：颜色分级负责冷暖分离的整体方向，
//  分离色调负责高光 / 阴影各自的最后一点染色。
// ---------------------------------------------------------------------------
struct SplitToningSettings {
    float highlightsHue = 0.0f;        ///< 高光色相 0~360
    float highlightsSaturation = 0.0f; ///< 高光染色强度 0~100
    float shadowsHue = 0.0f;           ///< 阴影色相 0~360
    float shadowsSaturation = 0.0f;    ///< 阴影染色强度 0~100

    /// 高光 / 阴影的平衡 -100~100，正值偏向高光
    float balance = 0.0f;

    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  曲线（需求 §3 模块 D curve）
// ---------------------------------------------------------------------------

/// 曲线控制点。x/y 均为归一化 0~1，曲线在**线性光域**上作用。
struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
};

/// 单条曲线的数据。控制点按 x 升序保存，首尾通常固定为 (0,0) 与 (1,1)。
struct CurveData {
    std::vector<CurvePoint> points;

    /// 生成一条默认的恒等曲线（仅两个端点）
    static CurveData makeIdentity();

    /// 是否为恒等曲线（无实质调整）
    bool isIdentity() const;
};

/// 参数曲线：按阴影 / 中间调 / 高光三个区域调整亮度。
///
/// 与点曲线的区别：点曲线是任意形状的精确控制，参数曲线是三个区域滑块，
/// 更适合快速建立整体影调。两者在管线中叠加，参数曲线先于点曲线。
struct ParametricCurveSettings {
    float shadows = 0.0f;     ///< 阴影区域 -100 ~ 100
    float midtones = 0.0f;    ///< 中间调区域 -100 ~ 100
    float highlights = 0.0f;  ///< 高光区域 -100 ~ 100

    /// 三个区域的过渡宽度（感知域 0.05 ~ 0.50）。
    /// 越大影响范围越宽、过渡越柔和。
    float shadowRange = 0.18f;
    float midtoneRange = 0.18f;
    float highlightRange = 0.18f;

    bool isIdentity() const;
};

/// R/G/B/L 四条曲线。L 为明度曲线，在 R/G/B 之后应用
struct CurveSettings {
    CurveData red;
    CurveData green;
    CurveData blue;
    CurveData luminance;

    /// 参数曲线（阴影 / 中间调 / 高光），先于点曲线应用
    ParametricCurveSettings parametric;

    /// 构造时初始化四条恒等曲线
    CurveSettings();

    bool isIdentity() const;
};

// ---------------------------------------------------------------------------
//  3D LUT（需求 §2 管线第十二项）
// ---------------------------------------------------------------------------

/// .cube 3D LUT 参数
struct LutSettings {
    bool enable = false;      ///< 是否启用 LUT 叠加
    std::string path;         ///< .cube 文件路径
    float opacity = 1.0f;     ///< 混合强度 0~1
};

// ---------------------------------------------------------------------------
//  细节处理（需求 §3 模块 D detail）
// ---------------------------------------------------------------------------

/// 降噪算法（D5）
enum class NoiseReductionMethod : std::uint8_t {
    Wavelet = 0,   ///< à trous 小波软阈值：速度快，适合交互预览
    NLMeans = 1,   ///< 非局部均值：纹理与边缘保持更好，适合最终输出
    Bm3d = 2,      ///< 块匹配 + 2D DCT 收缩（BM3D 单阶段近似），质量最高、最慢
};

const char* noiseReductionMethodName(NoiseReductionMethod method);
NoiseReductionMethod noiseReductionMethodFromString(const std::string& text);

/// 锐化算法（D6）
enum class SharpenMethod : std::uint8_t {
    UnsharpMask = 0,   ///< 双尺度 USM：直接、稳定，适合绝大多数场景
    Deconvolution = 1, ///< Richardson-Lucy 反卷积：恢复镜头 / 去拜耳造成的模糊
};

const char* sharpenMethodName(SharpenMethod method);
SharpenMethod sharpenMethodFromString(const std::string& text);

struct DetailSettings {
    /// 明度降噪强度 0 ~ 100
    float noiseLuma = 0.0f;

    /// 色度降噪强度 0 ~ 100。
    /// 默认给一个很低的强度：去拜耳后的彩色噪点在暗部比较明显，
    /// 轻微抑制即可让初始画面干净，同时不会抹掉真实色彩细节。
    float noiseChroma = 15.0f;

    /// 降噪算法（D5）。默认小波，交互更快；
    /// 最终输出可切换为非局部均值以获得更好的纹理保持。
    NoiseReductionMethod noiseMethod = NoiseReductionMethod::Wavelet;

    /// 非局部均值的搜索窗口半径（像素，3 ~ 7）。
    /// 越大越能利用远处相似块，但耗时按半径平方增长。
    int nlMeansSearchRadius = 4;

    /// USM 锐化强度 0 ~ 100。
    /// 默认 30：去拜耳与 FBDD 会损失部分高频，1:1 查看时需要适度补回；
    /// 半径保持 0.8 px，不会产生明显光晕或过锐的塑料感。
    float sharpenAmount = 30.0f;

    /// USM 模糊半径，按高斯 σ 解释（单位 px，0.5 ~ 5.0）。
    /// 小半径只恢复细节，不产生明显光晕；0.8 是 1:1 查看的常用起点。
    float sharpenRadius = 0.8f;

    /// 锐化边缘掩码阈值 0 ~ 100。
    /// 越高越只在强边缘处锐化，避免噪点被放大（需求 §2 明确要求带边缘掩码）。
    /// 默认 25：略放宽参与锐化的边缘范围，让纹理细节也能得到补偿。
    float sharpenMasking = 25.0f;

    /// 锐化算法（D6）。默认 USM；反卷积用于补偿镜头 / 低通滤镜的模糊。
    SharpenMethod sharpenMethod = SharpenMethod::UnsharpMask;

    /// 反卷积迭代次数 3 ~ 30。次数越多恢复越强，也越容易放大噪点。
    int deconvolutionIterations = 8;

    // ---- 输出锐化（D6）----
    //
    // 与捕获锐化（上面的细节参数）不同，输出锐化只在导出时按**输出像素**
    // 追加一次小半径锐化，用来抵消缩放重采样与 JPEG / WebP 压缩造成的软化。
    /// 导出时追加输出锐化
    bool outputSharpenEnable = false;
    /// 输出锐化强度 0 ~ 100
    float outputSharpenAmount = 30.0f;
    /// 输出锐化半径（输出像素）0.3 ~ 2.0
    float outputSharpenRadius = 0.6f;
};

// ---------------------------------------------------------------------------
//  Demosaic 空间边缘 / 伪色策略（diy.md §5）
//
//  去拜耳后的色度清理按局部边缘强度与用户笔刷分区：
//   - 平滑区：更强的色差中值，压制伪色；
//   - 边缘区 / 保护区：减弱清理，保留细节；
//   - 强压制笔刷：即使在高频边缘也加强色差中值。
// ---------------------------------------------------------------------------
struct DemosaicSpatialSettings {
    bool enable = false;

    /// 边缘判定阈值 0~100，越高越少像素被视为边缘
    float edgeThreshold = 50.0f;

    /// 边缘保护强度 0~1
    float protectStrength = 0.5f;

    /// 强伪色压制强度 0~1
    float suppressStrength = 0.5f;

    /// 空间笔刷（归一化坐标，半径相对短边）
    std::vector<MaskStrokePoint> protectStrokes;
    std::vector<MaskStrokePoint> suppressStrokes;

    bool isIdentity() const
    {
        return !enable
            || (protectStrokes.empty() && suppressStrokes.empty()
                && edgeThreshold <= 1e-3f && suppressStrength <= 1e-3f);
    }
};

// ---------------------------------------------------------------------------
//  暗电流补偿（需求 v2 §2.2）
//
//  高 ISO / 长曝光时传感器的暗底电平会抬升，并在暗部表现为彩色底噪。
//  这里按 ISO 分段给出归一化偏移量，在坏点修复之后、串扰补偿之前减去它。
//  偏移表随机型配置可覆盖，强度滑块用于在 0 ~ 100% 之间微调。
// ---------------------------------------------------------------------------
struct DarkCurrentSettings {
    /// 是否启用。默认关闭：未标定的机型强行扣除会压掉真实暗部细节。
    bool enable = false;

    /// 补偿强度 0 ~ 1。最终偏移 = ISO 分段基准值 × strength。
    float strength = 0.5f;

    /// 机型配置给出的 ISO→偏移表（归一化 0~1 单位）。
    /// 为空时使用内置通用曲线。表项按 ISO 升序排列。
    std::vector<std::pair<float, float>> isoOffsets;

    /// 按 ISO 查基准偏移；表为空时使用内置通用分段曲线。
    float offsetForIso(float iso) const;
};

// ---------------------------------------------------------------------------
//  暗场帧 / 平场帧校正（需求 append_v3 §五 1、2）
//
//  两种帧都以独立的 RAW 文件提供，解码后与主图在同一归一化基准上做
//  逐像素运算：
//    - 暗场帧：减去暗噪声（热像素底噪、放大器偏移），夹取到 >= 0；
//    - 平场帧：除以归一化响应，补偿镜头渐晕与像素响应不均。
//  帧路径只记录在工程文件里，不嵌入；缺失时管线跳过该步并给出警告。
// ---------------------------------------------------------------------------
struct FrameCalibrationSettings {
    /// 暗场帧开关
    bool enableDarkFrame = false;
    /// 暗场帧 RAW 路径
    std::string darkFramePath;
    /// 暗场扣除强度 0 ~ 1
    float darkFrameStrength = 1.0f;

    /// 平场帧开关
    bool enableFlatFrame = false;
    /// 平场帧 RAW 路径
    std::string flatFramePath;
    /// 平场校正强度 0 ~ 1
    float flatFrameStrength = 1.0f;
};

// ---------------------------------------------------------------------------
//  彩边 / 色散校正参数（requirements/requirement_append_falsecolor.md）
//
//  全部模块作用于**线性传感器 RGB 域**，位于白平衡与 CCM 之前，
//  与相机色彩标定完全解耦。
//
//  参数优先级（高 → 低）：
//    用户实时参数 > 镜头配置 > 机型默认配置 > 全局默认
//  userOverrideMask / lensOverrideMask 记录参数来源：
//    - 用户改动某个模块时置 user 位；
//    - 加载镜头配置时置 lens 位；
//    - 加载机型校准档案时只填充两个掩码都没有置位的模块。
//  这样低优先级来源不会覆盖用户或镜头显式设定的值。
// ---------------------------------------------------------------------------

/// 可独立开关的彩边校正模块。枚举值同时用作掩码位序号。
enum class CorrectionModule : std::uint8_t {
    Crosstalk = 0,        ///< CMOS 串扰补偿
    HighlightRolloff,     ///< 高光软饱和 Roll-off
    Lca,                  ///< 横向色差
    Loca,                 ///< 轴向色散
    EdgeFalseColor,       ///< 边缘导向伪色抑制
    Count,
};

const char* correctionModuleName(CorrectionModule module);

/// CMOS 串扰补偿：3×3 解耦矩阵
struct CrosstalkSettings {
    bool enable = true;

    /// 补偿强度 0~1。0 表示完全不做补偿；
    /// 实际使用的矩阵 = 单位矩阵 + 强度 ×（解耦矩阵 − 单位矩阵）。
    float strength = 0.5f;

    /// 机型标定文件给出的解耦矩阵。默认单位矩阵（无补偿）。
    Matrix3x3 decouplingMatrix = identityMatrix3x3();
};

/// 高光软饱和压缩：避免 RGB 通道不同步饱和产生彩边
struct HighlightRolloffSettings {
    bool enable = true;

    /// 高光拐点（线性 0~1）。低于该值的像素完全不变。
    float knee = 0.9f;

    /// 压缩坡度 0~1。越大高光压缩越强、过渡越陡。
    float slope = 0.5f;
};

/// 横向色差（LCA）校正：以 G 为基准缩放 R / B
struct LcaSettings {
    bool enable = true;

    /// R 通道缩放系数。1.0 表示不校正；>1 放大 R，<1 收缩 R。
    float redScale = 1.0f;

    /// B 通道缩放系数，含义同 redScale。
    float blueScale = 1.0f;

    /// 径向增强强度，范围 -0.008 ~ +0.008（需求 v2 §1.4）。
    /// 校正量在基础缩放之上再乘以 (1 + radialStrength × r²)，
    /// r² 为到画面中心距离的归一化平方：中心为 0、四角约为 1。
    /// 正值增强四角校正，负值在四角反向缩放。
    float radialStrength = 0.0f;
};

/// 轴向色散（LoCA）校正：基于 DoG 高光光斑掩码做色散衰减
struct LocaSettings {
    bool enable = true;

    /// 校正强度 0~1，默认 0.4。
    float strength = 0.4f;

    /// 高光光斑阈值（线性 0~1）。只有亮于此值的区域才参与校正。
    float highlightThreshold = 0.75f;
};

/// 边缘导向伪色抑制：G 通道梯度生成掩码，只平滑 R / B
struct EdgeFalseColorSettings {
    bool enable = true;

    /// 抑制强度 0~1，默认 0.5。
    float strength = 0.5f;

    /// 边缘敏感度 0~1。越高越多边缘参与，越低只处理最强边缘。
    float edgeSensitivity = 0.5f;

    /// 中值平滑半径（像素，1~3，需求 v2 §1.5）。
    /// 实际半径会按渲染分辨率自适应缩放：预览用小半径、导出用大半径，
    /// 保证在归一化尺寸上的一致性。默认 1 即 3×3 邻域。
    int smoothingRadius = 1;
};

struct FalseColorSettings {
    CrosstalkSettings crosstalk;
    HighlightRolloffSettings highlightRolloff;
    LcaSettings lca;
    LocaSettings loca;
    EdgeFalseColorSettings edgeFalseColor;

    /// 用户手动修改过的模块位掩码（bit = CorrectionModule）
    std::uint8_t userOverrideMask = 0;

    /// 由镜头配置文件提供参数的模块位掩码（只有 LCA 会用到）
    std::uint8_t lensOverrideMask = 0;

    bool isUserOverride(CorrectionModule module) const;
    bool isLensOverride(CorrectionModule module) const;
    void markUserOverride(CorrectionModule module);
    void markLensOverride(CorrectionModule module);

    /// 用机型校准档案的推荐值填充尚未被用户/镜头设定的模块。
    void applyCameraDefaults(const FalseColorSettings& cameraDefaults);

    /// 用镜头配置覆盖 LCA。若 LCA 已被用户手动修改则不覆盖，
    /// @return true 表示已应用
    bool applyLensProfile(const LcaSettings& lensLca);

    /// 用镜头配置覆盖 LoCA（需求 v2 §2.9），规则同 LCA。
    /// @return true 表示已应用
    bool applyLensProfileLoca(const LocaSettings& lensLoca);

    /// 清除全部来源标记（恢复全局默认时使用）
    void clearOverrides();
};

// ---------------------------------------------------------------------------
//  软打样（预览显示）
// ---------------------------------------------------------------------------

/// 软打样设置：在屏幕上模拟打印/纸张的最终效果。
///
/// 只作用于预览显示，不会改变导出像素。profilePath 是打样目标
/// （打印机 / 纸张）的 ICC 文件路径；路径失效时预览自动退回普通模式。
struct SoftProofSettings {
    /// 总开关
    bool enabled = false;

    /// 打样目标 ICC 文件路径（绝对路径优先，便于工程重开后复用）
    std::string profilePath;

    /// 模拟纸白：使用绝对比色意图，保留纸张白的偏色与亮度
    bool simulatePaperWhite = false;

    /// 是否有可用的打样配置
    bool isConfigured() const { return enabled && !profilePath.empty(); }
};

// ---------------------------------------------------------------------------
//  非破坏性裁剪（C1）
//
//  裁剪矩形使用归一化坐标（0~1），与原始图像尺寸无关：
//   - 预览降采样、1:1 与全尺寸导出使用同一组坐标；
//   - 裁剪只改变输出范围，不修改 RAW 数据，也不影响蒙版的归一化坐标；
//   - 旋转 / 拉直（C2）后续在同一结构中扩展。
// ---------------------------------------------------------------------------
struct CropSettings {
    bool enable = false;

    float x = 0.0f;      ///< 左边界 0~1
    float y = 0.0f;      ///< 上边界 0~1
    float width = 1.0f;  ///< 宽度 0~1
    float height = 1.0f; ///< 高度 0~1

    /// 拉直角度（度，顺时针为正），-45 ~ 45。
    /// 非零时管线会在裁剪区域中心旋转，并自动收窄到不出现黑边的最大矩形。
    float angle = 0.0f;

    /// 是否覆盖整幅图（完全未裁剪、未旋转）
    bool isFullFrame() const;

    /// 是否产生实际裁剪 / 拉直
    bool isActive() const
    {
        return enable && (!isFullFrame() || std::fabs(angle) > 1e-4f);
    }

    /// 把裁剪输出图的归一化坐标映射回裁剪前源图的归一化坐标。
    ///
    /// 灰卡拾取、画笔等工具作用在裁剪之前，而画布显示的是裁剪后的图；
    /// 点击坐标必须先换算回源图，否则裁剪 / 拉直后取样会错位。
    /// 角度为 0 时是简单的矩形映射；有拉直角度时使用与 applyCrop
    /// 相同的自动内接矩形与逆旋转公式。
    void mapOutputToSource(float u, float v,
                           int sourceWidth, int sourceHeight,
                           float& outU, float& outV) const;

    /// 裁剪 / 拉直输出短边相对源图短边的比例。
    /// 画笔半径以“当前显示图短边”存储，写入源图坐标前要乘该比例。
    float outputShortEdgeRatio(int sourceWidth, int sourceHeight) const;
};

// ---------------------------------------------------------------------------
//  透视 / 几何校正（C3）
//
//  只做梯形校正与缩放；旋转 / 拉直由 CropSettings.angle 负责。
//  管线位置固定在镜头校正之后、白平衡与局部调整之前：
//   - 后续工具（蒙版、灰卡、裁剪）都作用在几何校正后的图像上；
//   - 输出尺寸不变，自动缩放四角采样区域以避免黑边。
// ---------------------------------------------------------------------------
struct PerspectiveSettings {
    bool enable = false;

    /// 垂直透视 -100 ~ 100。正值用来校正“上窄下宽”的汇聚竖线。
    float vertical = 0.0f;

    /// 水平透视 -100 ~ 100。正值用来校正“左窄右宽”的汇聚横线。
    float horizontal = 0.0f;

    /// 缩放 0 ~ 100。正值放大画面（采样区域缩小），用于裁掉校正后的边缘。
    float scale = 0.0f;

    bool isIdentity() const
    {
        return !enable
            || (std::fabs(vertical) <= 1e-4f
                && std::fabs(horizontal) <= 1e-4f
                && std::fabs(scale) <= 1e-4f);
    }
};

// ---------------------------------------------------------------------------
//  污点修复 / 仿制（C4）
//
//  每个修复点是一对归一化坐标：
//   - target：要修掉的位置；
//   - source：取样位置；
//   - radius 相对图像短边；
//   - Heal 会把源纹理的颜色均值对齐到目标区域，Clone 直接复制源像素。
//  修复发生在局部调整之后、裁剪之前，因此坐标相对透视校正后的图像。
// ---------------------------------------------------------------------------
enum class RetouchMode : std::uint8_t {
    Heal = 0,   ///< 修复：复制纹理并匹配目标颜色
    Clone = 1,  ///< 仿制：直接复制源像素
};

const char* retouchModeName(RetouchMode mode);
RetouchMode retouchModeFromName(const std::string& name);

struct RetouchSpot {
    float targetX = 0.5f;
    float targetY = 0.5f;
    float sourceX = 0.45f;
    float sourceY = 0.5f;

    /// 目标半径（相对图像短边）0.001 ~ 0.5
    float radius = 0.03f;

    /// 边缘羽化 0 ~ 1（相对半径）
    float feather = 0.5f;

    /// 强度 0 ~ 1
    float opacity = 1.0f;

    RetouchMode mode = RetouchMode::Heal;
};

struct RetouchSettings {
    std::vector<RetouchSpot> spots;

    bool isIdentity() const { return spots.empty(); }
};

// ---------------------------------------------------------------------------
//  输出设置（用于导出，需求 §3 模块 G）
// ---------------------------------------------------------------------------

/// 导出文件格式
enum class ExportFormat : std::uint8_t {
    Tiff16,  ///< 16 位 TIFF
    Jpeg8,   ///< 8 位 JPEG
    WebP,    ///< WebP
    Exr16,   ///< 16 位半浮点 OpenEXR（append_v3 §3.8，HDR 场景）
    Png16,   ///< 16 位 PNG（E5）
    JpegXl,  ///< JPEG XL（E5，需要 Qt 插件）
    Avif,    ///< AVIF（E5，需要 Qt 插件）
};

/// 缩放重采样核（E6）
enum class ResampleFilter : std::uint8_t {
    Bilinear = 0,  ///< 双线性：速度最快，默认
    Bicubic = 1,   ///< Catmull-Rom 双三次：较锐利
    Lanczos = 2,   ///< Lanczos-3：高质量缩小，边缘锐利
    Mitchell = 3,  ///< Mitchell-Netravali：兼顾锐度与振铃
};

const char* resampleFilterName(ResampleFilter filter);
ResampleFilter resampleFilterFromString(const std::string& text);

/// 水印设置（E6）
enum class WatermarkPosition : std::uint8_t {
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
};

const char* watermarkPositionName(WatermarkPosition position);
WatermarkPosition watermarkPositionFromString(const std::string& text);

struct WatermarkSettings {
    bool enable = false;

    /// 文字水印（与图片水印二选一；图片优先）
    std::string text;

    /// 图片水印路径（PNG / JPEG，支持透明通道）
    std::string imagePath;

    WatermarkPosition position = WatermarkPosition::BottomRight;

    /// 不透明度 0~100
    float opacity = 60.0f;

    /// 边距占图像短边的比例（0.005 ~ 0.10）
    float margin = 0.02f;

    /// 水印尺寸：文字为字号、图片为宽度，均相对图像短边的比例
    /// （0.005 ~ 0.20）
    float scale = 0.05f;

    /// 文字颜色（sRGB 8 位）
    std::uint8_t colorR = 255;
    std::uint8_t colorG = 255;
    std::uint8_t colorB = 255;

    bool isActive() const { return enable && (!text.empty() || !imagePath.empty()); }
};

/// 导出元数据编辑（E6）
struct MetadataEditSettings {
    std::string artist;
    std::string copyright;
    std::string description;
    std::string software;

    bool isIdentity() const
    {
        return artist.empty() && copyright.empty()
            && description.empty() && software.empty();
    }
};

const char* exportFormatName(ExportFormat format);

struct OutputSettings {
    ExportFormat format = ExportFormat::Tiff16;

    /// 目标色彩空间（sRGB / AdobeRGB）
    ColorSpace targetSpace = ColorSpace::sRGB;

    /// JPEG / WebP 质量 1 ~ 100
    int quality = 90;

    /// 输出尺寸缩放系数（1.0 = 原始尺寸）
    float scale = 1.0f;

    /// 是否在导出文件中嵌入 ICC 配置文件
    bool embedIccProfile = true;

    /// 是否把预览影调（自动曝光归一 + 弱 S 曲线）同步到导出图像。
    ///
    /// 默认 false：预览的自动影调只用于屏幕显示，导出保留未经
    /// 预览影调处理的线性结果。用户开启后，导出会沿用当前预览视图
    /// 的影调模式（观感预览 / 线性基准预览）。
    ///
    /// 该开关与传感器 CCM、白平衡无关，也不会修改任何线性原始 RGB。
    bool syncPreviewTone = false;

    // ---- E6：导出流程增强 ----
    /// 缩小 / 放大时的重采样核
    ResampleFilter resampleFilter = ResampleFilter::Bilinear;

    /// 文件命名模板。支持 {name} {date} {time} {seq} {width} {height} {ext}。
    /// 为空时沿用“源文件名 + _edited”的默认规则。
    std::string namingTemplate;

    WatermarkSettings watermark;
    MetadataEditSettings metadata;
};

// ---------------------------------------------------------------------------
//  管线顺序（diy.md §4）
//
//  只保存阶段键名序列；真正执行前会用 PipelineStages 注册表校验依赖与
//  数据格式。空序列表示默认顺序。
// ---------------------------------------------------------------------------
struct PipelineOrderSettings {
    bool enable = false;
    std::vector<std::string> stages;

    bool isIdentity() const { return !enable || stages.empty(); }
};

// ---------------------------------------------------------------------------
//  总参数模型
// ---------------------------------------------------------------------------

/// 编辑上下文：一次编辑会话的全部参数。
/// 对应一个 .rawedit.json 工程文件。
struct EditContext {
    /// 源 RAW 文件路径（仅作记录，不修改该文件）
    std::string sourceRawPath;

    RawSettings raw;
    PipelineOrderSettings pipelineOrder;  ///< diy.md §4 阶段顺序
    DemosaicSpatialSettings demosaicSpatial;  ///< diy.md §5 边缘 / 伪色空间策略
    DarkCurrentSettings darkCurrent;  ///< 暗电流补偿（坏点修复之后、串扰之前）
    FrameCalibrationSettings frameCalibration;  ///< 暗场帧 / 平场帧（暗电流之后、串扰之前）
    CcmSettings ccm;
    DcpSettings dcp;  ///< DCP 相机配置文件（全链路，RAW 标定层）
    BaseWhiteBalanceSettings baseWhiteBalance;  ///< RAW 标定层：相机中性基准
    LensCorrectionSettings lens;
    FalseColorSettings falseColor;  ///< 彩边 / 色散校正（白平衡之前、CCM 之前）
    WhiteBalanceSettings whiteBalance;  ///< 上层：基准白点之上的创意偏移
    LightSettings light;
    PresenceSettings presence;  ///< 全局色彩与质感（光影之后、HSL 之前）
    HslSettings hsl;
    BlackWhiteSettings blackWhite;      ///< 黑白混色（HSL 之后、颜色分级之前）
    ColorGradingSettings colorGrading;  ///< 三段式颜色分级（黑白之后、曲线之前）
    SplitToningSettings splitToning;    ///< 高光 / 阴影分离色调
    CurveSettings curve;
    LutSettings lut;      ///< 预留
    DetailSettings detail;
    SoftProofSettings softProof;  ///< 屏幕软打样（不影响导出）
    OutputSettings output;

    /// 蒙版与局部调整（B1/B2），已接入渲染管线
    MaskStack masks;

    /// 非破坏性裁剪（C1）
    CropSettings crop;

    /// 透视 / 几何校正（C3），位于工作空间转换之后、白平衡之前
    PerspectiveSettings perspective;

    /// 污点修复 / 仿制（C4），位于局部调整之后、裁剪之前
    RetouchSettings retouch;

    /// 参数模型版本号，用于工程文件向前兼容。
    /// v2：拆分基准白平衡与创意白平衡偏移。
    static constexpr int kSchemaVersion = 2;

    /// 把旧工程 / 旧快照的白平衡迁移到"基准 + 创意偏移"模型。
    ///
    /// - 工程缺少 base_white_balance 时：RAW 用 as-shot 建立基准，
    ///   普通位图关闭基准；
    /// - 工程只保存了绝对白点时：按基准白点换算成创意偏移，
    ///   使旧工程的最终画面保持一致。
    ///
    /// 只应在"从工程 / 快照载入"后调用；新建的 EditContext 不应调用，
    /// 否则会用 as-shot 覆盖用户手动设置的基准。
    void resolveLegacyWhiteBalance(bool isRgbSource,
                                   float asShotTemperature,
                                   float asShotTint);

    /// 重置为默认参数（保留 sourceRawPath）
    void resetToDefaults();

    /// 判断是否为初始默认状态（UI 用于提示"未做任何调整"）
    bool isDefault() const;
};

}  // namespace rawcore
