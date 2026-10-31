// ============================================================================
//  PipelineOps.h —— 管线各阶段的具体图像运算
//
//  设计原则：
//   1. 全部为**无状态自由函数**，输入输出明确，便于 OpenMP 并行与单元测试。
//   2. 每个函数只做一件事，阶段顺序由 PipelineRenderer 编排（需求 §2）。
//   3. 明确标注每个函数作用在**线性域**还是**Gamma 域**。
//      除 Gamma 编码函数外，其余全部在线性域工作（需求 §1.3.1）。
//   4. 依赖 Qt / lcms2 的步骤（如 XYZ→ProPhoto）不在此处，由 PipelineRenderer
//      调用 ColorManagement 完成。
//
//  所有函数均就地修改传入的缓冲，避免大图反复分配（需求 §5.3）。
// ============================================================================

#pragma once

#include "EditContext.h"
#include "Types.h"

#include <array>
#include <functional>
#include <vector>

namespace rawcore {
namespace ops {

// ===========================================================================
//  1. 黑白电平（拜耳域，线性）
// ===========================================================================

/// 白电平饱和裁剪 + 归一化到 0~1。
/// 需求 §2 管线第三步。黑电平扣除已在 RawLoader 中完成。
/// @param bayer      拜耳数据（就地修改）
/// @param whiteLevel 饱和电平（原始刻度），<= 0 时跳过
void applyWhiteLevelClip(BayerImage& bayer, float whiteLevel);

/// 白电平裁剪 + 归一化，支持 R/G1/B/G2 四个通道独立阈值。
/// @param whiteLevels 每通道解码值阈值；0 表示回退到 fallbackWhiteLevel
void applyWhiteLevelClipPerChannel(BayerImage& bayer,
                                   const std::array<float, 4>& whiteLevels,
                                   float fallbackWhiteLevel);

/// 拜耳域的简单坏点修复。
/// 需求 §2 管线第五步：检出与邻域中位数偏差过大的孤立点并用中位数替换。
/// @param threshold 判定阈值（相对于局部均值的倍数），典型值 3.0~6.0
void repairBadPixels(BayerImage& bayer, float threshold = 4.0f);

/// 暗电流补偿（需求 v2 §2.2）。
/// 按 ISO 分段减去传感器暗底偏移，抑制高 ISO / 长曝光暗部的彩色底噪。
/// 偏移量在归一化线性域中扣除，结果夹取到 >= 0，不改变图像尺寸。
/// @param strength 补偿强度 0~1
/// @param offset   归一化基准偏移（由 DarkCurrentSettings::offsetForIso 给出）
void applyDarkCurrentCompensation(BayerImage& bayer,
                                  float strength,
                                  float offset);

// ===========================================================================
//  2. 拜耳域 CCM 说明
//
//  真正的 3x3 线性变换需要在三通道之间混合，而拜耳域每个像素只有单通道值，
//  因此无法直接完成跨通道运算。实现放在 ops::applyBayerLinearMatrix：
//  逐像素用 3x3 同通道邻域平均值估计三通道 → 应用矩阵 → 只写回自身通道，
//  既满足"矩阵作用于 Demosaic 之前"的管线语义，又完成了真实的线性变换，
//  且不需要分配整幅三通道临时图像。
//  （需求 §1.3.3：CCM 是线性变换，不是 3D LUT。）
// ===========================================================================

// ===========================================================================
//  3. Demosaic（拜耳域 → 线性 RGB）
// ===========================================================================

/// 去拜耳：把单通道拜耳数据插值为三通道线性 RGB。
/// 需求 §2 管线第六步。
/// @param bayer   输入拜耳数据
/// @param output  输出三通道图像（线性，未归一化的原始刻度）
/// @param smooth  摩尔纹抑制强度 0~1
ErrorInfo demosaic(const BayerImage& bayer,
                   ImageBuffer& output,
                   DemosaicAlgorithm algorithm,
                   float smooth);

// ===========================================================================
//  3.5 彩边 / 色散校正（线性传感器域，白平衡与 CCM 之前）
//
//  对应 requirements/requirement_append_falsecolor.md。顺序固定为：
//    串扰补偿 → Demosaic（含 FBDD） → 高光 Roll-off
//    → LCA → LoCA → 边缘导向伪色抑制
//  所有函数都就地修改数据，且不改变图像尺寸。
// ===========================================================================

/// 拜耳域 3×3 线性矩阵变换。
///
/// 拜耳数据每个像素只有单通道值，无法直接做跨通道混合。这里逐像素用
/// 3x3 同通道邻域平均估计三通道，应用矩阵后只写回该像素自身的 CFA 通道，
/// 与拜耳域自定义 CCM 使用同一套语义（真正作用于去拜耳之前）。
/// 只需一路拜耳大小的临时源数据，适合高像素机型的预览渲染。
ErrorInfo applyBayerLinearMatrix(BayerImage& bayer, const Matrix3x3& matrix);

/// CMOS 串扰补偿。
/// 实际矩阵 = 单位矩阵 + strength ×（解耦矩阵 − 单位矩阵）。
/// 输出夹取到 [0,1]，避免解耦产生非物理的负响应污染后续处理。
void applyCrosstalkCompensation(BayerImage& bayer,
                                const Matrix3x3& decouplingMatrix,
                                float strength);

/// FBDD 四方向拜耳域降噪（伪色抑制）。
///
/// 对每个采样点取上下左右距离 2 的同色邻点，用中值作为局部期望；
/// 只有偏差显著超过局部波动时才向中值靠拢，因此平坦区域与真实纹理
/// 基本不受影响，而孤立的异常采样（插值伪色的主要来源）会被压低。
/// 必须在去拜耳之前执行。
void applyFbddDenoise(BayerImage& bayer, float strength);

/// 高光软饱和压缩（线性域逐通道）。
///
/// 低于拐点的值完全不变；高于拐点的值按指数曲线平滑逼近 1.0，
/// 让三通道以相近的斜率进入饱和区，减少通道不同步截断造成的彩边。
/// @param knee  高光拐点（0~1）
/// @param slope 压缩坡度（0~1），越大压缩越强
void applyHighlightRolloff(ImageBuffer& image, float knee, float slope);

/// 横向色差（LCA）校正：以 G 为基准对 R / B 做亚像素径向缩放。
///
/// 缩放是相对量（以图像中心为原点），因此预览与全尺寸渲染结果一致，
/// 无需按分辨率换算半径；radialStrength 让校正量随半径平方增长，
/// 使画面四角的色差得到更强补偿。
/// @param redScale       R 通道缩放系数（1.0 = 不校正）
/// @param blueScale      B 通道缩放系数（1.0 = 不校正）
/// @param radialStrength 径向增强 -0.008 ~ +0.008（四角校正量的额外增益）
void applyLateralChromaticAberration(ImageBuffer& image,
                                     float redScale,
                                     float blueScale,
                                     float radialStrength);

/// 轴向色散（LoCA）校正。
///
/// 用亮度通道的 DoG（两个半径的高斯差）检测高光光斑，
/// 再用高光阈值与光斑响应共同构成掩码；掩码内把 R−G / B−G 色差
/// 向局部平滑值靠拢，削弱焦外点光源与散景边缘的紫边/绿边。
/// G 通道本身不修改。
/// @param strength            校正强度 0~1
/// @param highlightThreshold  高光阈值（线性 0~1）
void applyLongitudinalChromaticAberration(ImageBuffer& image,
                                          float strength,
                                          float highlightThreshold);

/// 边缘导向伪色抑制。
///
/// 用 G 通道 Sobel 梯度生成边缘掩码；仅在掩码覆盖的高对比度边缘，
/// 对色差平面（R−G / B−G）做中值平滑，G 保持不变。
/// 只压缩色度异常、不改变亮度结构，因此比直接平滑 R / B 更保细节。
/// 平坦区域不做任何平滑，避免抹除原生彩色细节。
/// @param strength        抑制强度 0~1
/// @param edgeSensitivity 边缘敏感度 0~1（越高越多边缘参与）
/// @param smoothingRadius 中值平滑半径 1~3（像素，按分辨率自适应缩放）
void suppressEdgeFalseColor(ImageBuffer& image,
                            float strength,
                            float edgeSensitivity,
                            int smoothingRadius);

/// Demosaic 后的空间边缘 / 伪色策略（diy.md §5）。
///
/// 按 G 通道梯度划分边缘区与平滑区，并在用户笔刷覆盖处加强 / 减弱
/// 色差（R−G / B−G）中值清理；G 通道本身不变。
void applyDemosaicSpatialStrategy(ImageBuffer& image,
                                  const DemosaicSpatialSettings& settings,
                                  float pixelScale = 1.0f);

// ===========================================================================
//  4. 白平衡（线性，在 XYZ→ProPhoto 之后）
// ===========================================================================

/// 白平衡白点缩放。
/// 需求 §2 明确：白平衡位于 XYZ 转 ProPhoto **之后**。
/// 实现为对角矩阵缩放：对 ProPhotoRGB 三通道分别乘以增益。
///
/// @param image        工作空间（ProPhotoRGB 线性）图像，就地修改
/// @param temperature  色温（开尔文）
/// @param tint         色调 -100~100
/// @param grayPickX    灰点拾取坐标（归一化），< 0 表示未使用
/// @param grayPickY    灰点拾取坐标
/// @param useGrayPick  是否使用灰点拾取
/// @param referenceTemperature 非 0 时把该白点当作"已中性的基准"，
///        只补偿它与 temperature 的差异（普通位图用 5000K）。
///        RAW 传 0，保持按绝对白点补偿。
/// @param referenceTint 基准色调，与 referenceTemperature 配对使用。
///        创意白平衡偏移传入基准白平衡的色温 / 色调，即可得到相对增益。
/// @param normalizeReferenceLuma 相对增益是否再做一次亮度归一化。
///        调用方已经先应用了基准增益时传 false，保证
///        "基准 × 相对" 严格等于目标白点的绝对增益；单独使用相对校正时传 true。
void applyWhiteBalance(ImageBuffer& image,
                       float temperature,
                       float tint,
                       float grayPickX,
                       float grayPickY,
                       bool useGrayPick,
                       float referenceTemperature = 0.0f,
                       float referenceTint = 0.0f,
                       bool normalizeReferenceLuma = true);

/// 从图像中采样指定坐标的颜色，用于灰点吸管。
/// @param normalizedX/Y 归一化坐标 0~1
/// @return 采样到的 RGB 值（已做 3x3 邻域平均以降噪）
std::array<float, 3> samplePixel(const ImageBuffer& image, float normalizedX, float normalizedY);

/// 由用户拾取的灰点反算色温与色调，用于回填 UI 控件。
/// @return 成功时 true，并把结果写入 outTemperature / outTint
bool grayPointToTemperatureTint(const std::array<float, 3>& grayRgb,
                                float& outTemperature,
                                float& outTint);

// ===========================================================================
//  4.5 预览影调基准（自动曝光归一 + 弱 S 曲线）
// ===========================================================================

/// 计算自动曝光增益：把亮度通道的 percentile 分位映射到白点 1.0。
///
/// 与按最大值归一相比，分位数不会被单个镜面反光、噪点或炫光拉偏，
/// 代价是约 0.5% 的最亮像素会顶到白点——这与 dcraw 的 auto-bright 思路一致。
///
/// 白点归一之外还有一道中位亮度封顶：只回答"白点在哪"不足以判断画面该多亮，
/// 阴天天空、雾、雪这类大面积高调场景若只按白点归一，整个中间调会被抬到
/// 接近纯白，暗部抬起、层次丢失，观感发灰。封顶只限制提亮，不会主动压暗。
///
/// 增益限制在 [1/8, 8]（约 ±3 EV），避免极暗或极端数据把画面推到不可用范围。
///
/// 本函数只读图像，不修改任何数据，也不参与白平衡 / CCM 标定。
///
/// @param image      线性工作空间图像（应在白平衡之后统计）
/// @param percentile 分位点，0.995 表示 99.5% 分位
/// @return 曝光增益；图像为空或统计无效时返回 1.0
float computeAutoExposureScale(const ImageBuffer& image, float percentile = 0.995f);

/// 应用预览影调基准：线性域曝光缩放 + 可选的 S 型对比度曲线。
///
/// 仅用于**屏幕预览链路**的基准影调。调用方可以把它用在导出链路上，
/// 但默认导出不启用（由「预览影调同步导出」开关决定）。
///
/// 必须放在白平衡之后、用户光影调整之前：
///  - 自动曝光统计基于白平衡后的工作空间亮度；
///  - 用户的曝光 / 对比度 / 曲线随后作用在基准之上，因此可以覆盖自动结果。
///
/// S 曲线在感知域计算（与 applyLightAdjustments 的对比度同一套做法），
/// 只作用于 [0,1] 区间，超过白点的部分原样保留，交给管线末尾的软膝压缩，
/// 避免增益把高光推出 1.0 后在这里被硬截断而破坏高光层次。
///
/// 自动曝光与预览曲线都**不修改线性原始 RGB**，也不参与传感器 CCM 标定。
///
/// @param image         线性工作空间图像，就地修改
/// @param exposureScale 曝光增益，来自 computeAutoExposureScale
/// @param applySCurve   true = 观感预览；false = 线性基准预览（仅曝光归一）
void applyPreviewTone(ImageBuffer& image, float exposureScale, bool applySCurve);

// ===========================================================================
//  5. 光影调整（线性域）
// ===========================================================================

/// 曝光、高光压缩、阴影提升、对比度、去雾。
/// 需求 §2 管线。
/// 全部在线性光域运算（除对比度与去雾的 S 曲线在感知域更自然，
/// 内部会做临时 gamma 往返——详见实现注释）。
void applyLightAdjustments(ImageBuffer& image, const LightSettings& settings);

// ===========================================================================
//  5.5 色彩与质感（饱和度 / 自然饱和度 / 清晰度 / 纹理）
// ===========================================================================

/// 全局色彩与质感调整（A1）。
///
/// 饱和度与自然饱和度在感知域计算：
///  - 饱和度按同一比例缩放色度，-100 得到灰度（保留亮度）；
///  - 自然饱和度优先作用于低饱和颜色，并对橙色肤色做保护，
///    因此提高时不容易让人脸先变“塑料”。
///
/// 清晰度与纹理只修改亮度结构，不改变色相：
///  - 清晰度使用较大半径的亮度高通，增强中间调立体感；
///  - 纹理使用较小半径的高通，强化织物、头发等细节；
///  - 两者都带中间调掩码与增量限幅，避免高光/阴影溢出与明显光晕。
///
/// 半径按图像长边自适应（与 `applySharpen` 的预览一致性策略一致）。
/// 必须作用在**整幅工作图**上；若先裁剪再调用，半径会随裁剪尺寸缩小。
///
/// @param image    线性工作空间图像，就地修改
/// @param settings 四项参数，全部为 0 时函数直接返回
void applyPresence(ImageBuffer& image, const PresenceSettings& settings);

// ===========================================================================
//  6. HSL 调色（线性域，感知色相角度计算）
// ===========================================================================

/// 七通道 HSL 偏移。
/// 需求 §2 管线。色相角与饱和度在感知域计算，亮度调整作用于线性域。
void applyHsl(ImageBuffer& image, const HslSettings& settings);

// ===========================================================================
//  6.2 黑白混色（A3）
// ===========================================================================

/// 转换为黑白，并按原色相调整明度。
///
/// 算法：在感知域计算亮度与 HSV 色相，再用与 HSL 相同的余弦钟形权重
/// 累加七个色相通道的明度偏移，最后把三通道统一写成调整后的亮度。
/// 因此输出是严格的中性灰，后续颜色分级 / 分离色调仍可给它重新染色。
///
/// 未启用时逐位不改变图像；启用但七通道全零时输出标准黑白。
void applyBlackWhite(ImageBuffer& image, const BlackWhiteSettings& settings);

// ===========================================================================
//  6.5 颜色分级与分离色调（A2）
// ===========================================================================

/// 三段式颜色分级 + 高光 / 阴影分离色调。
///
/// 全部在感知域计算，并按亮度选择作用区域：
///  - 颜色分级：阴影 / 中间调 / 高光各自一个色相 + 饱和度 + 明度；
///    色偏按"保持亮度"的方式叠加（减去色偏自身的亮度），
///    因此给暗部染青不会顺带把暗部提亮；
///  - 平衡：正值把作用重心推向高光，负值推向阴影；
///  - 混合：控制三段高斯权重的过渡宽度，越大越柔和；
///  - 分离色调：高光 / 阴影各自染色，通常与颜色分级叠加使用。
///
/// 两者都为空时函数直接返回，逐位不改变图像。
void applyColorGrading(ImageBuffer& image,
                       const ColorGradingSettings& grading,
                       const SplitToningSettings& splitToning);

// ===========================================================================
//  6.8 局部调整（B1）
// ===========================================================================

/// 按蒙版权重逐层应用局部调整。
///
/// 每一层独立求权重图，并依次叠加上一层的结果：
///   线性域：相对白平衡增益 → 曝光 → 高光 / 阴影
///   感知域：对比度 → 饱和度
/// 最后按蒙版权重 `weight` 与原像素线性混合（weight=0 完全不改变，
/// weight=1 完全采用局部调整结果）。权重图已计入羽化、反相与不透明度。
///
/// 放在管线末端、细节处理之前调用：局部调整不应改变 RAW 源阶段缓存，
/// 也不应被后续的降噪 / 锐化当作噪声放大。
///
/// @param image             线性工作空间图像，就地修改
/// @param masks             蒙版栈（enable=false 时直接返回）
/// @param globalTemperature 当前全局白平衡色温，用于计算相对增益
/// @param globalTint        当前全局白平衡色调
/// @param weightCache 可选的权重图缓存（B5）。预览尺寸下可避免每次重算
///        径向 / 线性 / 画笔权重；传 nullptr 时直接构建临时权重图。
void applyLocalAdjustments(ImageBuffer& image,
                           const MaskStack& masks,
                           float globalTemperature,
                           float globalTint,
                           MaskWeightCache* weightCache = nullptr);

// ===========================================================================
//  7. 曲线（线性域）
// ===========================================================================

/// 把控制点数组编译为查找表（LUT）。
/// @param points  控制点（x 升序，归一化 0~1）
/// @param size    输出 LUT 的采样点数，建议 1024 或 4096
/// @return LUT 数组；points 无效时返回空数组
std::vector<float> compileCurveLut(const std::vector<CurvePoint>& points, int size = 4096);

/// 参数曲线：按阴影 / 中间调 / 高光三个区域调整亮度。
/// 在感知域计算，只改变亮度结构，不改变色相；先于点曲线应用。
void applyParametricCurve(ImageBuffer& image,
                          const ParametricCurveSettings& settings);

/// 应用 R/G/B/L 四条曲线。
/// 需求 §2 管线。L 明度曲线在 R/G/B 之后应用。
void applyCurves(ImageBuffer& image, const CurveSettings& settings);

// ===========================================================================
//  8. 3D LUT
// ===========================================================================

/// 叠加 .cube 3D LUT（三线性插值），并按 opacity 与原值混合。
/// @return 未选择文件、文件缺失或格式错误时返回错误，此时图像保持原样
ErrorInfo applyLut(ImageBuffer& image, const LutSettings& settings);

// ===========================================================================
//  9. 降噪（线性域）
// ===========================================================================

/// 明度与色度降噪。
/// 需求 §2 管线。在线性域做边缘保持的模糊（guided/bilateral 近似），
/// 明度与色度分离处理：色度可重度平滑而不损细节。
/// @param pixelScale 当前渲染图与原图的比例（预览降采样时 < 1）。
///                   小波层间距按该比例换算，保证 fit 视图与 1:1 的
///                   平滑空间尺度一致，而不是把预览过度涂抹。
/// @param method      明度降噪算法（D5）：小波 / 非局部均值 / BM3D 近似
/// @param searchRadius 非局部方法（NLM / BM3D）的搜索半径（像素，3~7），
///        小波算法忽略
void applyNoiseReduction(ImageBuffer& image,
                         float lumaStrength,
                         float chromaStrength,
                         float pixelScale = 1.0f,
                         NoiseReductionMethod method = NoiseReductionMethod::Wavelet,
                         int searchRadius = 4);

// ===========================================================================
//  9.5 彩纹抑制（低频色度重建）
// ===========================================================================

/// 抑制高密度周期纹理上的彩色摩尔纹（绿 / 洋红交替的成片彩纹）。
///
/// 瓦楞板、百叶窗、瓷砖、织物这类周期约 3~8 像素的强纹理，会让拜耳阵列
/// 的红 / 绿 / 蓝采样点分别落在纹理的波峰或波谷上。去马赛克无法区分
/// "纹理亮"与"该通道偏亮"，于是输出成片的错误色度。
///
/// 关键观察：拜耳阵列把像素分成 4 个采样格点（2x2 单元内的四个位置），
/// 每个格点上的通道组合是固定的。纯采样相位误差表现为**格点之间的色差
/// 偏置**；真实色彩细节则在四个格点上连续、彼此一致。因此按 8x8 分块
/// 统计四个格点各自的色差均值，把每块内的偏置平滑地摊平，就能消除彩纹，
/// 而格点内部的逐像素色度波动（真实细节）完全保留。
///
/// 与"把整片色度低通重建"的做法相比，本方法不会把纹理区抹成色块，
/// 1:1 查看时树叶、织物等细密色彩不会被糊掉。
///
/// 必须放在白平衡之后、裁剪之后执行：前者保证色度是平衡的（拜耳域
/// 通道增益不同，色差会失真），后者让 1:1 局部渲染只处理可见区域。
/// 感知域（平方根）计算，与 8 位显示域的观感一致。
///
/// 前提：图像必须与拜耳阵列逐像素对齐。经过 2x2 合并、RGB 降采样或
/// 镜头畸变重采样后，格点语义不再成立，调用方应跳过本函数。
///
/// @param strength    0~1，0 表示完全关闭（不改变任何像素）。
/// @param cfaOriginX  图像 (0,0) 在拜耳坐标系中的 x 偏移（裁剪时非 0）。
/// @param cfaOriginY  图像 (0,0) 在拜耳坐标系中的 y 偏移。
void suppressChromaMoire(ImageBuffer& image, float strength,
                         int cfaOriginX = 0, int cfaOriginY = 0);

/// 抑制大尺度色纹（低频色度重建）。
///
/// suppressChromaMoire() 处理的是 2x2 采样格点之间的偏置，只能消除
/// 与 CFA 相位直接耦合的高频彩纹。瓦片、幕墙、百叶窗在**略低于**
/// 尼奎斯特频率的周期纹理上还会产生另一种误差：纹理周期约 8~20 像素，
/// 与 CFA 的 2 像素栅格形成拍频，去马赛克后色度沿纹理方向出现成片的
/// 绿 / 洋红交替条纹（1:1 查看时尤其明显）。这类误差在 8x8 块内四个
/// 格点上是一致的，格点均衡无法消除。
///
/// 这里把两个色差平面压到 σ≈4 像素的尺度再重建：亮度不动，只去掉
/// 细尺度的色度起伏，因此画面不会发软，真实的大面积颜色完全保留。
/// 相机 JPEG 的色度本身也经过类似的低通，观感与机内直出一致。
///
/// 计算在感知域（平方根）进行，与 8 位显示域的观感一致；色度在
/// 1/2 分辨率上做大核高斯，再双线性放大，开销约为全分辨率高斯 1/4。
///
/// 前提与 suppressChromaMoire() 相同：图像与拜耳阵列逐像素对齐。
/// 预览 2x2 合并路径已经过合并+色度抗锯齿，不需要再调用本函数。
///
/// @param strength 0~1，0 表示完全关闭（不改变任何像素）。
void suppressLargeChromaMoire(ImageBuffer& image, float strength = 1.0f);

// ===========================================================================
//  10. 锐化（线性域）
// ===========================================================================

/// USM 锐化，**带边缘掩码**避免噪点被放大（需求 §2 明确要求）。
/// @param amount   强度 0~100
/// @param radius   半径（**原始像素**）0.5~5
/// @param masking  边缘掩码阈值 0~100，越高越只锐化强边缘
/// @param pixelScale 当前渲染图与原图的比例。半径按此缩小，
///                   避免降采样预览看起来比全尺寸导出更锐。
/// @param method     锐化算法（D6）：双尺度 USM 或 Richardson-Lucy 反卷积
/// @param iterations 反卷积迭代次数（3~30），USM 忽略
void applySharpen(ImageBuffer& image, float amount, float radius, float masking,
                  float pixelScale = 1.0f,
                  SharpenMethod method = SharpenMethod::UnsharpMask,
                  int iterations = 8);

/// 输出锐化（D6）：在导出重采样之后按**输出像素**追加一次小半径 USM。
///
/// 与捕获锐化不同，它不依赖场景像素比例，也不带大半径粗尺度分量：
/// 目的只是抵消缩放与 JPEG 压缩带来的轻微软化。
/// 输入是**已完成 Gamma 编码**的导出数据（不是线性光），因此这里直接在
/// 编码值上做亮度 USM，不再做感知域平方根变换。
/// 只在导出链路上调用，预览管线不经过这里。
void applyOutputSharpen(ImageBuffer& image, float amount, float radius);

// ===========================================================================
//  11. 预览降采样（2x2 合并 + RGB 域面积平均）
// ===========================================================================

/// 2x2 合并（拜耳 binning）：把每个 2x2 CFA 单元合并成一个 RGB 像素。
///
/// 与"拜耳域块平均 + 低分辨率去马赛克"相比，本方法让 R/G/B 来自**同一
/// 空间块**，边缘处三个通道的混合程度一致，因此不会产生彩色马赛克。
/// 代价是最高只能降到原图的 1/2（与 2x2 合并等价），更高倍数需要
/// 在 RGB 域继续用 downsampleRgbArea 处理。

/// 饱和标记的位定义，由 binBayerToRgb 填写、flattenClippedHighlights 使用
inline constexpr std::uint8_t kClipRed = 1u;
inline constexpr std::uint8_t kClipGreen = 2u;
inline constexpr std::uint8_t kClipBlue = 4u;
///
/// @param clipMask 可选输出：与输出图像同尺寸的饱和标记，每个字节按位组合
///                 kClipRed / kClipGreen / kClipBlue，表示该 2x2 单元内对应
///                 通道是否有采样点达到白电平。用于后续高光处理。
/// @return 拜耳排列未知或尺寸过小时返回错误，调用方应回退到普通去马赛克
ErrorInfo binBayerToRgb(const BayerImage& bayer,
                        ImageBuffer& output,
                        std::vector<std::uint8_t>* clipMask = nullptr);

/// RGB 域面积平均降采样：三个通道使用完全相同的盒式核。
/// 与拜耳域降采样不同，这里不涉及 CFA 相位，任何整数倍数都可用。
void downsampleRgbArea(const ImageBuffer& input, ImageBuffer& output, int factor);

/// 从拜耳数据生成**全分辨率**饱和掩码。
///
/// 预览的 2x2 合并路径由 binBayerToRgb 直接给出每个合并像素的饱和位；
/// 全分辨率去拜耳没有这个中间步骤，必须在拜耳域另行判断。这里对每个
/// 输出像素检查 5x5 邻域：只要某个通道的真实采样在邻域内达到白电平，
/// 就在该像素上置对应位。
///
/// 为什么用 5x5 而不是只看自身采样：红蓝插值的支撑域是 5x5，饱和采样
/// 会被插值"摊开"到附近像素；只看自身会漏掉这些被污染的邻居，高光边缘
/// 便会出现一圈洋红/绿色。5x5 与去拜耳的支撑域一致，也与预览合并时
/// 一个 2x2 单元覆盖的范围相当。
///
/// 必须在白电平裁剪之后、拜耳数据释放之前调用。
/// @return 数据为空、尺寸过小或排列未知时返回 false，mask 保持不变
bool buildClipMaskFromBayer(const BayerImage& bayer,
                            std::vector<std::uint8_t>& mask);

/// 部分饱和高光的还原（clip to white）。
///
/// 2x2 合并时某个通道顶到白电平时，该通道的颜色信息已被彻底截断，合并后
/// 得到的是随机色。此时用邻域颜色去补会染上周围物体的颜色（例如玻璃幕墙的
/// 蓝绿色），正确做法是按"高光"处理：把三通道统一到最大通道值，还原为中性白
/// ——这与 dcraw `-H 0`、各类转换器的 clip highlights 一致。
///
/// 判定为高光需要未饱和通道也足够亮（≥ minLevel）：若未饱和通道很暗
/// （例如很红的物体只有 R 饱和），说明本体是深色物体、颜色仍然可信，
/// 不做处理，避免把真实彩色高光洗白。
///
/// **必须在白平衡之后调用**：相机原始通道对白色的响应并不相等，在相机域
/// 把三通道"拉平"得到的是偏色（实测偏蓝），只有在白平衡之后的工作空间里
/// 三通道等值才真正是中性白。
///
/// @param image     线性 RGB 图像（就地修改，应为白平衡之后的工作空间数据）
/// @param clipMask  与图像同尺寸、由 binBayerToRgb 输出（降采样时按位或归约）
///                  的饱和标记
/// @param minLevel  判定高光所需的最低未饱和通道亮度
void flattenClippedHighlights(ImageBuffer& image,
                              const std::uint8_t* clipMask,
                              float minLevel = 0.5f);

/// 伪彩抑制（false color suppression）。
///
/// 2x2 合并时 R / B 是单点采样（位于单元对角），当场景细节恰好落在
/// 像素级（高光点、细线、镜面反光）时，三通道采到的内容不同，会合成出
/// 随机颜色的孤立斑点。这是所有 binning/低分辨率去马赛克的固有问题。
///
/// 只处理**未饱和的孤立伪彩**：在线性 RGB 上计算色比 R/G、B/G（色比与
/// 白平衡增益无关，因此在白平衡之前调用也不会被通道增益推偏），在 5x5 范围内
/// 按步长 2 取 8 个邻点求色比中值（步长 2 是为了避开 2x2 的伪彩簇本身）；
/// 仅当中心色比相对中值偏离超过 threshold 时才用中值重建 R / B，
/// 因此大面积正常色彩与粗色彩边缘不受影响。
/// 这与 RawTherapee 的 False Color Suppression、dcraw 的 `-f` 思路一致。
///
/// 饱和高光由 flattenClippedHighlights 单独处理：本步在白平衡之前按色比
/// 清除伪彩，高光还原则在白平衡之后执行。
///
/// @param relativeThreshold 色比的**相对**偏离阈值，0.08 表示偏离 8%；
///                          越大越保守，越小清得越干净但可能削弱真实细节色
void suppressFalseColor(ImageBuffer& image, float relativeThreshold = 0.08f);

/// 预览 2x2 合并路径的色度抗混叠。
///
/// 2x2 合并时 R / B 各自只取单元内的一个采样点（G 是两点平均），
/// 色度因此没有做任何抗混叠。百叶窗、幕墙竖肋、织物等接近 Nyquist 的
/// 周期纹理，会让 R / B 的采样格点与纹理周期产生拍频，表现为成片的
/// 绿 / 洋红彩纹；孤立像素的伪彩由 suppressFalseColor 处理，但成片的
/// 拍频不会被中值判据命中。
///
/// 这里在**亮度不变的**前提下，对 Cr / Cb 做一次半径 1 像素的十字低通
/// （中心 1/2、四邻各 1/8）。支撑范围与"全分辨率去马赛克后再做 2x2 面积
/// 平均"的色度响应相当：足以压掉拍频，又不会把真实的大块颜色抹开；
/// 亮度平面完全沿用原值，因此不会让画面发糊。
///
/// 只用于预览的 2x2 合并路径（导出 / 1:1 走全分辨率去马赛克，不需要）；
/// 必须在白平衡与高光还原之后调用，避免把饱和高光的单通道值摊到邻域。
void antialiasPreviewChroma(ImageBuffer& image);

// ===========================================================================
//  12. 色域映射与 Gamma 编码
// ===========================================================================

/// 把超出 [0,1] 的线性值软裁切回可用范围（色域映射的一部分）。
/// 需求 §2 管线"色域映射"步骤。采用高斯软膝而非硬裁剪，
/// 保留高光层次、避免生硬的截断边缘。
void applyGamutSoftClip(ImageBuffer& image);

/// Gamma 编码：线性 → 目标空间的编码值。
/// 需求 §2 管线**最后**一步。这是整个管线中唯一允许做 gamma 编码的位置。
/// @param space 目标色彩空间，决定使用哪种传递函数
void applyGammaEncode(ImageBuffer& image, ColorSpace space);

/// 线性 → sRGB 编码（便捷封装，用于预览显示路径）
void applySrgbEncode(ImageBuffer& image);

// ===========================================================================
//  辅助：缩放（预览降采样）
// ===========================================================================

/// 对拜耳域数据做降采样。
///
/// 实现要点：必须**保持拜耳排列**。做法是按 2x2 单元对齐地取块平均，
/// 这样降采样后每个像素仍对应原有的滤镜颜色，后续管线无需特殊处理。
///
/// 注意：预览路径**不再使用**本函数。拜耳域大块平均会让块内 R/G/B 的
/// 采样位置不一致，强边缘处产生彩色马赛克；预览改用 binBayerToRgb +
/// downsampleRgbArea。本函数保留给需要保持拜耳结构的场景（如低分辨率
/// 拜耳域分析、未来其他用途）。
///
/// @param bayer      输入拜耳数据
/// @param output     输出降采样后的拜耳数据
/// @param factor     整数下采样倍数（2 表示边长减半）
ErrorInfo downsampleBayer(const BayerImage& bayer, BayerImage& output, int factor);

/// 对三通道图像做双线性缩放。用于导出时的尺寸调整。
void resizeImage(const ImageBuffer& source, ImageBuffer& dest, int newWidth, int newHeight);

/// 污点修复 / 仿制（C4）。
///
/// 每个修复点从 source 取样：
///  - Heal：复制源纹理，并把颜色均值对齐到目标区域；
///  - Clone：直接复制源像素；
///  - 按 feather / opacity 与目标区域混合。
/// 位于局部调整之后、裁剪之前。
void applyRetouch(ImageBuffer& image, const RetouchSettings& settings);

/// 透视 / 几何校正（C3）。
///
/// 用四角梯形 + 单应矩阵做双线性重采样，输出尺寸与输入相同；
/// 采样区域会自动收窄到 [0,1] 内，避免出现黑边。
/// 位于镜头校正之后、白平衡 / 局部调整之前。
/// @return 是否实际执行了重采样
bool applyPerspective(ImageBuffer& image, const PerspectiveSettings& settings);

/// 按归一化矩形裁剪图像（C1），就地替换图像内容。
/// 坐标会先夹取到 [0,1]，并保证至少 1 像素；整幅图时不做任何操作。
/// @param cropLeft/cropTop 可选输出：裁剪区域在**裁剪前工作图**中的左上角像素坐标。
///        用于 1:1 局部渲染时还原 CFA 原点与抖动坐标。
/// @return 是否实际发生了裁剪
bool applyCrop(ImageBuffer& image, const CropSettings& settings,
               int* cropLeft = nullptr, int* cropTop = nullptr);

/// 截取三通道图像的一块子区域（原样拷贝，不插值）。
///
/// 用于 1:1 放大预览只渲染可见区域：屏幕外的像素既看不见，
/// 又要多跑几十倍计算。区域越界时自动夹取到图像范围内。
///
/// @param source 源图像
/// @param dest   输出子图（尺寸为实际截取到的宽高）
/// @param x      子图左上角在源图中的坐标
/// @param y      同上（纵向）
/// @param width  期望宽度，可为源图宽度以内的任意值
/// @param height 期望高度
void cropImage(const ImageBuffer& source, ImageBuffer& dest,
               int x, int y, int width, int height);

/// 线性域高斯模糊（分离式，供降噪与 USM 内部使用）
void gaussianBlur(const ImageBuffer& source, ImageBuffer& dest, float radius, bool perChannel);

/// 对单个通道平面做高斯模糊（内部工具）
void gaussianBlurPlane(const std::vector<float>& src,
                       std::vector<float>& dst,
                       int width, int height,
                       float radius);

}  // namespace ops
}  // namespace rawcore
