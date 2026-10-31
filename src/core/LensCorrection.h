// ============================================================================
//  LensCorrection.h —— lensfun 镜头校正封装
//
//  需求 §2 管线位置：Demosaic 之后、CamRGB→XYZ 之前。
//  校正项目（需求 §3 模块 F2 第 4 组）：
//   - 畸变（distortion）
//   - 暗角（vignetting）
//   - 轴向色差（lateral chromatic aberration）
//
//  需求 §6：lensfun 无匹配镜头数据时自动关闭校正并给出文字提示，
//  而不是报错中断渲染。
// ============================================================================

#pragma once

#include "EditContext.h"
#include "Types.h"

#include <memory>
#include <string>
#include <vector>

namespace rawcore {

/// 镜头校正执行结果
struct LensCorrectionResult {
    bool applied = false;          ///< 是否实际执行了校正
    bool lensMatched = false;      ///< 是否在 lensfun 数据库中匹配到镜头
    std::string matchedLensName;   ///< 匹配到的镜头名（用于 UI 显示）
    std::string message;           ///< 面向用户的中文说明
};

/// 镜头校正处理器。
///
/// 使用说明：lensfun 使用全局镜头数据库，加载耗时较长。
/// 因此本类设计为可复用实例——同一张图多次渲染预览时不要重复构造。
class LensCorrector {
public:
    LensCorrector();
    ~LensCorrector();

    LensCorrector(const LensCorrector&) = delete;
    LensCorrector& operator=(const LensCorrector&) = delete;

    /// 本编译是否启用了 lensfun 支持（由 CMake 注入）
    static bool isSupported();

    /// 在 lensfun 数据库中查找匹配的相机与镜头。
    ///
    /// D7：提供 settings 且 useManualLens 为真时，优先按 manualLensName
    /// 查找数据库中的镜头，并可覆盖焦距 / 光圈。手动选择失败时回退到
    /// EXIF 自动匹配，避免转接环写错型号后完全无法校正。
    ///
    /// @param metadata  RAW 元数据（提供相机、镜头型号与焦距光圈）
    /// @param settings  镜头校正设置；nullptr 等价于纯自动匹配
    /// @return 成功时 true，并把匹配结果写入 lastMatchMessage()
    bool findLens(const RawMetadata& metadata,
                  const LensCorrectionSettings* settings = nullptr);

    /// 列出数据库中适用于当前相机的镜头名（供手动选择下拉框使用）。
    /// 结果按名称排序并去重，最多返回 maxCount 项。
    /// 未编译 lensfun、数据库加载失败或没有匹配相机时返回空列表。
    std::vector<std::string> availableLensNames(const RawMetadata& metadata,
                                                int maxCount = 300);

    /// 对图像执行镜头校正（就地修改）。
    /// 需求 §2 管线位置：Demosaic 之后。
    /// @param image     线性 RGB 图像（就地修改）
    /// @param settings  校正开关与强度系数
    /// @return 执行结果，含是否成功与用户提示
    LensCorrectionResult apply(ImageBuffer& image, const LensCorrectionSettings& settings);

    /// 上一次匹配失败的说明
    const std::string& lastMatchMessage() const;

    /// 实际使用的镜头数据库目录；空表示尚未加载或加载失败
    const std::string& databasePath() const;

    /// 镜头数据库是否已成功加载（首次 findLens 时惰性加载）
    bool databaseLoaded() const;

    /// 最近一次匹配到的镜头规范名（空表示尚未匹配成功）
    const std::string& matchedLensName() const;

    /// 用户可写的 lensfun 数据库目录（D7）。
    /// Windows：%APPDATA%/RawEdit/lensfun/version_1；
    /// Linux：$XDG_DATA_HOME/RawEdit/lensfun/version_1。
    /// 该目录会作为数据库候选目录之一参与加载，导入的 XML 无需管理员权限。
    static std::string writableDatabaseDirectory();

    /// 导入 lensfun XML 数据库文件（D7）。
    ///
    /// lensfun 的数据库是若干 XML 文件，本方法做最基础的合法性检查后
    /// 复制到用户可写目录，并重新加载数据库。不会覆盖同名文件。
    /// @param xmlPath 源 XML 路径
    /// @param savedPath 输出：实际写入路径
    /// @param error 失败原因
    static bool importDatabaseFile(const std::string& xmlPath,
                                   std::string& savedPath,
                                   std::string& error);

    /// 重新加载镜头数据库（导入后调用）。成功后 databaseLoaded() 为 true。
    bool reloadDatabase();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawcore
