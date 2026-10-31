// ============================================================================
//  main.cpp —— 程序入口
//
//  本应用定位为快速选图工作台，不再提供修图界面。
//  启动流程：
//   1. 设置应用元信息
//   2. 配置深色风格
//   3. 安装全局日志处理器（把 Qt 警告转发到应用日志）
//   4. 创建并显示选图工作台
// ============================================================================

#include "ui/CullWindow.h"

#include "app/AppLog.h"
#include "app/PortableMode.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QPalette>
#include <QProxyStyle>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTimer>
#include <QTranslator>

namespace {

/// 在 Fusion 之上微调 tooltip 的计时，让参数说明响应更干脆。
/// Fusion 默认悬停约 0.7 秒才弹出，鼠标在参数名、滑块之间移动时
/// 这个等待还会被反复重置，体感就是"浮窗出得很慢、很卡"。
class PromptToolTipStyle : public QProxyStyle
{
public:
    explicit PromptToolTipStyle(QStyle* base)
        : QProxyStyle(base)
    {
    }

    int styleHint(StyleHint hint,
                  const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override
    {
        switch (hint) {
        case SH_ToolTip_WakeUpDelay:
            return 250;  // 悬停到弹出之间只等 0.25 秒
        case SH_ToolTip_FallAsleepDelay:
            return 100;  // 相邻参数之间切换时几乎立即重新弹出
        default:
            break;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

/// 把旧版本（RawColorEditor / RAW Color Editor）保存的 QSettings 复制到新名字下。
/// 只在当前配置为空时执行，之后用户的改动仍以 PicPic 为准；旧配置保留不动，
/// 便于需要时回退旧版本。
void migrateLegacySettings()
{
    QSettings current;
    if (!current.allKeys().isEmpty()) {
        return;
    }
    QSettings legacy(QStringLiteral("RawColorEditor"),
                     QStringLiteral("RAW Color Editor"));
    const QStringList keys = legacy.allKeys();
    for (const QString& key : keys) {
        current.setValue(key, legacy.value(key));
    }
    current.sync();
}

/// 把 Qt 自身的消息转发到应用日志，便于统一排查。
/// 注意：只记录 Warning 及以上级别，避免 Info 级日志刷屏。
void installQtMessageHandler()
{
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context,
                              const QString& message) {
        Q_UNUSED(context);

        switch (type) {
        case QtDebugMsg:
            // 调试信息量大，仅在需要时开启
            break;
        case QtInfoMsg:
            rawapp::appLog().info(message);
            break;
        case QtWarningMsg:
            rawapp::appLog().warning(message);
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            rawapp::appLog().error(message);
            break;
        }
    });
}

}  // namespace

int main(int argc, char* argv[])
{
    // Qt6 默认已启用高 DPI 缩放，无需再设置 AA_EnableHighDpiScaling。
    // 但圆角像素图的平滑策略仍需指定，以保证缩略图与图标显示清晰。
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    // ---- 应用元信息 ----
    // 应用更名为 PicPic；旧版本的配置保存在 RawColorEditor / RAW Color Editor 下，
    // 这里在首次启动时迁移一次，避免源目录、目标目录与构图参数丢失。
    QApplication::setApplicationName(QStringLiteral("PicPic"));
    // 注意：这里刻意不调用 setApplicationDisplayName。
    // Qt 的 Windows 平台插件会把“显示名”以「 - 显示名」的形式追加到原生窗口标题后面，
    // 而标题本身已经写了 PicPic，结果标题栏会出现「开始选图吧 - PicPic」这样重复的应用名。
    // 窗口标题统一由 CullWindow 的 setWindowTitle 决定即可。
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("PicPic"));

    // ---- 便携模式 ----
    // 免安装包在 exe 旁放了 portable.ini：配置改用 INI 文件并落在程序目录，
    // 不写注册表；日志的落点由 AppLog 按同一标记处理。
    // 必须赶在任何 QSettings 构造之前设置默认格式与路径。
    if (rawapp::isPortableMode()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QCoreApplication::applicationDirPath());
    }

    // ---- Qt 内置控件的中文翻译 ----
    // 颜色选择面板、标准对话框按钮（OK / Cancel）、非原生文件对话框等
    // 都是 Qt 自己的文案，默认英文；本机 Qt 安装又没带 translations 目录，
    // 所以把官方 qtbase_zh_CN.qm 编进资源里，启动时直接加载。
    auto* qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QStringLiteral(":/translations/qtbase_zh_CN.qm"))) {
        app.installTranslator(qtTranslator);
    }

    migrateLegacySettings();

    // 运行时窗口图标（标题栏、任务栏、对话框统一使用）。
    // exe 在资源管理器里的文件图标由 resources/app.rc 嵌入，两者同源。
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // ---- 风格 ----
    // 选图时需要中性深色界面，避免界面亮度影响对图像明暗与色彩的判断。
    // Windows 原生风格在深色模式下表现不稳定，统一用 Fusion 并配合调色板。
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        QApplication::setStyle(
            new PromptToolTipStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    }

    {
        QPalette palette;
        // 深色中性灰配色。刻意避免饱和度高的颜色，以免干扰对图像色彩的判断。
        const QColor windowColor(53, 53, 53);
        const QColor baseColor(42, 42, 42);
        const QColor textColor(220, 220, 220);

        palette.setColor(QPalette::Window, windowColor);
        palette.setColor(QPalette::WindowText, textColor);
        palette.setColor(QPalette::Base, baseColor);
        palette.setColor(QPalette::AlternateBase, QColor(48, 48, 48));
        palette.setColor(QPalette::ToolTipBase, baseColor);
        palette.setColor(QPalette::ToolTipText, textColor);
        palette.setColor(QPalette::Text, textColor);
        palette.setColor(QPalette::Button, QColor(60, 60, 60));
        palette.setColor(QPalette::ButtonText, textColor);
        palette.setColor(QPalette::BrightText, QColor(255, 80, 80));
        palette.setColor(QPalette::Link, QColor(120, 170, 220));
        palette.setColor(QPalette::Highlight, QColor(70, 110, 160));
        palette.setColor(QPalette::HighlightedText, Qt::white);

        // 禁用态颜色，保证不可用控件仍然可辨识
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 120));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));

        QApplication::setPalette(palette);
    }

    // ---- 全局样式表 ----
    // 在 Fusion 调色板之上统一控件细节：细滚动条、紧凑的输入框与下拉框、
    // 克制的按钮状态。调色板负责基础配色，这里只补间距、圆角与边框，
    // 避免各面板各写一套导致疏密不一。
    // 各面板自己的高优先级样式表仍然会覆盖这里的同名属性。
    app.setStyleSheet(QStringLiteral(R"QSS(
/* 浮动提示：深色底、细边框，和参数说明弹窗保持一致 */
QToolTip {
    background: #2b2b2b;
    color: #e2e2e2;
    border: 1px solid #4a4a4a;
    border-radius: 3px;
    padding: 4px 7px;
}

/* 细窄滚动条：去掉两端箭头，只留一条圆角滑块 */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #454545;
    min-height: 28px;
    border-radius: 4px;
    margin: 2px;
}
QScrollBar::handle:vertical:hover { background: #5a5a5a; }
QScrollBar::handle:vertical:pressed { background: #6e6e6e; }
/* 常驻占位但没有滚动范围时（内容不足一屏）不画滑块，
   只保留滚动条宽度，避免折叠 / 展开分组时左栏内容宽度跳动 */
QScrollBar::handle:vertical:disabled { background: transparent; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #454545;
    min-width: 28px;
    border-radius: 4px;
    margin: 2px;
}
QScrollBar::handle:horizontal:hover { background: #5a5a5a; }
QScrollBar::handle:horizontal:pressed { background: #6e6e6e; }
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
    background: transparent;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* 输入框：统一的暗底细边框，聚焦时用强调色描边 */
QLineEdit, QSpinBox, QDoubleSpinBox {
    background: #1f1f1f;
    border: 1px solid #3a3a3a;
    border-radius: 3px;
    padding: 2px 5px;
    color: #dcdcdc;
    selection-background-color: #3d5f80;
    selection-color: #ffffff;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #4f4f4f; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #7fb3e8; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: #242424;
    border-color: #3a3a3a;
    color: #6f6f6f;
}

/* 多行文本（分拣日志）：与单行输入框同一套暗底细边框 */
QTextEdit, QPlainTextEdit {
    background: #1f1f1f;
    border: 1px solid #3a3a3a;
    border-radius: 3px;
    color: #dcdcdc;
    selection-background-color: #3d5f80;
    selection-color: #ffffff;
}
QTextEdit:focus, QPlainTextEdit:focus { border-color: #7fb3e8; }

/* 下拉框：与输入框同色系。只改外框，保留 Fusion 自带的箭头 */
QComboBox {
    background: #2f2f2f;
    border: 1px solid #454545;
    border-radius: 3px;
    /* 右侧留出箭头宽度，文字不会压到箭头上 */
    padding: 2px 22px 2px 6px;
    color: #dcdcdc;
    min-height: 20px;
    /* 固定用列表视图弹窗：菜单式弹窗会把「当前项」对齐到鼠标位置，
       不同选中项打开时高度和位置都不一样，看起来像下拉框在跳动 */
    combobox-popup: 0;
}
/* 箭头也交给样式表绘制：只改 QComboBox 却不定义 ::drop-down / ::down-arrow 时，
   样式表与原生绘制混用会导致点击弹出列表的瞬间重算文字位置，看起来像控件在跳动 */
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 20px;
    border: none;
    background: transparent;
}
QComboBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 10px;
    height: 10px;
}
QComboBox:hover { border-color: #5a5a5a; }
QComboBox:focus { border-color: #7fb3e8; }
QComboBox:disabled {
    background: #242424;
    border-color: #3a3a3a;
    color: #6f6f6f;
}
QComboBox QAbstractItemView {
    background: #2f2f2f;
    color: #dcdcdc;
    border: 1px solid #454545;
    selection-background-color: #465f78;
    selection-color: #ffffff;
    outline: 0;
}
/* 固定行高和行内边距：弹出列表的尺寸在显示前后保持一致 */
QComboBox QAbstractItemView::item {
    min-height: 22px;
    padding: 1px 6px;
}

/* 普通按钮：中等灰底，悬停只提亮一档，避免抢画面的注意力 */
QPushButton {
    background: #333333;
    border: 1px solid #474747;
    border-radius: 3px;
    padding: 4px 10px;
    color: #dcdcdc;
}
QPushButton:hover { background: #3d3d3d; border-color: #5a5a5a; }
QPushButton:pressed { background: #2a2a2a; }
/* 键盘聚焦时给出明确描边：按钮本身不加粗、不位移，避免布局抖动 */
QPushButton:focus { border-color: #7fb3e8; }
QPushButton:checked {
    background: #3a566f;
    border-color: #7fb3e8;
    color: #ffffff;
}
QPushButton:disabled {
    background: #2c2c2c;
    border-color: #3a3a3a;
    color: #6f6f6f;
}

/* 页签：底部细下划线指示当前页，与右侧参数栏保持一致 */
QTabBar::tab {
    background: #2d2d2d;
    color: #b0b0b0;
    padding: 6px 10px;
    border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { background: #383838; color: #dcdcdc; }
QTabBar::tab:selected {
    background: #333333;
    color: #f0f0f0;
    border-bottom: 2px solid #7fb3e8;
}

/* 菜单栏：与页签栏同色，去掉 Fusion 默认的亮灰底和立体亮边 */
QMenuBar {
    background: #232323;
    color: #c9c9c9;
    border: none;
    border-bottom: 1px solid #1a1a1a;
    padding: 2px 6px;
}
QMenuBar::item {
    background: transparent;
    padding: 4px 9px;
    border-radius: 3px;
}
QMenuBar::item:selected { background: #343434; color: #f0f0f0; }
QMenuBar::item:pressed { background: #2c2c2c; }

/* 菜单：去掉系统高亮色，跟随深色界面 */
QMenu {
    background: #2d2d2d;
    border: 1px solid #464646;
    padding: 4px;
}
QMenu::item {
    padding: 5px 24px 5px 12px;
    border-radius: 3px;
}
QMenu::item:selected { background: #3d3d3d; }
QMenu::separator {
    height: 1px;
    background: #3c3c3c;
    margin: 4px 8px;
}

/* 滑块：细轨道 + 小圆点手柄。已填充部分用低饱和蓝灰，
   既能看出当前值的位置，又不会抢走画面的注意力 */
QSlider { background: transparent; }
QSlider::groove:horizontal {
    height: 3px;
    background: #474747;
    border-radius: 1px;
}
QSlider::sub-page:horizontal {
    /* 只用略亮的灰表示"已到哪一侧"，不用彩色填充：
       多数滑块是 ±100 的双极参数，彩色填充会让人误以为"一半" */
    background: #565656;
    border-radius: 1px;
}
QSlider::add-page:horizontal {
    background: #474747;
    border-radius: 1px;
}
QSlider::handle:horizontal {
    width: 10px;
    margin: -4px 0;
    border-radius: 5px;
    background: #d6d6d6;
    border: 1px solid #1e1e1e;
}
QSlider::handle:horizontal:hover { background: #ffffff; }
QSlider::handle:horizontal:pressed { background: #7fb3e8; }
QSlider::sub-page:horizontal:disabled { background: #3d3d3d; }
QSlider::handle:horizontal:disabled { background: #5a5a5a; }

/* 勾选框：Fusion 默认的空心小方框在深色底上几乎看不见，
   这里自绘高对比方框；勾选时蓝色填充 + 白色对勾（check.svg）。
   注意：一旦接管 indicator 的绘制，未勾选状态下也不再有原生对勾，
   因此选中态必须靠填充色和对勾图标明确区分。 */
QCheckBox { spacing: 7px; color: #dcdcdc; }
QCheckBox::indicator {
    width: 15px;
    height: 15px;
    border: 1px solid #9a9a9a;
    border-radius: 3px;
    background: #1f1f1f;
}
QCheckBox::indicator:hover {
    border-color: #c8c8c8;
    background: #272727;
}
QCheckBox::indicator:checked {
    background: #3f6f9e;
    border-color: #7fb3e8;
    image: url(:/icons/check.svg);
}
QCheckBox::indicator:checked:hover { background: #4a80b5; }
/* 键盘聚焦：未勾选时也能看出焦点落在哪个勾选框 */
QCheckBox::indicator:focus { border-color: #7fb3e8; }
QCheckBox::indicator:unchecked:disabled {
    border-color: #454545;
    background: #242424;
}
QCheckBox::indicator:checked:disabled {
    border-color: #4f6070;
    background: #3a4a58;
}
QCheckBox:disabled { color: #6f6f6f; }
QCheckBox:focus { color: #ffffff; }
QSplitter::handle { background: #1e1e1e; }
QSplitter::handle:hover { background: #3a3a3a; }

/* 状态栏：与菜单栏同色，用一条更暗的顶边和内容区分 */
QStatusBar {
    background: #232323;
    color: #c9c9c9;
    border-top: 1px solid #1a1a1a;
}
QStatusBar::item { border: none; }
)QSS"));

    // ---- 日志 ----
    installQtMessageHandler();
    rawapp::appLog().info(QStringLiteral("================ 应用启动 ================"));

    // ---- 主窗口：快速选图工作台 ----
    rawui::CullWindow window;
    window.show();

    // 若命令行带了图片或目录参数，直接打开（便于"用此程序打开"的文件关联）
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        if (!path.isEmpty()) {
            // 延后到事件循环开始后再扫描，确保窗口与工作线程已完成初始化
            QTimer::singleShot(0, &window, [&window, path]() {
                window.openDirectory(path);
            });
        }
    } else {
        // 无启动参数时恢复上次的源目录，符合"重启后自动恢复上次路径"的要求。
        // 目录已被删除或暂时不可用时保持空白，由用户重新选择。
        const QString lastSource =
            QSettings().value(QStringLiteral("cull/sourceDir")).toString();
        if (!lastSource.isEmpty() && QFileInfo(lastSource).isDir()) {
            QTimer::singleShot(0, &window, [&window, lastSource]() {
                window.openDirectory(lastSource);
            });
        }
    }

    const int exitCode = app.exec();
    rawapp::appLog().info(QStringLiteral("应用退出，返回码 %1").arg(exitCode));
    return exitCode;
}
