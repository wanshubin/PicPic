// ============================================================================
//  IccSimulatorWindow.cpp —— 悬浮式 ICC / 显示效果模拟窗口实现
//
//  设计要点：
//   - 窗口无标题栏，只画一圈 2px 边框；边框内是「被模拟后的画面」。
//   - 镜片模式截取窗口下方的屏幕内容：先调 SetWindowDisplayAffinity 把
//     本窗口排除在屏幕截取之外，再用 QScreen::grabWindow（Windows 上走
//     BitBlt）抓取同一块区域，得到的就是窗口下面原本的画面，不会自拍。
//   - 参数变换只在本窗口内进行，不接触主预览、渲染管线与导出。
// ============================================================================

#include "IccSimulatorWindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QMutex>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
/// Windows 10 2004 起支持：把窗口从屏幕截取结果中排除（旧 SDK 头文件里没有）
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace rawui {

namespace {

/// 窗口边框宽度（像素）
constexpr int kBorderWidth = 2;
/// 左上 / 右上浮动按钮的尺寸与边距
constexpr int kControlSize = 26;
constexpr int kControlMargin = 8;
/// 距边框多少像素以内算作缩放热区
constexpr int kResizeMargin = 6;
/// 屏幕镜片的刷新间隔：静止 60fps，拖动 / 缩放时 125fps。
/// 拖动时不再在鼠标事件里同步抓屏，避免输入被截屏阻塞；高刷新由定时器驱动。
constexpr int kIdleRefreshIntervalMs = 16;
constexpr int kInteractionRefreshIntervalMs = 8;
/// 同一位置的重绘在这个间隔内不再重复截屏，避免一次拖动触发两次抓取
constexpr int kCaptureThrottleMs = 4;
/// 后台抓屏的看门狗：每 250ms 检查一次是否还有新帧送来
constexpr int kLensWatchdogIntervalMs = 250;
/// 超过这个时间没收到新帧，就认为后台抓屏卡住，需要在绘制时同步兜底
constexpr int kLensFrameStaleMs = 400;
/// 重开窗口时原生窗口要等到 showEvent 之后才建好，这段间隔内每隔一小会儿
/// 重试一次排除设置；句柄已经有效但设置失败，则按「可能不支持」放慢重试
constexpr int kExclusionRetryWindowMs = 16;
constexpr int kExclusionRetryUnsupportedMs = 500;
/// 参数面板宽度
constexpr int kPanelWidth = 300;

/// 浮在画面上的圆形按钮：深色半透明底，保证在任何画面内容上都看得清
QString overlayButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: rgba(20, 20, 20, 200);"
        " border: 1px solid rgba(255, 255, 255, 70);"
        " border-radius: 4px; padding: 0; }"
        "QPushButton:hover { background: rgba(62, 62, 62, 230);"
        " border-color: rgba(255, 255, 255, 130); }"
        "QPushButton:pressed { background: rgba(12, 12, 12, 235); }"
        "QPushButton:checked { background: rgba(58, 86, 111, 235);"
        " border-color: #7fb3e8; }");
}

/// 参数面板滑块：双击恢复构造时给定的默认值。
/// 重置后的标签刷新、画面重绘与配置落盘通过回调交给窗口处理。
class ResetOnDoubleClickSlider : public QSlider
{
public:
    explicit ResetOnDoubleClickSlider(int defaultValue, QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
        , m_defaultValue(defaultValue)
    {
    }

    void setResetHandler(std::function<void()> handler)
    {
        m_resetHandler = std::move(handler);
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            setValue(m_defaultValue);
            if (m_resetHandler) {
                m_resetHandler();
            }
            event->accept();
            return;
        }
        QSlider::mouseDoubleClickEvent(event);
    }

private:
    int m_defaultValue = 0;
    std::function<void()> m_resetHandler;
};

/// 参数面板里的普通按钮（重置）
QString panelButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: #333333; border: 1px solid #474747;"
        " border-radius: 3px; color: #dcdcdc; padding: 3px 10px; }"
        "QPushButton:hover { background: #3d3d3d; border-color: #5a5a5a; }"
        "QPushButton:pressed { background: #2a2a2a; }");
}

/// 把 0~255 的浮点通道值四舍五入并钳位
inline int toByte(float value)
{
    const int rounded = static_cast<int>(value + 0.5f);
    return rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded);
}

/// 色温 → sRGB 近似色（Tanner Helland 的经典拟合）。
/// 只用于观感模拟，不追求色度学精度；6500K 作为参考白点。
void kelvinToSrgb(double kelvin, double& r, double& g, double& b)
{
    const double temperature = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (temperature <= 66.0) {
        red = 255.0;
        green = 99.4708025861 * std::log(temperature) - 161.1195681661;
        blue = temperature <= 19.0
            ? 0.0
            : 138.5177312231 * std::log(temperature - 10.0) - 305.0447927307;
    } else {
        red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
        green = 288.1221695283 * std::pow(temperature - 60.0, -0.0755148492);
        blue = 255.0;
    }
    r = std::clamp(red, 0.0, 255.0) / 255.0;
    g = std::clamp(green, 0.0, 255.0) / 255.0;
    b = std::clamp(blue, 0.0, 255.0) / 255.0;
}

/// 色温对应的 R/G/B 增益（相对 6500K），并限制在温和的范围内，
/// 避免色温滑块到头时把某个通道直接压成 0（看起来就是偏色而不是色温）。
void temperatureGains(int kelvin, double& r, double& g, double& b)
{
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double refRed = 0.0;
    double refGreen = 0.0;
    double refBlue = 0.0;
    kelvinToSrgb(kelvin, red, green, blue);
    kelvinToSrgb(6500.0, refRed, refGreen, refBlue);

    r = refRed > 1e-6 ? std::clamp(red / refRed, 0.4, 2.0) : 1.0;
    g = refGreen > 1e-6 ? std::clamp(green / refGreen, 0.4, 2.0) : 1.0;
    b = refBlue > 1e-6 ? std::clamp(blue / refBlue, 0.4, 2.0) : 1.0;
}

/// 按参数把 source 变换后写入 target（target 复用，避免每帧重新分配内存）。
/// 变换顺序：曝光（亮度）→ 对比度 → 色温通道增益 → 伽马 → 饱和度 → 钳位。
/// 既被 UI 线程（色卡 / 照片）使用，也被后台抓屏线程使用，因此不依赖任何成员状态。
void applySimulationTo(const QImage& source, const IccSimParams& params,
                       QImage& target)
{
    const QImage rgb = source.format() == QImage::Format_RGB32
                           ? source
                           : source.convertToFormat(QImage::Format_RGB32);
    if (rgb.isNull()) {
        target = QImage();
        return;
    }

    if (target.size() != rgb.size()
        || target.format() != QImage::Format_RGB32) {
        target = QImage(rgb.size(), QImage::Format_RGB32);
    }
    if (target.isNull()) {
        target = QImage();
        return;
    }
    target.setDevicePixelRatio(rgb.devicePixelRatio());
    // 并行写各行之前先脱离共享数据，避免多个线程同时触发 detach
    target.detach();

    // 亮度按曝光倍数工作：滑块 ±100 约 ±0.8EV，并用 1.4 次幂软化小幅度调节。
    // 旧的 ±0.5 线性偏移过于激进，深色界面稍微往左一点就会整块变成纯黑。
    const double brightnessSlider = params.brightness / 100.0;
    const double exposure = std::pow(
        2.0, std::copysign(0.8 * std::pow(std::abs(brightnessSlider), 1.4),
                           brightnessSlider));
    const double contrast = params.contrast / 100.0;
    const double gamma = params.gamma / 100.0;
    double gainRed = 1.0;
    double gainGreen = 1.0;
    double gainBlue = 1.0;
    temperatureGains(params.temperature, gainRed, gainGreen, gainBlue);

    // 每通道 LUT：亮度（曝光）→ 对比度 → 通道增益 → 伽马
    unsigned char lutRed[256];
    unsigned char lutGreen[256];
    unsigned char lutBlue[256];
    const auto buildLut = [exposure, contrast, gamma](double gain,
                                                      unsigned char* lut) {
        for (int value = 0; value < 256; ++value) {
            double normalized = value / 255.0;
            normalized *= exposure;
            normalized = (normalized - 0.5) * contrast + 0.5;
            normalized *= gain;
            normalized = std::clamp(normalized, 0.0, 1.0);
            normalized = std::pow(normalized, gamma);
            lut[value] = static_cast<unsigned char>(
                std::clamp(std::round(normalized * 255.0), 0.0, 255.0));
        }
    };
    buildLut(gainRed, lutRed);
    buildLut(gainGreen, lutGreen);
    buildLut(gainBlue, lutBlue);

    const float saturation = params.saturation / 100.0f;
    const bool applySaturation = saturation != 1.0f;

    const int width = rgb.width();
    const int height = rgb.height();

    // 逐行处理，行与行互不依赖。
    // 只有大图才交给 OpenMP：镜片窗口这种几十万像素的小图，串行只要 1~2ms，
    // 而 MSVC 的 OpenMP 运行时在线程空闲后会自旋等待，帧率高时会白白占满 CPU。
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static) if (width * height >= 1000000)
#endif
    for (int y = 0; y < height; ++y) {
        const auto* sourceLine =
            reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        auto* targetLine =
            reinterpret_cast<QRgb*>(target.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = sourceLine[x];
            float red = lutRed[qRed(pixel)];
            float green = lutGreen[qGreen(pixel)];
            float blue = lutBlue[qBlue(pixel)];

            // 饱和度：按 sRGB 感知亮度在彩色与灰度之间混合。
            // 0% 即完全灰度，所以不再需要单独的「灰度」参数。
            if (applySaturation) {
                const float luma = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
                red = luma + (red - luma) * saturation;
                green = luma + (green - luma) * saturation;
                blue = luma + (blue - luma) * saturation;
            }

            targetLine[x] = qRgb(toByte(red), toByte(green), toByte(blue));
        }
    }
}

}  // namespace

#ifdef Q_OS_WIN
/// 后台镜片抓屏线程。
///
/// 把「抓取窗口下方的屏幕 → 施加模拟变换」整条链路放到独立线程：
///  - BitBlt 直接从桌面 DC 读取像素，不需要 UI 线程参与；
///  - 模拟变换也在这里完成，UI 线程每帧只把现成的位图贴到窗口上；
///  - 抓屏节奏完全由 BitBlt 的速度决定（本机约 4ms 一帧），拖动时不会被
///    同步截屏卡住，视觉上接近显示器刷新率。
///
/// 内容区位置每帧用 GetWindowRect 现取，窗口拖动时自动跟随。
class ScreenLensCapture : public QThread {
public:
    explicit ScreenLensCapture(IccSimulatorWindow* owner)
        : m_owner(owner)
        , m_hwnd(reinterpret_cast<HWND>(owner->winId()))
    {
    }

    ~ScreenLensCapture() override
    {
        requestStop();
        wait();
    }

    /// 打开 / 关闭抓屏（关闭时线程休眠，不占用 CPU）
    void setActive(bool active)
    {
        m_active.store(active, std::memory_order_relaxed);
    }

    void setParameters(const IccSimParams& params)
    {
        m_brightness.store(params.brightness, std::memory_order_relaxed);
        m_contrast.store(params.contrast, std::memory_order_relaxed);
        m_saturation.store(params.saturation, std::memory_order_relaxed);
        m_temperature.store(params.temperature, std::memory_order_relaxed);
        m_gamma.store(params.gamma, std::memory_order_relaxed);
    }

    void setDevicePixelRatio(double ratio)
    {
        m_pixelRatio.store(ratio, std::memory_order_relaxed);
    }

    void requestStop() { m_stop.store(true, std::memory_order_relaxed); }

    /// 取走最新一帧（UI 线程调用）；没有新帧时返回空图
    QImage takePendingFrame()
    {
        QMutexLocker locker(&m_frameMutex);
        QImage frame = std::move(m_pendingFrame);
        m_pendingFrame = QImage();
        m_deliveryPosted = false;
        return frame;
    }

protected:
    void run() override
    {
        HDC screenDc = ::GetDC(nullptr);
        HDC memoryDc = ::CreateCompatibleDC(screenDc);
        HBITMAP dib = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        void* dibBits = nullptr;
        int frameWidth = 0;
        int frameHeight = 0;
        QImage scratch;

        while (!m_stop.load(std::memory_order_relaxed)) {
            if (!m_active.load(std::memory_order_relaxed)) {
                QThread::msleep(8);
                continue;
            }

            RECT windowRect;
            if (!::IsWindow(m_hwnd) || !::IsWindowVisible(m_hwnd)
                || !::GetWindowRect(m_hwnd, &windowRect)) {
                QThread::msleep(8);
                continue;
            }

            // 每次抓屏前确认「本窗口不参与截屏」仍然生效。万一系统把排除标志
            // 丢了而我们还照抓不误，就会拍到自己，画面会冻结或参数越调越黑。
            DWORD affinity = 0;
            if (!::GetWindowDisplayAffinity(m_hwnd, &affinity)
                || affinity != WDA_EXCLUDEFROMCAPTURE) {
                QThread::msleep(8);
                continue;
            }

            const double ratio =
                std::max(1.0, m_pixelRatio.load(std::memory_order_relaxed));
            const int inset = qMax(1, qRound(kBorderWidth * ratio));
            const int width =
                int(windowRect.right - windowRect.left) - inset * 2;
            const int height =
                int(windowRect.bottom - windowRect.top) - inset * 2;
            if (width <= 0 || height <= 0) {
                QThread::msleep(8);
                continue;
            }

            // 尺寸变化时重建 DIB；负高度表示自上而下，与 QImage 的行序一致
            if (dib == nullptr || width != frameWidth || height != frameHeight) {
                if (dib != nullptr) {
                    ::SelectObject(memoryDc, oldBitmap);
                    ::DeleteObject(dib);
                    dib = nullptr;
                }
                BITMAPINFO info = {};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                dib = ::CreateDIBSection(screenDc, &info, DIB_RGB_COLORS,
                                         &dibBits, nullptr, 0);
                if (dib == nullptr) {
                    QThread::msleep(16);
                    continue;
                }
                oldBitmap = ::SelectObject(memoryDc, dib);
                frameWidth = width;
                frameHeight = height;
            }

            if (!::BitBlt(memoryDc, 0, 0, width, height, screenDc,
                          int(windowRect.left) + inset,
                          int(windowRect.top) + inset,
                          SRCCOPY | CAPTUREBLT)) {
                QThread::msleep(4);
                continue;
            }

            // 直接以 DIB 内存构造只读视图，省掉一次整图拷贝
            const QImage view(static_cast<const uchar*>(dibBits), width, height,
                              width * 4, QImage::Format_RGB32);
            const IccSimParams params = currentParams();
            if (params.isIdentity()) {
                deliverFrame(view.copy());
            } else {
                applySimulationTo(view, params, scratch);
                if (!scratch.isNull()) {
                    deliverFrame(scratch.copy());
                }
            }
        }

        if (dib != nullptr) {
            ::SelectObject(memoryDc, oldBitmap);
            ::DeleteObject(dib);
        }
        ::DeleteDC(memoryDc);
        ::ReleaseDC(nullptr, screenDc);
    }

private:
    IccSimParams currentParams() const
    {
        IccSimParams params;
        params.brightness = m_brightness.load(std::memory_order_relaxed);
        params.contrast = m_contrast.load(std::memory_order_relaxed);
        params.saturation = m_saturation.load(std::memory_order_relaxed);
        params.temperature = m_temperature.load(std::memory_order_relaxed);
        params.gamma = m_gamma.load(std::memory_order_relaxed);
        return params;
    }

    /// 只保留最新一帧；UI 线程来不及处理时直接覆盖，不会积压
    void deliverFrame(QImage frame)
    {
        bool needPost = false;
        {
            QMutexLocker locker(&m_frameMutex);
            m_pendingFrame = std::move(frame);
            if (!m_deliveryPosted) {
                m_deliveryPosted = true;
                needPost = true;
            }
        }
        if (needPost) {
            QMetaObject::invokeMethod(
                m_owner, [owner = m_owner]() { owner->onScreenFrameReady(); },
                Qt::QueuedConnection);
        }
    }

    IccSimulatorWindow* m_owner = nullptr;
    HWND m_hwnd = nullptr;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stop{false};
    std::atomic<double> m_pixelRatio{1.0};
    std::atomic<int> m_brightness{0};
    std::atomic<int> m_contrast{100};
    std::atomic<int> m_saturation{100};
    std::atomic<int> m_temperature{6500};
    std::atomic<int> m_gamma{100};

    QMutex m_frameMutex;
    QImage m_pendingFrame;
    bool m_deliveryPosted = false;
};
#endif  // Q_OS_WIN

IccSimulatorWindow::IccSimulatorWindow(QWidget* parent)
    : QWidget(parent)
{
    // Tool 窗口跟随父窗口工作，不会单独出现在任务栏里；无边框 + 自绘边框。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setWindowTitle(QStringLiteral("ICC 模拟器"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
    // 整窗自绘：不让系统先擦一遍背景，拖动时少一次全窗填充，避免闪烁
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    buildUi();
    m_lastCaptureTimer.start();
    m_lastFrameTimer.start();

    // 默认摆在父窗口中间；loadSettings 里有保存过的位置时会覆盖
    QSize initialSize(760, 560);
    initialSize = initialSize.expandedTo(minimumSize());
    QRect geometry(QPoint(0, 0), initialSize);
    if (parent != nullptr) {
        geometry.moveCenter(parent->frameGeometry().center());
    } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
        geometry.moveCenter(screen->availableGeometry().center());
    }
    setGeometry(geometry);

    loadSettings();
    updateValueLabels();
}

IccSimulatorWindow::~IccSimulatorWindow()
{
    // 后台线程的析构会先请求停止再等待退出，最多阻塞一帧（约 5ms）
    delete m_lensCapture;
    m_lensCapture = nullptr;
}

// ===========================================================================
//  界面搭建
// ===========================================================================

void IccSimulatorWindow::buildUi()
{
    // ---- 左上角：设置参数面板开关 ----
    m_settingsButton = new QPushButton(this);
    m_settingsButton->setCheckable(true);
    m_settingsButton->setFixedSize(kControlSize, kControlSize);
    m_settingsButton->setIcon(QIcon(QStringLiteral(":/icons/settings.svg")));
    m_settingsButton->setIconSize(QSize(15, 15));
    m_settingsButton->setFocusPolicy(Qt::NoFocus);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setStyleSheet(overlayButtonStyle());
    m_settingsButton->setToolTip(
        QStringLiteral("ICC 模拟参数：亮度、对比度、饱和度、色温、伽马\n"
                       "点击展开面板（Esc 收起）"));
    m_settingsButton->setAccessibleName(QStringLiteral("ICC 模拟参数"));

    // ---- 右上角：关闭 ----
    m_closeButton = new QPushButton(this);
    m_closeButton->setFixedSize(kControlSize, kControlSize);
    m_closeButton->setIcon(QIcon(QStringLiteral(":/icons/remove.svg")));
    m_closeButton->setIconSize(QSize(14, 14));
    m_closeButton->setFocusPolicy(Qt::NoFocus);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setStyleSheet(overlayButtonStyle());
    m_closeButton->setToolTip(QStringLiteral("关闭 ICC 模拟器（Esc）"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭 ICC 模拟器"));
    // 用 hide 而不是 close：close() 会让 Qt 销毁原生窗口，下次打开时
    // HWND 要等下一轮事件循环才真正建好，截取排除和抓屏都会失败（黑屏）。
    connect(m_closeButton, &QPushButton::clicked, this, &QWidget::hide);

    // ---- 参数面板 ----
    m_settingsPanel = buildSettingsPanel();
    m_settingsPanel->hide();
    connect(m_settingsButton, &QPushButton::toggled, m_settingsPanel,
            &QWidget::setVisible);

    // ---- 屏幕镜片刷新定时器 ----
    // 后台抓屏可用时它只当看门狗：后台线程长时间不送帧就主动重绘一次，
    // paintEvent 里会退回同步截屏，避免画面永远冻住；
    // 后台抓屏不可用时它负责按 60 / 125fps 驱动重绘。
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setTimerType(Qt::PreciseTimer);
    m_refreshTimer->setInterval(kIdleRefreshIntervalMs);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (m_lensCapture != nullptr) {
            if (m_lastFrameTimer.elapsed() > kLensFrameStaleMs) {
                update();
            }
            return;
        }
        update();
    });

    // ---- 截取排除的重试定时器 ----
    // 关掉再打开时，Qt 会销毁并重建原生窗口，showEvent 里拿到的还是旧句柄，
    // SetWindowDisplayAffinity 只会返回 ERROR_INVALID_WINDOW_HANDLE；
    // 等新句柄建好后再补设一次，避免误判成「系统不支持」。
    m_exclusionRetryTimer = new QTimer(this);
    m_exclusionRetryTimer->setSingleShot(true);
    connect(m_exclusionRetryTimer, &QTimer::timeout, this,
            &IccSimulatorWindow::retryScreenExclusion);

    layoutOverlays();

    // 窗口最小尺寸要保证参数面板完整可见
    const int panelHeight = m_settingsPanel->sizeHint().height();
    setMinimumSize(qMax(340, kPanelWidth + 2 * kControlMargin),
                   2 * kControlMargin + kControlSize + 6 + panelHeight + kControlMargin);
}

QWidget* IccSimulatorWindow::buildSettingsPanel()
{
    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("IccSimSettingsPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    // 点击面板空白处不应冒泡到父窗口，否则会误触发拖动窗口
    panel->setAttribute(Qt::WA_NoMousePropagation, true);
    panel->setCursor(Qt::ArrowCursor);
    panel->setStyleSheet(QStringLiteral(
        "QFrame#IccSimSettingsPanel { background: rgba(28, 28, 28, 246);"
        " border: 1px solid #5f5f5f; border-radius: 8px; }"));
    panel->setFixedWidth(kPanelWidth);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(7);

    // ---- 标题行 ----
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("ICC 模拟"), panel);
    title->setStyleSheet(
        QStringLiteral("color: #e6e6e6; font-weight: 600; background: transparent;"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    auto* scopeHint = new QLabel(QStringLiteral("仅作用于本窗口"), panel);
    scopeHint->setStyleSheet(
        QStringLiteral("color: #7a7a7a; background: transparent;"));
    titleRow->addWidget(scopeHint);
    layout->addLayout(titleRow);

    // ---- 画面来源 ----
    auto* sourceRow = new QHBoxLayout;
    sourceRow->setSpacing(8);
    auto* sourceLabel = new QLabel(QStringLiteral("画面"), panel);
    sourceLabel->setFixedWidth(38);
    sourceLabel->setStyleSheet(
        QStringLiteral("color: #c8c8c8; background: transparent;"));
    sourceRow->addWidget(sourceLabel);

    m_sourceCombo = new QComboBox(panel);
    m_sourceCombo->addItem(QStringLiteral("实时屏幕（镜片）"), int(ScreenLens));
    m_sourceCombo->addItem(QStringLiteral("测试色卡"), int(TestChart));
    m_sourceCombo->setCursor(Qt::ArrowCursor);
    m_sourceCombo->setFocusPolicy(Qt::NoFocus);
    m_sourceCombo->setToolTip(
        QStringLiteral("实时屏幕：截取窗口下方的屏幕内容，拖到主界面上即可模拟\n"
                       "测试色卡：24 色块 + 灰阶 + RGB 渐变"));
    sourceRow->addWidget(m_sourceCombo, 1);
    layout->addLayout(sourceRow);

    auto* divider = new QWidget(panel);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: #3a3a3a;"));
    layout->addWidget(divider);

    // ---- 参数滑块 ----
    auto addSliderRow =
        [this, panel, layout](const QString& name, int minimum, int maximum, int value,
                              QSlider** sliderOut, QLabel** valueOut) {
            auto* row = new QWidget(panel);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);

            auto* nameLabel = new QLabel(name, row);
            nameLabel->setFixedWidth(38);
            nameLabel->setStyleSheet(
                QStringLiteral("color: #c8c8c8; background: transparent;"));
            rowLayout->addWidget(nameLabel);

            auto* slider = new ResetOnDoubleClickSlider(value, row);
            slider->setRange(minimum, maximum);
            slider->setValue(value);
            slider->setFocusPolicy(Qt::NoFocus);
            slider->setCursor(Qt::ArrowCursor);
            rowLayout->addWidget(slider, 1);

            auto* valueLabel = new QLabel(row);
            valueLabel->setFixedWidth(52);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            valueLabel->setStyleSheet(
                QStringLiteral("color: #dcdcdc; background: transparent;"));
            rowLayout->addWidget(valueLabel);

            layout->addWidget(row);
            *sliderOut = slider;
            *valueOut = valueLabel;

            connect(slider, &QSlider::valueChanged, this, [this]() {
                // updateValueLabels 里会同步参数给后台抓屏线程
                updateValueLabels();
                // 参数改变立即重绘；屏幕镜片模式下下一帧截取也会带上新参数
                update();
            });
            // 拖动过程中不反复写配置，松手时落盘一次
            connect(slider, &QSlider::sliderReleased,
                    this, &IccSimulatorWindow::saveSettings);
            // 双击恢复默认值：刷新标签、画面并立即保存（valueChanged 只在
            // 值确实变化时触发，这里统一兜底一次）
            slider->setResetHandler([this]() {
                updateValueLabels();
                update();
                saveSettings();
            });
        };

    // 亮度按曝光倍数工作（见 applySimulation），对比度 / 伽马用温和的百分比范围，
    // 避免滑块稍微一动画面就整个压黑或过曝。
    // 范围保持对称的 ±100：0 居中，正负方向都能调节约 ±0.8EV。
    // 不再单列「灰度」：饱和度 0% 本身就是完全灰度，两者是同一件事。
    addSliderRow(QStringLiteral("亮度"), -100, 100, 0,
                 &m_brightnessSlider, &m_brightnessValue);
    addSliderRow(QStringLiteral("对比度"), 50, 150, 100,
                 &m_contrastSlider, &m_contrastValue);
    addSliderRow(QStringLiteral("饱和度"), 0, 200, 100,
                 &m_saturationSlider, &m_saturationValue);
    addSliderRow(QStringLiteral("色温"), 2000, 12000, 6500,
                 &m_temperatureSlider, &m_temperatureValue);
    addSliderRow(QStringLiteral("伽马"), 50, 200, 100,
                 &m_gammaSlider, &m_gammaValue);
    m_saturationSlider->setToolTip(
        QStringLiteral("0% 为完全灰度，100% 为原始饱和度，200% 为双倍"));

    // ---- 底部：重置 ----
    auto* footerRow = new QHBoxLayout;
    footerRow->setSpacing(8);
    auto* footerHint = new QLabel(
        QStringLiteral("拖动窗口移动，拖边框缩放；双击滑块恢复默认"), panel);
    footerHint->setStyleSheet(
        QStringLiteral("color: #7a7a7a; background: transparent;"));
    footerHint->setWordWrap(true);
    footerRow->addWidget(footerHint, 1);

    m_resetButton = new QPushButton(QStringLiteral("重置"), panel);
    m_resetButton->setFocusPolicy(Qt::NoFocus);
    m_resetButton->setCursor(Qt::PointingHandCursor);
    m_resetButton->setStyleSheet(panelButtonStyle());
    m_resetButton->setToolTip(QStringLiteral("把全部模拟参数恢复为默认值"));
    footerRow->addWidget(m_resetButton, 0, Qt::AlignVCenter);
    layout->addLayout(footerRow);

    // ---- 信号 ----
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_sourceMode = index;
        if (m_sourceMode == ScreenLens && isVisible()) {
            ensureScreenExclusion();
        }
        updateScreenCaptureState();
        update();
        saveSettings();
    });

    connect(m_resetButton, &QPushButton::clicked, this, [this]() {
        const QSignalBlocker blockBrightness(m_brightnessSlider);
        const QSignalBlocker blockContrast(m_contrastSlider);
        const QSignalBlocker blockSaturation(m_saturationSlider);
        const QSignalBlocker blockTemperature(m_temperatureSlider);
        const QSignalBlocker blockGamma(m_gammaSlider);
        m_brightnessSlider->setValue(0);
        m_contrastSlider->setValue(100);
        m_saturationSlider->setValue(100);
        m_temperatureSlider->setValue(6500);
        m_gammaSlider->setValue(100);
        updateValueLabels();
        update();
        saveSettings();
    });

    return panel;
}

void IccSimulatorWindow::layoutOverlays()
{
    if (m_settingsButton != nullptr) {
        m_settingsButton->move(kControlMargin, kControlMargin);
    }
    if (m_closeButton != nullptr) {
        m_closeButton->move(width() - kControlMargin - kControlSize, kControlMargin);
    }
    if (m_settingsPanel != nullptr) {
        m_settingsPanel->adjustSize();
        m_settingsPanel->move(kControlMargin,
                              kControlMargin + kControlSize + 6);
    }
}

void IccSimulatorWindow::updateValueLabels()
{
    if (m_brightnessSlider == nullptr) {
        return;
    }
    const int brightness = m_brightnessSlider->value();
    m_brightnessValue->setText(brightness > 0
                                   ? QStringLiteral("+%1").arg(brightness)
                                   : QString::number(brightness));
    m_contrastValue->setText(QStringLiteral("%1%").arg(m_contrastSlider->value()));
    m_saturationValue->setText(QStringLiteral("%1%").arg(m_saturationSlider->value()));
    m_temperatureValue->setText(QStringLiteral("%1K").arg(m_temperatureSlider->value()));
    m_gammaValue->setText(
        QString::number(m_gammaSlider->value() / 100.0, 'f', 2));

#ifdef Q_OS_WIN
    // 参数变化后立刻通知后台抓屏线程，重置按钮等直接改值的路径也能覆盖到
    if (m_lensCapture != nullptr) {
        m_lensCapture->setParameters(currentParams());
    }
#endif
}

IccSimParams IccSimulatorWindow::currentParams() const
{
    IccSimParams params;
    params.brightness = m_brightnessSlider->value();
    params.contrast = m_contrastSlider->value();
    params.saturation = m_saturationSlider->value();
    params.temperature = m_temperatureSlider->value();
    params.gamma = m_gammaSlider->value();
    return params;
}

QRect IccSimulatorWindow::contentRect() const
{
    return rect().adjusted(kBorderWidth, kBorderWidth,
                           -kBorderWidth, -kBorderWidth);
}

QRect IccSimulatorWindow::contentGlobalRect() const
{
    const QRect content = contentRect();
    return QRect(mapToGlobal(content.topLeft()), content.size());
}

// ===========================================================================
//  绘制
// ===========================================================================

void IccSimulatorWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    // 镜片模式取帧：
    //  - 后台抓屏不可用：就在绘制前同步抓屏（老路径）；
    //  - 后台抓屏可用：直接用线程送来的最新帧；只有当它超过 100ms 没送帧时，
    //    才临时退回同步抓屏，避免窗口拖动等场景下画面冻结。
    if (m_sourceMode == ScreenLens) {
        if (m_lensCapture == nullptr) {
            const QRect globalContent = contentGlobalRect();
            const bool areaChanged = !m_screenFrameValid
                || m_screenFrameContentGlobal != globalContent;
            if (areaChanged
                || m_lastCaptureTimer.elapsed() >= kCaptureThrottleMs) {
                captureScreenFrame();
            }
        } else if (!m_screenFrameValid
                   || m_lastFrameTimer.elapsed() > kLensFrameStaleMs) {
            captureScreenFrame();
        }
    }

    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x18, 0x18, 0x18));

    const QRect content = contentRect();
    if (content.width() <= 0 || content.height() <= 0) {
        return;
    }

    // 内容区：当前来源 + 模拟参数
    const QImage source = renderSourceImage();
    // 后台抓屏线程送来的帧已经施加过模拟参数，这里不能再算一次
    const bool lensFromWorker =
        (m_sourceMode == ScreenLens && m_lensCapture != nullptr);
    const QImage simulated = lensFromWorker ? source : applySimulation(source);
    // 像素尺寸正好对上时按 1:1 贴图，关掉平滑采样能省一笔每帧开销
    const qreal dpr = devicePixelRatioF();
    const bool exactSize =
        simulated.size() == QSize(qRound(content.width() * dpr),
                                  qRound(content.height() * dpr));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !exactSize);
    painter.drawImage(content, simulated);

    // 只保留一圈边框，不加标题栏
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0x9a, 0x9a, 0x9a), kBorderWidth));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));

    // 提示文字（贴着内容区中央，不参与模拟，保证可读）
    QString message;
    if (m_sourceMode == ScreenLens) {
        if (m_screenExclusionChecked && !m_screenExclusionOk) {
            message = QStringLiteral("当前系统不支持把窗口排除在屏幕截取之外");
        } else if (!m_screenFrameValid) {
            message = QStringLiteral("正在截取屏幕…");
        }
    }

    if (!message.isEmpty()) {
        QFont messageFont = font();
        messageFont.setPixelSize(13);
        painter.setFont(messageFont);
        const QFontMetrics metrics(messageFont);
        QRect box(0, 0, metrics.horizontalAdvance(message) + 26,
                  metrics.height() + 14);
        box.moveCenter(content.center());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 180));
        painter.drawRoundedRect(box, 6, 6);
        painter.setPen(QColor(0xe2, 0xe2, 0xe2));
        painter.drawText(box, Qt::AlignCenter, message);
    }
}

QImage IccSimulatorWindow::renderSourceImage() const
{
    const QRect content = contentRect();
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(qMax(1, qRound(content.width() * dpr)),
                          qMax(1, qRound(content.height() * dpr)));

    // 镜片模式最常见的路径：抓取结果正好覆盖内容区，像素尺寸也一致，
    // 直接返回抓取图，省掉一次整图绘制与内存分配（拖动时差异很明显）
    if (m_sourceMode == ScreenLens && m_screenFrameValid
        && !m_screenFrame.isNull()
        && m_screenFrameRect.size() == content.size()
        && m_screenFrame.size() == pixelSize
        && qFuzzyCompare(m_screenFrame.devicePixelRatio(), dpr)) {
        return m_screenFrame;
    }

    QImage canvas(pixelSize, QImage::Format_RGB32);
    // 标好 DPR 后，后面按逻辑尺寸 drawImage 是 1:1，避免每帧都做一次重采样
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(QColor(0x1b, 0x1b, 0x1b));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.scale(dpr, dpr);

    const QRectF area(0, 0, content.width(), content.height());

    switch (m_sourceMode) {
    case ScreenLens:
        if (m_screenFrameValid && !m_screenFrame.isNull()) {
            painter.drawImage(QRectF(m_screenFrameRect), m_screenFrame);
        }
        break;

    case TestChart:
        drawTestChart(painter, area);
        break;
    }

    painter.end();
    return canvas;
}

void IccSimulatorWindow::drawTestChart(QPainter& painter, const QRectF& area) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);

    // 近似 ColorChecker 的 24 个色块（sRGB 参考值）
    static const QRgb kPatches[24] = {
        qRgb(115, 82, 68),   qRgb(194, 150, 130), qRgb(98, 122, 157),
        qRgb(87, 108, 67),   qRgb(133, 128, 177), qRgb(103, 189, 170),
        qRgb(214, 126, 44),  qRgb(80, 91, 166),   qRgb(193, 90, 99),
        qRgb(94, 60, 108),   qRgb(157, 188, 64),  qRgb(224, 163, 46),
        qRgb(56, 61, 150),   qRgb(70, 148, 73),   qRgb(175, 54, 60),
        qRgb(231, 199, 31),  qRgb(187, 86, 149),  qRgb(8, 133, 161),
        qRgb(243, 243, 242), qRgb(200, 200, 200), qRgb(160, 160, 160),
        qRgb(122, 122, 121), qRgb(85, 85, 85),    qRgb(52, 52, 52),
    };

    const qreal pad = qMax(8.0, qMin(area.width(), area.height()) * 0.035);
    const qreal gap = qMax(2.0, qMin(area.width(), area.height()) * 0.008);
    const QRectF inner = area.adjusted(pad, pad, -pad, -pad);

    // 上方 56%：6 × 4 色块
    const QRectF patchArea(inner.left(), inner.top(), inner.width(),
                           inner.height() * 0.56);
    constexpr int kColumns = 6;
    constexpr int kRows = 4;
    const qreal cellWidth = (patchArea.width() - gap * (kColumns - 1)) / kColumns;
    const qreal cellHeight = (patchArea.height() - gap * (kRows - 1)) / kRows;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const QRectF cell(patchArea.left() + column * (cellWidth + gap),
                              patchArea.top() + row * (cellHeight + gap),
                              cellWidth, cellHeight);
            painter.fillRect(cell, QColor::fromRgb(kPatches[row * kColumns + column]));
        }
    }

    // 中间：连续灰阶（看亮度 / 对比度 / 伽马）
    const QRectF grayRect(inner.left(), patchArea.bottom() + gap * 2,
                          inner.width(), inner.height() * 0.16);
    QLinearGradient gray(grayRect.topLeft(), grayRect.topRight());
    gray.setColorAt(0.0, QColor(0, 0, 0));
    gray.setColorAt(1.0, QColor(255, 255, 255));
    painter.fillRect(grayRect, gray);

    // 下方：R / G / B 三条渐变（看饱和度 / 色温偏色）
    const qreal rampsTop = grayRect.bottom() + gap * 2;
    const qreal rampsHeight = inner.bottom() - rampsTop;
    const qreal rampHeight = (rampsHeight - gap * 2) / 3.0;
    const QColor primaries[3] = {QColor(255, 0, 0), QColor(0, 255, 0),
                                 QColor(0, 0, 255)};
    for (int index = 0; index < 3; ++index) {
        const QRectF ramp(inner.left(), rampsTop + index * (rampHeight + gap),
                          inner.width(), rampHeight);
        QLinearGradient gradient(ramp.topLeft(), ramp.topRight());
        gradient.setColorAt(0.0, QColor(0, 0, 0));
        gradient.setColorAt(0.5, primaries[index]);
        gradient.setColorAt(1.0, QColor(255, 255, 255));
        painter.fillRect(ramp, gradient);
    }

    painter.restore();
}

// ===========================================================================
//  模拟变换
// ===========================================================================

QImage IccSimulatorWindow::applySimulation(const QImage& source)
{
    if (source.isNull()) {
        return source;
    }
    const IccSimParams params = currentParams();
    if (params.isIdentity()) {
        return source;
    }
    applySimulationTo(source, params, m_simulatedFrame);
    if (m_simulatedFrame.isNull()) {
        return source;
    }
    return m_simulatedFrame;
}

// ===========================================================================
//  屏幕镜片
// ===========================================================================

bool IccSimulatorWindow::ensureScreenExclusion()
{
    // 每次显示都重新设置一遍：窗口关闭后 Qt 会销毁原生窗口，重新打开时
    // 换了一个 HWND，旧的排除标志就丢了；这个调用本身开销可以忽略。
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        // 关掉再打开时，Qt 要等 show() 走完才重建原生窗口，showEvent 里
        // winId() 拿到的还是已被销毁的旧句柄（ERROR_INVALID_WINDOW_HANDLE）。
        // 这属于「还没准备好」，不能记成「系统不支持」，稍后再补设一次。
        m_screenExclusionChecked = false;
        m_screenExclusionOk = false;
        scheduleScreenExclusionRetry();
        return false;
    }

    m_screenExclusionChecked = true;
    m_screenExclusionOk = false;
    // 让窗口本体不进入任何屏幕截取结果，这样抓到的就是「窗口下面」的画面。
    // Windows 10 2004+ 才支持 WDA_EXCLUDEFROMCAPTURE；更早的系统会退化成
    // WDA_MONITOR（截到黑块），所以这里同时校验实际生效的值。
    if (::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        DWORD affinity = 0;
        m_screenExclusionOk =
            ::GetWindowDisplayAffinity(hwnd, &affinity)
            && affinity == WDA_EXCLUDEFROMCAPTURE;
    }
    if (!m_screenExclusionOk) {
        scheduleScreenExclusionRetry();
    }
#else
    m_screenExclusionChecked = true;
    m_screenExclusionOk = false;
#endif
    return m_screenExclusionOk;
}

void IccSimulatorWindow::scheduleScreenExclusionRetry()
{
    if (m_exclusionRetryTimer == nullptr || !isVisible()
        || m_sourceMode != ScreenLens) {
        return;
    }
    // 已经在等待重试就不再重启：paintEvent 每帧都会尝试补设，
    // 反复重启会把单次触发定时器一直往后推，永远等不到重试。
    if (m_exclusionRetryTimer->isActive()) {
        return;
    }
    // 句柄还没建好就紧凑重试，尽快赶上原生窗口的创建；句柄有效却失败，
    // 多半是真不支持 WDA_EXCLUDEFROMCAPTURE，放慢重试降低开销。
    m_exclusionRetryTimer->start(m_screenExclusionChecked
                                     ? kExclusionRetryUnsupportedMs
                                     : kExclusionRetryWindowMs);
}

void IccSimulatorWindow::retryScreenExclusion()
{
    if (!isVisible() || m_sourceMode != ScreenLens) {
        return;
    }
    if (!ensureScreenExclusion()) {
        return;  // ensureScreenExclusion 内部已经安排好下一次重试
    }
    // 补设成功后立刻拉起抓屏线程，不用等下一次绘制或显示事件
    updateScreenCaptureState();
    update();
}

void IccSimulatorWindow::captureScreenFrame()
{
    if (m_sourceMode != ScreenLens || !isVisible()) {
        m_screenFrameValid = false;
        return;
    }
    m_lastCaptureTimer.restart();

    if (!ensureScreenExclusion()) {
        m_screenFrameValid = false;
        return;
    }

    const QRect globalContent = contentGlobalRect();
    if (globalContent.width() <= 0 || globalContent.height() <= 0) {
        m_screenFrameValid = false;
        return;
    }

    QScreen* screen = QGuiApplication::screenAt(globalContent.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        m_screenFrameValid = false;
        return;
    }

    // 窗口跨屏时只抓当前屏幕内的那一块，其余区域保持中性灰
    const QRect screenGeometry = screen->geometry();
    const QRect clipped = globalContent.intersected(screenGeometry);
    if (clipped.isEmpty()) {
        m_screenFrameValid = false;
        return;
    }

    // grabWindow 的 x / y 是相对屏幕的设备无关像素
    const QPixmap pixmap = screen->grabWindow(
        0, clipped.x() - screenGeometry.x(), clipped.y() - screenGeometry.y(),
        clipped.width(), clipped.height());
    if (pixmap.isNull()) {
        m_screenFrameValid = false;
        return;
    }

    m_screenFrame = pixmap.toImage();
    m_screenFrameRect =
        QRect(clipped.topLeft() - globalContent.topLeft(), clipped.size());
    m_screenFrameContentGlobal = globalContent;
    m_screenFrameValid = !m_screenFrame.isNull();
}

void IccSimulatorWindow::updateScreenCaptureState()
{
    const bool wantCapture = isVisible() && m_sourceMode == ScreenLens;

#ifdef Q_OS_WIN
    // 有后台抓屏线程时，帧率由 BitBlt 的实际速度决定（本机约 200fps），
    // 既不需要定时器补帧，也不会在 UI 线程里同步截屏。
    if (wantCapture && m_screenExclusionOk) {
        if (m_lensCapture == nullptr) {
            // 每次显示都新建抓屏线程：窗口隐藏 / 重新显示时 HWND 可能已经变了，
            // 旧线程里缓存的句柄会失效，表现就是重新打开后画面冻住。
            m_screenFrameValid = false;
            m_lastFrameTimer.restart();
            m_lensCapture = new ScreenLensCapture(this);
            m_lensCapture->setDevicePixelRatio(devicePixelRatioF());
            m_lensCapture->setParameters(currentParams());
            m_lensCapture->start();
        }
        m_lensCapture->setActive(true);
        // 看门狗：后台线程万一不送帧，仍能定时触发同步兜底
        if (m_refreshTimer != nullptr) {
            m_refreshTimer->setInterval(kLensWatchdogIntervalMs);
            m_refreshTimer->start();
        }
    } else if (m_lensCapture != nullptr) {
        delete m_lensCapture;
        m_lensCapture = nullptr;
        m_screenFrameValid = false;
        stopRefreshTimer();
    }
#endif

    if (wantCapture && m_lensCapture == nullptr) {
        // 回退路径：UI 线程定时抓屏
        startRefreshTimer();
        captureScreenFrame();
    } else if (!wantCapture) {
        stopRefreshTimer();
        m_screenFrameValid = false;
    }
}

void IccSimulatorWindow::stopScreenCapture()
{
    if (m_exclusionRetryTimer != nullptr) {
        m_exclusionRetryTimer->stop();
    }
    delete m_lensCapture;
    m_lensCapture = nullptr;
    m_screenFrameValid = false;
    stopRefreshTimer();
}

void IccSimulatorWindow::onScreenFrameReady()
{
    if (m_lensCapture == nullptr) {
        return;
    }
    QImage frame = m_lensCapture->takePendingFrame();
    if (frame.isNull() || m_sourceMode != ScreenLens || !isVisible()) {
        return;
    }
    // 后台线程按物理像素抓取；这里标上窗口当前的 DPR，绘制时按逻辑尺寸 1:1 贴图
    frame.setDevicePixelRatio(devicePixelRatioF());
    m_lastFrameTimer.restart();
    m_screenFrame = std::move(frame);
    m_screenFrameRect = contentRect();
    m_screenFrameContentGlobal = contentGlobalRect();
    m_screenFrameValid = !m_screenFrame.isNull();
    update();
}

void IccSimulatorWindow::startRefreshTimer()
{
    if (m_lensCapture != nullptr) {
        return;  // 后台抓屏线程接管，不需要定时器
    }
    if (m_refreshTimer != nullptr && isVisible()
        && m_sourceMode == ScreenLens && !m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
}

void IccSimulatorWindow::stopRefreshTimer()
{
    if (m_refreshTimer != nullptr) {
        m_refreshTimer->stop();
    }
}

void IccSimulatorWindow::setInteractionRefreshRate(bool interacting)
{
    if (m_refreshTimer == nullptr) {
        return;
    }
    // 有后台抓屏时定时器是 250ms 的看门狗，不参与交互刷新
    if (m_lensCapture != nullptr) {
        return;
    }
    // 回退路径：拖动 / 缩放时 125fps，静止时 60fps
    m_refreshTimer->setInterval(interacting ? kInteractionRefreshIntervalMs
                                            : kIdleRefreshIntervalMs);
}

void IccSimulatorWindow::refreshNow()
{
    if (m_sourceMode == ScreenLens && m_lensCapture == nullptr) {
        captureScreenFrame();
    }
    update();
}

// ===========================================================================
//  持久化
// ===========================================================================

void IccSimulatorWindow::loadSettings()
{
    QSettings settings;
    const auto loadSlider = [&settings](QSlider* slider, const char* key,
                                        int fallback) {
        const int value = settings.value(QString::fromLatin1(key), fallback).toInt();
        slider->setValue(std::clamp(value, slider->minimum(), slider->maximum()));
    };
    loadSlider(m_brightnessSlider, "iccSim/brightness", 0);
    loadSlider(m_contrastSlider, "iccSim/contrast", 100);
    loadSlider(m_saturationSlider, "iccSim/saturation", 100);
    loadSlider(m_temperatureSlider, "iccSim/temperature", 6500);
    loadSlider(m_gammaSlider, "iccSim/gamma", 100);
    updateValueLabels();

    // 旧版本可能存过已移除的「当前照片」（索引 2），越界时钳到最后一个有效项
    const int source = settings.value(QStringLiteral("iccSim/source"),
                                      int(ScreenLens)).toInt();
    m_sourceCombo->setCurrentIndex(
        std::clamp(source, 0, m_sourceCombo->count() - 1));

    // 恢复上次的位置与大小；已经不在任何屏幕上的旧几何直接丢弃
    const QRect saved = settings.value(QStringLiteral("iccSim/geometry")).toRect();
    if (saved.isValid() && saved.width() >= minimumWidth()
        && saved.height() >= minimumHeight()) {
        bool onScreen = false;
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(saved)) {
                onScreen = true;
                break;
            }
        }
        if (onScreen) {
            setGeometry(saved);
            layoutOverlays();
        }
    }
}

void IccSimulatorWindow::saveSettings()
{
    if (m_sourceCombo == nullptr || m_brightnessSlider == nullptr) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("iccSim/brightness"), m_brightnessSlider->value());
    settings.setValue(QStringLiteral("iccSim/contrast"), m_contrastSlider->value());
    settings.setValue(QStringLiteral("iccSim/saturation"), m_saturationSlider->value());
    settings.setValue(QStringLiteral("iccSim/temperature"), m_temperatureSlider->value());
    settings.setValue(QStringLiteral("iccSim/gamma"), m_gammaSlider->value());
    settings.setValue(QStringLiteral("iccSim/source"), m_sourceMode);
    settings.setValue(QStringLiteral("iccSim/geometry"), geometry());
}

// ===========================================================================
//  窗口事件与交互
// ===========================================================================

bool IccSimulatorWindow::event(QEvent* event)
{
    // 原生窗口句柄变了（某些显示 / 隐藏流程会重建 HWND）：
    // 旧的排除标志和抓屏线程里的句柄都会失效，需要整体重建。
    if (event->type() == QEvent::WinIdChange) {
        // 旧线程里缓存的句柄和排除标志都随旧窗口一起失效，先停掉再重建
        stopScreenCapture();
        // 新句柄此时已经建好：补设排除标志，并立即按新句柄重启抓屏线程
        // （重开窗口时 showEvent 早于原生窗口创建，靠这里补上）
        if (ensureScreenExclusion()) {
            updateScreenCaptureState();
        }
    }
    return QWidget::event(event);
}

void IccSimulatorWindow::hideEvent(QHideEvent* event)
{
    // 隐藏时销毁抓屏线程，重新显示时按当前句柄重建
    stopScreenCapture();
    saveSettings();
    // 右上角关闭按钮 / Esc / 顶部按钮取消勾选最终都走 hide，
    // 统一在这里通知主窗口同步按钮状态
    emit closed();
    QWidget::hideEvent(event);
}

void IccSimulatorWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // 先完成截取排除，再开始抓屏，避免第一帧把自己拍进去
    ensureScreenExclusion();
    updateScreenCaptureState();
    if (m_sourceMode == ScreenLens && m_lensCapture == nullptr) {
        // 回退路径：等本轮显示完成、窗口属性生效后再抓第一帧
        QTimer::singleShot(0, this, [this]() {
            if (isVisible()) {
                refreshNow();
            }
        });
    }
}

void IccSimulatorWindow::closeEvent(QCloseEvent* event)
{
    // Alt+F4 等途径也会走到这里。不真正关闭：close() 会让 Qt 销毁原生窗口，
    // 下次打开时 HWND 还没建好，截取排除与抓屏都会失败（表现为黑屏）。
    // 改成隐藏，保留窗口实例和句柄，hideEvent 里会保存状态并通知主窗口。
    event->ignore();
    hide();
}

void IccSimulatorWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutOverlays();
    // 异步重绘即可：paintEvent 会按窗口当前几何重新抓屏；
    // 定时器在交互期间以 8ms 节拍持续补帧，不会因输入密集而停更。
    update();
}

void IccSimulatorWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
#ifdef Q_OS_WIN
    // 跨屏移动时 DPI 可能变化，同步给后台抓屏线程用于换算内容区像素
    if (m_lensCapture != nullptr) {
        m_lensCapture->setDevicePixelRatio(devicePixelRatioF());
    }
#endif
    update();
}

void IccSimulatorWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_settingsPanel != nullptr && m_settingsPanel->isVisible()) {
            m_settingsPanel->hide();
            m_settingsButton->setChecked(false);
        } else {
            // 同上：隐藏即可，保留原生窗口句柄
            hide();
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

int IccSimulatorWindow::resizeEdgesAt(const QPoint& position) const
{
    int edges = EdgeNone;
    if (position.x() <= kResizeMargin) {
        edges |= EdgeLeft;
    }
    if (position.x() >= width() - 1 - kResizeMargin) {
        edges |= EdgeRight;
    }
    if (position.y() <= kResizeMargin) {
        edges |= EdgeTop;
    }
    if (position.y() >= height() - 1 - kResizeMargin) {
        edges |= EdgeBottom;
    }
    return edges;
}

Qt::CursorShape IccSimulatorWindow::cursorForEdges(int edges) const
{
    switch (edges) {
    case EdgeLeft | EdgeTop:
    case EdgeRight | EdgeBottom:
        return Qt::SizeFDiagCursor;
    case EdgeRight | EdgeTop:
    case EdgeLeft | EdgeBottom:
        return Qt::SizeBDiagCursor;
    case EdgeLeft:
    case EdgeRight:
        return Qt::SizeHorCursor;
    case EdgeTop:
    case EdgeBottom:
        return Qt::SizeVerCursor;
    default:
        return Qt::OpenHandCursor;
    }
}

void IccSimulatorWindow::updateCursorFor(const QPoint& position)
{
    const int edges = resizeEdgesAt(position);
    setCursor(edges != EdgeNone ? cursorForEdges(edges) : Qt::OpenHandCursor);
}

void IccSimulatorWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_resizeEdges = resizeEdgesAt(event->position().toPoint());
    if (m_resizeEdges != EdgeNone) {
        m_resizing = true;
        m_resizeStartGlobal = event->globalPosition().toPoint();
        m_resizeStartGeometry = geometry();
        setCursor(cursorForEdges(m_resizeEdges));
    } else {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
    }
    // 交互期间把镜片刷新率提到 125fps
    setInteractionRefreshRate(m_dragging || m_resizing);
    event->accept();
}

void IccSimulatorWindow::mouseMoveEvent(QMouseEvent* event)
{
    // 拖动 / 缩放期间系统会把 WM_PAINT 往后压，单纯 update() 会让镜片内容
    // 看起来「冻住」；现在每帧只是一次贴图，同步 repaint 的代价很低，直接强制刷新。
    if (m_resizing) {
        applyResize(event->globalPosition().toPoint());
        repaint();
        event->accept();
        return;
    }
    if (m_dragging) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        repaint();
        event->accept();
        return;
    }
    updateCursorFor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void IccSimulatorWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_resizing = false;
        m_resizeEdges = EdgeNone;
        setInteractionRefreshRate(false);
        updateCursorFor(event->position().toPoint());
        saveSettings();
    }
    QWidget::mouseReleaseEvent(event);
}

void IccSimulatorWindow::applyResize(const QPoint& globalPosition)
{
    QRect geometry = m_resizeStartGeometry;
    const int deltaX = globalPosition.x() - m_resizeStartGlobal.x();
    const int deltaY = globalPosition.y() - m_resizeStartGlobal.y();

    if ((m_resizeEdges & EdgeLeft) != 0) {
        geometry.setLeft(geometry.left() + deltaX);
    }
    if ((m_resizeEdges & EdgeRight) != 0) {
        geometry.setRight(geometry.right() + deltaX);
    }
    if ((m_resizeEdges & EdgeTop) != 0) {
        geometry.setTop(geometry.top() + deltaY);
    }
    if ((m_resizeEdges & EdgeBottom) != 0) {
        geometry.setBottom(geometry.bottom() + deltaY);
    }

    // 保证不小于最小尺寸，并且拖动左 / 上边时右 / 下边保持不动
    if (geometry.width() < minimumWidth()) {
        if ((m_resizeEdges & EdgeLeft) != 0) {
            geometry.setLeft(geometry.right() - minimumWidth() + 1);
        } else {
            geometry.setRight(geometry.left() + minimumWidth() - 1);
        }
    }
    if (geometry.height() < minimumHeight()) {
        if ((m_resizeEdges & EdgeTop) != 0) {
            geometry.setTop(geometry.bottom() - minimumHeight() + 1);
        } else {
            geometry.setBottom(geometry.top() + minimumHeight() - 1);
        }
    }

    setGeometry(geometry);
}

}  // namespace rawui
