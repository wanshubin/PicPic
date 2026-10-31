// ============================================================================
//  IccSimulatorWindow.h —— 悬浮式 ICC / 显示效果模拟窗口
//
//  从选图工作台左侧顶部栏的「ICC 模拟器」按钮弹出：
//   - 无标题栏，只保留一圈边框；边框内显示经过模拟参数处理后的画面；
//   - 左上角按钮展开参数面板，右上角按钮关闭窗口；
//   - 按住窗口内容区可拖动，按住边框 / 四角可缩放。
//
//  画面来源（可在参数面板切换）：
//   - 实时屏幕（镜片）：截取窗口下方屏幕内容，像一片滤镜镜片一样显示模拟效果。
//     通过 Windows 的 WDA_EXCLUDEFROMCAPTURE 把本窗口排除在截屏之外，
//     因此截到的是「窗口下面」的画面，不会自我递归；
//   - 测试色卡：内置 24 色块 + 灰阶 + RGB 渐变，不依赖当前选中照片。
//
//  模拟参数（全部只影响本窗口，不动主预览，也不影响导出）：
//   - 亮度、对比度、饱和度、色温、伽马。
//     饱和度拉到 0% 就是完全灰度，因此不再单列「灰度」参数。
//
//  变换顺序参照 requirements/device_mode.md：
//     亮度（曝光）→ 对比度 → 色温 RGB 增益 → 伽马 → 饱和度 → 钳位
// ============================================================================

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QCloseEvent;
class QComboBox;
class QHideEvent;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPainter;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QSlider;
class QTimer;

namespace rawui {

class ScreenLensCapture;

/// 一组 ICC 模拟参数（滑块原始值）。
/// 在 UI 线程与后台抓屏线程之间传递，因此保持纯 POD。
struct IccSimParams {
    int brightness = 0;
    int contrast = 100;
    int saturation = 100;
    int temperature = 6500;
    int gamma = 100;

    /// 全部为默认值：不需要做任何像素变换
    bool isIdentity() const
    {
        return brightness == 0 && contrast == 100 && saturation == 100
            && temperature == 6500 && gamma == 100;
    }
};

class IccSimulatorWindow : public QWidget {
    Q_OBJECT

public:
    explicit IccSimulatorWindow(QWidget* parent = nullptr);
    ~IccSimulatorWindow() override;

signals:
    /// 窗口被关闭（点击右上角关闭按钮或 Esc），主窗口据此同步按钮状态。
    void closed();

protected:
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    friend class ScreenLensCapture;

    /// 画面来源
    enum SourceMode {
        ScreenLens = 0,  ///< 实时截取窗口下方屏幕
        TestChart = 1    ///< 内置测试色卡
    };

    /// 缩放命中的边（可组合）
    enum ResizeEdge {
        EdgeNone = 0,
        EdgeLeft = 1,
        EdgeRight = 2,
        EdgeTop = 4,
        EdgeBottom = 8
    };

    // ---- 界面 ----
    void buildUi();
    QWidget* buildSettingsPanel();
    /// 把左上 / 右上按钮与参数面板重新贴到窗口内边距上
    void layoutOverlays();
    /// 更新参数数值文字
    void updateValueLabels();
    /// 边框内的内容区（去掉 2px 边框）
    QRect contentRect() const;
    /// 内容区在屏幕坐标系中的矩形（用于截屏）
    QRect contentGlobalRect() const;

    // ---- 画面来源 ----
    /// 把当前来源渲染成一张与内容区同像素尺寸的图（尚未施加模拟参数）
    QImage renderSourceImage() const;
    /// 在 QPainter 上绘制 24 色块 + 灰阶 + RGB 渐变测试图
    void drawTestChart(QPainter& painter, const QRectF& area) const;

    // ---- 模拟变换 ----
    /// 读取当前滑块值
    IccSimParams currentParams() const;
    /// 按当前参数处理画面；参数全为默认值时原样返回
    QImage applySimulation(const QImage& source);

    // ---- 屏幕镜片 ----
    /// 把本窗口排除在屏幕截取之外；返回 false 表示暂时失败或系统不支持。
    /// 原生窗口还没建好时只算「暂时失败」，会安排重试，不当作系统不支持。
    bool ensureScreenExclusion();
    /// 排除设置失败后安排一次重试（窗口还没显示 / 源不是镜片时直接忽略）
    void scheduleScreenExclusionRetry();
    /// 重试定时器到点：补设排除标志，成功后立刻拉起后台抓屏线程
    void retryScreenExclusion();
    /// 截取窗口下方屏幕区域，存入 m_screenFrame（仅在无后台抓屏时使用）
    void captureScreenFrame();
    /// 根据当前来源 / 可见性启停后台抓屏线程（Windows）或回退定时器
    void updateScreenCaptureState();
    /// 停止并销毁后台抓屏线程（隐藏 / 关闭时调用）
    void stopScreenCapture();
    /// 后台抓屏线程送来新的一帧（在 UI 线程执行）
    void onScreenFrameReady();
    void startRefreshTimer();
    void stopRefreshTimer();
    /// 拖动 / 缩放时把刷新率提高到 125fps，结束后回到 60fps
    void setInteractionRefreshRate(bool interacting);
    /// 立即截取一帧并重绘
    void refreshNow();

    // ---- 持久化 ----
    void loadSettings();
    void saveSettings();

    // ---- 交互 ----
    int resizeEdgesAt(const QPoint& position) const;
    Qt::CursorShape cursorForEdges(int edges) const;
    void applyResize(const QPoint& globalPosition);
    void updateCursorFor(const QPoint& position);

    // 浮在画面上的控件
    QPushButton* m_settingsButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QWidget* m_settingsPanel = nullptr;

    QComboBox* m_sourceCombo = nullptr;
    QSlider* m_brightnessSlider = nullptr;
    QSlider* m_contrastSlider = nullptr;
    QSlider* m_saturationSlider = nullptr;
    QSlider* m_temperatureSlider = nullptr;
    QSlider* m_gammaSlider = nullptr;
    QLabel* m_brightnessValue = nullptr;
    QLabel* m_contrastValue = nullptr;
    QLabel* m_saturationValue = nullptr;
    QLabel* m_temperatureValue = nullptr;
    QLabel* m_gammaValue = nullptr;
    QPushButton* m_resetButton = nullptr;

    /// 后台抓屏线程：独立完成「截屏 + 模拟变换」，UI 线程只负责贴图。
    /// 当前系统不支持时保持为空，退回 UI 线程同步抓屏。
    ScreenLensCapture* m_lensCapture = nullptr;
    /// 回退路径的截取定时器（后台抓屏可用时不启用）
    QTimer* m_refreshTimer = nullptr;
    /// 排除标志的设置重试定时器：重开窗口时 HWND 晚于 showEvent 才建好
    QTimer* m_exclusionRetryTimer = nullptr;
    QImage m_screenFrame;
    /// m_screenFrame 在内容区坐标系中的位置（窗口跨屏时只覆盖一部分）
    QRect m_screenFrameRect;
    /// m_screenFrame 对应的屏幕全局矩形，用于判断是否需要重新抓取
    QRect m_screenFrameContentGlobal;
    /// 上次抓取时刻；同一位置连续重绘时跳过重复截屏
    QElapsedTimer m_lastCaptureTimer;
    /// 上次收到后台抓屏帧的时刻，用于发现后台线程卡住时兜底
    QElapsedTimer m_lastFrameTimer;
    bool m_screenFrameValid = false;
    /// 已经拿到有效 HWND 真正尝试过设置（只有这种情况失败才算「系统不支持」）
    bool m_screenExclusionChecked = false;
    bool m_screenExclusionOk = false;

    /// 模拟输出的复用缓冲，避免拖动时每帧重新分配整幅图像
    QImage m_simulatedFrame;

    int m_sourceMode = ScreenLens;

    // 拖动 / 缩放状态
    bool m_dragging = false;
    bool m_resizing = false;
    int m_resizeEdges = EdgeNone;
    QPoint m_dragOffset;
    QPoint m_resizeStartGlobal;
    QRect m_resizeStartGeometry;
};

}  // namespace rawui
