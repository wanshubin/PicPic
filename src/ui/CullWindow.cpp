// ============================================================================
//  CullWindow.cpp —— 快速选图工作台实现
// ============================================================================

#include "CullWindow.h"

#include "CullPreviewWidget.h"
#include "CullWorkers.h"
#include "HelpDialog.h"
#include "IccSimulatorWindow.h"
#include "core/RawLoader.h"
#include "panels/CollapsibleSection.h"
#include "panels/WheelArbiter.h"

#include <QAbstractItemView>
#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileSystemWatcher>
#include <QDialog>
#include <QInputDialog>
#include <QMenu>
#include <QStringConverter>
#include <QTextStream>
#include <QTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QListView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTemporaryFile>
#include <QTextCursor>
#include <QTextEdit>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>

namespace rawui {

namespace {

/// 缩略图卡片（图片区域）的画布尺寸
const QSize kThumbCanvas(160, 120);
/// 卡片上下各留一点透明留白：选中 / 悬停底色不会紧贴卡片上沿，
/// 卡片底边也和文件名之间有点距离，看起来不会像一条分隔线。
constexpr int kThumbCardVMargin = 4;
/// 列表项图标区域的尺寸 = 卡片 + 上下留白
const QSize kThumbIconSize(kThumbCanvas.width(),
                          kThumbCanvas.height() + kThumbCardVMargin * 2);
/// 网格单元尺寸：图标区域 + 两行文字（文件名 / 拍摄时间）+ 少量留白。
// 之前留到 192，图标下方空出一大块，行与行看起来离得很远；
// 收到 176 后仍能放下两行 9pt 文字，行距更紧凑。
const QSize kGridSize(178, 176);

/// 缩略图列表里「时间分组标题」行的标记（普通图片行没有这个数据）
constexpr int kGroupHeaderRole = Qt::UserRole + 1;
/// 分组标题行右侧显示的图片张数
constexpr int kGroupCountRole = Qt::UserRole + 2;
/// 分组标题的稳定标识（用标题文本）：列表重建后据此恢复折叠状态
constexpr int kGroupKeyRole = Qt::UserRole + 3;
/// 分组当前是否处于折叠状态
constexpr int kGroupCollapsedRole = Qt::UserRole + 4;
/// 分组标题行高度：比缩略图网格矮很多，天然把上下两组隔开
constexpr int kGroupHeaderHeight = 30;

/// 分组标题行左侧折叠按钮的绘制 / 点击区域（与委托绘制保持一致）
QRectF groupToggleRect(const QRect& itemRect)
{
    const QRectF rect = QRectF(itemRect).adjusted(1.0, 3.0, -1.0, -3.0);
    return QRectF(rect.left() + 16.0, rect.center().y() - 8.0, 16.0, 16.0);
}

/// 缩略图列表：分组标题行不可选中，但 Home / End / PageUp / PageDown
/// 由 QListWidget 自己导航时仍可能把当前项落到标题上。
/// 这里在返回前把标题行替换成最近的图片行。
/// 折叠分组后组内图片被隐藏，导航也必须跳过它们。
class ThumbnailListWidget : public QListWidget {
public:
    explicit ThumbnailListWidget(QWidget* parent = nullptr)
        : QListWidget(parent)
    {
        // 悬停在分组折叠按钮上时显示手型光标
        setMouseTracking(true);
    }

    /// 点击分组标题左侧折叠按钮时回调（参数为该分组标题行）
    void setGroupToggleHandler(std::function<void(QListWidgetItem*)> handler)
    {
        m_groupToggleHandler = std::move(handler);
    }

protected:
    QModelIndex moveCursor(CursorAction action,
                           Qt::KeyboardModifiers modifiers) override
    {
        const QModelIndex index = QListWidget::moveCursor(action, modifiers);
        if (!index.isValid() || isVisiblePhoto(index)) {
            return index;
        }
        // 向上翻页 / 上一行从标题上方找图片；Home 与其余方向往后找。
        const bool backward = (action == MoveUp || action == MovePageUp);
        const int step = backward ? -1 : 1;
        auto findPhoto = [this](int start, int direction) -> QModelIndex {
            const int count = model()->rowCount();
            for (int row = start; row >= 0 && row < count; row += direction) {
                const QModelIndex candidate = model()->index(row, 0);
                if (isVisiblePhoto(candidate)) {
                    return candidate;
                }
            }
            return QModelIndex();
        };
        QModelIndex replacement = findPhoto(index.row() + step, step);
        if (!replacement.isValid()) {
            replacement = findPhoto(index.row() - step, -step);
        }
        return replacement.isValid() ? replacement : index;
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (isOnGroupToggle(event->position())) {
            setCursor(Qt::PointingHandCursor);
        } else {
            unsetCursor();
        }
        QListWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        unsetCursor();
        QListWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        // 按在折叠按钮上时不让列表把当前项切到标题行：
        // 直接吞掉按下事件，松手时再执行折叠 / 展开。
        if (event->button() == Qt::LeftButton && isOnGroupToggle(event->position())) {
            // 点按钮同样把键盘焦点交给列表，之后方向键可以继续切图
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && isOnGroupToggle(event->position())
            && m_groupToggleHandler != nullptr) {
            m_groupToggleHandler(itemAt(event->position().toPoint()));
            event->accept();
            return;
        }
        QListWidget::mouseReleaseEvent(event);
    }

private:
    /// 该行是否是可见的图片（排除分组标题与折叠后隐藏的图片）
    bool isVisiblePhoto(const QModelIndex& index) const
    {
        QListWidgetItem* item = itemFromIndex(index);
        return item != nullptr && !item->data(kGroupHeaderRole).toBool()
            && !item->isHidden();
    }

    /// 鼠标位置是否落在某个分组标题的折叠按钮上
    bool isOnGroupToggle(const QPointF& pos) const
    {
        QListWidgetItem* item = itemAt(pos.toPoint());
        if (item == nullptr || !item->data(kGroupHeaderRole).toBool()) {
            return false;
        }
        // 命中区域比绘制的按钮略大，点起来不费劲
        return groupToggleRect(visualItemRect(item))
            .adjusted(-4.0, -4.0, 4.0, 4.0)
            .contains(pos);
    }

    std::function<void(QListWidgetItem*)> m_groupToggleHandler;
};

/// 缩略图列表的自定义绘制：普通行交给默认委托；
/// 时间分组标题行画成一条横贯列表的浅色分隔条。
/// 标题行通过 sizeHint 占满列表可视宽度，后面的缩略图会被挤到下一行，
/// 从而形成"分栏"效果（IconMode 本身不支持真正的跨列单元格）。
class ThumbnailListDelegate : public QStyledItemDelegate {
public:
    explicit ThumbnailListDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (!index.data(kGroupHeaderRole).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect = QRectF(option.rect).adjusted(1.0, 3.0, -1.0, -3.0);
        // 比列表底色 #262626 略亮的一条，与缩略图网格区分开
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x2f, 0x2f, 0x2f));
        painter->drawRoundedRect(rect, 4.0, 4.0);
        // 左侧一道主题色短竖条，分组起点更容易一眼看到
        painter->setBrush(QColor(0x5b, 0x8f, 0xc7));
        painter->drawRoundedRect(QRectF(rect.left() + 7.0, rect.top() + 6.0,
                                        3.0, rect.height() - 12.0),
                                 1.5, 1.5);

        // 折叠按钮：展开时三角朝下，折叠时朝右。
        // 点击命中由 ThumbnailListWidget::isOnGroupToggle 用同一套矩形判断。
        const bool collapsed = index.data(kGroupCollapsedRole).toBool();
        const QRectF toggleRect = groupToggleRect(option.rect);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x3b, 0x3b, 0x3b));
        painter->drawRoundedRect(toggleRect, 4.0, 4.0);
        const QPointF center = toggleRect.center();
        QPolygonF arrow;
        if (collapsed) {
            arrow << QPointF(center.x() - 2.0, center.y() - 4.0)
                  << QPointF(center.x() + 3.0, center.y())
                  << QPointF(center.x() - 2.0, center.y() + 4.0);
        } else {
            arrow << QPointF(center.x() - 4.0, center.y() - 2.0)
                  << QPointF(center.x() + 4.0, center.y() - 2.0)
                  << QPointF(center.x(), center.y() + 3.0);
        }
        painter->setBrush(QColor(0xc7, 0xd6, 0xe6));
        painter->drawPolygon(arrow);

        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(QColor(0xe2, 0xe2, 0xe2));
        const QString title = index.data(Qt::DisplayRole).toString();
        // 左边给折叠按钮让出位置
        const QRectF titleRect = rect.adjusted(40.0, 0.0, -72.0, 0.0);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(
                              title, Qt::ElideRight,
                              static_cast<int>(titleRect.width())));

        painter->setFont(option.font);
        painter->setPen(QColor(0x9a, 0x9a, 0x9a));
        painter->drawText(rect.adjusted(0.0, 0.0, -10.0, 0.0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QStringLiteral("%1 张").arg(index.data(kGroupCountRole).toInt()));
        painter->restore();
    }
};

/// 构图比例预设。最后一项 "自定义" 的数值不使用。
struct AspectPreset {
    const char* label;
    double aspect;
};

const AspectPreset kAspectPresets[] = {
    // 横构图
    {"3:2", 3.0 / 2.0},
    {"4:3", 4.0 / 3.0},
    {"16:9", 16.0 / 9.0},
    {"1:1", 1.0},
    {"2.39:1", 2.39},
    // 竖构图（人像 / 手机分享常用）
    {"2:3", 2.0 / 3.0},
    {"3:4", 3.0 / 4.0},
    {"4:5", 4.0 / 5.0},
    {"9:16", 9.0 / 16.0},
    {"自定义", 0.0},
};

constexpr int kCustomAspectIndex = 9;

/// 框外压暗范围与默认值（40%~100%，默认 75%）
constexpr int kDimMin = 40;
constexpr int kDimMax = 100;
constexpr int kDimDefault = 75;

/// 已标记图片按住 Space 多久后弹出色板（毫秒）。
/// 短按 = 复制并下一张，长按 = 重新选择标记颜色。
constexpr int kSpaceHoldMs = 1000;

/// 目标目录可选颜色。颜色名同时作为侧车 label 的值，沿用英文以兼容旧侧车；
/// 每个目标目录必须独占一种颜色，批量复制靠颜色判断"这张图去哪个目录"。
struct TargetColorInfo {
    const char* name;     ///< 侧车里保存的名字
    const char* display;  ///< 界面显示
    const char* hex;      ///< 圆点颜色
};

const TargetColorInfo kTargetColors[] = {
    {"Red", "红色", "#d9534f"},
    {"Yellow", "黄色", "#d9b23a"},
    {"Green", "绿色", "#4ca96b"},
    {"Blue", "蓝色", "#4a84d0"},
    {"Purple", "紫色", "#9a6bd6"},
    {"Cyan", "青色", "#3fa9a0"},
    {"Orange", "橙色", "#d97b2f"},
    {"Pink", "粉色", "#c25f96"},
    {"Brown", "棕色", "#8a6a4a"},
    {"Gray", "灰色", "#8b9096"},
};
constexpr int kTargetColorCount = static_cast<int>(std::size(kTargetColors));

/// 目标目录数量上限。每个目录要独占一种标记颜色，因此不能超过颜色表容量。
constexpr int kMaxTargetDirs = 10;
static_assert(kTargetColorCount >= kMaxTargetDirs,
              "标记颜色不够 10 种，无法支持 10 个目标目录");

const TargetColorInfo* findTargetColor(const QString& name)
{
    for (const TargetColorInfo& info : kTargetColors) {
        if (name == QLatin1String(info.name)) {
            return &info;
        }
    }
    return nullptr;
}

/// 标记颜色圆点的样式：纯色圆点，选中时加白色描边
QString colorLabelChipStyle(const QString& fill)
{
    return QStringLiteral(
        "QToolButton {"
        "  background-color: %1;"
        "  border: 1px solid #444444;"
        "  border-radius: 9px;"
        "  color: #d8d8d8;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QToolButton:hover { border-color: #f0f0f0; }"
        "QToolButton:checked { border: 2px solid #ffffff; }"
        "QToolButton:disabled { background-color: #3a3a3a; border-color: #2d2d2d; }")
        .arg(fill);
}

/// 目标目录行里的颜色圆点：小圆形按钮，点击进入取色器
QString colorDotStyle(const QString& fill)
{
    return QStringLiteral(
        "QToolButton {"
        "  background-color: %1;"
        "  border: 1px solid #202020;"
        "  border-radius: 8px;"
        "}"
        "QToolButton:hover { border: 1px solid #ffffff; }")
        .arg(fill);
}

/// 面板里的下拉框：列表没展开时不响应滚轮。
/// QComboBox 默认会让滚轮直接改当前项，鼠标在「格式 / 显示 / 排序 / 比例」
/// 上扫过时很容易误改；这里把滚轮当成滚动面板的手势处理，
/// 只有弹出列表已经展开（滚轮用于在选项间移动）时才保留原生行为。
class WheelGuardComboBox : public QComboBox {
public:
    explicit WheelGuardComboBox(QWidget* parent = nullptr) : QComboBox(parent)
    {
        // QComboBox 默认是 WheelFocus：滚轮扫过时会把焦点抢过来，
        // 下拉框随即亮起蓝色聚焦框。改成只有点击 / Tab 才获得焦点。
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        QAbstractItemView* popup = view();
        if (popup != nullptr && popup->isVisible()) {
            QComboBox::wheelEvent(event);
            return;
        }
        // 未展开：不切换选项。侧栏里的下拉框把滚轮转交给滚动区域，
        // 保证鼠标扫过时仍然能滚动面板；浮层里的下拉框找不到滚动区域，
        // 直接吞掉，避免继续传给预览视图触发缩放。
        rawui::forwardWheelToParentScrollArea(this, event);
        event->accept();
    }
};

/// 日志里的颜色名还原成圆点色值：预设颜色记的是中文显示名（红色…），
/// 自定义颜色记的是 #RRGGBB 本身。认不出来时返回空串（不画圆点）。
QString logColorHex(const QString& colorName)
{
    if (colorName.startsWith(QLatin1Char('#')) && QColor::isValidColorName(colorName)) {
        return colorName;
    }
    for (const TargetColorInfo& info : kTargetColors) {
        if (colorName == QString::fromUtf8(info.display)) {
            return QLatin1String(info.hex);
        }
    }
    return QString();
}

/// 把一条分拣日志渲染成带颜色圆点的富文本。
/// formatLogLine 的末尾是「空格 + 颜色名」，这里在颜色名前补一个同色的 ●，
/// 一眼就能看出这张图落到了哪一类；其余文本仍然全部转义。
QString logLineHtml(const rawio::PickLogEntry& entry)
{
    QString html = rawio::PickStore::formatLogLine(entry).toHtmlEscaped();
    const QString hex = logColorHex(entry.color);
    if (hex.isEmpty()) {
        return html;
    }
    const QString suffix = QLatin1Char(' ') + entry.color.toHtmlEscaped();
    if (!html.endsWith(suffix)) {
        return html;
    }
    html.chop(suffix.size());
    html += QStringLiteral(" <span style=\"color:%1;\">●</span> %2")
                .arg(hex, entry.color.toHtmlEscaped());
    return html;
}

/// 透明底图标按钮的统一样式：目标目录行与预览视图控制共用。
/// 不描边框，只在悬停 / 按下时出现浅色底，让一排小图标保持安静。
QString iconButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 0; }"
        "QPushButton:hover { background: #3d3d3d; border-radius: 3px; }"
        "QPushButton:pressed { background: #2a2a2a; }");
}

/// 可选中图标按钮的样式：「构图与视图」「设置」等浮层开关按钮用。
/// 面板展开（按钮处于选中态）时用蓝灰底标出，一眼能看出面板是从这里开的。
QString toggleIconButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: transparent; border: none; padding: 0;"
        " border-radius: 4px; }"
        "QPushButton:hover { background: #3d3d3d; }"
        "QPushButton:pressed { background: #2a2a2a; }"
        "QPushButton:checked { background: #3c5068; }"
        "QPushButton:checked:hover { background: #465e7a; }"
        // 置灰时用中性灰底：开关可能仍处于选中态，但当前图片用不上，
        // 不能让用户误以为内嵌 JPEG 正在生效。
        "QPushButton:disabled { background: transparent; }"
        "QPushButton:disabled:checked { background: #3a3a3a; }");
}

/// 带禁用态的方向箭头图标。
/// QIcon 直接指向 SVG 文件时不会为 Disabled 模式单独出图，按钮置灰后
/// 图标看起来和可用时一模一样；这里从同一张 SVG 取 2x 位图，另外补一份
/// 淡化的副本作为 Disabled 图标，首张 / 末张时箭头能明显变暗。
QIcon dimmedArrowIcon(const QString& path)
{
    const QIcon source(path);
    const QPixmap normal = source.pixmap(QSize(14, 14), 2.0, QIcon::Normal);
    if (normal.isNull()) {
        // SVG 插件不可用时退回原始图标，至少还能显示
        return source;
    }
    QIcon icon;
    icon.addPixmap(normal, QIcon::Normal);
    QPixmap dimmed(normal.size());
    dimmed.setDevicePixelRatio(normal.devicePixelRatio());
    dimmed.fill(Qt::transparent);
    QPainter painter(&dimmed);
    painter.setOpacity(0.35);
    painter.drawPixmap(0, 0, normal);
    painter.end();
    icon.addPixmap(dimmed, QIcon::Disabled);
    return icon;
}

QString formatFileSize(qint64 bytes){
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    if (bytes >= 1024LL * 1024LL) {
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

/// 把当前框线颜色画成一个 16×16 的色块图标
void applySwatchIcon(QToolButton* button, const QColor& color)
{
    QPixmap swatch(16, 16);
    swatch.fill(color);
    QPainter painter(&swatch);
    painter.setPen(QColor(20, 20, 20));
    painter.drawRect(0, 0, 15, 15);
    painter.end();
    button->setIcon(QIcon(swatch));
}

/// 路径超长时从左侧裁剪，保留尽量多、层级尽量深的尾段。
/// 直接在字符中间截断会把"2026"切成"26"，所以优先在分隔符处断开；
/// 但分隔符对齐的写法常常比可用宽度短一大截（左对齐后会空出一条空白），
/// 因此只有在几乎填满时才采用，否则退回字符级左省略把宽度用起来。
QString elidePathLeft(const QString& path, int width, const QFontMetrics& metrics)
{
    if (path.isEmpty() || width <= 0
        || metrics.horizontalAdvance(path) <= width) {
        return path;
    }
    // Windows 路径用反斜杠；同时兼容以正斜杠保存的旧数据
    const QChar separator =
        path.contains(QLatin1Char('\\')) ? QLatin1Char('\\') : QLatin1Char('/');
    const QString prefix = QStringLiteral("…") + separator;
    QString aligned;
    for (int index = path.indexOf(separator); index >= 0;
         index = path.indexOf(separator, index + 1)) {
        const QString suffix = path.mid(index + 1);
        // UNC 路径以两个反斜杠开头，跳过会产生"…\\server"这种重复分隔符
        if (suffix.isEmpty() || suffix.startsWith(separator)) {
            continue;
        }
        const QString candidate = prefix + suffix;
        if (metrics.horizontalAdvance(candidate) <= width) {
            // 分隔符越靠后后缀越短，第一个放得下的就是最长的对齐写法
            aligned = candidate;
            break;
        }
    }
    // 对齐写法剩余的空隙还能再放下一个字符时，改用字符级左省略，
    // 把这段宽度也用来显示路径（优先保证层级最深的目录名完整可见）。
    const int slack = aligned.isEmpty()
        ? width
        : width - metrics.horizontalAdvance(aligned);
    if (slack >= metrics.horizontalAdvance(QLatin1Char('x'))) {
        return metrics.elidedText(path, Qt::ElideLeft, width);
    }
    return aligned;
}

/// 判断 dest 是否像是从 src 复制出来的副本：文件名必须与源文件相同，
/// 或符合 uniqueDestination 生成的 name_1 / name_2 规则。
/// 侧车日志允许手工修改，删除文件前用这个规则兜底，避免误删无关文件。
bool isCopyOfSourceFile(const QFileInfo& dest, const QFileInfo& source)
{
    if (dest.absoluteFilePath() == source.absoluteFilePath()) {
        return false;  // 绝不能把源文件本身当成待删除的副本
    }
    if (dest.suffix().compare(source.suffix(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    const QString stem = dest.completeBaseName();
    const QString sourceStem = source.completeBaseName();
    if (stem == sourceStem) {
        return true;
    }
    const QString prefix = sourceStem + QLatin1Char('_');
    if (!stem.startsWith(prefix)) {
        return false;
    }
    const QString index = stem.mid(prefix.size());
    return !index.isEmpty()
           && std::all_of(index.cbegin(), index.cend(),
                          [](QChar ch) { return ch.isDigit(); });
}

/// 判断两个文件内容是否完全相同：先比大小，再按块逐字节比较。
/// 只看文件名不够——同名文件也可能是另一张照片；只有内容完全一致
/// 才能当作“同一张图”跳过复制，否则应当追加序号另存一份。
bool filesHaveSameContent(const QString& firstPath, const QString& secondPath)
{
    QFile first(firstPath);
    QFile second(secondPath);
    if (!first.open(QIODevice::ReadOnly) || !second.open(QIODevice::ReadOnly)) {
        // 读不了就无法证明是同一张图，按“不同”处理（随后会追加序号复制，不覆盖）
        return false;
    }
    if (first.size() != second.size()) {
        return false;
    }
    // 1 MiB 一块：内存占用恒定；内容不同时通常第一块就能判定，RAW 也不会太慢
    constexpr qint64 kChunkSize = 1024 * 1024;
    while (true) {
        const QByteArray firstChunk = first.read(kChunkSize);
        const QByteArray secondChunk = second.read(kChunkSize);
        if (firstChunk != secondChunk) {
            return false;
        }
        if (firstChunk.isEmpty()) {
            // 读到文件末尾；中途读取出错也按“不同”处理，宁可多复制一份也不误判
            return first.error() == QFileDevice::NoError
                   && second.error() == QFileDevice::NoError;
        }
    }
}

/// 两个路径是否指向同一个目录（Windows 文件系统不区分大小写）
bool isSameDirectory(const QString& first, const QString& second)
{
    if (first.trimmed().isEmpty() || second.trimmed().isEmpty()) {
        return false;
    }
    const QString normalizedFirst = QDir::cleanPath(QFileInfo(first).absoluteFilePath());
    const QString normalizedSecond = QDir::cleanPath(QFileInfo(second).absoluteFilePath());
#ifdef Q_OS_WIN
    return normalizedFirst.compare(normalizedSecond, Qt::CaseInsensitive) == 0;
#else
    return normalizedFirst == normalizedSecond;
#endif
}

/// 右对齐的路径标签：宽度不够时从左侧省略，优先露出层级最深的目录。
/// 完整路径放在 tooltip 里，避免省略后看不到全貌。
class ElidedPathLabel : public QLabel {
public:
    explicit ElidedPathLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // 宽度交给布局决定，超长路径不能把左侧面板整体撑宽
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    /// 设置完整路径；显示文本按当前可用宽度自动省略
    void setPathText(const QString& path)
    {
        m_fullPath = path;
        setToolTip(path);
        updateElidedText();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        updateElidedText();
    }

private:
    void updateElidedText()
    {
        QLabel::setText(
            elidePathLeft(m_fullPath, contentsRect().width(), fontMetrics()));
    }

    QString m_fullPath;
};

/// 由窗口统一处理方向键：缩略图网格与预览都要同一套语义
/// （← → 前后切换，↑ ↓ 按行上下移动）。
/// PageUp / PageDown / Home / End 仍交给 QListWidget 自己处理。

/// 可在「设置」面板里改键的动作。id 同时用作 QSettings 的键名后缀，
/// 一旦发布就不要再改（改了老用户的按键配置会丢）。
/// 星级 0~5 与构图比例 Ctrl+1~5 是成组语义，不放进可改键列表。
struct ShortcutAction {
    const char* id;
    const char* label;
    Qt::Key defaultKey;
};

/// 顺序即设置面板里的显示顺序，按挑片时的操作频次排列
const QVector<ShortcutAction>& shortcutActions()
{
    static const QVector<ShortcutAction> actions = {
        {"prevImage", "上一张", Qt::Key_Left},
        {"nextImage", "下一张", Qt::Key_Right},
        {"prevRow", "上一行", Qt::Key_Up},
        {"nextRow", "下一行", Qt::Key_Down},
        {"copyNext", "复制并下一张", Qt::Key_Space},
        {"clearColor", "清除标记", Qt::Key_X},
        {"toggleComposition", "开关固定视野", Qt::Key_R},
        {"toggleMask", "开关框外压暗", Qt::Key_M},
        {"toggleFullScreen", "全屏 / 退出全屏", Qt::Key_F},
    };
    return actions;
}

/// 动作 id 对应的中文名；找不到时回落到 id 本身
QString shortcutActionLabel(const QString& id)
{
    for (const ShortcutAction& action : shortcutActions()) {
        if (id == QLatin1String(action.id)) {
            return QString::fromUtf8(action.label);
        }
    }
    return id;
}

/// 按键在按钮上的显示文字：方向键、空格用符号 / 中文，
/// 其余交给 Qt 的本地化文本（字母键就是大写字母）。
QString shortcutDisplayText(const QKeySequence& sequence)
{
    if (sequence.isEmpty()) {
        return QStringLiteral("未设置");
    }
    const QKeyCombination combination = sequence[0];
    if (combination.keyboardModifiers() == Qt::NoModifier) {
        switch (combination.key()) {
        case Qt::Key_Left:
            return QStringLiteral("←");
        case Qt::Key_Right:
            return QStringLiteral("→");
        case Qt::Key_Up:
            return QStringLiteral("↑");
        case Qt::Key_Down:
            return QStringLiteral("↓");
        case Qt::Key_Space:
            return QStringLiteral("空格");
        case Qt::Key_Escape:
            return QStringLiteral("Esc");
        default:
            break;
        }
    }
    return sequence.toString(QKeySequence::NativeText);
}

}  // namespace

// ===========================================================================
//  星级评分控件
// ===========================================================================

/// 一排扁平的五角星评分控件。不画按钮边框，只画星星本身：
/// 默认空心，鼠标悬停时预览填充，点击落定，再点当前星级清除。
/// 比一排 QToolButton 更像 Lightroom 的评级栏，也不会出现按钮高亮块。
class StarRatingWidget : public QWidget {
public:
    explicit StarRatingWidget(QWidget* parent = nullptr);

    int rating() const { return m_rating; }
    void setRating(int rating);

    /// 点击星星后的回调：参数为 1~5；点击当前星级时传 0（清除）
    std::function<void(int)> ratingClicked;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    static constexpr int kStarCount = 5;
    static constexpr qreal kStarSize = 19.0;
    static constexpr qreal kStarSpacing = 3.0;

    QRectF starRect(int index) const;
    static QPainterPath starPath(const QRectF& rect);
    int starAt(const QPoint& pos) const;

    int m_rating = 0;
    int m_hover = 0;  ///< 悬停预览的星级（0 = 未悬停）
};

StarRatingWidget::StarRatingWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("星级"));
    setFixedSize(sizeHint());
}

QSize StarRatingWidget::sizeHint() const
{
    const int width = static_cast<int>(kStarCount * kStarSize
                                       + (kStarCount - 1) * kStarSpacing);
    return QSize(width, static_cast<int>(kStarSize) + 2);
}

void StarRatingWidget::setRating(int rating)
{
    rating = std::clamp(rating, 0, kStarCount);
    if (m_rating == rating) {
        return;
    }
    m_rating = rating;
    update();
}

QRectF StarRatingWidget::starRect(int index) const
{
    const qreal y = (height() - kStarSize) / 2.0;
    return QRectF(index * (kStarSize + kStarSpacing), y, kStarSize, kStarSize);
}

QPainterPath StarRatingWidget::starPath(const QRectF& rect)
{
    // 标准五角星：外半径取格子的一半，内半径 0.44 让星形更饱满
    constexpr qreal kPi = 3.14159265358979323846;
    const QPointF center = rect.center();
    const qreal outer = std::min(rect.width(), rect.height()) / 2.0 - 1.0;
    const qreal inner = outer * 0.44;
    QPainterPath path;
    for (int i = 0; i < 10; ++i) {
        const qreal radius = (i % 2 == 0) ? outer : inner;
        const qreal angle = -kPi / 2.0 + i * kPi / 5.0;
        const QPointF point(center.x() + std::cos(angle) * radius,
                            center.y() + std::sin(angle) * radius);
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    path.closeSubpath();
    return path;
}

int StarRatingWidget::starAt(const QPoint& pos) const
{
    for (int i = 0; i < kStarCount; ++i) {
        if (starRect(i).adjusted(-1, -1, 1, 1).contains(pos)) {
            return i + 1;
        }
    }
    return 0;
}

void StarRatingWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 禁用时整体压暗：没有当前图片时不该看起来还能点
    painter.setOpacity(isEnabled() ? 1.0 : 0.35);

    // 悬停时按悬停位置预览；没悬停时显示已保存的星级
    const int filled = m_hover > 0 ? m_hover : m_rating;
    for (int i = 0; i < kStarCount; ++i) {
        const QRectF rect = starRect(i);
        if (i < filled) {
            // 填充：竖向金色渐变 + 稍深的描边，比纯色更耐看
            QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
            gradient.setColorAt(0.0, QColor(0xf7, 0xd0, 0x63));
            gradient.setColorAt(1.0, QColor(0xd9, 0xa8, 0x32));
            painter.setBrush(gradient);
            painter.setPen(QPen(QColor(0xb0, 0x83, 0x22), 1.0));
        } else {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0x7d, 0x7d, 0x7d), 1.2));
        }
        painter.drawPath(starPath(rect));
    }
}

void StarRatingWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (isEnabled()) {
        const int hover = starAt(event->position().toPoint());
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
    }
    QWidget::mouseMoveEvent(event);
}

void StarRatingWidget::mousePressEvent(QMouseEvent* event)
{
    if (isEnabled() && event->button() == Qt::LeftButton) {
        const int star = starAt(event->position().toPoint());
        if (star > 0) {
            // 再点当前星级 = 清除（0 星）
            if (ratingClicked) {
                ratingClicked(star == m_rating ? 0 : star);
            }
            // 点击后先显示落定结果；鼠标再动一下才会重新进入悬停预览
            m_hover = 0;
            update();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void StarRatingWidget::leaveEvent(QEvent* event)
{
    if (m_hover != 0) {
        m_hover = 0;
        update();
    }
    QWidget::leaveEvent(event);
}

// ===========================================================================
//  快捷键录入按钮
// ===========================================================================

/// 设置面板里的一颗按键按钮：平时显示当前按键，点击进入「按下新键」状态，
/// 下一次按键直接成为该动作的快捷键；Esc 或点击别处取消。
/// 录入期间在 event() 里吃掉 ShortcutOverride，否则空格这类窗口级
/// QShortcut 会先被触发，按钮根本收不到这次按键。
class ShortcutKeyButton : public QPushButton {
public:
    explicit ShortcutKeyButton(QWidget* parent = nullptr);

    void setSequence(const QKeySequence& sequence);
    QKeySequence sequence() const { return m_sequence; }
    void beginCapture();
    void cancelCapture();

    /// 录入成功后的回调；参数一定是非空序列
    std::function<void(const QKeySequence&)> sequenceCaptured;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;

private:
    /// 录入时短暂显示一句提示，稍后回到「按下新键…」
    void showHint(const QString& text);
    /// 录入状态变化后重新应用样式（capturing 属性选择器）
    void repolish();

    QKeySequence m_sequence;
    bool m_capturing = false;
};

ShortcutKeyButton::ShortcutKeyButton(QWidget* parent)
    : QPushButton(parent)
{
    setFixedSize(96, 24);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #333333;"
        "  border: 1px solid #474747;"
        "  border-radius: 3px;"
        "  color: #dcdcdc;"
        "  padding: 2px 6px;"
        "}"
        "QPushButton:hover { background: #3d3d3d; border-color: #5a5a5a; }"
        "QPushButton[capturing=\"true\"] {"
        "  background: #3c5068; border-color: #7fb3e8; color: #ffffff;"
        "}"));
    setText(shortcutDisplayText(m_sequence));
    setToolTip(QStringLiteral("未设置：点击后按下新键"));
}

void ShortcutKeyButton::setSequence(const QKeySequence& sequence)
{
    m_sequence = sequence;
    if (m_capturing) {
        return;  // 录入中先不改按钮文字，松手完成后再刷新
    }
    setText(shortcutDisplayText(m_sequence));
    setToolTip(m_sequence.isEmpty() ? QStringLiteral("未设置：点击后按下新键")
                                    : QStringLiteral("点击后按下新键；Esc 取消"));
}

void ShortcutKeyButton::beginCapture()
{
    if (m_capturing) {
        return;
    }
    m_capturing = true;
    setText(QStringLiteral("按下新键…"));
    setProperty("capturing", true);
    // 录入期间才接受焦点：平时 NoFocus，权限键不会把焦点抢到这里
    setFocusPolicy(Qt::StrongFocus);
    setFocus(Qt::MouseFocusReason);
    repolish();
}

void ShortcutKeyButton::cancelCapture()
{
    if (!m_capturing) {
        return;
    }
    m_capturing = false;
    setProperty("capturing", false);
    setText(shortcutDisplayText(m_sequence));
    setFocusPolicy(Qt::NoFocus);
    clearFocus();
    repolish();
}

bool ShortcutKeyButton::event(QEvent* event)
{
    // 录入期间先于 QShortcut 收到按键：ShortcutOverride 一律收下，
    // 否则空格 / 字母会被窗口级快捷键抢走，这里只能收到普通 KeyPress。
    if (m_capturing && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QPushButton::event(event);
}

void ShortcutKeyButton::keyPressEvent(QKeyEvent* event)
{
    if (!m_capturing) {
        QPushButton::keyPressEvent(event);
        return;
    }
    event->accept();
    if (event->isAutoRepeat()) {
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelCapture();
        return;
    }
    // 只接受不带修饰键的单个按键：Ctrl+数字已经留给构图比例，
    // Shift 会改变 key 本身（Shift+R 与 R 不是同一个键），一并拒绝更不容易出错。
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier
                     | Qt::ShiftModifier)) {
        showHint(QStringLiteral("暂不支持组合键"));
        return;
    }
    switch (event->key()) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_unknown:
        return;  // 修饰键本身不算
    default:
        break;
    }
    const QKeySequence sequence(event->keyCombination());
    m_capturing = false;
    setProperty("capturing", false);
    setFocusPolicy(Qt::NoFocus);
    clearFocus();
    repolish();
    setSequence(sequence);
    if (sequenceCaptured) {
        sequenceCaptured(sequence);
    }
}

void ShortcutKeyButton::focusOutEvent(QFocusEvent* event)
{
    // 点击别处 / 面板收起：放弃这次录入，保留原来的按键
    if (m_capturing) {
        cancelCapture();
    }
    QPushButton::focusOutEvent(event);
}

void ShortcutKeyButton::showHint(const QString& text)
{
    setText(text);
    QTimer::singleShot(900, this, [this]() {
        if (m_capturing) {
            setText(QStringLiteral("按下新键…"));
        }
    });
}

void ShortcutKeyButton::repolish()
{
    style()->unpolish(this);
    style()->polish(this);
}

// ===========================================================================
//  构造 / 析构
// ===========================================================================

CullWindow::CullWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 顶部栏常驻应用名与标语；具体打开的源目录在左侧目录栏里显示，
    // 不再覆盖这里的标题，避免不同目录下标题文案来回跳。
    setWindowTitle(QStringLiteral("PicPic —— 开始选图吧"));
    setMinimumSize(1100, 720);
    resize(1500, 940);

    // 跨线程传递 rawio::ImageFileInfo 需要注册元类型
    qRegisterMetaType<rawio::ImageFileInfo>("rawio::ImageFileInfo");

    // 先把默认快捷键填好：设置面板在 buildUi 里就要显示当前键位，
    // loadSettings 之后再被用户保存过的配置覆盖。
    initDefaultShortcuts();
    buildUi();
    loadSettings();
    connectSignals();
    applyCompositionToPreview();
    updateInfoBar();
    updateActionState();

    // 复制并下一张是高速挑片最常用的键，做成窗口级快捷键：
    // 即使焦点落在某个按钮上，也总是执行"复制 + 下一张"，
    // 不会变成按下那个按钮。输入框会通过 ShortcutOverride 正常输入。
    // 键位来自设置面板，改键 / 清除后由 applyCopyShortcut 同步。
    m_copyShortcut = new QShortcut(this);
    m_copyShortcut->setContext(Qt::WindowShortcut);
    // 按住时不重复触发：长按由 m_spaceHoldTimer 单独判定
    m_copyShortcut->setAutoRepeat(false);
    connect(m_copyShortcut, &QShortcut::activated, this, [this]() { handleSpacePressed(); });
    applyCopyShortcut();

    // Space 长按判定：已标记的图片按住满 1 秒弹出色板改标记
    m_spaceHoldTimer = new QTimer(this);
    m_spaceHoldTimer->setSingleShot(true);
    connect(m_spaceHoldTimer, &QTimer::timeout, this, &CullWindow::onSpaceHoldTimeout);

    // ---- 缩略图 / 元数据线程 ----
    m_thumbThread = new QThread(this);
    m_thumbWorker = new CullThumbnailWorker;
    m_thumbWorker->moveToThread(m_thumbThread);
    connect(this, &CullWindow::metadataRequested,
            m_thumbWorker, &CullThumbnailWorker::requestMetadata);
    connect(this, &CullWindow::thumbnailRequested,
            m_thumbWorker, &CullThumbnailWorker::requestThumbnail);
    connect(this, &CullWindow::usePipelineThumbnailsChanged,
            m_thumbWorker, &CullThumbnailWorker::setUsePipelineThumbnails);
    connect(this, &CullWindow::lensCorrectionChanged,
            m_thumbWorker, &CullThumbnailWorker::setLensCorrection);
    connect(m_thumbWorker, &CullThumbnailWorker::metadataReady,
            this, &CullWindow::onWorkerMetadata);
    connect(m_thumbWorker, &CullThumbnailWorker::thumbnailReady,
            this, [this](const QString& path, const QImage& image, bool) {
                onWorkerThumbnail(path, image);
            });
    connect(m_thumbWorker, &CullThumbnailWorker::thumbnailFailed,
            this, &CullWindow::onWorkerThumbnailFailed);
    m_thumbThread->start();

    // ---- 大图预览线程 ----
    m_previewThread = new QThread(this);
    m_previewWorker = new CullPreviewWorker;
    m_previewWorker->moveToThread(m_previewThread);
    connect(this, &CullWindow::previewRequested,
            m_previewWorker, &CullPreviewWorker::requestPreview);
    connect(this, &CullWindow::lensCorrectionChanged,
            m_previewWorker, &CullPreviewWorker::setLensCorrection);
    connect(m_previewWorker, &CullPreviewWorker::previewReady,
            this, &CullWindow::onPreviewReady);
    connect(m_previewWorker, &CullPreviewWorker::previewFailed,
            this, &CullWindow::onPreviewFailed);
    m_previewThread->start();

    // 设置里保存的缩略图模式要先同步给工作对象
    emit usePipelineThumbnailsChanged(m_pipelineThumbCheck->isChecked());
    // 镜头畸变校正同理：loadSettings 已经把开关状态读回来
    emit lensCorrectionChanged(m_lensCorrectionCheck->isChecked());

    // F10：目录自动刷新（防抖 800ms，避免复制大文件时连续重扫）
    m_watcher = new QFileSystemWatcher(this);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(800);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (m_rebuilding || m_items.isEmpty()) {
            return;
        }
        // 写侧车 .rawedit.pick.json 也会触发目录变化；只有图片数量变化
        // 才真正重扫，避免每次点星标都重建整个网格。
        const QDir dir(m_sourceEdit->text().trimmed());
        int count = 0;
        for (const QFileInfo& info : dir.entryInfoList(QDir::Files, QDir::Name)) {
            if (rawio::isSupportedImageFile(info.absoluteFilePath())) {
                ++count;
            }
        }

        // 数量变化、或列表里的文件已不存在（删除 / 重命名）→ 整体重扫
        bool needRescan = (count != m_items.size());
        if (!needRescan) {
            for (const Item& item : m_items) {
                if (!QFileInfo::exists(item.path)) {
                    needRescan = true;
                    break;
                }
            }
        }
        if (needRescan) {
            const QString keep = m_currentPath;
            scanDirectory();
            if (!keep.isEmpty()) {
                selectPath(keep);
            }
            return;
        }

        // 数量不变时文件仍可能被覆盖（例如被其他软件改写）：
        // 只刷新大小 / 修改时间变化的那几张，不重建整个网格。
        QStringList changedPaths;
        for (const Item& item : m_items) {
            const QFileInfo info(item.path);
            if (info.size() != item.info.fileSize
                || info.lastModified() != item.lastModified) {
                changedPaths.append(item.path);
            }
        }
        for (const QString& path : changedPaths) {
            refreshItemFromDisk(path);
        }
    });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString&) { m_refreshTimer->start(); });

    // 恢复上次的源目录（需求 §一 1：软件重启自动恢复）
    const QString sourceDir = m_sourceEdit->text().trimmed();
    if (!sourceDir.isEmpty() && QDir(sourceDir).exists()) {
        scanDirectory();
    } else {
        showStatus(QStringLiteral("请选择源目录开始挑片"));
    }

    // 启动焦点交给缩略图列表的工作放在 showEvent / changeEvent 里：
    // 构造阶段窗口还没显示，这时 setFocus 会被 show() 的默认焦点链覆盖。
}

CullWindow::~CullWindow()
{
    // 先停线程：quit() 会让正在执行的任务跑完，wait() 保证之后不再访问成员
    if (m_thumbThread != nullptr) {
        m_thumbThread->quit();
        m_thumbThread->wait();
    }
    if (m_previewThread != nullptr) {
        m_previewThread->quit();
        m_previewThread->wait();
    }
    delete m_thumbWorker;
    m_thumbWorker = nullptr;
    delete m_previewWorker;
    m_previewWorker = nullptr;
}

// ===========================================================================
//  界面搭建
// ===========================================================================

void CullWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 上半部分：左侧设置 + 右侧预览；下半部分：整幅宽的缩略图网格
    auto* vertical = new QSplitter(Qt::Vertical, central);
    auto* top = new QSplitter(Qt::Horizontal, vertical);
    QWidget* leftContent = buildLeftPanel();

    // 左侧栏顶部栏：常驻在滚动区上方，不随参数栏一起滚走。
    // 这里只放「设置」（缩略图管线 / 畸变校正 / 快捷键），面板浮到画面左上角，
    // 不占用预览区顶部的空间。「构图与视图」已挪到预览下方标记行的最左边。
    auto* leftTopBar = new QWidget(top);
    leftTopBar->setObjectName(QStringLiteral("CullLeftTopBar"));
    leftTopBar->setAttribute(Qt::WA_StyledBackground, true);
    leftTopBar->setStyleSheet(
        QStringLiteral("QWidget#CullLeftTopBar { background: #262626; }"));
    auto* topBarLayout = new QHBoxLayout(leftTopBar);
    topBarLayout->setContentsMargins(10, 6, 10, 6);
    topBarLayout->setSpacing(6);
    m_settingsButton = new QPushButton(leftTopBar);
    m_settingsButton->setCheckable(true);
    m_settingsButton->setFixedSize(24, 24);
    m_settingsButton->setIcon(QIcon(QStringLiteral(":/icons/settings.svg")));
    m_settingsButton->setIconSize(QSize(16, 16));
    m_settingsButton->setFocusPolicy(Qt::NoFocus);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setStyleSheet(toggleIconButtonStyle());
    m_settingsButton->setToolTip(
        QStringLiteral("设置：缩略图渲染方式、镜头畸变校正与快捷键\n"
                       "点击展开设置面板（浮在画面上，Esc 收起）"));
    m_settingsButton->setAccessibleName(QStringLiteral("设置"));
    topBarLayout->addWidget(m_settingsButton);

    // 「设置」右边的 ICC 模拟器：弹出一个无边框悬浮窗口，把窗口下方
    // 的画面按亮度 / 对比度 / 饱和度 / 色温 / 伽马等参数实时模拟。
    m_iccSimButton = new QPushButton(leftTopBar);
    m_iccSimButton->setCheckable(true);
    m_iccSimButton->setFixedSize(24, 24);
    m_iccSimButton->setIcon(QIcon(QStringLiteral(":/icons/icc.svg")));
    m_iccSimButton->setIconSize(QSize(16, 16));
    m_iccSimButton->setFocusPolicy(Qt::NoFocus);
    m_iccSimButton->setCursor(Qt::PointingHandCursor);
    m_iccSimButton->setStyleSheet(toggleIconButtonStyle());
    m_iccSimButton->setToolTip(
        QStringLiteral("ICC 模拟器：打开一个悬浮窗口\n"
                       "拖到画面上即可实时模拟亮度、对比度、饱和度、色温、伽马"));
    m_iccSimButton->setAccessibleName(QStringLiteral("ICC 模拟器"));
    connect(m_iccSimButton, &QPushButton::clicked,
            this, &CullWindow::toggleIccSimulator);
    topBarLayout->addWidget(m_iccSimButton);

    // 「ICC 模拟器」右边的帮助：功能介绍、标准工作流示例、快捷键与作者联系方式。
    // 非模态窗口，可以开着说明对照操作；再次点击只把已有窗口置前。
    m_helpButton = new QPushButton(leftTopBar);
    m_helpButton->setFixedSize(24, 24);
    m_helpButton->setIcon(QIcon(QStringLiteral(":/icons/help.svg")));
    m_helpButton->setIconSize(QSize(16, 16));
    m_helpButton->setFocusPolicy(Qt::NoFocus);
    m_helpButton->setCursor(Qt::PointingHandCursor);
    m_helpButton->setStyleSheet(toggleIconButtonStyle());
    m_helpButton->setToolTip(
        QStringLiteral("帮助：功能介绍、标准工作流示例、快捷键与联系方式"));
    m_helpButton->setAccessibleName(QStringLiteral("帮助"));
    connect(m_helpButton, &QPushButton::clicked, this, &CullWindow::openHelp);
    topBarLayout->addWidget(m_helpButton);

    topBarLayout->addStretch(1);

    // 顶部栏与滚动区之间的一条细分隔线，和分组之间保持同一种分隔语言
    auto* topBarDivider = new QWidget(top);
    topBarDivider->setFixedHeight(1);
    topBarDivider->setStyleSheet(QStringLiteral("background: #333333;"));

    auto* leftColumn = new QWidget(top);
    auto* leftColumnLayout = new QVBoxLayout(leftColumn);
    leftColumnLayout->setContentsMargins(0, 0, 0, 0);
    leftColumnLayout->setSpacing(0);
    leftColumnLayout->addWidget(leftTopBar);
    leftColumnLayout->addWidget(topBarDivider);

    // 左侧分组变多以后，窗口较矮时整栏可以纵向滚动，
    // 避免最下面的「分拣日志」被挤出可视区域。
    auto* leftScroll = new QScrollArea(leftColumn);
    // 左栏滚动区自己不接收键盘焦点：否则 Tab / 点击空白处后焦点落在滚动区，
    // 方向键只会滚动参数栏而不会用来挑图。滚轮和滚动条照常工作。
    leftScroll->setFocusPolicy(Qt::NoFocus);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 纵向滚动条常驻占位：折叠 / 展开分组时内容高度变化，按需显示的滚动条
    // 会时隐时现，viewport 宽度随之在 10px 上来回跳，看起来像左栏宽度变了。
    // 这里始终保留滚动条的位置；没有可滚动范围时把它禁用，
    // 由全局样式表把禁用态滑块画成透明，视觉上是一条空白的右侧留白。
    leftScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    QScrollBar* leftScrollBar = leftScroll->verticalScrollBar();
    leftScrollBar->setEnabled(false);
    connect(leftScrollBar, &QScrollBar::rangeChanged, leftScrollBar,
            [leftScrollBar](int minimum, int maximum) {
                leftScrollBar->setEnabled(maximum > minimum);
            });
    leftScrollBar->setEnabled(leftScrollBar->maximum() > leftScrollBar->minimum());
    leftScroll->setWidget(leftContent);
    // 只给 viewport 自己上底色：用对象名选择器，避免把 #262626 波及到
    // 里面的按钮 / 输入框（它们有各自的全局样式）。
    leftScroll->viewport()->setObjectName(QStringLiteral("CullLeftViewport"));
    leftScroll->viewport()->setStyleSheet(
        QStringLiteral("QWidget#CullLeftViewport { background: #262626; }"));
    leftColumnLayout->addWidget(leftScroll, 1);
    // m_leftPanel 指向整列（顶部栏 + 滚动区），全屏时整列一起隐藏
    m_leftPanel = leftColumn;
    QWidget* previewPanel = buildPreviewPanel();
    top->addWidget(m_leftPanel);
    top->addWidget(previewPanel);
    top->setStretchFactor(0, 0);
    top->setStretchFactor(1, 1);
    top->setSizes({340, 1120});
    top->setChildrenCollapsible(false);

    m_thumbnailPanel = buildThumbnailPanel();
    // ui_color.md：工作台侧栏 / 缩略图面板 #262626
    m_leftPanel->setObjectName(QStringLiteral("CullLeftPanel"));
    m_thumbnailPanel->setObjectName(QStringLiteral("CullThumbnailPanel"));
    // 缩略图悬停 / 选中：只改底色，不加位移或缩放，避免网格抖动
    setStyleSheet(QStringLiteral(
        "QWidget#CullLeftPanel, QWidget#CullThumbnailPanel { background: #262626; }"
        // outline: none 关掉 Fusion 在选中项文字周围画的焦点虚线框。
        // 该项已经有 #3c5068 的选中底色，虚线框只会让文件名上面多一条细线。
        "QWidget#CullThumbnailPanel QListWidget {"
        " background: #262626; border: none; outline: none; }"
        "QWidget#CullThumbnailPanel QListWidget::item { border-radius: 4px; }"
        "QWidget#CullThumbnailPanel QListWidget::item:hover { background: #303030; }"
        "QWidget#CullThumbnailPanel QListWidget::item:selected {"
        " background: #3c5068; color: #ffffff; }"));

    vertical->addWidget(top);
    vertical->addWidget(m_thumbnailPanel);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 1);
    vertical->setSizes({620, 260});
    vertical->setChildrenCollapsible(false);

    outer->addWidget(vertical);
    setCentralWidget(central);

    statusBar()->showMessage(QStringLiteral("就绪"));

    // 右下角常驻挑片进度（已标记 / 已处理 / 总数）：左侧状态文字会随操作
    // 不断变化，进度放进状态栏的永久区域，挑片过程中始终可见。
    m_selectionLabel = new QLabel(statusBar());
    m_selectionLabel->setStyleSheet(
        QStringLiteral("color: #b8b8b8; padding-right: 6px;"));
    statusBar()->addPermanentWidget(m_selectionLabel);
    updateSelectionInfo();
}

QWidget* CullWindow::buildLeftPanel()
{
    auto* panel = new QWidget;
    panel->setMinimumWidth(300);
    // QScrollArea 会给内容控件打开 autoFillBackground，默认用调色板的
    // Window 色（#353535）填底；分组之间的 8px 间距和面板内边距会露出这层颜色，
    // 折叠后看起来比分组内容（#262626）亮一截。这里统一成面板底色。
    panel->setObjectName(QStringLiteral("CullLeftPanelContent"));
    panel->setStyleSheet(
        QStringLiteral("QWidget#CullLeftPanelContent { background: #262626; }"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    // ---- 目录 ----
    // 左侧设置改成纵向折叠分组：低频项（排序、日志）收起后，
    // 常用项仍有空间；交互沿用 Lightroom 参数栏的习惯。
    auto* dirSection =
        new CollapsibleSection(QStringLiteral("源 / 目标目录"), panel);
    dirSection->setSettingsKey(QStringLiteral("cull/section/directories"), true);
    auto* dirContent = new QWidget(dirSection);
    auto* dirGrid = new QGridLayout(dirContent);
    dirGrid->setContentsMargins(0, 0, 0, 0);
    m_sourceEdit = new QLineEdit(dirContent);
    m_sourceEdit->setPlaceholderText(QStringLiteral("RAW / JPG 所在目录"));
    // 源目录只允许通过右侧的铅笔图标按钮选择：只读避免手输拼错路径，
    // 同时保留文本选择能力，方便复制路径。
    m_sourceEdit->setReadOnly(true);
    // 外观与下方目标目录的路径一致：只显示文字，不画输入框的边框和底色。
    // 仍然保留 QLineEdit 的能力（可选中、可复制路径）。
    m_sourceEdit->setObjectName(QStringLiteral("CullSourcePath"));
    m_sourceEdit->setStyleSheet(QStringLiteral(
        "QLineEdit#CullSourcePath, QLineEdit#CullSourcePath:hover,"
        " QLineEdit#CullSourcePath:focus {"
        "  background: transparent; border: none; padding: 0;"
        "  color: #d0d0d0; selection-background-color: #3d5f80;"
        "  selection-color: #ffffff; }"));
    // 有路径时右对齐：路径超出输入框宽度时优先显示层级最深的尾部，
    // 和下方目标目录的路径保持同一阅读方向；空路径时左对齐，
    // 让占位提示保持正常的阅读位置。
    m_sourceEdit->setAlignment(Qt::AlignLeft);
    m_sourceEdit->setToolTip(QStringLiteral("只能通过右侧的图标按钮选择源目录，不能手动输入"));
    // 右对齐后左侧会被裁掉，把完整路径放进 tooltip 方便查看
    connect(m_sourceEdit, &QLineEdit::textChanged, m_sourceEdit,
            [this](const QString& text) {
                m_sourceEdit->setAlignment(text.isEmpty() ? Qt::AlignLeft
                                                          : Qt::AlignRight);
                m_sourceEdit->setToolTip(
                    text.isEmpty()
                        ? QStringLiteral("只能通过右侧的图标按钮选择源目录，不能手动输入")
                        : QStringLiteral("%1\n只能通过右侧的图标按钮选择源目录，不能手动输入")
                              .arg(QDir::toNativeSeparators(text)));
            });

    // 与目标目录行的「更改」按钮用同一颗铅笔图标：两处做的是同一件事
    // （重新选择一个目录），图标不该分成文件夹 / 铅笔两套。
    auto* sourceButton = new QPushButton(dirContent);
    sourceButton->setFixedSize(22, 22);
    sourceButton->setIcon(QIcon(QStringLiteral(":/icons/edit.svg")));
    sourceButton->setIconSize(QSize(14, 14));
    sourceButton->setFocusPolicy(Qt::NoFocus);
    sourceButton->setCursor(Qt::PointingHandCursor);
    sourceButton->setStyleSheet(iconButtonStyle());
    sourceButton->setToolTip(QStringLiteral("更改源目录"));
    sourceButton->setAccessibleName(QStringLiteral("更改源目录"));

    // 目标目录可以有多个，每个目录一个标记颜色：挑片时点颜色圆点，
    // 批量复制时按颜色把图片分发到对应目录。这里只放容器，
    // 具体行由 rebuildTargetDirRows() 按 m_targetDirs 动态生成。
    m_targetRowsHost = dirContent;
    m_targetRowsLayout = new QVBoxLayout;
    m_targetRowsLayout->setContentsMargins(0, 0, 0, 0);
    m_targetRowsLayout->setSpacing(3);
    // 按钮不直接放进网格：位置由 rebuildTargetDirRows() 决定，
    // 没有目录行时和「目标目录」标题同一行，有目录行时排在所有行下面。
    m_addTargetButton = new QPushButton(QStringLiteral("＋ 添加目标目录"), dirContent);
    m_addTargetButton->setFocusPolicy(Qt::NoFocus);
    m_addTargetButton->setAccessibleName(QStringLiteral("添加目标目录"));
    m_addTargetButton->setToolTip(
        QStringLiteral("每个目标目录自动分配一种标记颜色（最多 %1 个）")
            .arg(kMaxTargetDirs));

    m_recursiveCheck = new QCheckBox(QStringLiteral("包含子文件夹"), dirContent);
    m_recursiveCheck->setToolTip(QStringLiteral("需求默认不递归；勾选后扫描整棵目录树。"));

    // 递归开关紧跟源目录摆放：它只管源目录的扫描范围，
    // 放到目标目录下面容易被误以为是"复制到子文件夹"。
    dirGrid->addWidget(new QLabel(QStringLiteral("源目录"), dirContent), 0, 0);
    dirGrid->addWidget(m_sourceEdit, 0, 1);
    dirGrid->addWidget(sourceButton, 0, 2, Qt::AlignVCenter);
    dirGrid->addWidget(m_recursiveCheck, 1, 1, 1, 2);
    // 「目标目录」标题固定贴在第一个目录行左侧：目录变多时不再跟着整块
    // 垂直居中（否则会跑到中间某一行旁边）。与上面的「包含子文件夹」
    // 之间留 4px 空隙，两组参数不会挤在一起。
    auto* targetGap = new QWidget(dirContent);
    targetGap->setFixedHeight(4);
    dirGrid->addWidget(targetGap, 2, 0, 1, 3);
    auto* targetCaption = new QLabel(QStringLiteral("目标目录"), dirContent);
    // 首行按钮高 22px、文字约 16px：上边距 3px 让标题正好落在首行中线
    targetCaption->setContentsMargins(0, 3, 0, 0);
    dirGrid->addWidget(targetCaption, 3, 0, Qt::AlignLeft | Qt::AlignTop);
    dirGrid->addLayout(m_targetRowsLayout, 3, 1, 1, 2);
    dirGrid->setColumnStretch(1, 1);
    m_dirGrid = dirGrid;
    dirSection->setContent(dirContent);

    connect(sourceButton, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择源目录"), m_sourceEdit->text());
        if (!dir.isEmpty()) {
            m_sourceEdit->setText(QDir::toNativeSeparators(dir));
            scanDirectory();
            saveSettings();
        }
    });
    connect(m_addTargetButton, &QPushButton::clicked, this,
            &CullWindow::addTargetDirectory);

    // ---- 挑片动作 ----
    // 复制与导出都是"对当前选择做什么"，集中放在左侧固定位置：
    // 预览区域留给画面、标记颜色与星级，按钮不会随窗口缩放忽左忽右。
    auto* actionSection =
        new CollapsibleSection(QStringLiteral("选片操作"), panel);
    actionSection->setSettingsKey(QStringLiteral("cull/section/actions"), true);
    auto* actionContent = new QWidget(actionSection);
    auto* actionGrid = new QGridLayout(actionContent);
    actionGrid->setContentsMargins(0, 0, 0, 0);
    actionGrid->setSpacing(6);

    m_copyButton = new QPushButton(QStringLiteral("复制到目标目录"), actionContent);
    m_copyButton->setToolTip(
        QStringLiteral("把当前图片复制到它标记颜色对应的目标目录（不是移动）。\n"
                       "同名文件自动追加序号，不会覆盖。"));
    // "&&" 转义：QPushButton 会把单个 & 当成助记符，导致按钮上不显示 &
    m_copyNextButton = new QPushButton(QStringLiteral("复制&&下一张"), actionContent);
    m_copyNextButton->setToolTip(
        QStringLiteral("快捷键 Space：按标记颜色复制后自动切到下一张"));
    m_copyMarkedButton =
        new QPushButton(QStringLiteral("批量复制标记图片到对应目录"), actionContent);
    m_copyMarkedButton->setToolTip(
        QStringLiteral("把所有已标记颜色的图片，分别复制到对应颜色的目标目录。\n"
                       "未标记颜色、或颜色没有对应目录的图片会被跳过。"));
    // CSV 导出的是整个源目录的选片清单，按钮直接写「导出 CSV 目录」；
    // 另外两个按钮不再带省略号，执行动作时本来就会弹对话框。
    auto* csvButton = new QPushButton(QStringLiteral("导出 CSV 目录"), actionContent);
    csvButton->setToolTip(QStringLiteral("把当前目录的星级、颜色标记与 EXIF 信息导出成 CSV 表"));
    auto* renameButton = new QPushButton(QStringLiteral("批量重命名"), actionContent);
    renameButton->setToolTip(QStringLiteral("按规则重命名当前选中的图片（可多选）"));
    auto* copySelectionButton = new QPushButton(QStringLiteral("创建副本"), actionContent);
    copySelectionButton->setToolTip(
        QStringLiteral("在当前源目录里为选中的图片创建文件副本\n"
                       "文件名追加「 - 副本」，已有同名副本时追加编号，不会覆盖"));
    auto* sampleButton = new QPushButton(QStringLiteral("截取构图视野"), actionContent);
    sampleButton->setToolTip(
        QStringLiteral("按当前取景框截取画面，直接保存到原图所在目录\n"
                       "文件名为「原名_composition_编号.jpg」，不会覆盖原图"));

    actionGrid->addWidget(m_copyButton, 0, 0);
    actionGrid->addWidget(m_copyNextButton, 0, 1);
    actionGrid->addWidget(m_copyMarkedButton, 1, 0, 1, 2);
    actionGrid->addWidget(csvButton, 2, 0);
    actionGrid->addWidget(renameButton, 2, 1);
    actionGrid->addWidget(copySelectionButton, 3, 0);
    actionGrid->addWidget(sampleButton, 3, 1);
    actionGrid->setColumnStretch(0, 1);
    actionGrid->setColumnStretch(1, 1);

    // 动作按钮不抢焦点：点击后焦点仍留在列表 / 画布上，
    // 方向键与 Space 可以继续高速挑片。
    for (QPushButton* button : {m_copyButton, m_copyNextButton, m_copyMarkedButton,
                                csvButton, renameButton, copySelectionButton,
                                sampleButton}) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    connect(csvButton, &QPushButton::clicked, this, [this]() { exportCsv(); });
    connect(renameButton, &QPushButton::clicked, this, [this]() { renameSelection(); });
    connect(copySelectionButton, &QPushButton::clicked, this,
            [this]() { createSelectionCopies(); });
    connect(sampleButton, &QPushButton::clicked, this,
            [this]() { captureCompositionSample(); });
    actionSection->setContent(actionContent);

    // ---- 筛选与排序 ----
    auto* viewSection =
        new CollapsibleSection(QStringLiteral("筛选与排序"), panel);
    viewSection->setSettingsKey(QStringLiteral("cull/section/filter"), true);
    auto* viewContent = new QWidget(viewSection);
    auto* viewGrid = new QGridLayout(viewContent);
    viewGrid->setContentsMargins(0, 0, 0, 0);
    m_formatCombo = new WheelGuardComboBox(viewContent);
    m_formatCombo->addItems({QStringLiteral("全部格式"),
                             QStringLiteral("仅 RAW"),
                             QStringLiteral("仅普通图片")});
    m_filterCombo = new WheelGuardComboBox(viewContent);
    // 选项由 rebuildFilterCombo() 统一生成：星级用 ★ 的个数表示，
    // 已标记颜色带颜色圆点和名称。目标目录加载 / 变化后会再重建一次。
    rebuildFilterCombo();
    m_sortCombo = new WheelGuardComboBox(viewContent);
    m_sortCombo->addItems({QStringLiteral("文件名"),
                           QStringLiteral("拍摄时间"),
                           QStringLiteral("文件大小")});
    m_descendingCheck = new QCheckBox(QStringLiteral("降序"), viewContent);

    // 时间分组：勾选后缩略图按拍摄时间连续排列，
    // 相邻两张间隔超过阈值就插入一条分组标题，时间相近的照片自然成组。
    m_timeGroupCheck = new QCheckBox(QStringLiteral("时间分组"), viewContent);
    m_timeGroupCheck->setToolTip(
        QStringLiteral("按拍摄时间排序，并把时间相近的照片分成一组。\n"
                       "分组时排序下拉框不生效，方向仍由「降序」控制。"));
    m_timeGroupGapCombo = new WheelGuardComboBox(viewContent);
    m_timeGroupGapCombo->addItem(QStringLiteral("1 分钟"), 1);
    m_timeGroupGapCombo->addItem(QStringLiteral("5 分钟"), 5);
    m_timeGroupGapCombo->addItem(QStringLiteral("15 分钟"), 15);
    m_timeGroupGapCombo->addItem(QStringLiteral("30 分钟"), 30);
    m_timeGroupGapCombo->addItem(QStringLiteral("1 小时"), 60);
    m_timeGroupGapCombo->addItem(QStringLiteral("3 小时"), 180);
    m_timeGroupGapCombo->addItem(QStringLiteral("6 小时"), 360);
    m_timeGroupGapCombo->addItem(QStringLiteral("1 天"), 1440);
    m_timeGroupGapCombo->setCurrentIndex(3);

    viewGrid->addWidget(new QLabel(QStringLiteral("格式"), viewContent), 0, 0);
    viewGrid->addWidget(m_formatCombo, 0, 1, 1, 2);
    viewGrid->addWidget(new QLabel(QStringLiteral("显示"), viewContent), 1, 0);
    viewGrid->addWidget(m_filterCombo, 1, 1, 1, 2);
    viewGrid->addWidget(new QLabel(QStringLiteral("排序"), viewContent), 2, 0);
    viewGrid->addWidget(m_sortCombo, 2, 1);
    viewGrid->addWidget(m_descendingCheck, 2, 2);
    viewGrid->addWidget(new QLabel(QStringLiteral("分组"), viewContent), 3, 0);
    viewGrid->addWidget(m_timeGroupCheck, 3, 1, 1, 2);
    viewGrid->addWidget(new QLabel(QStringLiteral("间隔"), viewContent), 4, 0);
    viewGrid->addWidget(m_timeGroupGapCombo, 4, 1, 1, 2);
    viewGrid->setColumnStretch(1, 1);
    viewSection->setContent(viewContent);

    // ---- 分拣日志 ----
    auto* logSection = new CollapsibleSection(QStringLiteral("分拣日志"), panel);
    logSection->setSettingsKey(QStringLiteral("cull/section/log"), true);
    auto* logContent = new QWidget(logSection);
    auto* logLayout = new QVBoxLayout(logContent);
    logLayout->setContentsMargins(0, 0, 0, 0);
    m_logView = new QTextEdit(logContent);
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(120);
    m_logView->setPlaceholderText(QStringLiteral("复制动作会记录在这里"));
    // 只读日志：点击后可以选中 / 复制文字，但不参与 Tab 键与滚轮焦点链，
    // 避免启动时把键盘焦点从缩略图列表抢走。
    m_logView->setFocusPolicy(Qt::ClickFocus);
    // 即使点进了日志，方向键也仍然用来翻图（见 eventFilter）
    m_logView->installEventFilter(this);
    auto* clearLogButton = new QPushButton(QStringLiteral("清空日志"), logContent);
    logLayout->addWidget(m_logView, 1);
    logLayout->addWidget(clearLogButton, 0, Qt::AlignRight);
    logSection->setContent(logContent);

    connect(clearLogButton, &QPushButton::clicked, this, [this]() {
        if (m_store) {
            m_store->clearLog();
        }
        m_logView->clear();
        showStatus(QStringLiteral("分拣日志已清空"));
    });

    layout->addWidget(dirSection);
    layout->addWidget(actionSection);
    layout->addWidget(viewSection);
    layout->addWidget(logSection);
    // 空白留在最后一个分组的下方：如果把拉伸因子直接给分拣日志，
    // 折叠后多出来的高度会被 QVBoxLayout 摊到其它分组里，
    // 最后一个分组就会被推离上一栏。这里让所有分组都保持自然高度。
    layout->addStretch(1);
    return panel;
}

QWidget* CullWindow::buildPreviewPanel()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // ---- 构图与视图（浮层面板的内容，浮层本身在预览控件之后创建）----
    // 左侧栏顶部栏的图标按钮负责开关这块面板；面板叠在画面上，
    // 不再占用预览区顶部的纵向空间。
    // 按操作顺序分成几行：开关 → 比例 → 取景框样式 → 辅助线 / 镜头 / 视图。
    auto* compContent = new QWidget(panel);
    auto* compLayout = new QVBoxLayout(compContent);
    compLayout->setContentsMargins(0, 0, 0, 0);
    // 面板里的项目之间留出更明显的呼吸感，逐行扫参数时不至于挤在一起
    compLayout->setSpacing(12);

    // 开关单独占一行并加粗：固定视野没开启时，下面的比例等参数没有意义，
    // 因此明确"先开固定视野，再选比例"的操作顺序。
    m_compositionCheck = new QCheckBox(QStringLiteral("固定视野"), compContent);
    {
        QFont switchFont = m_compositionCheck->font();
        switchFont.setBold(true);
        switchFont.setPointSizeF(switchFont.pointSizeF() + 1.0);
        m_compositionCheck->setFont(switchFont);
    }
    m_compositionCheck->setToolTip(QStringLiteral("快捷键 R：开关固定视野构图"));
    auto* switchHint =
        new QLabel(QStringLiteral("开启后按所选比例显示取景框，框外压暗"), compContent);
    switchHint->setStyleSheet(QStringLiteral("color: #b0b0b0;"));
    auto* switchRow = new QHBoxLayout;
    switchRow->setSpacing(8);
    switchRow->addWidget(m_compositionCheck);
    switchRow->addWidget(switchHint, 1);
    compLayout->addLayout(switchRow);

    m_aspectCombo = new WheelGuardComboBox(compContent);
    for (const AspectPreset& preset : kAspectPresets) {
        m_aspectCombo->addItem(QString::fromUtf8(preset.label));
    }
    m_aspectCombo->setToolTip(
        QStringLiteral("快捷键 Ctrl+1~5 切换预设比例；\n"
                       "需求中的 1~5 已用于星级，因此比例放在 Ctrl 组。"));
    // 自定义比例拆成"宽 / 高"两个输入框，宽高比 = 宽 ÷ 高。
    // 整组只在比例下拉选中「自定义」时显示，避免常驻一行用不上的输入框。
    m_customAspectBox = new QWidget(compContent);
    auto* customAspectRow = new QHBoxLayout(m_customAspectBox);
    customAspectRow->setContentsMargins(0, 0, 0, 0);
    customAspectRow->setSpacing(4);
    m_customAspectWidth = new QDoubleSpinBox(m_customAspectBox);
    m_customAspectWidth->setRange(0.1, 99.0);
    m_customAspectWidth->setDecimals(2);
    m_customAspectWidth->setSingleStep(0.01);
    m_customAspectWidth->setValue(1.85);
    m_customAspectWidth->setFixedWidth(66);
    // 自定义比例只需要输入：去掉右侧的上下调节箭头，避免误点改值
    m_customAspectWidth->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_customAspectWidth->setToolTip(QStringLiteral("自定义比例的宽度，例如 1.85"));
    m_customAspectHeight = new QDoubleSpinBox(m_customAspectBox);
    m_customAspectHeight->setRange(0.1, 99.0);
    m_customAspectHeight->setDecimals(2);
    m_customAspectHeight->setSingleStep(0.01);
    m_customAspectHeight->setValue(1.0);
    m_customAspectHeight->setFixedWidth(66);
    m_customAspectHeight->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_customAspectHeight->setToolTip(QStringLiteral("自定义比例的高度，例如 1"));
    customAspectRow->addWidget(m_customAspectWidth);
    auto* aspectColon = new QLabel(QStringLiteral(":"), m_customAspectBox);
    aspectColon->setAlignment(Qt::AlignCenter);
    customAspectRow->addWidget(aspectColon);
    customAspectRow->addWidget(m_customAspectHeight);

    m_frameColorButton = new QToolButton(compContent);
    m_frameColorButton->setText(QStringLiteral("框线"));
    m_frameColorButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_frameColorButton->setIconSize(QSize(16, 16));
    m_frameColorButton->setToolTip(QStringLiteral("选择构图框边框颜色；框线固定 1px"));
    // QToolButton 不吃 QPushButton 的全局样式，这里对齐普通按钮的外观，
    // 免得取色按钮在表单里显得像一块没有边框的纯文字。
    m_frameColorButton->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  background: #333333;"
        "  border: 1px solid #474747;"
        "  border-radius: 3px;"
        "  padding: 3px 8px;"
        "  color: #dcdcdc;"
        "}"
        "QToolButton:hover { background: #3d3d3d; border-color: #5a5a5a; }"
        "QToolButton:pressed { background: #2a2a2a; }"
        "QToolButton:focus { border-color: #7fb3e8; }"));

    m_maskCheck = new QCheckBox(QStringLiteral("显示压暗"), compContent);
    m_maskCheck->setToolTip(QStringLiteral("快捷键 M：一键开关框外暗化遮罩"));
    m_ratioLabelCheck = new QCheckBox(QStringLiteral("比例文字"), compContent);
    m_ratioLabelCheck->setToolTip(
        QStringLiteral("显示 / 隐藏取景框左上角的比例文字。\n"
                       "文字写法与左侧「比例」下拉保持一致。"));

    m_thirdsCheck = new QCheckBox(QStringLiteral("构图线"), compContent);
    m_thirdsCheck->setToolTip(QStringLiteral("显示三分法构图线，辅助安排主体位置"));

    // ---- 参数行：比例 / 取景框 / 辅助线 ----
    // 旋转、全屏、适合窗口这三个高频的“看”操作都做成了图标按钮，
    // 统一放在信息卡右侧的 2×2 宫格里，参数表单不再出现按钮；
    // 镜头畸变校正属于全局渲染选项，挪到左侧顶部栏的「设置」浮层。
    auto* compForm = new QFormLayout;
    compForm->setContentsMargins(0, 0, 0, 0);
    compForm->setHorizontalSpacing(8);
    compForm->setVerticalSpacing(10);
    compForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto* aspectRow = new QHBoxLayout;
    aspectRow->setSpacing(6);
    // 比例下拉按内容取宽，不再拉伸铺满整行：条目都是短文本，
    // 过长的下拉条既浪费空间，也让右侧「自定义」输入框离得很远。
    m_aspectCombo->setFixedWidth(110);
    aspectRow->addWidget(m_aspectCombo);
    aspectRow->addWidget(m_customAspectBox);
    aspectRow->addStretch(1);
    compForm->addRow(QStringLiteral("比例"), aspectRow);

    auto* frameRow = new QHBoxLayout;
    frameRow->setSpacing(6);
    frameRow->addWidget(m_frameColorButton);
    frameRow->addWidget(new QLabel(QStringLiteral("压暗"), compContent));
    // 框外压暗范围 40%~100%，默认 75%：60% 时压暗不够明显，
    // 上限放开到 100% 可以让框外完全变黑，突出取景框。
    // 只保留滑条 + 数值展示，不再提供数值框：拖拽已足够精确，
    // 也避免一行里塞太多控件。
    m_dimSlider = new QSlider(Qt::Horizontal, compContent);
    m_dimSlider->setRange(kDimMin, kDimMax);
    m_dimSlider->setValue(kDimDefault);
    m_dimSlider->setFixedWidth(120);
    m_dimSlider->setToolTip(
        QStringLiteral("拖动调节框外纯黑遮罩透明度（40~100%），双击恢复默认 75%"));
    frameRow->addWidget(m_dimSlider);
    m_dimValueLabel = new QLabel(QStringLiteral("%1%").arg(kDimDefault), compContent);
    m_dimValueLabel->setMinimumWidth(38);
    m_dimValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_dimValueLabel->setToolTip(QStringLiteral("框外遮罩透明度（40~100%，纯黑无彩色）"));
    frameRow->addWidget(m_dimValueLabel);
    // 沿用参数栏的统一交互：点中滑杆后滚轮才改值，双击恢复默认 75%
    m_dimSlider->installEventFilter(this);
    frameRow->addWidget(m_maskCheck);
    frameRow->addWidget(m_ratioLabelCheck);
    frameRow->addStretch(1);
    compForm->addRow(QStringLiteral("取景框"), frameRow);

    auto* guideRow = new QHBoxLayout;
    guideRow->setSpacing(12);
    guideRow->addWidget(m_thirdsCheck);
    guideRow->addStretch(1);
    compForm->addRow(QStringLiteral("辅助线"), guideRow);

    compLayout->addLayout(compForm);

    // ---- 当前图片信息栏（需求 §一 2）----
    m_infoLabel = new QLabel(panel);
    m_infoLabel->setTextFormat(Qt::RichText);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoLabel->setMinimumHeight(66);
    // 信息卡：多行内容（文件名 / EXIF / 可选拍摄信息 / 标记），内边距略放宽，
    // 和左侧参数栏 8px 的呼吸感保持一致。
    m_infoLabel->setStyleSheet(
        QStringLiteral("QLabel { background: #2b2b2b; border: 1px solid #333333;"
                       " border-radius: 4px; padding: 7px 10px; }"));

    // 信息卡右侧的视图控制，2×2 宫格：
    //   上排：向左旋转 / 向右旋转（成对，改的是朝向）
    //   下排：适合窗口 / 全屏（成对，改的是“看多大”）
    // 都是透明底的小图标按钮，整块贴在卡片右边、与三行信息垂直居中。
    auto* infoRow = new QWidget(panel);
    auto* infoLayout = new QHBoxLayout(infoRow);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(6);
    infoLayout->addWidget(m_infoLabel, 1);

    auto* viewControls = new QWidget(infoRow);
    auto* viewControlsLayout = new QGridLayout(viewControls);
    viewControlsLayout->setContentsMargins(0, 0, 0, 0);
    viewControlsLayout->setHorizontalSpacing(4);
    viewControlsLayout->setVerticalSpacing(4);

    // 视图控制与标记组件两侧的箭头共用这个工厂：22px 命中区域 + 14px 图标，
    // 不显示文字，外观完全一致。图标由调用方传入，方便箭头用带禁用态的版本。
    auto makeViewButton = [](QWidget* parent, const QIcon& icon,
                             const QString& tip, const QString& accessibleName) {
        auto* button = new QPushButton(parent);
        button->setFixedSize(22, 22);
        button->setIcon(icon);
        button->setIconSize(QSize(14, 14));
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(iconButtonStyle());
        button->setToolTip(tip);
        button->setAccessibleName(accessibleName);
        return button;
    };

    QPushButton* rotateLeftButton = makeViewButton(
        viewControls,
        QIcon(QStringLiteral(":/icons/rotate-left.svg")),
        QStringLiteral("向左旋转 90°（仅预览，不写入文件）"),
        QStringLiteral("向左旋转"));
    QPushButton* rotateRightButton = makeViewButton(
        viewControls,
        QIcon(QStringLiteral(":/icons/rotate-right.svg")),
        QStringLiteral("向右旋转 90°（仅预览，不写入文件）"),
        QStringLiteral("向右旋转"));
    // 双击画面也能适合窗口，这里给它同样的图标按钮，位置固定好找
    QPushButton* fitButton = makeViewButton(
        viewControls,
        QIcon(QStringLiteral(":/icons/fit.svg")),
        QStringLiteral("适合窗口：把画面缩放到正好放得下（双击画面同效）"),
        QStringLiteral("适合窗口"));
    // 全屏按钮的图标随状态切换（进入全屏后变成「退出全屏」的四角内收图标），
    // 因此保存成成员。全屏时只隐藏左侧设置与缩略图面板，预览面板仍可见。
    m_fullScreenButton = makeViewButton(
        viewControls,
        QIcon(QStringLiteral(":/icons/fullscreen.svg")),
        QStringLiteral("进入全屏预览（快捷键 F）"),
        QStringLiteral("进入全屏"));

    viewControlsLayout->addWidget(rotateLeftButton, 0, 0);
    viewControlsLayout->addWidget(rotateRightButton, 0, 1);
    viewControlsLayout->addWidget(fitButton, 1, 0);
    viewControlsLayout->addWidget(m_fullScreenButton, 1, 1);
    infoLayout->addWidget(viewControls, 0, Qt::AlignVCenter);

    connect(rotateLeftButton, &QPushButton::clicked, this, [this]() {
        m_preview->rotateBy(-90);
    });
    connect(rotateRightButton, &QPushButton::clicked, this, [this]() {
        m_preview->rotateBy(90);
    });
    connect(fitButton, &QPushButton::clicked, this, [this]() {
        m_preview->resetView();
    });
    connect(m_fullScreenButton, &QPushButton::clicked, this, [this]() {
        setFullScreenMode(!m_fullScreen);
    });

    // ---- 预览 ----
    m_preview = new CullPreviewWidget(panel);
    m_preview->setToolTip(
        QStringLiteral("滚轮：缩放（围绕光标）\n"
                       "拖动：平移；双击：适合窗口\n"
                       "← →：上一张 / 下一张\n"
                       "↑ ↓：缩略图按行上下移动\n"
                       "放大超过预览分辨率后会自动生成 1:1 细节"));
    m_preview->installEventFilter(this);
    // 放大到预览图像素不够用时，按需补一张原图分辨率的细节图
    connect(m_preview, &CullPreviewWidget::zoomChanged,
            this, [this](double) { maybeRequestDetail(); });

    // ---- 构图与视图浮层 ----
    // 面板是预览控件的子控件，直接叠在画面左上角，不参与布局，因此
    // 既不会占掉画面的纵向空间，也能放下「框线 + 压暗滑条 + 勾选项」一整行。
    // 开关放在预览下方标记行最左边的图标按钮上，右上角的叉号与 Esc 同样可以收起。
    auto* compPanel = new QFrame(m_preview);
    compPanel->setObjectName(QStringLiteral("CullCompositionPanel"));
    compPanel->setAttribute(Qt::WA_StyledBackground, true);
    // 预览控件为了拖动画布把自己设成了手型指针，子控件会一路继承下来，
    // 鼠标停在这块面板上也会变成手；面板里的控件不参与拖拽，显式改回箭头。
    // 圆点 / 图标按钮各自设过手型指针，不受影响。
    compPanel->setCursor(Qt::ArrowCursor);
    compPanel->setStyleSheet(QStringLiteral(
        "QFrame#CullCompositionPanel { background: rgba(28, 28, 28, 244);"
        " border: 1px solid #5f5f5f; border-radius: 8px; }"));
    auto* compPanelLayout = new QVBoxLayout(compPanel);
    compPanelLayout->setContentsMargins(12, 9, 12, 12);
    compPanelLayout->setSpacing(8);

    auto* compTitleRow = new QHBoxLayout;
    compTitleRow->setSpacing(8);
    auto* compTitle =
        new QLabel(QStringLiteral("构图与视图（仅预览，不修改原图）"), compPanel);
    compTitle->setStyleSheet(
        QStringLiteral("color: #e6e6e6; font-weight: 600; background: transparent;"));
    compTitleRow->addWidget(compTitle);
    compTitleRow->addStretch(1);
    auto* compCloseButton = new QPushButton(compPanel);
    compCloseButton->setFixedSize(20, 20);
    compCloseButton->setIcon(QIcon(QStringLiteral(":/icons/remove.svg")));
    compCloseButton->setIconSize(QSize(12, 12));
    compCloseButton->setFocusPolicy(Qt::NoFocus);
    compCloseButton->setCursor(Qt::PointingHandCursor);
    compCloseButton->setStyleSheet(iconButtonStyle());
    compCloseButton->setToolTip(QStringLiteral("收起构图与视图面板（Esc）"));
    compCloseButton->setAccessibleName(QStringLiteral("收起构图与视图"));
    compTitleRow->addWidget(compCloseButton);
    compPanelLayout->addLayout(compTitleRow);
    // compContent 先建在预览面板下，加进浮层的布局时会被自动重新挂到浮层里
    compPanelLayout->addWidget(compContent);
    compPanel->hide();
    m_compositionPanel = compPanel;

    connect(compCloseButton, &QPushButton::clicked, this, [this]() {
        setCompositionPanelVisible(false);
    });

    // ---- 设置浮层 ----
    // 与构图浮层同一套做法：预览的子控件、贴左上角、不参与布局。
    // 两个浮层互斥（打开一个自动收起另一个），都不会在画面上叠加。
    m_settingsPanel = buildSettingsPanel(m_preview);
    m_settingsPanel->hide();
    if (m_settingsButton != nullptr) {
        connect(m_settingsButton, &QPushButton::toggled, this, [this](bool on) {
            setSettingsPanelVisible(on);
        });
    }

    // ---- 按住空格的浮动色板 ----
    // 按住 Space 时，色板浮在预览底部居中：默认选中第一个颜色，
    // ← → 切换，最左边是「不选中」圆圈；松开空格后标记、复制并切到下一张。
    // 对已经标记的图片，「不选中」就是清除标记。
    auto* pickerPopup = new QFrame(m_preview);
    pickerPopup->setObjectName(QStringLiteral("CullColorPicker"));
    pickerPopup->setAttribute(Qt::WA_StyledBackground, true);
    // 同上：色板是预览的子控件，不指定就会被继承成拖拽用的手型指针；
    // 色板本身不拖动画布，恢复箭头，圆点按钮各自设过手型指针。
    pickerPopup->setCursor(Qt::ArrowCursor);
    pickerPopup->setStyleSheet(QStringLiteral(
        "QFrame#CullColorPicker { background: rgba(28, 28, 28, 240);"
        " border: 1px solid #5f5f5f; border-radius: 8px; }"));
    auto* pickerOuter = new QVBoxLayout(pickerPopup);
    pickerOuter->setContentsMargins(12, 10, 12, 10);
    pickerOuter->setSpacing(0);
    m_colorPickerLayout = new QHBoxLayout;
    m_colorPickerLayout->setContentsMargins(0, 0, 0, 0);
    m_colorPickerLayout->setSpacing(10);
    pickerOuter->addLayout(m_colorPickerLayout);
    pickerPopup->hide();
    m_colorPickerPopup = pickerPopup;

    // 标记颜色：圆点数量 = 目标目录数量，一个目录一种颜色。
    // 点圆点给当前图片打标；再点同一个圆点取消标记。
    m_colorGroup = new QButtonGroup(panel);
    m_colorGroup->setExclusive(true);
    m_colorChipsLayout = new QHBoxLayout;
    m_colorChipsLayout->setSpacing(4);

    // 星级：一整排扁平星星（自定义绘制），点第 N 颗 = N 星，
    // 再点当前星级 = 清除（0 星）。没有按钮底框，悬停时整排预览填充。
    m_starRating = new StarRatingWidget(panel);
    m_starRating->setToolTip(
        QStringLiteral("点击第 N 颗星设置星级（快捷键 0~5）；\n"
                       "再点当前星级、或再按一次相同数字即可清除"));
    m_starRating->ratingClicked = [this](int star) { setCurrentStar(star); };

    // 预览下方只保留一行：标记组水平居中到画面正下方，星级靠右，
    // 最左边是「构图与视图」入口。三组放在同一网格单元里分别做左 / 中 / 右对齐，
    // 标记组的中心才是真正的画面中心（用左右弹性空白会被右侧星级挤偏）。
    // 左右留同样的 12px 内边距，星级不会贴死在窗口右边。
    auto* footerRow = new QWidget(panel);
    auto* footerGrid = new QGridLayout(footerRow);
    footerGrid->setContentsMargins(12, 0, 12, 0);
    footerGrid->setSpacing(6);

    auto* markGroup = new QWidget(footerRow);
    auto* markLayout = new QHBoxLayout(markGroup);
    markLayout->setContentsMargins(0, 0, 0, 0);
    markLayout->setSpacing(6);

    // 标记组件两侧的上一张 / 下一张：纯箭头尖，不带尾巴，
    // 点击等同于按 ← / →。放在预览面板里，全屏预览下同样可见；
    // 到第一张 / 最后一张时自动置灰，点击也不会越界。
    m_prevImageButton = makeViewButton(
        markGroup,
        dimmedArrowIcon(QStringLiteral(":/icons/chevron-left.svg")),
        QStringLiteral("上一张（快捷键 ←）"),
        QStringLiteral("上一张"));
    m_nextImageButton = makeViewButton(
        markGroup,
        dimmedArrowIcon(QStringLiteral(":/icons/chevron-right.svg")),
        QStringLiteral("下一张（快捷键 →）"),
        QStringLiteral("下一张"));
    connect(m_prevImageButton, &QPushButton::clicked, this, [this]() {
        selectOffset(-1);
    });
    connect(m_nextImageButton, &QPushButton::clicked, this, [this]() {
        selectOffset(1);
    });

    auto* markCaption = new QLabel(QStringLiteral("标记"), markGroup);
    markCaption->setStyleSheet(QStringLiteral("color: #b8b8b8;"));
    // 箭头负责「翻页」，贴得太靠近颜色圆点会被当成标记组件的一部分：
    // 两侧各再让出 12px（布局本身还有 6px 间距，合计约 18px），
    // 左右对称，标记组的中心仍在画面正中。
    markLayout->addWidget(m_prevImageButton);
    markLayout->addSpacing(12);
    markLayout->addWidget(markCaption);
    markLayout->addLayout(m_colorChipsLayout);
    markLayout->addSpacing(12);
    markLayout->addWidget(m_nextImageButton);

    auto* starGroup = new QWidget(footerRow);
    auto* starLayout = new QHBoxLayout(starGroup);
    starLayout->setContentsMargins(0, 0, 0, 0);
    starLayout->setSpacing(6);
    auto* starCaption = new QLabel(QStringLiteral("星级"), starGroup);
    starCaption->setStyleSheet(QStringLiteral("color: #b8b8b8;"));
    starLayout->addWidget(starCaption);
    starLayout->addWidget(m_starRating);

    footerGrid->addWidget(markGroup, 0, 0, Qt::AlignCenter);
    footerGrid->addWidget(starGroup, 0, 0, Qt::AlignRight | Qt::AlignVCenter);

    // 标记行最左边一组图标按钮：构图与视图入口 + 内嵌 JPEG 预览开关。
    // 放在这里而不是左侧栏顶部栏，全屏预览下也能使用
    // （顶部栏会随左栏一起隐藏，标记行仍然可见）。
    auto* leftButtons = new QWidget(footerRow);
    auto* leftButtonsLayout = new QHBoxLayout(leftButtons);
    leftButtonsLayout->setContentsMargins(0, 0, 0, 0);
    leftButtonsLayout->setSpacing(6);

    // 「构图与视图」入口，和上一张 / 下一张同尺寸。
    m_compositionButton = new QPushButton(leftButtons);
    m_compositionButton->setCheckable(true);
    m_compositionButton->setFixedSize(22, 22);
    m_compositionButton->setIcon(QIcon(QStringLiteral(":/icons/crop.svg")));
    m_compositionButton->setIconSize(QSize(14, 14));
    m_compositionButton->setFocusPolicy(Qt::NoFocus);
    m_compositionButton->setCursor(Qt::PointingHandCursor);
    m_compositionButton->setStyleSheet(toggleIconButtonStyle());
    m_compositionButton->setToolTip(
        QStringLiteral("构图与视图：取景框比例、框线、压暗与辅助线\n"
                       "点击展开调整面板（浮在画面上，Esc 收起）"));
    m_compositionButton->setAccessibleName(QStringLiteral("构图与视图"));
    leftButtonsLayout->addWidget(m_compositionButton);

    // 内嵌 JPEG 预览：显示相机生成的 JPEG，比软件管线快，
    // 色彩是机内处理后的直出效果。当前文件没有内嵌 JPEG 时置灰。
    // 紧挨在「构图与视图」右侧。
    m_embeddedJpegButton = new QPushButton(leftButtons);
    m_embeddedJpegButton->setCheckable(true);
    m_embeddedJpegButton->setFixedSize(22, 22);
    // 复用 dimmedArrowIcon：它会给 SVG 额外补一份淡化图标，
    // 当前图片没有内嵌 JPEG 而置灰时，按钮能明显变暗。
    m_embeddedJpegButton->setIcon(
        dimmedArrowIcon(QStringLiteral(":/icons/embedded.svg")));
    m_embeddedJpegButton->setIconSize(QSize(14, 14));
    m_embeddedJpegButton->setFocusPolicy(Qt::NoFocus);
    m_embeddedJpegButton->setCursor(Qt::PointingHandCursor);
    m_embeddedJpegButton->setStyleSheet(toggleIconButtonStyle());
    m_embeddedJpegButton->setAccessibleName(QStringLiteral("内嵌 JPEG 预览"));
    // 默认开启；在连接 toggled 之前设置，避免构造期触发预览请求。
    // 工作线程的同名默认值也是 true，两者保持一致。
    m_embeddedJpegButton->setChecked(true);
    leftButtonsLayout->addWidget(m_embeddedJpegButton);

    footerGrid->addWidget(leftButtons, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    // 内嵌 JPEG 开关：切换后清掉旧模式的缓存，保留当前缩放位置重新请求。
    connect(m_embeddedJpegButton, &QPushButton::toggled, this, [this](bool on) {
        m_useEmbeddedJpeg = on;
        if (m_previewWorker != nullptr) {
            m_previewWorker->setUseEmbeddedJpeg(on);
        }
        // 两种模式的像素来源不同：预览缓存与 1:1 细节结果都不能再用
        m_previewCache.clear();
        m_detailPath.clear();
        m_detailTicket = 0;
        if (!m_currentPath.isEmpty()) {
            m_preserveViewPath = m_currentPath;
            requestPreviewForCurrent(true);
        }
        showStatus(on ? QStringLiteral("预览已切换为相机内嵌 JPEG")
                      : QStringLiteral("预览已切换为软件管线渲染"));
    });

    // 构图按钮按下即展开，再按收起。
    // 注意连接必须放在按钮创建之后（它在这里才创建，不再由 buildUi 提前建好）。
    connect(m_compositionButton, &QPushButton::toggled, this, [this](bool on) {
        setCompositionPanelVisible(on);
    });

    // 圆点按目标目录动态生成；默认先有一个空目录，加载设置后再重建
    rebuildColorChips();

    layout->addWidget(infoRow);
    layout->addWidget(m_preview, 1);
    // 预览下方只保留「标记 + 星级」一行
    layout->addWidget(footerRow);
    return panel;
}

QWidget* CullWindow::buildThumbnailPanel()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 4, 8, 8);
    layout->setSpacing(4);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("缩略图（Ctrl / Shift 多选后可批量重命名、对比）"), panel);
    // 缩略图来源（相机内嵌 JPEG / 软件管线）属于全局渲染设置，
    // 挪到左侧顶部栏的「设置」浮层，这里只留标题。
    header->addWidget(title);
    header->addStretch(1);
    layout->addLayout(header);

    auto* thumbList = new ThumbnailListWidget(panel);
    m_list = thumbList;
    // 点击分组标题左侧的折叠按钮：隐藏 / 显示该组缩略图
    thumbList->setGroupToggleHandler(
        [this](QListWidgetItem* groupHeader) {
            toggleTimeGroupCollapsed(groupHeader);
        });
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    // 分组标题行和缩略图行尺寸不同，不能固定网格 / 统一尺寸；
    // 普通缩略图仍由每项的 sizeHint（kGridSize）控制，外观与原来一致。
    m_list->setUniformItemSizes(false);
    // 图标区域比卡片本身高一点（上下各留 kThumbCardVMargin 的透明边），
    // 这样选中底色不会紧贴着卡片上沿。
    m_list->setIconSize(kThumbIconSize);
    m_list->setWordWrap(true);
    // 网格单元之间只留 2px：行距主要靠 kGridSize 控制，
    // 再叠加大间距会让缩略图看起来过于松散。
    m_list->setSpacing(2);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 自定义委托负责把时间分组标题画成横贯整行的分隔条
    m_list->setItemDelegate(new ThumbnailListDelegate(m_list));
    m_list->installEventFilter(this);
    // 可视宽度变化（窗口缩放、出现滚动条）后要重新计算标题行宽度，
    // 否则标题行可能不再独占一行，缩略图会挤到它右边。
    m_list->viewport()->installEventFilter(this);

    layout->addWidget(m_list, 1);
    return panel;
}

namespace {

/// 设置面板的滚动区：内容放得下时高度跟着内容走（面板不会凭空变矮），
/// 窗口太矮时（例如缩略图面板拉得很高）才出现滚动条，
/// 否则快捷键表的下半截会被面板下沿直接裁掉。
class FitScrollArea : public QScrollArea {
public:
    explicit FitScrollArea(QWidget* parent = nullptr) : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        // 浮层面板是半透明深色，滚动区本身不能刷自己的底色
        viewport()->setAutoFillBackground(false);
        setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; }"
            "QScrollArea > QWidget > QWidget { background: transparent; }"));
    }

    QSize sizeHint() const override
    {
        QWidget* content = widget();
        return content != nullptr ? content->sizeHint() : QScrollArea::sizeHint();
    }
};

}  // namespace

QWidget* CullWindow::buildSettingsPanel(QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("CullSettingsPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    // 与构图浮层一致：预览控件是手型指针，面板内的控件显式改回箭头
    panel->setCursor(Qt::ArrowCursor);
    panel->setStyleSheet(QStringLiteral(
        "QFrame#CullSettingsPanel { background: rgba(28, 28, 28, 244);"
        " border: 1px solid #5f5f5f; border-radius: 8px; }"));
    // 固定宽度：快捷键表两列对齐更整齐，也不至于在宽屏上被拉得很空
    panel->setFixedWidth(390);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    // ---- 标题行 ----
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("设置"), panel);
    title->setStyleSheet(
        QStringLiteral("color: #e6e6e6; font-weight: 600; background: transparent;"));
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    auto* closeButton = new QPushButton(panel);
    closeButton->setFixedSize(20, 20);
    closeButton->setIcon(QIcon(QStringLiteral(":/icons/remove.svg")));
    closeButton->setIconSize(QSize(12, 12));
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setStyleSheet(iconButtonStyle());
    closeButton->setToolTip(QStringLiteral("收起设置面板（Esc）"));
    closeButton->setAccessibleName(QStringLiteral("收起设置"));
    titleRow->addWidget(closeButton);
    layout->addLayout(titleRow);

    // 标题以下的全部内容放进滚动区：窗口够高时和普通面板没区别，
    // 窗口压到最小时也能滚动看到完整的快捷键表。
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("CullSettingsBody"));
    body->setStyleSheet(
        QStringLiteral("QWidget#CullSettingsBody { background: transparent; }"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    // 设置项之间保持和构图面板一致的纵向留白
    bodyLayout->setSpacing(12);

    // ---- 缩略图与预览 ----
    auto* displayTitle = new QLabel(QStringLiteral("缩略图与预览"), body);
    displayTitle->setStyleSheet(
        QStringLiteral("color: #8f8f8f; background: transparent;"));
    bodyLayout->addWidget(displayTitle);

    m_pipelineThumbCheck = new QCheckBox(
        QStringLiteral("缩略图展示为RAW"), body);
    m_pipelineThumbCheck->setToolTip(
        QStringLiteral("默认读取相机内嵌 JPEG，整目录挑片速度最快。\n"
                       "勾选后缩略图直接解码 RAW 并走软件管线渲染，"
                       "色彩与主预览一致，但每张都要解码，明显变慢。"));
    m_pipelineThumbCheck->setFocusPolicy(Qt::NoFocus);
    bodyLayout->addWidget(m_pipelineThumbCheck);

    // 镜头畸变校正是全局渲染选项：相机内嵌 JPEG 已按机内设置做过失真控制，
    // RAW 关掉校正会看到更宽的原始几何（直线在边缘外鼓），因此默认开启。
    m_lensCorrectionCheck =
        new QCheckBox(QStringLiteral("镜头畸变校正（按 EXIF 自动匹配）"), body);
    m_lensCorrectionCheck->setChecked(true);
    m_lensCorrectionCheck->setToolTip(
        QStringLiteral("按 EXIF 自动匹配镜头库，校正桶形 / 枕形畸变。\n"
                       "相机内嵌 JPEG 默认已做失真控制，关闭后预览会与缩略图\n"
                       "几何不一致，但能看到 RAW 的完整视野。"));
    m_lensCorrectionCheck->setFocusPolicy(Qt::NoFocus);
    bodyLayout->addWidget(m_lensCorrectionCheck);

    auto* divider = new QWidget(body);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: #3a3a3a;"));
    bodyLayout->addWidget(divider);

    // ---- 快捷键 ----
    auto* shortcutTitleRow = new QHBoxLayout;
    shortcutTitleRow->setSpacing(8);
    auto* shortcutTitle = new QLabel(QStringLiteral("快捷键"), body);
    shortcutTitle->setStyleSheet(
        QStringLiteral("color: #8f8f8f; background: transparent;"));
    shortcutTitleRow->addWidget(shortcutTitle);
    shortcutTitleRow->addStretch(1);
    auto* shortcutHint =
        new QLabel(QStringLiteral("点击按键后按下新键，Esc 取消"), body);
    shortcutHint->setStyleSheet(
        QStringLiteral("color: #7a7a7a; background: transparent;"));
    shortcutTitleRow->addWidget(shortcutHint);
    bodyLayout->addLayout(shortcutTitleRow);

    auto* shortcutGrid = new QGridLayout;
    shortcutGrid->setContentsMargins(0, 0, 0, 0);
    shortcutGrid->setHorizontalSpacing(6);
    // 快捷键一行一个动作，行距比原来放松一档，点击时不容易点错行
    shortcutGrid->setVerticalSpacing(8);
    int gridRow = 0;
    for (const ShortcutAction& action : shortcutActions()) {
        const QString id = QString::fromLatin1(action.id);
        const QString labelText = QString::fromUtf8(action.label);

        auto* label = new QLabel(labelText, body);
        label->setStyleSheet(QStringLiteral("color: #c8c8c8; background: transparent;"));

        auto* keyButton = new ShortcutKeyButton(body);
        keyButton->setSequence(shortcutSequence(id));
        // 点击进入录入状态；录入成功后由 sequenceCaptured 落盘
        connect(keyButton, &QPushButton::clicked, keyButton,
                [keyButton]() { keyButton->beginCapture(); });
        keyButton->sequenceCaptured = [this, id](const QKeySequence& sequence) {
            assignShortcut(id, sequence);
        };

        auto* clearButton = new QPushButton(body);
        clearButton->setFixedSize(20, 20);
        clearButton->setIcon(QIcon(QStringLiteral(":/icons/remove.svg")));
        clearButton->setIconSize(QSize(11, 11));
        clearButton->setFocusPolicy(Qt::NoFocus);
        clearButton->setCursor(Qt::PointingHandCursor);
        clearButton->setStyleSheet(iconButtonStyle());
        clearButton->setToolTip(QStringLiteral("清除「%1」的快捷键").arg(labelText));
        clearButton->setAccessibleName(QStringLiteral("清除快捷键"));
        connect(clearButton, &QPushButton::clicked, this,
                [this, id, labelText]() {
                    assignShortcut(id, QKeySequence());
                    showStatus(QStringLiteral("已清除「%1」的快捷键").arg(labelText));
                });

        shortcutGrid->addWidget(label, gridRow, 0);
        shortcutGrid->addWidget(keyButton, gridRow, 1);
        shortcutGrid->addWidget(clearButton, gridRow, 2);
        m_shortcutButtons.insert(id, keyButton);
        ++gridRow;
    }
    // 名称列吃掉多余宽度，按键与清除按钮始终靠右对齐
    shortcutGrid->setColumnStretch(0, 1);
    bodyLayout->addLayout(shortcutGrid);

    // ---- 底部：固定键说明 + 全部重置 ----
    auto* footerRow = new QHBoxLayout;
    footerRow->setSpacing(8);
    auto* fixedHint = new QLabel(
        QStringLiteral("星级 0~5、构图比例 Ctrl+1~5、Esc 为固定键"), body);
    fixedHint->setStyleSheet(
        QStringLiteral("color: #7a7a7a; background: transparent;"));
    fixedHint->setWordWrap(true);
    footerRow->addWidget(fixedHint, 1);
    auto* resetButton = new QPushButton(QStringLiteral("全部恢复默认"), body);
    resetButton->setFocusPolicy(Qt::NoFocus);
    resetButton->setCursor(Qt::PointingHandCursor);
    resetButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #333333; border: 1px solid #474747;"
        " border-radius: 3px; color: #dcdcdc; padding: 3px 10px; }"
        "QPushButton:hover { background: #3d3d3d; border-color: #5a5a5a; }"
        "QPushButton:pressed { background: #2a2a2a; }"));
    resetButton->setToolTip(QStringLiteral("把所有快捷键恢复成内置默认值"));
    connect(resetButton, &QPushButton::clicked, this,
            &CullWindow::resetAllShortcuts);
    footerRow->addWidget(resetButton, 0, Qt::AlignVCenter);
    bodyLayout->addLayout(footerRow);

    auto* scroll = new FitScrollArea(panel);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    connect(closeButton, &QPushButton::clicked, this,
            [this]() { setSettingsPanelVisible(false); });
    return panel;
}

void CullWindow::connectSignals()
{
    // ---- 目录 ----
    connect(m_sourceEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_sourceEdit->text().trimmed() != m_sourceEdit->property("loadedDir").toString()) {
            scanDirectory();
            saveSettings();
        }
    });
    connect(m_recursiveCheck, &QCheckBox::toggled, this, [this](bool) {
        if (!m_sourceEdit->text().trimmed().isEmpty()) {
            scanDirectory();
        }
        saveSettings();
    });

    // ---- 筛选 / 排序 ----
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        rebuildList();
        saveSettings();
    });
    connect(m_filterCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        rebuildList();
        saveSettings();
    });
    connect(m_sortCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        rebuildList();
        saveSettings();
    });
    connect(m_descendingCheck, &QCheckBox::toggled, this, [this](bool) {
        rebuildList();
        saveSettings();
    });
    connect(m_timeGroupCheck, &QCheckBox::toggled, this, [this](bool) {
        // 分组必须按拍摄时间连续排列：分组期间禁用排序下拉框，
        // 取消分组后恢复用户原来的排序方式。
        updateTimeGroupControls();
        rebuildList();
        saveSettings();
    });
    connect(m_timeGroupGapCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_timeGroupCheck->isChecked()) {
            rebuildList();
        }
        saveSettings();
    });
    connect(m_pipelineThumbCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        emit usePipelineThumbnailsChanged(enabled);
        // 切换策略后重新生成，否则已有缩略图仍沿用旧结果。
        // 信号是队列连接，标志位一定先于这些请求到达工作线程。
        for (const Item& item : m_items) {
            emit thumbnailRequested(item.path, kThumbnailSize);
        }
        if (enabled && !m_items.isEmpty()) {
            showStatus(QStringLiteral("正在用软件管线重新生成缩略图…"));
        }
        saveSettings();
    });

    // ---- 列表 ----
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) {
                if (!m_rebuilding) {
                    syncCurrentItem();
                }
            });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        updateActionState();
    });

    // ---- 构图 ----
    connect(m_compositionCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        setCompositionEnabled(enabled);
    });
    connect(m_aspectCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        applyAspectFromUi();
        saveSettings();
    });
    connect(m_customAspectWidth, &QDoubleSpinBox::valueChanged, this, [this](double) {
        applyAspectFromUi();
        saveSettings();
    });
    connect(m_customAspectHeight, &QDoubleSpinBox::valueChanged, this, [this](double) {
        applyAspectFromUi();
        saveSettings();
    });
    connect(m_frameColorButton, &QToolButton::clicked, this,
            &CullWindow::chooseFrameColor);
    connect(m_dimSlider, &QSlider::valueChanged, this, [this](int value) {
        m_dimValueLabel->setText(QStringLiteral("%1%").arg(value));
        applyCompositionToPreview();
        saveSettings();
    });
    connect(m_maskCheck, &QCheckBox::toggled, this, [this](bool) {
        applyCompositionToPreview();
        saveSettings();
    });
    connect(m_ratioLabelCheck, &QCheckBox::toggled, this, [this](bool) {
        applyCompositionToPreview();
        saveSettings();
    });
    connect(m_thirdsCheck, &QCheckBox::toggled, this, [this](bool) {
        applyCompositionToPreview();
        saveSettings();
    });
    connect(m_lensCorrectionCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        // 校正改变画面几何：缓存与 1:1 细节全部作废，重新渲染当前图。
        // 先发开关、再发请求，同一线程上的队列顺序保证渲染用的是新值。
        emit lensCorrectionChanged(enabled);
        m_previewCache.clear();
        m_detailPath.clear();
        m_detailTicket = 0;
        if (m_pipelineThumbCheck->isChecked()) {
            // 管线缩略图也受校正影响，重新生成，避免网格与主预览几何不一致
            for (const Item& item : m_items) {
                emit thumbnailRequested(item.path, kThumbnailSize);
            }
        }
        if (!m_currentPath.isEmpty()) {
            // 不换图、只换渲染选项：新像素到位后保留当前的缩放与平移
            m_preserveViewPath = m_currentPath;
            requestPreviewForCurrent(true);
        }
        saveSettings();
    });

    // ---- 挑片动作 ----
    connect(m_copyButton, &QPushButton::clicked, this, [this]() { copyCurrent(false); });
    connect(m_copyNextButton, &QPushButton::clicked, this, [this]() { copyCurrent(true); });
    connect(m_copyMarkedButton, &QPushButton::clicked, this,
            &CullWindow::copyMarked);
}

// ===========================================================================
//  设置持久化
// ===========================================================================

void CullWindow::loadSettings()
{
    QSettings settings;
    m_sourceEdit->setText(settings.value(QStringLiteral("cull/sourceDir")).toString());
    m_recursiveCheck->setChecked(settings.value(QStringLiteral("cull/recursive"), false).toBool());

    // ---- 目标目录（路径 + 标记颜色）----
    // 旧版本只保存单个 cull/targetDir，首次升级时迁移成第一个目标目录。
    const QStringList targetPaths = settings.value(QStringLiteral("cull/targetDirs")).toStringList();
    const QStringList targetColors = settings.value(QStringLiteral("cull/targetColors")).toStringList();
    // 自定义颜色与目录一一对应（空串 = 使用预设色），旧配置没有这一项
    const QStringList targetCustomColors =
        settings.value(QStringLiteral("cull/targetCustomColors")).toStringList();
    m_targetDirs.clear();
    for (int i = 0; i < targetPaths.size() && m_targetDirs.size() < kMaxTargetDirs; ++i) {
        // 空路径不算一个目录：界面上不保留空行，只显示「＋ 添加目标目录」按钮
        if (targetPaths.at(i).trimmed().isEmpty()) {
            continue;
        }
        TargetDir target;
        target.path = QDir::toNativeSeparators(targetPaths.at(i).trimmed());
        const QString color = i < targetColors.size() ? targetColors.at(i) : QString();
        // 颜色必须唯一：配置文件被手改出重复颜色时，重新分配一个空闲颜色
        const bool usable = findTargetColor(color) != nullptr
            && targetIndexForColor(color) < 0;
        target.color = usable ? color : nextFreeTargetColor();
        if (target.color.isEmpty()) {
            break;  // 颜色用尽，多出来的目录不再载入
        }
        // 自定义颜色按目录下标读取；无效值（手改配置）直接忽略，回落到预设色
        if (i < targetCustomColors.size()) {
            const QColor custom(targetCustomColors.at(i));
            if (custom.isValid()) {
                target.customHex = custom.name(QColor::HexRgb);
            }
        }
        m_targetDirs.append(target);
    }
    // 一个目录都没有时不建空行：界面里只剩「＋ 添加目标目录」按钮。
    // 仅当用户是从更老的版本升级（还没有 cull/targetDirs 这个键）且旧键里
    // 确实保存过路径时，才迁移成一个目标目录，避免把清空的列表又"复活"。
    if (m_targetDirs.isEmpty()
        && !settings.contains(QStringLiteral("cull/targetDirs"))) {
        const QString legacyPath =
            settings.value(QStringLiteral("cull/targetDir")).toString().trimmed();
        if (!legacyPath.isEmpty()) {
            TargetDir target;
            target.path = QDir::toNativeSeparators(legacyPath);
            target.color = QLatin1String(kTargetColors[0].name);
            m_targetDirs.append(target);
        }
    }

    rebuildTargetDirRows();
    rebuildColorChips();
    // 筛选选项里有「颜色圆点 + 名称」，必须在目标目录加载完之后重建
    rebuildFilterCombo();

    const int formatIndex = settings.value(QStringLiteral("cull/format"), 0).toInt();
    m_formatCombo->setCurrentIndex(std::clamp(formatIndex, 0, m_formatCombo->count() - 1));
    // 筛选保存的是稳定键；旧版本存的是下标，按旧列表迁移一次
    QString filterKey = settings.value(QStringLiteral("cull/filterKey")).toString();
    if (filterKey.isEmpty()) {
        static const char* const kLegacyFilterKeys[] = {
            "all", "star1", "star2", "star3", "star4",
            "star5", "marked", "unmarked"};
        const int legacyIndex = settings.value(QStringLiteral("cull/filter"), 0).toInt();
        filterKey = (legacyIndex >= 0 && legacyIndex < 8)
                        ? QString::fromLatin1(kLegacyFilterKeys[legacyIndex])
                        : QStringLiteral("all");
    }
    setFilterKey(filterKey);
    const int sortIndex = settings.value(QStringLiteral("cull/sort"), 0).toInt();
    m_sortCombo->setCurrentIndex(std::clamp(sortIndex, 0, m_sortCombo->count() - 1));
    m_descendingCheck->setChecked(settings.value(QStringLiteral("cull/descending"), false).toBool());
    m_timeGroupCheck->setChecked(settings.value(QStringLiteral("cull/timeGroup"), false).toBool());
    const int groupGap = settings.value(QStringLiteral("cull/timeGroupGap"), 30).toInt();
    const int groupGapIndex = m_timeGroupGapCombo->findData(groupGap);
    m_timeGroupGapCombo->setCurrentIndex(groupGapIndex >= 0 ? groupGapIndex : 3);
    updateTimeGroupControls();
    m_pipelineThumbCheck->setChecked(
        settings.value(QStringLiteral("cull/pipelineThumbs"), false).toBool());

    // ---- 构图 ----
    // 取景框默认值在 v2 调整为：1px 黑色框线 + 75% 压暗。
    // v3 把压暗范围从 40~60% 放开到 40~100%；旧版本保存的 60% 会被
    // 一次性迁移到新的默认值 75%，否则上限放开后仍然显示 60%，
    // 用户会以为默认值没有变化。
    constexpr int kCompositionDefaultsVersion = 3;
    const int compositionDefaultsVersion =
        settings.value(QStringLiteral("cull/compositionDefaultsVersion"), 1).toInt();
    if (compositionDefaultsVersion < 2) {
        settings.setValue(QStringLiteral("cull/frameColor"), QStringLiteral("#000000"));
    }
    if (compositionDefaultsVersion < 3) {
        settings.setValue(QStringLiteral("cull/dimOpacity"), kDimDefault);
        settings.remove(QStringLiteral("cull/thickness"));
        settings.remove(QStringLiteral("cull/spiral"));
        settings.remove(QStringLiteral("cull/cross"));
    }
    if (compositionDefaultsVersion < kCompositionDefaultsVersion) {
        settings.setValue(QStringLiteral("cull/compositionDefaultsVersion"),
                          kCompositionDefaultsVersion);
    }

    m_compositionCheck->setChecked(
        settings.value(QStringLiteral("cull/composition"), false).toBool());
    const int aspectIndex = settings.value(QStringLiteral("cull/aspect"), 0).toInt();
    m_aspectCombo->setCurrentIndex(std::clamp(aspectIndex, 0, kCustomAspectIndex));
    // 旧版本只保存一个宽高比（cull/customAspect），升级后作为宽度读入，
    // 高度保持 1.0，换算出的比例与旧版一致。
    m_customAspectWidth->setValue(std::clamp(
        settings.value(QStringLiteral("cull/customAspectW"),
                       settings.value(QStringLiteral("cull/customAspect"), 1.85))
            .toDouble(),
        0.1, 99.0));
    m_customAspectHeight->setValue(std::clamp(
        settings.value(QStringLiteral("cull/customAspectH"), 1.0).toDouble(), 0.1, 99.0));

    const QColor frameColor(
        settings.value(QStringLiteral("cull/frameColor"),
                       m_preview->frameColor().name()).toString());
    if (frameColor.isValid()) {
        m_preview->setFrameColor(frameColor);
    }
    applySwatchIcon(m_frameColorButton, m_preview->frameColor());

    m_dimSlider->setValue(std::clamp(
        settings.value(QStringLiteral("cull/dimOpacity"), kDimDefault).toInt(),
        kDimMin, kDimMax));
    // 设置没有变化时 valueChanged 不会触发，这里主动同步一次数值展示
    m_dimValueLabel->setText(QStringLiteral("%1%").arg(m_dimSlider->value()));
    m_maskCheck->setChecked(settings.value(QStringLiteral("cull/maskVisible"), true).toBool());
    m_ratioLabelCheck->setChecked(
        settings.value(QStringLiteral("cull/ratioLabel"), true).toBool());
    m_thirdsCheck->setChecked(settings.value(QStringLiteral("cull/thirds"), false).toBool());
    // 镜头畸变校正默认开启：与相机内嵌 JPEG 的几何保持一致
    m_lensCorrectionCheck->setChecked(
        settings.value(QStringLiteral("cull/lensCorrection"), true).toBool());

    // 快捷键放在最后：设置面板里的按键按钮在 buildUi 中已经建好，
    // 这里读回用户改过的键位并刷新按钮文字。
    loadShortcuts();
}

void CullWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("cull/sourceDir"), m_sourceEdit->text());
    QStringList targetPaths;
    QStringList targetColors;
    QStringList targetCustomColors;
    for (const TargetDir& target : m_targetDirs) {
        targetPaths << target.path;
        targetColors << target.color;
        targetCustomColors << target.customHex;
    }
    settings.setValue(QStringLiteral("cull/targetDirs"), targetPaths);
    settings.setValue(QStringLiteral("cull/targetColors"), targetColors);
    settings.setValue(QStringLiteral("cull/targetCustomColors"), targetCustomColors);
    // 旧的单目录键迁移完就没用了：留着会让「删光目标目录」在下次启动时被旧路径复活
    settings.remove(QStringLiteral("cull/targetDir"));
    // 旧版本的“颜色继承”配置已废弃（目录清除时标记会一并复位），顺手清掉残留
    settings.remove(QStringLiteral("cull/targetDirSeqs"));
    settings.remove(QStringLiteral("cull/orphanColorNames"));
    settings.remove(QStringLiteral("cull/orphanColorCustoms"));
    settings.remove(QStringLiteral("cull/orphanColorSeqs"));
    settings.setValue(QStringLiteral("cull/recursive"), m_recursiveCheck->isChecked());
    settings.setValue(QStringLiteral("cull/format"), m_formatCombo->currentIndex());
    settings.setValue(QStringLiteral("cull/filterKey"), currentFilterKey());
    settings.setValue(QStringLiteral("cull/sort"), m_sortCombo->currentIndex());
    settings.setValue(QStringLiteral("cull/descending"), m_descendingCheck->isChecked());
    settings.setValue(QStringLiteral("cull/timeGroup"), m_timeGroupCheck->isChecked());
    settings.setValue(QStringLiteral("cull/timeGroupGap"),
                      m_timeGroupGapCombo->currentData().toInt());
    settings.setValue(QStringLiteral("cull/pipelineThumbs"), m_pipelineThumbCheck->isChecked());

    settings.setValue(QStringLiteral("cull/composition"), m_compositionCheck->isChecked());
    settings.setValue(QStringLiteral("cull/aspect"), m_aspectCombo->currentIndex());
    settings.setValue(QStringLiteral("cull/customAspectW"), m_customAspectWidth->value());
    settings.setValue(QStringLiteral("cull/customAspectH"), m_customAspectHeight->value());
    settings.setValue(QStringLiteral("cull/frameColor"), m_preview->frameColor().name());
    settings.setValue(QStringLiteral("cull/dimOpacity"), m_dimSlider->value());
    settings.setValue(QStringLiteral("cull/maskVisible"), m_maskCheck->isChecked());
    settings.setValue(QStringLiteral("cull/ratioLabel"), m_ratioLabelCheck->isChecked());
    settings.setValue(QStringLiteral("cull/thirds"), m_thirdsCheck->isChecked());
    settings.setValue(QStringLiteral("cull/lensCorrection"),
                      m_lensCorrectionCheck->isChecked());
    saveShortcuts();
}

// ===========================================================================
//  构图辅助
// ===========================================================================

void CullWindow::applyCompositionToPreview()
{
    m_preview->setDimOpacity(m_dimSlider->value());
    m_preview->setMaskVisible(m_maskCheck->isChecked());
    m_preview->setRatioLabelVisible(m_ratioLabelCheck->isChecked());
    m_preview->setGuides(m_thirdsCheck->isChecked());
    applyAspectFromUi();
    // 开关放在最后：开启时会重置视图
    m_preview->setCompositionEnabled(m_compositionCheck->isChecked());
}

void CullWindow::applyAspectFromUi()
{
    const int index = m_aspectCombo->currentIndex();
    const bool custom = (index == kCustomAspectIndex);
    double aspect = 3.0 / 2.0;
    if (custom) {
        const double height = m_customAspectHeight->value();
        // 输入框下限是 0.1，这里仍做一次保护，避免除零
        aspect = height > 0.0 ? m_customAspectWidth->value() / height
                              : 3.0 / 2.0;
    } else if (index >= 0 && index < kCustomAspectIndex) {
        aspect = kAspectPresets[index].aspect;
    }
    m_preview->setFrameAspect(aspect);
    updateCompositionControls();
}

void CullWindow::updateCompositionControls()
{
    // 固定视野没开启时，比例 / 框线 / 压暗 / 辅助线都没有意义，
    // 统一置灰，避免用户在没有取景框的情况下调参数却看不到变化。
    const bool on = m_compositionCheck->isChecked();
    m_aspectCombo->setEnabled(on);
    m_frameColorButton->setEnabled(on);
    m_dimSlider->setEnabled(on);
    m_dimValueLabel->setEnabled(on);
    m_maskCheck->setEnabled(on);
    m_ratioLabelCheck->setEnabled(on);
    m_thirdsCheck->setEnabled(on);
    // 自定义比例的宽 / 高输入框只在比例下拉选中「自定义」时出现，
    // 出现后仍随固定视野开关一起置灰。
    const bool custom = m_aspectCombo->currentIndex() == kCustomAspectIndex;
    m_customAspectBox->setVisible(custom);
    m_customAspectBox->setEnabled(on);
}

void CullWindow::setCompositionEnabled(bool enabled)
{
    if (m_compositionCheck->isChecked() != enabled) {
        QSignalBlocker blocker(m_compositionCheck);
        m_compositionCheck->setChecked(enabled);
    }
    m_preview->setCompositionEnabled(enabled);
    updateCompositionControls();
    saveSettings();
}

void CullWindow::chooseFrameColor()
{
    const QColor color = QColorDialog::getColor(
        m_preview->frameColor(), this, QStringLiteral("选择构图框颜色"));
    if (!color.isValid()) {
        return;
    }
    m_preview->setFrameColor(color);
    applySwatchIcon(m_frameColorButton, color);
    saveSettings();
}

void CullWindow::setFullScreenMode(bool enabled)
{
    if (m_fullScreen == enabled) {
        return;
    }
    m_fullScreen = enabled;
    // 全屏预览去掉侧边面板，把画布让出来（需求 §三 MVP 4）
    m_leftPanel->setVisible(!enabled);
    m_thumbnailPanel->setVisible(!enabled);
    // 预览面板在全屏下仍然可见，按钮图标与提示跟着状态切换
    if (m_fullScreenButton != nullptr) {
        m_fullScreenButton->setIcon(
            QIcon(enabled ? QStringLiteral(":/icons/fullscreen-exit.svg")
                          : QStringLiteral(":/icons/fullscreen.svg")));
        m_fullScreenButton->setAccessibleName(
            enabled ? QStringLiteral("退出全屏") : QStringLiteral("进入全屏"));
        // 提示里的键位来自设置面板，可能被改过，统一由 refreshShortcutHints 拼
        refreshShortcutHints();
    }
    if (enabled) {
        // 顶部栏的「设置」按钮随左栏一起隐藏，浮层没有入口了，一并收起
        setSettingsPanelVisible(false);
        showFullScreen();
        m_preview->setFocus();
        showStatus(QStringLiteral(
            "全屏预览：R 开关固定视野 · ← → 前后切换（上下行已禁用）· 滚轮缩放 · F / Esc 退出"));
    } else {
        showNormal();
        showStatus(QStringLiteral("已退出全屏"));
    }
}

void CullWindow::setCompositionPanelVisible(bool visible)
{
    if (m_compositionPanel == nullptr) {
        return;
    }
    // 标记行左侧的图标按钮是面板状态的唯一入口：从叉号 / Esc 收起时把按钮同步回未选中，
    // 下次点它才能重新展开（信号屏蔽，避免和按钮自己的 toggled 来回触发）。
    if (m_compositionButton != nullptr
        && m_compositionButton->isChecked() != visible) {
        QSignalBlocker blocker(m_compositionButton);
        m_compositionButton->setChecked(visible);
    }
    if (!visible) {
        m_compositionPanel->hide();
        return;
    }
    // 两个浮层都贴左上角，同时打开会互相遮挡：先收起设置面板
    setSettingsPanelVisible(false);
    positionCompositionPanel();
    m_compositionPanel->show();
    m_compositionPanel->raise();
}

void CullWindow::positionCompositionPanel()
{
    if (m_compositionPanel == nullptr || m_preview == nullptr) {
        return;
    }
    const int margin = 12;
    m_compositionPanel->adjustSize();
    QSize size = m_compositionPanel->sizeHint();
    // 预览区比面板还窄时按可用宽度收窄，避免右侧被裁掉
    size.setWidth(qMin(size.width(), qMax(0, m_preview->width() - margin * 2)));
    m_compositionPanel->resize(size);
    m_compositionPanel->move(margin, margin);
}

void CullWindow::setSettingsPanelVisible(bool visible)
{
    if (m_settingsPanel == nullptr) {
        return;
    }
    // 与构图浮层相同：按钮是面板状态的唯一入口，其它关闭方式都要把按钮同步回来
    if (m_settingsButton != nullptr && m_settingsButton->isChecked() != visible) {
        QSignalBlocker blocker(m_settingsButton);
        m_settingsButton->setChecked(visible);
    }
    if (!visible) {
        m_settingsPanel->hide();
        return;
    }
    // 互斥：打开设置时收起构图浮层，画面上永远只有一个浮层
    setCompositionPanelVisible(false);
    positionSettingsPanel();
    m_settingsPanel->show();
    m_settingsPanel->raise();
}

void CullWindow::positionSettingsPanel()
{
    if (m_settingsPanel == nullptr || m_preview == nullptr) {
        return;
    }
    const int margin = 12;
    m_settingsPanel->adjustSize();
    QSize size = m_settingsPanel->sizeHint();
    // 宽度已经固定；高度在很矮的窗口里按可用高度收窄，避免下沿被裁掉
    size.setWidth(qMin(size.width(), qMax(0, m_preview->width() - margin * 2)));
    size.setHeight(qMin(size.height(), qMax(0, m_preview->height() - margin * 2)));
    m_settingsPanel->resize(size);
    m_settingsPanel->move(margin, margin);
}

void CullWindow::toggleIccSimulator()
{
    const bool shouldShow = m_iccSimButton != nullptr && m_iccSimButton->isChecked();

    if (m_iccSimulator == nullptr) {
        m_iccSimulator = new IccSimulatorWindow(this);
        connect(m_iccSimulator, &IccSimulatorWindow::closed, this, [this]() {
            if (m_iccSimButton != nullptr) {
                QSignalBlocker blocker(m_iccSimButton);
                m_iccSimButton->setChecked(false);
            }
        });
    }

    if (shouldShow) {
        m_iccSimulator->show();
        m_iccSimulator->raise();
        m_iccSimulator->activateWindow();
    } else {
        m_iccSimulator->hide();
    }
}

void CullWindow::openHelp()
{
    // 首次点击时创建；之后重复点击只是把窗口重新提到最前，
    // 不会叠出多个帮助窗口（关掉只是隐藏，内容不需要重新构建）。
    if (m_helpDialog == nullptr) {
        m_helpDialog = new HelpDialog(this);
    }
    m_helpDialog->show();
    m_helpDialog->raise();
    m_helpDialog->activateWindow();
}

// ===========================================================================
//  快捷键配置
// ===========================================================================

void CullWindow::initDefaultShortcuts()
{
    m_shortcuts.clear();
    for (const ShortcutAction& action : shortcutActions()) {
        m_shortcuts.insert(QString::fromLatin1(action.id),
                           QKeySequence(QKeyCombination(action.defaultKey)));
    }
}

void CullWindow::loadShortcuts()
{
    initDefaultShortcuts();
    QSettings settings;
    for (const ShortcutAction& action : shortcutActions()) {
        const QString id = QString::fromLatin1(action.id);
        const QString key = QStringLiteral("cull/shortcuts/") + id;
        // contains 用来区分「没保存过」和「保存为空」：
        // 前者沿用默认键，后者是用户主动清除了这个动作的快捷键。
        if (!settings.contains(key)) {
            continue;
        }
        const QString text = settings.value(key).toString();
        m_shortcuts.insert(
            id, text.isEmpty() ? QKeySequence()
                               : QKeySequence(text, QKeySequence::PortableText));
    }
    refreshShortcutButtons();
    applyCopyShortcut();
}

void CullWindow::saveShortcuts()
{
    QSettings settings;
    for (const ShortcutAction& action : shortcutActions()) {
        const QString id = QString::fromLatin1(action.id);
        settings.setValue(QStringLiteral("cull/shortcuts/") + id,
                          shortcutSequence(id).toString(QKeySequence::PortableText));
    }
}

void CullWindow::refreshShortcutButtons()
{
    for (auto it = m_shortcutButtons.cbegin(); it != m_shortcutButtons.cend(); ++it) {
        it.value()->setSequence(shortcutSequence(it.key()));
    }
    refreshShortcutHints();
}

void CullWindow::refreshShortcutHints()
{
    // 提示里写死键位，改键后就过期了：这里按当前配置重新拼一遍。
    // 动作被清除后不再出现「快捷键 …」的尾巴。
    auto withKey = [this](const QString& id, const QString& text) {
        const QKeySequence sequence = shortcutSequence(id);
        return sequence.isEmpty()
                   ? text
                   : QStringLiteral("%1（快捷键 %2）")
                         .arg(text, shortcutDisplayText(sequence));
    };
    if (m_compositionCheck != nullptr) {
        m_compositionCheck->setToolTip(withKey(QStringLiteral("toggleComposition"),
                                               QStringLiteral("开关固定视野构图")));
    }
    if (m_maskCheck != nullptr) {
        m_maskCheck->setToolTip(withKey(QStringLiteral("toggleMask"),
                                        QStringLiteral("一键开关框外暗化遮罩")));
    }
    if (m_prevImageButton != nullptr) {
        m_prevImageButton->setToolTip(
            withKey(QStringLiteral("prevImage"), QStringLiteral("上一张")));
    }
    if (m_nextImageButton != nullptr) {
        m_nextImageButton->setToolTip(
            withKey(QStringLiteral("nextImage"), QStringLiteral("下一张")));
    }
    if (m_fullScreenButton != nullptr) {
        const QKeySequence sequence =
            shortcutSequence(QStringLiteral("toggleFullScreen"));
        QString text = m_fullScreen ? QStringLiteral("退出全屏")
                                    : QStringLiteral("进入全屏预览");
        if (!sequence.isEmpty()) {
            text += QStringLiteral("（快捷键 %1%2）")
                        .arg(shortcutDisplayText(sequence),
                             m_fullScreen ? QStringLiteral(" 或 Esc") : QString());
        } else if (m_fullScreen) {
            text += QStringLiteral("（Esc）");
        }
        m_fullScreenButton->setToolTip(text);
    }
}

void CullWindow::applyCopyShortcut()
{
    if (m_copyShortcut == nullptr) {
        return;  // 构造时 loadSettings 先跑，QShortcut 还没建
    }
    const QKeySequence sequence = shortcutSequence(QStringLiteral("copyNext"));
    if (sequence.isEmpty()) {
        // 已清除：先禁用，避免 QShortcut 在空键位上报 "No key assigned"
        m_copyShortcut->setEnabled(false);
        return;
    }
    m_copyShortcut->setKey(sequence);
    m_copyShortcut->setEnabled(true);
}

QKeySequence CullWindow::shortcutSequence(const QString& id) const
{
    return m_shortcuts.value(id);
}

bool CullWindow::matchesShortcut(const QString& id, const QKeyEvent* event) const
{
    if (event == nullptr) {
        return false;
    }
    const QKeySequence sequence = shortcutSequence(id);
    return !sequence.isEmpty() && QKeySequence(event->keyCombination()) == sequence;
}

void CullWindow::assignShortcut(const QString& id, const QKeySequence& sequence)
{
    if (!sequence.isEmpty()) {
        // 一个按键只绑定一个动作：同键已被别的动作占用时把旧绑定清掉，
        // 否则按一次会同时触发两件事。
        for (auto it = m_shortcuts.begin(); it != m_shortcuts.end(); ++it) {
            if (it.key() == id || it.value() != sequence) {
                continue;
            }
            it.value() = QKeySequence();
            if (ShortcutKeyButton* other = m_shortcutButtons.value(it.key())) {
                other->setSequence(QKeySequence());
            }
            showStatus(QStringLiteral("快捷键 %1 已从「%2」移给「%3」")
                           .arg(shortcutDisplayText(sequence),
                                shortcutActionLabel(it.key()),
                                shortcutActionLabel(id)));
        }
    }
    m_shortcuts.insert(id, sequence);
    if (ShortcutKeyButton* button = m_shortcutButtons.value(id)) {
        button->setSequence(sequence);
    }
    refreshShortcutHints();
    applyCopyShortcut();
    saveShortcuts();
}

void CullWindow::resetAllShortcuts()
{
    initDefaultShortcuts();
    refreshShortcutButtons();
    applyCopyShortcut();
    saveShortcuts();
    showStatus(QStringLiteral("快捷键已恢复默认"));
}

void CullWindow::triggerShortcutAction(const QString& id)
{
    if (id == QLatin1String("prevImage")) {
        selectOffset(-1);
    } else if (id == QLatin1String("nextImage")) {
        selectOffset(1);
    } else if (id == QLatin1String("prevRow")) {
        // 全屏预览下没有缩略图网格，上下行没有意义：只保留前后切图
        if (!m_fullScreen) {
            selectVertical(-1);
        }
    } else if (id == QLatin1String("nextRow")) {
        if (!m_fullScreen) {
            selectVertical(1);
        }
    } else if (id == QLatin1String("copyNext")) {
        // 与空格完全同一条路径：未标记弹色板，已标记短按复制、长按改标记
        handleSpacePressed();
    } else if (id == QLatin1String("clearColor")) {
        setCurrentColorLabel(QString());
    } else if (id == QLatin1String("toggleComposition")) {
        setCompositionEnabled(!m_compositionCheck->isChecked());
    } else if (id == QLatin1String("toggleMask")) {
        m_maskCheck->setChecked(!m_maskCheck->isChecked());
        showStatus(m_maskCheck->isChecked() ? QStringLiteral("框外压暗：开")
                                            : QStringLiteral("框外压暗：关"));
    } else if (id == QLatin1String("toggleFullScreen")) {
        setFullScreenMode(!m_fullScreen);
    }
}

// ===========================================================================
//  目录扫描与列表
// ===========================================================================

void CullWindow::openDirectory(const QString& directoryOrFile)
{
    const QFileInfo info(directoryOrFile);
    QString dir;
    if (info.isDir()) {
        dir = info.absoluteFilePath();
    } else if (info.isFile()) {
        dir = info.absolutePath();
    }
    if (dir.isEmpty()) {
        return;
    }
    m_sourceEdit->setText(QDir::toNativeSeparators(dir));
    scanDirectory();
    saveSettings();
    if (info.isFile()) {
        selectPath(info.absoluteFilePath());
    }
}

CullWindow::Item CullWindow::makeItem(const QString& path, const QDir& rootDir) const
{
    const QFileInfo fileInfo(path);
    Item item;
    item.path = fileInfo.absoluteFilePath();
    item.relativePath =
        QDir::fromNativeSeparators(rootDir.relativeFilePath(item.path));
    item.info.path = item.path;
    item.info.fileName = fileInfo.fileName();
    item.info.fileSize = fileInfo.size();
    item.lastModified = fileInfo.lastModified();
    item.info.isRaw =
        rawcore::RawLoader::isSupportedExtension(item.path.toStdString());
    if (m_store) {
        const rawio::PickEntry entry = m_store->entry(item.relativePath);
        item.star = entry.star;
        item.colorLabel = entry.colorLabel;
    }
    return item;
}

void CullWindow::scanDirectory()
{
    const QString text = m_sourceEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    const QFileInfo dirInfo(text);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        showStatus(QStringLiteral("源目录不存在：%1").arg(QDir::toNativeSeparators(text)));
        return;
    }

    const QString dirPath = dirInfo.absoluteFilePath();

    // F10：监听目录变化（新增 / 删除 / 重命名后自动刷新网格）
    if (m_watcher != nullptr) {
        const QStringList watched = m_watcher->directories();
        if (!watched.isEmpty()) {
            m_watcher->removePaths(watched);
        }
        m_watcher->addPath(dirPath);
    }

    m_sourceEdit->setText(QDir::toNativeSeparators(dirPath));
    m_sourceEdit->setProperty("loadedDir", m_sourceEdit->text());
    // 顶部栏保持固定的应用标语，不随目录名变化（目录已显示在源目录栏中）

    // 丢弃上一目录的状态与在途预览
    ++m_previewTicket;
    if (m_previewWorker != nullptr) {
        m_previewWorker->setLatestTicket(m_previewTicket);
    }
    m_previewCache.clear();
    m_currentPath.clear();
    m_detailPath.clear();
    m_detailTicket = 0;
    m_previewReadyPath.clear();
    m_preview->clearImage();

    m_items.clear();
    m_indexByPath.clear();
    m_pendingMetadata = 0;
    m_rebuilding = true;
    m_list->clear();
    m_rebuilding = false;
    m_logView->clear();

    // ---- 侧车标记 ----
    m_store = std::make_unique<rawio::PickStore>(dirPath);
    const bool storeLoaded = m_store->load();
    // 先重载日志视图再追加警告：reloadLogView 会清空视图
    reloadLogView();
    if (!storeLoaded) {
        appendLogLine(QStringLiteral("侧车文件解析失败，本次不加载历史标记（不会覆盖原文件）"));
    }

    // 侧车日志里「复制」「跳过」代表这张图已经处理过（跳过表示目标目录里
    // 本来就有内容相同的副本），「删除」代表副本又被清掉了。
    // 按时间顺序重放，重启后既能保留已处理进度，也不会把改标记后删掉的副本
    // 重新算成已处理。
    QHash<QString, QStringList> copiedDestinations;
    for (const rawio::PickLogEntry& entry : m_store->logEntries()) {
        const QString source = QDir::fromNativeSeparators(entry.source);
        if (source.isEmpty() || entry.target.isEmpty()) {
            continue;
        }
        if (entry.action == QStringLiteral("复制")
            || entry.action == QStringLiteral("跳过")) {
            copiedDestinations[source].append(entry.target);
        } else if (entry.action == QStringLiteral("删除")) {
            copiedDestinations[source].removeAll(entry.target);
        }
    }

    // ---- 扫描文件（需求默认不递归，可开关）----
    const QDir dir(dirPath);
    QDirIterator iterator(dirPath,
                          QDir::Files | QDir::Readable,
                          m_recursiveCheck->isChecked()
                              ? QDirIterator::Subdirectories
                              : QDirIterator::NoIteratorFlags);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!rawio::isSupportedImageFile(path)) {
            continue;
        }
        Item item = makeItem(path, dir);
        if (m_store) {
            // 路径比较统一用 '/'：日志里存的是 Qt 内部格式，Windows 下
            // 可能混有反斜杠，避免分隔符不同导致匹配不上。
            item.copiedDestinations =
                copiedDestinations.value(QDir::fromNativeSeparators(item.path));
        }
        m_indexByPath.insert(item.path, m_items.size());
        m_items.append(item);
    }

    m_pendingMetadata = m_items.size();
    rebuildList();
    queueFileJobs();

    if (m_items.isEmpty()) {
        showStatus(QStringLiteral("目录中没有找到支持的 RAW / JPG 文件"));
        appendLogLine(QStringLiteral("目录中没有找到支持的 RAW / JPG 文件。"));
    } else {
        showStatus(QStringLiteral("已扫描 %1 张图片").arg(m_items.size()));
    }
}

void CullWindow::refreshItemFromDisk(const QString& path)
{
    Item* item = itemForPath(path);
    if (item == nullptr) {
        return;
    }
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        return;
    }

    // 更新扫描快照：目录监听的后续通知不会再把这张图判成"已变化"
    item->lastModified = fileInfo.lastModified();
    item->info.fileSize = fileInfo.size();
    item->metadataLoaded = false;
    item->thumbnail = QImage();
    refreshItemText(*item);
    refreshItemIcon(*item);

    // 旧的预览缓存与 1:1 细节都基于被覆盖前的文件，必须作废
    m_previewCache.remove(path);
    if (path == m_currentPath) {
        m_detailPath.clear();
        m_detailTicket = 0;
        m_previewReadyPath.clear();
        m_preview->clearImage();
        updateInfoBar();
        requestPreviewForCurrent(true);
    }

    // 重新读取元数据（尺寸 / 拍摄时间可能已变）和缩略图
    emit metadataRequested(path);
    emit thumbnailRequested(path, kThumbnailSize);
}

void CullWindow::queueFileJobs()
{
    for (const Item& item : m_items) {
        emit metadataRequested(item.path);
        emit thumbnailRequested(item.path, kThumbnailSize);
    }
}

void CullWindow::rebuildList()
{
    if (m_rebuilding) {
        return;
    }
    m_rebuilding = true;

    const QString keep = m_currentPath;
    m_list->clear();
    for (Item& item : m_items) {
        item.listItem = nullptr;
    }

    // ---- 筛选 ----
    // 需求 §一 4：格式过滤（全部 / 仅 RAW / 仅 JPG）与标记过滤是两套独立条件
    const int formatFilter = m_formatCombo->currentIndex();
    // 用稳定键判断筛选项：选项里包含按颜色生成的条目，下标会随目标目录变化
    const QString filterKey = currentFilterKey();
    QVector<int> order;
    order.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items.at(i);
        bool visible = true;
        if (formatFilter == 1) {
            visible = item.info.isRaw;
        } else if (formatFilter == 2) {
            visible = !item.info.isRaw;
        }
        if (filterKey.startsWith(QStringLiteral("star"))) {
            visible = item.star >= filterKey.mid(4).toInt();
        } else if (filterKey == QLatin1String("marked")) {
            visible = !item.colorLabel.isEmpty();
        } else if (filterKey == QLatin1String("unmarked")) {
            visible = item.colorLabel.isEmpty();
        } else if (filterKey.startsWith(QStringLiteral("color:"))) {
            visible = item.colorLabel == filterKey.mid(6);
        }
        if (visible) {
            order.append(i);
        }
    }

    // ---- 排序 ----
    // 时间分组要求照片按拍摄时间连续排列，因此分组期间强制按时间排序；
    // 其余情况仍由「排序」下拉框决定。
    const bool groupByTime = m_timeGroupCheck != nullptr && m_timeGroupCheck->isChecked();
    const int sortIndex = groupByTime ? 1 : m_sortCombo->currentIndex();
    const bool descending = m_descendingCheck->isChecked();
    std::stable_sort(order.begin(), order.end(),
                     [this, sortIndex, descending, groupByTime](int a, int b) {
        const Item& left = m_items.at(a);
        const Item& right = m_items.at(b);
        int compare = 0;
        if (sortIndex == 1) {
            const bool leftValid = left.info.shotTime.isValid();
            const bool rightValid = right.info.shotTime.isValid();
            if (leftValid != rightValid) {
                // 没有拍摄时间的排在最后。分组模式下不受降序影响，
                // 否则「无拍摄时间」会夹在时间分组中间。
                if (groupByTime) {
                    return leftValid;
                }
                compare = leftValid ? -1 : 1;
            } else if (leftValid) {
                compare = left.info.shotTime < right.info.shotTime
                              ? -1
                              : (left.info.shotTime == right.info.shotTime ? 0 : 1);
            }
        } else if (sortIndex == 2) {
            compare = left.info.fileSize < right.info.fileSize
                          ? -1
                          : (left.info.fileSize == right.info.fileSize ? 0 : 1);
        }
        if (compare == 0) {
            compare = QString::localeAwareCompare(left.info.fileName,
                                                  right.info.fileName);
        }
        return descending ? compare > 0 : compare < 0;
    });

    // ---- 建行 ----
    auto addPhotoItem = [this](int index, bool hidden) {
        Item& item = m_items[index];
        auto* listItem = new QListWidgetItem;
        listItem->setData(Qt::UserRole, index);
        listItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        // 单元格尺寸直接固定成网格大小。缩略图文字会在 EXIF 读取完成后再
        // 补上第二行拍摄时间；若交给视图按初始（只有文件名）的一行文字
        // 自动算 sizeHint，补第二行时高度不会重算，时间会被裁掉、
        // 连文件名也会提前出现省略号。
        listItem->setSizeHint(kGridSize);
        m_list->addItem(listItem);
        // 加入列表后再设置隐藏：折叠分组的图片在重建时直接不可见
        listItem->setHidden(hidden);
        item.listItem = listItem;
        refreshItemText(item);
        refreshItemIcon(item);
    };
    auto addGroupHeader = [this](const QString& title, int count, bool collapsed) {
        auto* header = new QListWidgetItem;
        // 只可显示、不可选中：鼠标点击与方向键都不会停在标题行上
        header->setFlags(Qt::ItemIsEnabled);
        header->setData(kGroupHeaderRole, true);
        header->setData(kGroupCountRole, count);
        header->setData(kGroupKeyRole, title);
        header->setData(kGroupCollapsedRole, collapsed);
        header->setText(title);
        header->setSizeHint(QSize(groupHeaderWidth(), kGroupHeaderHeight));
        m_list->addItem(header);
    };

    if (!groupByTime) {
        for (int index : order) {
            addPhotoItem(index, false);
        }
    } else {
        // 相邻两张的拍摄时间间隔超过阈值就另起一组；
        // 没有拍摄时间的照片统一归入最后一组。
        const qint64 gapSeconds =
            static_cast<qint64>(m_timeGroupGapCombo->currentData().toInt()) * 60;
        int groupStart = 0;
        while (groupStart < order.size()) {
            int groupEnd = groupStart + 1;
            while (groupEnd < order.size()) {
                const QDateTime& previous =
                    m_items.at(order.at(groupEnd - 1)).info.shotTime;
                const QDateTime& next = m_items.at(order.at(groupEnd)).info.shotTime;
                if (previous.isValid() != next.isValid()) {
                    break;  // 有时间的和无时间的永远不在同一组
                }
                if (!next.isValid()) {
                    ++groupEnd;  // 连续的无时间照片归入同一组
                    continue;
                }
                if (qAbs(previous.secsTo(next)) > gapSeconds) {
                    break;
                }
                ++groupEnd;
            }

            // 降序时组内第一张是较晚的，标题仍按时间先后显示
            QDateTime groupFirst = m_items.at(order.at(groupStart)).info.shotTime;
            QDateTime groupLast = m_items.at(order.at(groupEnd - 1)).info.shotTime;
            if (groupFirst.isValid() && groupLast.isValid()
                && groupFirst > groupLast) {
                std::swap(groupFirst, groupLast);
            }
            QString title;
            if (!groupFirst.isValid()) {
                title = QStringLiteral("无拍摄时间");
            } else if (groupFirst.date() == groupLast.date()) {
                title = QStringLiteral("%1 – %2")
                            .arg(groupFirst.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                 groupLast.toString(QStringLiteral("HH:mm")));
            } else {
                title = QStringLiteral("%1 – %2")
                            .arg(groupFirst.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                 groupLast.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            }
            // 标题在本次列表内唯一，用它作为折叠状态的稳定键；
            // 筛选、元数据到齐后重建列表时，同名分组保持原来的折叠状态。
            const bool collapsed = m_collapsedTimeGroups.contains(title);
            addGroupHeader(title, groupEnd - groupStart, collapsed);
            for (int i = groupStart; i < groupEnd; ++i) {
                addPhotoItem(order.at(i), collapsed);
            }
            groupStart = groupEnd;
        }
    }

    m_rebuilding = false;

    // 恢复选中项；原项被筛掉时退到第一张
    QListWidgetItem* target = nullptr;
    if (!keep.isEmpty()) {
        if (Item* item = itemForPath(keep); item != nullptr) {
            target = item->listItem;
        }
    }
    if (target == nullptr) {
        // 第一行可能是时间分组标题或折叠后被隐藏的图片，取第一张可见图片
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* candidate = m_list->item(i);
            if (candidate != nullptr && !candidate->data(kGroupHeaderRole).toBool()
                && !candidate->isHidden()) {
                target = candidate;
                break;
            }
        }
    }
    if (target != nullptr) {
        m_list->setCurrentItem(target);
        m_list->scrollToItem(target);
    }
    syncCurrentItem();
}

int CullWindow::groupHeaderWidth() const
{
    if (m_list == nullptr) {
        return 120;
    }
    // 留出滚动条、列表内边距与少量余量：标题行必须比视图可用宽度还宽一点点，
    // 排在它后面的缩略图才会被挤到下一行。
    return std::max(120, m_list->viewport()->width() - 2 * m_list->spacing() - 20);
}

void CullWindow::updateGroupHeaderWidths()
{
    if (m_list == nullptr) {
        return;
    }
    // 没有开启分组时列表里没有标题行，不必每次缩放都遍历一遍
    if (m_timeGroupCheck == nullptr || !m_timeGroupCheck->isChecked()) {
        return;
    }
    const int width = groupHeaderWidth();
    bool changed = false;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        if (item == nullptr || !item->data(kGroupHeaderRole).toBool()) {
            continue;
        }
        if (item->sizeHint().width() == width) {
            continue;
        }
        item->setSizeHint(QSize(width, kGroupHeaderHeight));
        changed = true;
    }
    // 尺寸变化后立即重新排布，否则要等到下一次滚动 / 缩放才会生效，
    // 期间缩略图会挤在标题行右边。
    if (changed) {
        m_list->doItemsLayout();
    }
}

void CullWindow::toggleTimeGroupCollapsed(QListWidgetItem* header)
{
    if (header == nullptr || !header->data(kGroupHeaderRole).toBool()) {
        return;
    }

    const bool collapsed = !header->data(kGroupCollapsedRole).toBool();
    header->setData(kGroupCollapsedRole, collapsed);

    // 记住折叠状态：筛选、元数据到齐后列表会重建，同名分组保持折叠
    const QString key = header->data(kGroupKeyRole).toString();
    if (collapsed) {
        m_collapsedTimeGroups.insert(key);
    } else {
        m_collapsedTimeGroups.remove(key);
    }

    // 组内缩略图 = 标题之后、下一个标题之前的连续行
    const int headerRow = m_list->row(header);
    for (int row = headerRow + 1; row < m_list->count(); ++row) {
        QListWidgetItem* item = m_list->item(row);
        if (item == nullptr || item->data(kGroupHeaderRole).toBool()) {
            break;
        }
        item->setHidden(collapsed);
    }

    // 隐藏 / 显示会改变整行排布，立即重排一次
    m_list->doItemsLayout();
    updateGroupHeaderWidths();

    // 当前图片恰好在这个分组里：它现在不可见了，不能继续停在隐藏行上，
    // 就近挪到下一组的首张；后面没有就退回上一张。
    QListWidgetItem* current = m_list->currentItem();
    if (collapsed && current != nullptr && current->isHidden()) {
        QListWidgetItem* replacement = nullptr;
        for (int row = headerRow + 1; row < m_list->count() && replacement == nullptr;
             ++row) {
            QListWidgetItem* candidate = m_list->item(row);
            if (candidate != nullptr && !candidate->data(kGroupHeaderRole).toBool()
                && !candidate->isHidden()) {
                replacement = candidate;
            }
        }
        for (int row = headerRow - 1; row >= 0 && replacement == nullptr; --row) {
            QListWidgetItem* candidate = m_list->item(row);
            if (candidate != nullptr && !candidate->data(kGroupHeaderRole).toBool()
                && !candidate->isHidden()) {
                replacement = candidate;
            }
        }
        if (replacement != nullptr) {
            // setCurrentItem 触发 currentItemChanged，预览随之切换
            m_list->setCurrentItem(replacement);
            m_list->scrollToItem(replacement);
        }
    }

    updateNavButtons();
}

void CullWindow::updateTimeGroupControls()
{
    if (m_timeGroupCheck == nullptr || m_sortCombo == nullptr) {
        return;
    }
    const bool enabled = m_timeGroupCheck->isChecked();
    // 分组必须按拍摄时间连续排列，此时排序下拉框不再生效
    m_sortCombo->setEnabled(!enabled);
    if (m_timeGroupGapCombo != nullptr) {
        m_timeGroupGapCombo->setEnabled(enabled);
    }
}

void CullWindow::refreshItemText(Item& item)
{
    if (item.listItem == nullptr) {
        return;
    }
    // 缩略图固定两行：第一行文件名，第二行 EXIF 拍摄时间。
    // 星级与标记颜色改用缩略图角标 / 边框提示，不再占用文字行；
    // 相机型号等细节放在顶部信息栏与悬停提示里。
    QString text = item.info.fileName;
    if (item.info.shotTime.isValid()) {
        text += QLatin1Char('\n')
                + item.info.shotTime.toString(QStringLiteral("MM-dd HH:mm"));
    }
    item.listItem->setText(text);

    QString tip = item.path;
    if (item.info.width > 0 && item.info.height > 0) {
        tip += QStringLiteral("\n%1 × %2").arg(item.info.width).arg(item.info.height);
    }
    tip += QStringLiteral("\n%1").arg(formatFileSize(item.info.fileSize));
    const int targetIndex = targetIndexForColor(item.colorLabel);
    if (targetIndex >= 0) {
        tip += QStringLiteral("\n复制到：%1").arg(m_targetDirs.at(targetIndex).path);
    }
    item.listItem->setToolTip(tip);

    // 文字不染色（颜色名写在标记行），缩略图边框用标记颜色提示
    item.listItem->setForeground(QBrush());
}

void CullWindow::refreshItemIcon(Item& item)
{
    if (item.listItem == nullptr) {
        return;
    }

    // 按屏幕缩放因子渲染：以前用 1x 画布，高分屏上整张图被拉伸，
    // 2~3px 的边框和星标都会发虚。这里按物理像素出图、以逻辑坐标绘制，
    // 边框 / 圆角 / 文字在任何缩放比例下都是锐利的。
    qreal dpr = m_list != nullptr ? m_list->devicePixelRatioF() : 1.0;
    if (dpr < 1.0) {
        dpr = 1.0;
    }
    const QSize physicalSize(qRound(kThumbIconSize.width() * dpr),
                             qRound(kThumbIconSize.height() * dpr));

    QImage canvas(physicalSize, QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    // 圆角以外的区域留透明，直接透出列表项的悬停 / 选中底色
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 卡片在图标区域内上下各内缩 kThumbCardVMargin：多出来的那条透明边
    // 让选中 / 悬停底色和卡片之间留出空隙，卡片不会“贴着”选中背景的上沿。
    const QRectF cell(0, kThumbCardVMargin,
                      kThumbCanvas.width(), kThumbCanvas.height());
    const qreal radius = 6.0;
    QPainterPath cellPath;
    // 内缩半像素：让 1px 宽的抗锯齿边缘落在像素中心，圆角更干净
    cellPath.addRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    painter.fillPath(cellPath, QColor(34, 34, 34));
    painter.save();
    painter.setClipPath(cellPath);
    if (!item.thumbnail.isNull()) {
        // 缩略图按卡片（不含上下留白）的物理像素缩放后居中绘制，
        // 避免被 QPainter 二次插值
        const QSize cardPhysical(qRound(cell.width() * dpr),
                                 qRound(cell.height() * dpr));
        const QImage scaled = item.thumbnail.scaled(cardPhysical,
                                                    Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation);
        const QSizeF logicalSize(scaled.width() / dpr, scaled.height() / dpr);
        painter.drawImage(
            QRectF(cell.left() + (cell.width() - logicalSize.width()) / 2.0,
                   cell.top() + (cell.height() - logicalSize.height()) / 2.0,
                   logicalSize.width(), logicalSize.height()),
            scaled);
    } else {
        painter.setPen(QColor(110, 110, 110));
        painter.drawText(cell, Qt::AlignCenter,
                         item.info.isRaw ? QStringLiteral("RAW") : QStringLiteral("JPG"));
    }
    painter.restore();

    // 标记：细边框 + 右上角圆点。比原来的 3px 硬直角矩形克制得多，
    // 边框贴着圆角卡片走，角点让颜色在缩略图很小的时候也能一眼认出。
    if (!item.colorLabel.isEmpty()) {
        const QColor mark(targetColorHex(item.colorLabel));
        if (mark.isValid()) {
            QPainterPath ring;
            ring.addRoundedRect(cell.adjusted(1.5, 1.5, -1.5, -1.5), radius - 1.5,
                                radius - 1.5);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(mark, 2.0));
            painter.drawPath(ring);

            const QPointF dot(cell.right() - 12, cell.top() + 12);
            painter.setPen(QPen(QColor(20, 20, 20, 200), 1.5));
            painter.setBrush(mark);
            painter.drawEllipse(dot, 4.5, 4.5);
        }
    }

    // 星级：小圆角胶囊，不再横贯整张缩略图
    if (item.star > 0) {
        const QString stars(item.star, QChar(0x2605));
        QFont font = painter.font();
        font.setBold(true);
        font.setPointSizeF(9.0);
        const QFontMetricsF metrics(font);
        // 左右各留 9px 内边距：星星贴着胶囊边缘会显得局促
        constexpr qreal kPillPadding = 9.0;
        const QRectF pill(cell.left() + 6, cell.bottom() - 25,
                          metrics.horizontalAdvance(stars) + kPillPadding * 2, 18);
        QPainterPath pillPath;
        pillPath.addRoundedRect(pill, 9, 9);
        painter.fillPath(pillPath, QColor(0, 0, 0, 140));
        painter.setPen(QColor(255, 205, 70));
        painter.setFont(font);
        // 左对齐 + 固定内边距，保证星星与胶囊左边始终有距离
        painter.drawText(pill.adjusted(kPillPadding, 0, -kPillPadding, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, stars);
    }
    painter.end();

    item.listItem->setIcon(QIcon(QPixmap::fromImage(canvas)));
}

void CullWindow::syncCurrentItem()
{
    if (m_rebuilding) {
        return;
    }
    QListWidgetItem* listItem = m_list->currentItem();
    if (listItem != nullptr && listItem->data(kGroupHeaderRole).toBool()) {
        // 分组标题不是图片。Home / PageDown 等交给 QListWidget 的按键
        // 可能把当前项落到标题行上，这里自动挪到最近的图片。
        const int headerRow = m_list->row(listItem);
        QListWidgetItem* replacement = nullptr;
        for (int i = headerRow + 1; i < m_list->count() && replacement == nullptr; ++i) {
            QListWidgetItem* candidate = m_list->item(i);
            if (candidate != nullptr && !candidate->data(kGroupHeaderRole).toBool()
                && !candidate->isHidden()) {
                replacement = candidate;
            }
        }
        for (int i = headerRow - 1; i >= 0 && replacement == nullptr; --i) {
            QListWidgetItem* candidate = m_list->item(i);
            if (candidate != nullptr && !candidate->data(kGroupHeaderRole).toBool()
                && !candidate->isHidden()) {
                replacement = candidate;
            }
        }
        if (replacement != nullptr) {
            // setCurrentItem 会再次触发同步，由那次调用完成状态更新
            m_list->setCurrentItem(replacement);
            m_list->scrollToItem(replacement);
            return;
        }
        listItem = nullptr;
    }
    Item* item = listItem != nullptr
                     ? &m_items[listItem->data(Qt::UserRole).toInt()]
                     : nullptr;
    const QString newPath = item != nullptr ? item->path : QString();
    const bool samePath = (item != nullptr && newPath == m_currentPath);
    m_currentPath = newPath;

    // 同步右下角星星与标记颜色的显示（没有当前图片时清空）
    updateStarRating(item != nullptr ? item->star : 0);
    updateColorLabelSelector(item != nullptr ? item->colorLabel : QString());
    updateInfoBar();
    updateActionState();

    if (!samePath) {
        // 换了图：上一张留下的"保留视图"标记作废
        m_preserveViewPath.clear();
        if (item != nullptr) {
            requestPreviewForCurrent(false);
        } else {
            m_preview->clearImage();
        }
    }
}

void CullWindow::updateInfoBar()
{
    // 内嵌 JPEG 按钮的可用性随当前图片变化；没有选中图片时也要置灰
    updateEmbeddedJpegButton();

    Item* item = itemForPath(m_currentPath);
    if (item == nullptr) {
        m_infoLabel->setText(QStringLiteral(
            "<span style='color:#c9c9c9'>未选择图片</span>"
            "<span style='color:#8f8f8f'>&nbsp;&nbsp;请选择源目录，然后在下方缩略图中点选一张。</span>"));
        return;
    }

    const rawio::ImageFileInfo& info = item->info;
    const QString suffix = QFileInfo(info.path).suffix().toUpper();
    const QString format = info.isRaw
                               ? QStringLiteral("%1 · RAW").arg(suffix)
                               : QStringLiteral("%1 · JPG").arg(suffix);
    const QString dimensions = (info.width > 0 && info.height > 0)
                                   ? QStringLiteral("%1 × %2").arg(info.width).arg(info.height)
                                   : QStringLiteral("—");
    const QString shotTime = info.shotTime.isValid()
                                 ? info.shotTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                 : QStringLiteral("—");
    const QString camera = info.cameraModel.isEmpty() ? QStringLiteral("—")
                                                      : info.cameraModel;
    const QString lens = info.lensModel.isEmpty() ? QStringLiteral("—")
                                                  : info.lensModel;
    const QString focal = info.focalLength > 0.0f
                              ? QStringLiteral("%1 mm").arg(info.focalLength, 0, 'g', 3)
                              : QStringLiteral("—");
    const QString aperture = info.aperture > 0.0f
                                 ? QStringLiteral("f/%1").arg(info.aperture, 0, 'g', 3)
                                 : QStringLiteral("—");
    const QString shutter = rawio::formatShutterSpeed(info.shutter);
    const QString iso = info.iso > 0.0f ? QString::number(static_cast<int>(info.iso))
                                        : QStringLiteral("—");

    // ---- 标记：星级与颜色拆开，各自用自己的颜色 ----
    QString stars;
    if (item->star > 0) {
        stars = QStringLiteral("<span style='color:#ffcd46'>%1</span>")
                    .arg(QString(item->star, QChar(0x2605)));
    }
    QString colorMark;
    if (!item->colorLabel.isEmpty()) {
        const QString color = targetColorHex(item->colorLabel);
        const int targetIndex = targetIndexForColor(item->colorLabel);
        QString destination = QStringLiteral("无对应目录");
        if (targetIndex >= 0) {
            const QString path = m_targetDirs.at(targetIndex).path.trimmed();
            const QString name = QFileInfo(path).fileName();
            destination = path.isEmpty()
                ? QStringLiteral("未选择目录")
                : (name.isEmpty() ? QDir::toNativeSeparators(path) : name);
        }
        colorMark = QStringLiteral("<span style='color:%1'>●</span> "
                                   "<span style='color:#d6d6d6'>%2</span>"
                                   "<span style='color:#8f8f8f'> → %3</span>")
                        .arg(color,
                             targetColorDisplayName(item->colorLabel).toHtmlEscaped(),
                             destination.toHtmlEscaped());
    }

    // 统一的字段节奏：说明文字用暗灰、数值用亮灰，字段之间用暗色圆点分隔。
    // 比全角空格更稳定，换行后也能看出字段边界。
    const QString fieldSeparator =
        QStringLiteral("&nbsp;&nbsp;<span style='color:#4f4f4f'>·</span>&nbsp;&nbsp;");
    auto field = [](const QString& caption, const QString& value) {
        return QStringLiteral("<span style='color:#8f8f8f'>%1</span> "
                              "<span style='color:#d6d6d6'>%2</span>")
            .arg(caption, value.toHtmlEscaped());
    };

    // 白平衡并入上方的曝光参数行（ISO 之后）；RAW 的色温由厂商增益反算
    QString whiteBalanceText;
    if (!info.whiteBalance.isEmpty() || info.whiteBalanceTemperature > 0.0f) {
        whiteBalanceText = info.whiteBalance;
        if (info.whiteBalanceTemperature > 0.0f) {
            const QString temperature = QStringLiteral("%1K")
                                            .arg(static_cast<int>(info.whiteBalanceTemperature + 0.5f));
            whiteBalanceText = whiteBalanceText.isEmpty()
                                   ? temperature
                                   : QStringLiteral("%1 (%2)").arg(whiteBalanceText, temperature);
        }
    }

    QString html;
    // 三行各用一个段落承载，靠段间距把行与行稍微拉开一点。
    // 之前用 <br/> 换行，行距完全由字体决定，三行信息贴得太紧。
    // 首段不留上边距，避免卡片顶部被额外顶开。
    html += QStringLiteral("<p style='margin:0'>");
    // 第一行：文件名（主信息）+ 格式 / 尺寸 / 大小（次要信息）
    html += QStringLiteral("<span style='color:#f0f0f0'><b>%1</b></span>"
                           "&nbsp;&nbsp;<span style='color:#9a9a9a'>%2 · %3 · %4</span>")
                .arg(info.fileName.toHtmlEscaped(), format, dimensions,
                     formatFileSize(info.fileSize));
    html += QStringLiteral("</p>");

    // 第二行：完整 EXIF，一张图一行读完，不再和标记挤在一起
    html += QStringLiteral("<p style='margin-top:4px; margin-bottom:0'>");
    html += field(QStringLiteral("拍摄"), shotTime);
    html += fieldSeparator + field(QStringLiteral("相机"), camera);
    html += fieldSeparator + field(QStringLiteral("镜头"), lens);
    html += fieldSeparator + field(QStringLiteral("焦距"), focal);
    html += fieldSeparator + field(QStringLiteral("光圈"), aperture);
    html += fieldSeparator + field(QStringLiteral("快门"), shutter);
    html += fieldSeparator + field(QStringLiteral("ISO"), iso);
    if (!whiteBalanceText.isEmpty()) {
        html += fieldSeparator + field(QStringLiteral("白平衡"), whiteBalanceText);
    }
    html += QStringLiteral("</p>");

    // 可选信息行：对焦距离 / 测光 / GPS。
    // 相机没有记录对应字段时整项跳过，全部缺失时这一行不出现，避免信息栏留空。
    QStringList extraFields;
    if (info.focusDistance > 0.0f) {
        const QString distance = info.focusDistance < 10.0f
                                     ? QStringLiteral("%1 m").arg(info.focusDistance, 0, 'f', 1)
                                     : QStringLiteral("%1 m")
                                           .arg(QString::number(info.focusDistance, 'f', 0));
        extraFields << field(QStringLiteral("对焦距离"), distance);
    }
    if (!info.meteringMode.isEmpty()) {
        extraFields << field(QStringLiteral("测光"), info.meteringMode);
    }
    if (info.hasGps || info.hasAltitude) {
        // 经纬度用十进制度，保留 6 位小数（约 0.1 米精度）
        const auto formatCoordinate = [](double value, bool latitude) {
            const QChar hemisphere = latitude
                                         ? (value >= 0.0 ? QLatin1Char('N') : QLatin1Char('S'))
                                         : (value >= 0.0 ? QLatin1Char('E') : QLatin1Char('W'));
            return QStringLiteral("%1°%2")
                .arg(QString::number(qAbs(value), 'f', 6), QString(hemisphere));
        };
        QString gpsText;
        if (info.hasGps) {
            gpsText = QStringLiteral("%1, %2")
                          .arg(formatCoordinate(info.latitude, true),
                               formatCoordinate(info.longitude, false));
        }
        if (info.hasAltitude) {
            const QString altitudeText = QStringLiteral("海拔 %1 m")
                                             .arg(QString::number(info.altitude, 'f', 0));
            // 有经纬度时把海拔放在括号里，避免和字段分隔符的圆点混淆
            gpsText = gpsText.isEmpty()
                          ? altitudeText
                          : QStringLiteral("%1（%2）").arg(gpsText, altitudeText);
        }
        extraFields << field(QStringLiteral("GPS"), gpsText);
    }
    if (!extraFields.isEmpty()) {
        html += QStringLiteral("<p style='margin-top:4px; margin-bottom:0'>");
        html += extraFields.join(fieldSeparator);
        html += QStringLiteral("</p>");
    }

    // 标记单独成行，星级（金色）与颜色圆点互不干扰
    html += QStringLiteral("<p style='margin-top:4px; margin-bottom:0'>");
    html += QStringLiteral("<span style='color:#8f8f8f'>标记</span>&nbsp; ");
    if (!stars.isEmpty() && !colorMark.isEmpty()) {
        html += stars + fieldSeparator + colorMark;
    } else if (!stars.isEmpty()) {
        html += stars;
    } else if (!colorMark.isEmpty()) {
        html += colorMark;
    } else {
        html += QStringLiteral("<span style='color:#9a9a9a'>未标记</span>");
    }
    html += QStringLiteral("</p>");
    m_infoLabel->setText(html);
}

void CullWindow::updateEmbeddedJpegButton()
{
    if (m_embeddedJpegButton == nullptr) {
        return;
    }
    // 只有带内嵌 JPEG 的 RAW 才能用这个开关；普通位图与没有内嵌预览的
    // RAW 都置灰。开关的勾选状态保留，切回可用图片时继续生效。
    Item* item = itemForPath(m_currentPath);
    const bool available =
        item != nullptr && item->info.isRaw && item->info.hasEmbeddedPreview;
    m_embeddedJpegButton->setEnabled(available);
    m_embeddedJpegButton->setToolTip(
        available
            ? QStringLiteral("显示相机内嵌 JPEG：更快，色彩为相机直出\n"
                             "再次点击切回软件管线预览")
            : QStringLiteral("当前图片没有可用的内嵌 JPEG，预览使用软件管线"));
}

void CullWindow::updateActionState()
{
    const bool hasCurrent = currentItem() != nullptr;
    m_copyButton->setEnabled(hasCurrent);
    m_copyNextButton->setEnabled(hasCurrent);
    m_starRating->setEnabled(hasCurrent);
    if (m_colorGroup != nullptr) {
        for (QAbstractButton* button : m_colorGroup->buttons()) {
            button->setEnabled(hasCurrent);
        }
    }
    // 批量复制只看"有没有已标记颜色的图片"，与列表多选无关
    bool anyMarked = false;
    for (const Item& item : m_items) {
        if (!item.colorLabel.isEmpty()) {
            anyMarked = true;
            break;
        }
    }
    m_copyMarkedButton->setEnabled(anyMarked);
    updateSelectionInfo();
    // 标记组两侧的箭头跟着当前行位置置灰
    updateNavButtons();
}

void CullWindow::updateSelectionInfo()
{
    if (m_selectionLabel == nullptr) {
        return;
    }
    // 三个计数都以扫描到的全部图片为口径，不受排序 / 筛选影响：
    // 已标记 = 有任何目标颜色，已处理 = 已经成功复制过，总数 = 目录里的图片数。
    int markedCount = 0;
    int copiedCount = 0;
    for (const Item& item : m_items) {
        if (!item.colorLabel.isEmpty()) {
            ++markedCount;
        }
        if (!item.copiedDestinations.isEmpty()) {
            ++copiedCount;
        }
    }
    m_selectionLabel->setText(QStringLiteral("已标记 %1 / 已处理 %2 / 总数 %3")
                                  .arg(markedCount)
                                  .arg(copiedCount)
                                  .arg(m_items.size()));
    m_selectionLabel->setToolTip(
        QStringLiteral("已标记：设定了目标颜色的图片数\n"
                       "已处理：已经成功复制到目标目录的图片数\n"
                       "总数：当前源目录扫描到的图片总数（不受筛选 / 排序影响）"));
}

void CullWindow::showStatus(const QString& text)
{
    statusBar()->showMessage(text);
}

void CullWindow::updateStarRating(int star)
{
    if (m_starRating != nullptr) {
        m_starRating->setRating(std::clamp(star, 0, 5));
    }
}

void CullWindow::updateColorLabelSelector(const QString& label)
{
    if (m_colorGroup == nullptr) {
        return;
    }
    // 程序化勾选不触发 clicked，但仍阻止 group 的 toggled 信号，
    // 避免以后给 group 接状态监听时出现回环。
    const QSignalBlocker blocker(m_colorGroup);
    // QButtonGroup 在互斥模式下会忽略"把当前按钮设为未选中"。清除标记
    // （再点一次圆点 / X 键）时必须先临时关掉互斥，否则圆点会一直亮着。
    const bool exclusive = m_colorGroup->exclusive();
    m_colorGroup->setExclusive(false);
    for (QAbstractButton* button : m_colorGroup->buttons()) {
        button->setChecked(button->property("colorLabel").toString() == label);
    }
    m_colorGroup->setExclusive(exclusive);
}

// ===========================================================================
//  目标目录与标记颜色
// ===========================================================================

QString CullWindow::targetColorHex(const QString& color) const
{
    // 目录自定义过颜色时以自定义色为准；颜色名只是标记的稳定标识，
    // 不随外观变化，所以旧侧车里的 label 仍然能正确匹配。
    const int index = targetIndexForColor(color);
    if (index >= 0 && !m_targetDirs.at(index).customHex.isEmpty()) {
        return m_targetDirs.at(index).customHex;
    }
    const TargetColorInfo* info = findTargetColor(color);
    return info != nullptr ? QLatin1String(info->hex) : QString();
}

QString CullWindow::targetColorDisplayName(const QString& color) const
{
    const int index = targetIndexForColor(color);
    if (index >= 0 && !m_targetDirs.at(index).customHex.isEmpty()) {
        // 自定义颜色没有「名字」，直接显示十六进制色值，
        // 比笼统的「自定义」更能说明这张图属于哪个目录。
        return m_targetDirs.at(index).customHex.toUpper();
    }
    const TargetColorInfo* info = findTargetColor(color);
    return info != nullptr ? QString::fromUtf8(info->display)
                           : QStringLiteral("未命名颜色");
}

int CullWindow::targetIndexForColor(const QString& color) const
{
    if (color.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_targetDirs.size(); ++i) {
        if (m_targetDirs.at(i).color == color) {
            return i;
        }
    }
    return -1;
}

QIcon CullWindow::colorDotIcon(const QString& color) const
{
    const QColor fill(targetColorHex(color));
    if (!fill.isValid()) {
        return QIcon();
    }
    // 圆点按 1x / 2x 各出一张，下拉列表在任意缩放的屏幕上都不糊
    auto makePixmap = [&fill](qreal dpr) {
        constexpr int kLogical = 12;
        QPixmap pixmap(qRound(kLogical * dpr), qRound(kLogical * dpr));
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // 深色描边：浅色标记在深色列表底上也能看出轮廓
        painter.setPen(QPen(QColor(16, 16, 16, 190), 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(1.0, 1.0, 10.0, 10.0));
        painter.end();
        return pixmap;
    };
    QIcon icon;
    icon.addPixmap(makePixmap(1.0));
    icon.addPixmap(makePixmap(2.0));
    return icon;
}

QString CullWindow::currentFilterKey() const
{
    if (m_filterCombo == nullptr) {
        return QStringLiteral("all");
    }
    const QString key = m_filterCombo->currentData().toString();
    return key.isEmpty() ? QStringLiteral("all") : key;
}

void CullWindow::setFilterKey(const QString& key)
{
    if (m_filterCombo == nullptr) {
        return;
    }
    const int index = m_filterCombo->findData(key);
    // 找不到（比如筛选项对应的颜色已被删除）就回落到「全部图片」
    m_filterCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void CullWindow::rebuildFilterCombo()
{
    if (m_filterCombo == nullptr) {
        return;
    }
    const QString previous = currentFilterKey();
    {
        // 重建期间屏蔽信号，避免 clear() 触发一次无意义的列表刷新
        const QSignalBlocker blocker(m_filterCombo);
        m_filterCombo->clear();
        m_filterCombo->addItem(QStringLiteral("全部图片"), QStringLiteral("all"));
        // 星级用 ★ 的个数表示，比「3 星及以上」直观
        for (int star = 1; star <= 5; ++star) {
            QString text(star, QChar(0x2605));
            if (star < 5) {
                text += QStringLiteral(" 及以上");
            }
            m_filterCombo->addItem(text, QStringLiteral("star%1").arg(star));
        }
        m_filterCombo->addItem(QStringLiteral("已标记颜色"), QStringLiteral("marked"));
        // 每个目标目录一种标记颜色：圆点 + 名称，按颜色单独筛选
        for (const TargetDir& target : m_targetDirs) {
            m_filterCombo->addItem(colorDotIcon(target.color),
                                   targetColorDisplayName(target.color),
                                   QStringLiteral("color:") + target.color);
        }
        m_filterCombo->addItem(QStringLiteral("未标记颜色"), QStringLiteral("unmarked"));
    }
    setFilterKey(previous);
}

QString CullWindow::targetDirectoryForItem(const Item& item) const
{
    const int index = targetIndexForColor(item.colorLabel);
    if (index >= 0) {
        return m_targetDirs.at(index).path.trimmed();
    }
    // 有标记但颜色已经没有对应目录（目录被移除）：不兜底，避免错投
    if (!item.colorLabel.isEmpty()) {
        return QString();
    }
    // 未标记颜色：只有一个目标目录时直接用它（等同于旧版单目录的用法）；
    // 有多个目录时必须先标颜色，避免把图片错投到别的目录。
    if (m_targetDirs.size() == 1) {
        return m_targetDirs.first().path.trimmed();
    }
    return QString();
}

QString CullWindow::nextFreeTargetColor() const
{
    for (const TargetColorInfo& info : kTargetColors) {
        const QString name = QLatin1String(info.name);
        if (targetIndexForColor(name) < 0) {
            return name;
        }
    }
    return QString();
}

/// 把某个颜色下的所有标记恢复成未标记（目录被清除时同步清理图片标记）。
/// 返回被清理的图片数量。已复制的文件不动，只改侧车里的标记记录。
int CullWindow::clearColorMarks(const QString& color)
{
    if (color.isEmpty()) {
        return 0;
    }
    int cleared = 0;
    for (Item& item : m_items) {
        if (item.colorLabel != color) {
            continue;
        }
        item.colorLabel.clear();
        ++cleared;
    }
    if (cleared == 0) {
        return 0;
    }
    if (m_store) {
        // 内存里已经清过，这里只需要一次性把侧车落盘
        m_store->clearColorLabel(color);
    }
    // 列表行、筛选结果和当前项的颜色选择器都要按“未标记”重画
    rebuildList();
    updateColorLabelSelector(currentItem() != nullptr ? currentItem()->colorLabel
                                                      : QString());
    updateInfoBar();
    updateActionState();
    return cleared;
}

void CullWindow::rebuildTargetDirRows()
{
    if (m_targetRowsLayout == nullptr) {
        return;
    }
    // 清掉旧行（先隐藏再 deleteLater，避免在事件循环处理前残留在原位）
    while (QLayoutItem* layoutItem = m_targetRowsLayout->takeAt(0)) {
        if (QWidget* widget = layoutItem->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete layoutItem;
    }

    // 行内动作改用图标按钮：文字按钮（“更改”“移除”）太占宽度，
    // 会把本就很窄的路径区挤没。透明底，只在悬停时出现浅色底。
    // 样式与预览信息卡右侧的视图按钮共用。
    const QString rowIconButtonStyle = iconButtonStyle();
    const QSize rowIconSize(14, 14);

    for (int i = 0; i < m_targetDirs.size(); ++i) {
        const TargetDir& target = m_targetDirs.at(i);
        auto* row = new QWidget(m_targetRowsHost);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        // 颜色圆点：与预览下方的标记圆点一一对应；点击进入取色器自定义颜色
        auto* dot = new QToolButton(row);
        dot->setFixedSize(16, 16);
        dot->setFocusPolicy(Qt::NoFocus);
        dot->setCursor(Qt::PointingHandCursor);
        dot->setStyleSheet(colorDotStyle(targetColorHex(target.color)));
        dot->setToolTip(QStringLiteral("%1：点击自定义这个目录的标记颜色")
                            .arg(targetColorDisplayName(target.color)));
        dot->setAccessibleName(QStringLiteral("自定义目标目录颜色"));
        connect(dot, &QToolButton::clicked, this,
                [this, i]() { customizeTargetColor(i); });

        auto* pathLabel = new ElidedPathLabel(row);
        const QString nativePath = target.path.isEmpty()
            ? QStringLiteral("（未选择目录）")
            : QDir::toNativeSeparators(target.path);
        // 右侧对齐 + 左侧按层级省略：窄面板里优先露出最深的目录名
        pathLabel->setPathText(nativePath);
        pathLabel->setStyleSheet(target.path.isEmpty()
                                     ? QStringLiteral("color: #a0a0a0;")
                                     : QStringLiteral("color: #d0d0d0;"));

        auto* changeButton = new QPushButton(row);
        changeButton->setFixedSize(22, 22);
        changeButton->setIcon(QIcon(QStringLiteral(":/icons/edit.svg")));
        changeButton->setIconSize(rowIconSize);
        changeButton->setFocusPolicy(Qt::NoFocus);
        changeButton->setCursor(Qt::PointingHandCursor);
        changeButton->setStyleSheet(rowIconButtonStyle);
        changeButton->setToolTip(QStringLiteral("更改这个目标目录"));
        changeButton->setAccessibleName(QStringLiteral("更改目标目录"));
        connect(changeButton, &QPushButton::clicked, this,
                [this, i]() { changeTargetDirectory(i); });

        auto* removeButton = new QPushButton(row);
        removeButton->setFixedSize(22, 22);
        removeButton->setIcon(QIcon(QStringLiteral(":/icons/remove.svg")));
        removeButton->setIconSize(rowIconSize);
        removeButton->setFocusPolicy(Qt::NoFocus);
        removeButton->setCursor(Qt::PointingHandCursor);
        removeButton->setStyleSheet(rowIconButtonStyle);
        removeButton->setToolTip(QStringLiteral("移除这个目标目录（图片上的颜色标记会一并清除）"));
        removeButton->setAccessibleName(QStringLiteral("移除目标目录"));
        connect(removeButton, &QPushButton::clicked, this,
                [this, i]() { removeTargetDirectory(i); });

        rowLayout->addWidget(dot);
        rowLayout->addWidget(pathLabel, 1);
        // 「更改」和「移除」是一组动作：单独收紧这两个按钮的间距，
        // 让铅笔图标靠近右侧的叉号，两个按钮不像是各管各的。
        auto* rowActions = new QHBoxLayout;
        rowActions->setContentsMargins(0, 0, 0, 0);
        rowActions->setSpacing(1);
        rowActions->addWidget(changeButton);
        rowActions->addWidget(removeButton);
        rowLayout->addLayout(rowActions);
        m_targetRowsLayout->addWidget(row);
    }

    // 「＋ 添加目标目录」按钮跟着目录行数换位置：没有目录行时和「目标目录」
    // 标题同处一行（第 3 行），标题右侧不空出一整行；有目录行时仍然排在
    // 所有目录行的下方（第 4 行）。同一个控件先摘掉再重新放入即可。
    if (m_dirGrid != nullptr && m_addTargetButton != nullptr) {
        m_dirGrid->removeWidget(m_addTargetButton);
        m_dirGrid->addWidget(m_addTargetButton, m_targetDirs.isEmpty() ? 3 : 4,
                             1, 1, 2);
    }
}

void CullWindow::rebuildColorChips()
{
    if (m_colorGroup == nullptr || m_colorChipsLayout == nullptr) {
        return;
    }
    // 圆点数量必须与目标目录一致：先拆掉旧的圆点
    while (QLayoutItem* layoutItem = m_colorChipsLayout->takeAt(0)) {
        if (QWidget* widget = layoutItem->widget()) {
            m_colorGroup->removeButton(qobject_cast<QAbstractButton*>(widget));
            widget->hide();
            widget->deleteLater();
        }
        delete layoutItem;
    }

    QWidget* host = qobject_cast<QWidget*>(m_colorGroup->parent());
    for (const TargetDir& target : m_targetDirs) {
        auto* chip = new QToolButton(host);
        chip->setCheckable(true);
        chip->setFixedSize(20, 20);
        chip->setFocusPolicy(Qt::NoFocus);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chip->setProperty("colorLabel", target.color);
        chip->setStyleSheet(colorLabelChipStyle(targetColorHex(target.color)));
        chip->setToolTip(
            QStringLiteral("%1：标记后复制到\n%2\n再点一次取消标记\n右键：自定义颜色")
                .arg(targetColorDisplayName(target.color),
                     target.path.isEmpty() ? QStringLiteral("（未选择目录）")
                                           : QDir::toNativeSeparators(target.path)));
        chip->setAccessibleName(
            QStringLiteral("%1标记").arg(targetColorDisplayName(target.color)));
        const QString label = target.color;
        connect(chip, &QToolButton::clicked, this, [this, label]() {
            const Item* item = currentItem();
            const QString current = item != nullptr ? item->colorLabel : QString();
            // 再点一次同一个圆点 = 取消标记
            setCurrentColorLabel(current == label ? QString() : label);
        });
        // 左键是打标记，所以自定义颜色放在右键；与左侧目标目录的圆点同一入口
        chip->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(chip, &QToolButton::customContextMenuRequested, this,
                [this, label](const QPoint&) {
                    const int index = targetIndexForColor(label);
                    if (index >= 0) {
                        customizeTargetColor(index);
                    }
                });
        m_colorGroup->addButton(chip);
        m_colorChipsLayout->addWidget(chip);
    }

    const Item* item = currentItem();
    updateColorLabelSelector(item != nullptr ? item->colorLabel : QString());
    // 构建早期（缩略图列表还没创建）时不要碰 m_list
    if (m_list != nullptr) {
        updateActionState();
    }
}

// ---------------------------------------------------------------------------
//  按住空格的浮动色板
// ---------------------------------------------------------------------------

void CullWindow::handleSpacePressed()
{
    // 快捷键与窗口事件过滤器是两条入口，加上按键自动重复，
    // 这里统一挡掉：已经在等待松手或色板已弹出时不再重复开始。
    if (m_spacePressActive || m_spacePickerActive) {
        return;
    }
    const Item* item = currentItem();
    if (item == nullptr) {
        copyCurrent(true);
        return;
    }
    if (item->colorLabel.isEmpty()) {
        // 未标记：按下空格直接弹出色板，松手时确认
        beginSpaceColorPicker();
        return;
    }

    // 已标记：先不复制。松开得快就是"复制并下一张"，
    // 按住超过 1 秒才弹出色板重新选标记。
    m_spacePressActive = true;
    installSpaceEventFilter();
    m_spaceHoldTimer->start(kSpaceHoldMs);
    showStatus(QStringLiteral("松开空格复制并下一张；按住 1 秒可重新选择标记颜色"));
}

void CullWindow::handleSpaceReleased()
{
    if (!m_spacePressActive) {
        return;
    }
    m_spacePressActive = false;
    m_spaceHoldTimer->stop();
    uninstallSpaceEventFilter();
    // 短按（没等到长按弹板）：按当前标记复制并下一张
    copyCurrent(true);
}

void CullWindow::onSpaceHoldTimeout()
{
    if (!m_spacePressActive || m_spacePickerActive) {
        return;
    }
    if (m_targetDirs.isEmpty()) {
        // 没有目标目录时没有颜色可选，不能进入色板流程
        cancelSpacePress();
        showStatus(QStringLiteral("没有目标目录，无法选择标记颜色"));
        return;
    }
    // 仍然按着 Space：弹出色板，默认停在当前标记颜色上
    beginSpaceColorPicker();
}

void CullWindow::cancelSpacePress()
{
    if (!m_spacePressActive) {
        return;
    }
    m_spacePressActive = false;
    if (m_spaceHoldTimer != nullptr) {
        m_spaceHoldTimer->stop();
    }
    uninstallSpaceEventFilter();
}

void CullWindow::installSpaceEventFilter()
{
    if (!m_spaceFilterInstalled) {
        qApp->installEventFilter(this);
        m_spaceFilterInstalled = true;
    }
}

void CullWindow::uninstallSpaceEventFilter()
{
    if (m_spaceFilterInstalled) {
        qApp->removeEventFilter(this);
        m_spaceFilterInstalled = false;
    }
}

void CullWindow::rebuildSpaceColorPicker()
{
    if (m_colorPickerLayout == nullptr) {
        return;
    }
    // 每次弹出都按当前目标目录重建：增删目录或自定义颜色后立即生效
    while (QLayoutItem* item = m_colorPickerLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_colorPickerButtons.clear();

    auto addDot = [this](const QString& style, const QString& text,
                         const QString& tooltip, int index) {
        auto* dot = new QToolButton(m_colorPickerPopup);
        dot->setFixedSize(28, 28);
        dot->setCheckable(true);
        dot->setFocusPolicy(Qt::NoFocus);
        dot->setCursor(Qt::PointingHandCursor);
        dot->setToolButtonStyle(Qt::ToolButtonTextOnly);
        dot->setStyleSheet(style);
        dot->setText(text);
        dot->setToolTip(tooltip);
        dot->setAccessibleName(tooltip);
        // 鼠标点选也能确认；键盘路径（左右 + 松手）不经过这里
        connect(dot, &QToolButton::clicked, this, [this, index]() {
            if (!m_spacePickerActive) {
                return;
            }
            m_colorPickerIndex = index;
            updateSpaceColorPickerSelection();
            // 鼠标点选也是确认：点最左边的「不选中」等同"清除标记"
            finishSpaceColorPicker(true);
        });
        m_colorPickerLayout->addWidget(dot);
        m_colorPickerButtons.append(dot);
    };

    // 最左边：不选中。对已标记的图片来说，松手后会清除标记
    // （已经复制到目标目录的副本也会按规则一起删除），本身不复制、不切图。
    addDot(QStringLiteral(
               "QToolButton { background-color: #3a3a3a; color: #d8d8d8;"
               " border: 2px solid #202020; border-radius: 14px;"
               " font-size: 13px; font-weight: 600; }"
               "QToolButton:hover { border-color: #c8c8c8; }"
               "QToolButton:checked { border-color: #ffffff; color: #ffffff; }"),
           QStringLiteral("无"), QStringLiteral("不选中（有标记时清除标记）"), 0);

    for (int i = 0; i < m_targetDirs.size(); ++i) {
        const TargetDir& target = m_targetDirs.at(i);
        const QString style = QStringLiteral(
            "QToolButton { background-color: %1; border: 2px solid #202020;"
            " border-radius: 14px; }"
            "QToolButton:hover { border-color: #e8e8e8; }"
            "QToolButton:checked { border-color: #ffffff; }")
            .arg(targetColorHex(target.color));
        const QString tooltip = QStringLiteral("%1（复制到 %2）")
            .arg(targetColorDisplayName(target.color),
                 target.path.isEmpty() ? QStringLiteral("未选择目录")
                                       : QFileInfo(target.path).fileName());
        addDot(style, QString(), tooltip, i + 1);
    }
}

void CullWindow::updateSpaceColorPickerSelection()
{
    for (int i = 0; i < m_colorPickerButtons.size(); ++i) {
        m_colorPickerButtons.at(i)->setChecked(i == m_colorPickerIndex);
    }
}

void CullWindow::moveSpaceColorPickerSelection(int delta)
{
    const int count = m_colorPickerButtons.size();
    if (count <= 0) {
        return;
    }
    // 首尾循环：第一个颜色按 ← 到取消，取消再按 ← 到最后一个颜色
    m_colorPickerIndex = (m_colorPickerIndex + delta + count) % count;
    updateSpaceColorPickerSelection();
}

void CullWindow::beginSpaceColorPicker()
{
    rebuildSpaceColorPicker();
    if (m_colorPickerButtons.size() <= 1) {
        // 没有可选颜色：未标记图片保持"按下即复制"的旧行为；
        // 已标记图片是长按进来的，此时不能复制，直接收尾等松手。
        if (m_spacePressActive) {
            cancelSpacePress();
            showStatus(QStringLiteral("没有可选的目标颜色"));
        } else {
            copyCurrent(true);
        }
        return;
    }

    // 默认停在当前标记颜色上：长按改标记时不用先左右挪动；
    // 未标记的图片仍默认第一个颜色。
    m_colorPickerIndex = 1;
    const Item* item = currentItem();
    if (item != nullptr && !item->colorLabel.isEmpty()) {
        const int targetIndex = targetIndexForColor(item->colorLabel);
        if (targetIndex >= 0) {
            m_colorPickerIndex = targetIndex + 1;
        }
    }
    updateSpaceColorPickerSelection();
    m_colorPickerPopup->adjustSize();
    const QSize size = m_colorPickerPopup->sizeHint();
    const int margin = 24;
    m_colorPickerPopup->move(qMax(0, (m_preview->width() - size.width()) / 2),
                             qMax(margin, m_preview->height() - size.height() - margin));
    m_colorPickerPopup->show();
    m_colorPickerPopup->raise();

    m_spacePickerActive = true;
    installSpaceEventFilter();
    showStatus((item != nullptr && !item->colorLabel.isEmpty())
                   ? QStringLiteral("← → 改标记，最左边为不选中（清除标记）；"
                                    "选颜色松手后复制并下一张，Esc 取消")
                   : QStringLiteral("← → 选颜色，最左边为不选中；"
                                    "松手后复制并下一张，Esc 取消"));
}

void CullWindow::finishSpaceColorPicker(bool commit)
{
    if (!m_spacePickerActive) {
        return;
    }
    m_spacePickerActive = false;
    // 长按打开色板时还带着 Space 等待状态，一并收尾
    m_spacePressActive = false;
    if (m_spaceHoldTimer != nullptr) {
        m_spaceHoldTimer->stop();
    }
    uninstallSpaceEventFilter();
    if (m_colorPickerPopup != nullptr) {
        m_colorPickerPopup->hide();
    }
    if (!commit) {
        showStatus(QStringLiteral("已取消"));
        return;  // Esc / 点到色板外面：不改标记、不复制、不切图
    }
    const int index = m_colorPickerIndex - 1;
    if (index < 0) {
        // 最左边是「不选中」：已标记的图片在这里松手就清除标记
        // （之前复制到目标目录的副本会由 setCurrentColorLabel 一并删除）。
        // 本来就没标记的图片什么都不做；两种情况都不复制、不切到下一张。
        const Item* item = currentItem();
        if (item != nullptr && !item->colorLabel.isEmpty()) {
            setCurrentColorLabel(QString());
        } else {
            showStatus(QStringLiteral("未选择标记"));
        }
        return;
    }
    if (index >= m_targetDirs.size()) {
        return;
    }
    setCurrentColorLabel(m_targetDirs.at(index).color);
    copyCurrent(true);
}

void CullWindow::addTargetDirectory()
{
    if (m_targetDirs.size() >= kMaxTargetDirs) {
        QMessageBox::information(
            this, QStringLiteral("目标目录"),
            QStringLiteral("最多支持 %1 个目标目录（颜色数量有限）。").arg(kMaxTargetDirs));
        return;
    }
    const QString startDir = m_targetDirs.isEmpty() ? m_sourceEdit->text()
                                                    : m_targetDirs.last().path;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("添加目标目录"), startDir);
    if (dir.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(dir).absoluteFilePath();
    for (const TargetDir& target : m_targetDirs) {
        if (!target.path.isEmpty()
            && QFileInfo(target.path).absoluteFilePath() == absolute) {
            showStatus(QStringLiteral("这个目录已经在目标目录列表里了"));
            return;
        }
    }

    // 目录列表里不存在空行：每次都追加一个新目录（列表为空时就是第一行）
    TargetDir target;
    target.color = nextFreeTargetColor();
    if (target.color.isEmpty()) {
        return;
    }
    target.path = QDir::toNativeSeparators(dir);
    m_targetDirs.append(target);
    const int index = m_targetDirs.size() - 1;

    rebuildTargetDirRows();
    rebuildColorChips();
    rebuildFilterCombo();
    saveSettings();
    showStatus(QStringLiteral("已添加目标目录：%1（%2）")
                   .arg(QDir::toNativeSeparators(dir),
                        targetColorDisplayName(m_targetDirs.at(index).color)));
}

void CullWindow::changeTargetDirectory(int index)
{
    if (index < 0 || index >= m_targetDirs.size()) {
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("更改目标目录"), m_targetDirs.at(index).path);
    if (dir.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(dir).absoluteFilePath();
    for (int i = 0; i < m_targetDirs.size(); ++i) {
        if (i != index && !m_targetDirs.at(i).path.isEmpty()
            && QFileInfo(m_targetDirs.at(i).path).absoluteFilePath() == absolute) {
            showStatus(QStringLiteral("这个目录已经在目标目录列表里了"));
            return;
        }
    }
    m_targetDirs[index].path = QDir::toNativeSeparators(dir);
    rebuildTargetDirRows();
    rebuildColorChips();
    saveSettings();
}

void CullWindow::removeTargetDirectory(int index)
{
    if (index < 0 || index >= m_targetDirs.size()) {
        return;
    }
    const TargetDir target = m_targetDirs.at(index);
    const QString pathText = target.path.trimmed().isEmpty()
        ? QStringLiteral("（未选择目录）")
        : QDir::toNativeSeparators(target.path);
    int markedCount = 0;
    for (const Item& item : m_items) {
        if (!target.color.isEmpty() && item.colorLabel == target.color) {
            ++markedCount;
        }
    }
    // 移除目录：先确认。这个颜色下的图片标记会一并恢复为未标记。
    QString question = QStringLiteral("确定要移除这个目标目录吗？\n%1").arg(pathText);
    if (markedCount > 0) {
        question += QStringLiteral(
                        "\n\n已有 %1 张图片标记为「%2」，移除后这些图片会恢复为未标记。")
                        .arg(markedCount)
                        .arg(targetColorDisplayName(target.color));
    }
    // 用自定义按钮替代 Yes / No：动作用「移除」写明，
    // 取消统一叫「取消」，默认停在取消上，避免误按回车。
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("移除目标目录"));
    box.setText(question);
    QPushButton* confirmButton = box.addButton(QStringLiteral("移除"), QMessageBox::AcceptRole);
    QPushButton* cancelButton = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelButton);
    box.exec();
    if (box.clickedButton() != confirmButton) {
        return;
    }

    // 允许一个目录都不剩：最后一行也直接删掉，界面上只剩「＋ 添加目标目录」按钮，
    // 不再保留一行空路径占位。
    m_targetDirs.removeAt(index);
    rebuildTargetDirRows();
    rebuildColorChips();
    rebuildFilterCombo();
    saveSettings();
    // 目录没了，这个颜色下的标记也不再有意义：统一恢复为未标记
    clearColorMarks(target.color);
    updateInfoBar();
}

void CullWindow::customizeTargetColor(int index)
{
    if (index < 0 || index >= m_targetDirs.size()) {
        return;
    }
    const QString label = m_targetDirs.at(index).color;
    const QColor current(targetColorHex(label));
    const QColor chosen = QColorDialog::getColor(
        current.isValid() ? current : QColor(Qt::gray), this,
        QStringLiteral("选择这个目标目录的标记颜色"));
    if (!chosen.isValid()) {
        return;  // 用户取消
    }
    m_targetDirs[index].customHex = chosen.name(QColor::HexRgb);

    // 颜色名（侧车 label）保持不变，只换显示色：
    // 已经标记过的图片、批量复制与日志匹配都不受影响。
    rebuildTargetDirRows();
    rebuildColorChips();
    // 自定义颜色的显示名就是十六进制色值，筛选列表里的名称和圆点要跟着换
    rebuildFilterCombo();
    for (Item& item : m_items) {
        if (item.colorLabel == label) {
            refreshItemIcon(item);  // 缩略图边框的颜色圆点
        }
    }
    updateInfoBar();
    saveSettings();
}

void CullWindow::selectOffset(int delta)
{
    if (m_list->count() == 0 || delta == 0) {
        return;
    }
    auto isPhotoRow = [this](int row) {
        QListWidgetItem* item = m_list->item(row);
        // 分组标题与折叠后隐藏的图片都不参与逐张切换
        return item != nullptr && !item->data(kGroupHeaderRole).toBool()
            && !item->isHidden();
    };
    int row = m_list->currentRow();
    if (row < 0) {
        // 还没有选中项：从第一张图片开始（跳过可能存在的分组标题）
        for (int i = 0; i < m_list->count(); ++i) {
            if (!isPhotoRow(i)) {
                continue;
            }
            m_list->setCurrentRow(i);
            m_list->scrollToItem(m_list->item(i));
            return;
        }
        return;
    }
    // 分组标题行不可选中：沿方向跳过它，落到下一张图片
    const int step = delta > 0 ? 1 : -1;
    for (int i = row + step; i >= 0 && i < m_list->count(); i += step) {
        if (!isPhotoRow(i)) {
            continue;
        }
        m_list->setCurrentRow(i);
        m_list->scrollToItem(m_list->item(i));
        return;
    }
}

/// 标记组两侧箭头的可用状态：首张没有「上一张」，末张没有「下一张」。
/// 列表为空时两个都不可用；有图但还没选中项时，「下一张」可用（从第一张开始）。
void CullWindow::updateNavButtons()
{
    const int count = m_list != nullptr ? m_list->count() : 0;
    const int row = m_list != nullptr ? m_list->currentRow() : -1;
    bool hasPrevious = false;
    bool hasNext = false;
    for (int i = 0; i < count; ++i) {
        QListWidgetItem* item = m_list->item(i);
        // 分组标题不是图片、折叠后隐藏的图片也不可见，都不参与前后判断
        if (item == nullptr || item->data(kGroupHeaderRole).toBool()
            || item->isHidden()) {
            continue;
        }
        if (i < row) {
            hasPrevious = true;
        } else if (i > row) {
            hasNext = true;
        }
    }
    if (m_prevImageButton != nullptr) {
        m_prevImageButton->setEnabled(hasPrevious);
    }
    if (m_nextImageButton != nullptr) {
        m_nextImageButton->setEnabled(hasNext);
    }
}

void CullWindow::selectVertical(int direction)
{
    const int count = m_list->count();
    if (count == 0 || direction == 0) {
        return;
    }

    QListWidgetItem* current = m_list->currentItem();
    if (current == nullptr) {
        // 还没有选中项时，从第一张可见图片开始（跳过分组标题与折叠项）
        for (int i = 0; i < count; ++i) {
            QListWidgetItem* first = m_list->item(i);
            if (first == nullptr || first->data(kGroupHeaderRole).toBool()
                || first->isHidden()) {
                continue;
            }
            m_list->setCurrentRow(i);
            m_list->scrollToItem(first);
            return;
        }
        return;
    }

    // 当前图片所在的分组被折叠后，它的行已经不可见，没有可用的视觉坐标；
    // 直接按列表顺序落到相邻的可见图片。
    if (current->isHidden()) {
        selectOffset(direction > 0 ? 1 : -1);
        return;
    }

    // 缩略图是 IconMode 网格。按视觉坐标找目标方向的"最近一行"：
    // 先比纵向距离（同一行距离相同），再比横向距离，于是默认落到同一列；
    // 如果目标行在该列没有图（最后一排不满），会落到该行离得最近的图。
    const QRect currentRect = m_list->visualItemRect(current);
    QListWidgetItem* bestItem = nullptr;
    int bestRowDistance = std::numeric_limits<int>::max();
    int bestColumnDistance = std::numeric_limits<int>::max();

    for (int i = 0; i < count; ++i) {
        QListWidgetItem* item = m_list->item(i);
        // 分组标题横贯整行、折叠后隐藏的图片没有可见位置，都不能作为落点
        if (item == current || item->data(kGroupHeaderRole).toBool()
            || item->isHidden()) {
            continue;
        }
        const QRect rect = m_list->visualItemRect(item);
        const int deltaY = rect.center().y() - currentRect.center().y();
        if (direction < 0 ? deltaY >= 0 : deltaY <= 0) {
            continue;  // 只考虑目标方向上的行
        }

        const int rowDistance = std::abs(deltaY);
        const int columnDistance =
            std::abs(rect.center().x() - currentRect.center().x());
        if (rowDistance < bestRowDistance
            || (rowDistance == bestRowDistance
                && columnDistance < bestColumnDistance)) {
            bestRowDistance = rowDistance;
            bestColumnDistance = columnDistance;
            bestItem = item;
        }
    }

    if (bestItem == nullptr) {
        // 已经在最上 / 最下一行：保持不动，避免视觉上"跳回另一头"
        return;
    }
    m_list->setCurrentItem(bestItem);
    m_list->scrollToItem(bestItem);
}

void CullWindow::selectPath(const QString& path)
{
    Item* item = itemForPath(path);
    if (item == nullptr) {
        return;
    }
    if (item->listItem == nullptr) {
        // 被当前筛选条件挡住了：临时切回"全部图片"
        {
            QSignalBlocker blocker(m_filterCombo);
            m_filterCombo->setCurrentIndex(0);
        }
        rebuildList();
    }
    if (item->listItem != nullptr) {
        m_list->setCurrentItem(item->listItem);
        m_list->scrollToItem(item->listItem);
        syncCurrentItem();
    }
}

CullWindow::Item* CullWindow::itemForPath(const QString& path)
{
    if (path.isEmpty()) {
        return nullptr;
    }
    const auto it = m_indexByPath.constFind(path);
    if (it == m_indexByPath.constEnd()) {
        return nullptr;
    }
    return &m_items[it.value()];
}

CullWindow::Item* CullWindow::currentItem()
{
    return m_currentPath.isEmpty() ? nullptr : itemForPath(m_currentPath);
}

// ===========================================================================
//  挑片动作
// ===========================================================================

void CullWindow::setCurrentStar(int star)
{
    Item* item = currentItem();
    if (item == nullptr) {
        return;
    }
    item->star = std::clamp(star, 0, 5);
    bool saved = true;
    if (m_store) {
        saved = m_store->setStar(item->relativePath, item->star);
    }
    updateStarRating(item->star);
    refreshItemText(*item);
    refreshItemIcon(*item);
    updateInfoBar();
    QString status = item->star > 0
                         ? QStringLiteral("已设置 %1 星：%2")
                               .arg(item->star)
                               .arg(item->info.fileName)
                         : QStringLiteral("已清除星级：%1").arg(item->info.fileName);
    if (!saved) {
        status += QStringLiteral("（源目录不可写，标记仅本次有效）");
    }
    showStatus(status);
}

void CullWindow::copyCurrent(bool advance)
{
    Item* item = currentItem();
    if (item == nullptr) {
        showStatus(QStringLiteral("请先选择一张图片"));
        return;
    }
    const QString targetDir = targetDirectoryForItem(*item);
    if (targetDir.isEmpty()) {
        if (m_targetDirs.isEmpty()) {
            // 一个目标目录都没有：先让用户添加，而不是提示标记颜色
            showStatus(QStringLiteral("还没有目标目录，请先在左侧点「＋ 添加目标目录」"));
        } else if (!item->colorLabel.isEmpty()) {
            // 已标记颜色、但那个目录还没选路径时，提示改选路径而不是提示标记
            showStatus(QStringLiteral("“%1”对应的目标目录还没有选择路径：%2")
                           .arg(targetColorDisplayName(item->colorLabel),
                                item->info.fileName));
        } else {
            showStatus(QStringLiteral("请先给这张图标记颜色（对应一个目标目录）：%1")
                           .arg(item->info.fileName));
        }
        return;
    }
    QString error;
    if (!ensureTargetDirectory(targetDir, error)) {
        QMessageBox::warning(this, QStringLiteral("无法复制"), error);
        return;
    }
    bool alreadyExists = false;
    if (!copyOne(*item, targetDir, alreadyExists, error)) {
        QMessageBox::warning(this, QStringLiteral("复制失败"), error);
        return;
    }
    updateSelectionInfo();
    if (alreadyExists) {
        // 目标目录里已有内容完全相同的副本，本次不重复写入
        showStatus(QStringLiteral("目标目录已有相同图片，跳过复制：%1 → %2")
                       .arg(item->info.fileName, QDir::toNativeSeparators(targetDir)));
    } else {
        showStatus(QStringLiteral("已复制到 %1：%2")
                       .arg(QDir::toNativeSeparators(targetDir), item->info.fileName));
    }
    if (advance) {
        selectOffset(1);
    }
}

void CullWindow::copyMarked()
{
    // 按列表顺序收集所有带颜色标记的图片
    QVector<Item*> marked;
    for (Item& item : m_items) {
        if (!item.colorLabel.isEmpty()) {
            marked.append(&item);
        }
    }
    if (marked.isEmpty()) {
        showStatus(QStringLiteral("没有已标记颜色的图片"));
        return;
    }
    if (m_targetDirs.isEmpty()) {
        // 没有目标目录时，所有标记颜色都没有去向，直接说明原因
        showStatus(QStringLiteral("还没有目标目录，请先在左侧点「＋ 添加目标目录」"));
        return;
    }

    // 先把要用到的目标目录校验一遍（缺目录时在这里询问创建），
    // 避免在复制循环里弹询问框打断进度。
    QStringList dirs;
    int unresolved = 0;
    for (const Item* item : marked) {
        const QString dir = targetDirectoryForItem(*item);
        if (dir.isEmpty()) {
            ++unresolved;
            continue;
        }
        const QString normalized = QFileInfo(dir).absoluteFilePath();
        if (!dirs.contains(normalized)) {
            dirs.append(normalized);
        }
    }
    for (const QString& dir : dirs) {
        QString error;
        if (!ensureTargetDirectory(dir, error)) {
            QMessageBox::warning(this, QStringLiteral("无法复制"), error);
            return;
        }
    }

    QProgressDialog progress(QStringLiteral("正在复制…"), QStringLiteral("取消"),
                             0, marked.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    int succeeded = 0;
    int failed = 0;
    int skipped = unresolved;
    int alreadyPresent = 0;
    for (int i = 0; i < marked.size(); ++i) {
        if (progress.wasCanceled()) {
            break;
        }
        Item& item = *marked.at(i);
        progress.setLabelText(QStringLiteral("正在复制：%1").arg(item.info.fileName));
        progress.setValue(i);
        QApplication::processEvents();

        const QString targetDir = targetDirectoryForItem(item);
        if (targetDir.isEmpty()) {
            // 颜色没有对应目录（目录被删掉，或颜色表发生变化）
            appendLogLine(QStringLiteral("[跳过] %1：标记颜色没有对应目标目录")
                              .arg(item.info.fileName));
            continue;
        }
        QString oneError;
        bool alreadyExists = false;
        if (copyOne(item, targetDir, alreadyExists, oneError)) {
            if (alreadyExists) {
                // 同一张图已经在目标目录里，不重复复制
                ++alreadyPresent;
            } else {
                ++succeeded;
            }
        } else {
            ++failed;
            appendLogLine(QStringLiteral("[失败] %1").arg(oneError));
        }
    }
    progress.setValue(marked.size());
    // 批量复制后整批图片的"已处理"状态都变了，统一刷新一次状态栏计数
    updateSelectionInfo();

    QString status = QStringLiteral("批量复制完成：成功 %1 张，失败 %2 张")
                         .arg(succeeded)
                         .arg(failed);
    if (alreadyPresent > 0) {
        status += QStringLiteral("，跳过 %1 张（目标目录已有相同图片）")
                      .arg(alreadyPresent);
    }
    if (skipped > 0) {
        status += QStringLiteral("，跳过 %1 张（颜色没有对应目录，或目录还没选择路径）")
                      .arg(skipped);
    }
    showStatus(status);
}

bool CullWindow::copyOne(Item& item, const QString& targetDir, bool& alreadyExists,
                         QString& error)
{
    alreadyExists = false;
    const QString destination =
        uniqueDestination(targetDir, item.path, item.info.fileName, alreadyExists);
    if (destination.isEmpty()) {
        error = QStringLiteral("无法确定目标文件名。");
        return false;
    }
    // 日志里显示的颜色：优先用图片自己的标记颜色；未标记颜色时
    // （只有唯一目标目录的用法）取该目录的颜色，日志仍能看出落到了哪一类。
    QString colorName;
    if (!item.colorLabel.isEmpty()) {
        colorName = targetColorDisplayName(item.colorLabel);
    } else if (m_targetDirs.size() == 1 && !m_targetDirs.first().color.isEmpty()) {
        colorName = targetColorDisplayName(m_targetDirs.first().color);
    }
    if (alreadyExists) {
        // 目标目录里已有与源文件内容完全相同的副本：同一张图不重复复制。
        // 记成已处理，以后改标记时仍能按侧车记录清理；已经在记录里就不重复写日志。
        if (!item.copiedDestinations.contains(destination)) {
            item.copiedDestinations.append(destination);
            if (m_store) {
                if (!m_store->addLog(QStringLiteral("跳过"), item.path, destination,
                                     colorName)) {
                    appendLogLine(
                        QStringLiteral("[警告] 分拣日志无法写入侧车文件（源目录只读？）"));
                }
                if (!m_store->logEntries().isEmpty()) {
                    appendLogEntry(m_store->logEntries().last());
                }
            }
        }
        return true;
    }
    // 需求 §五 4：操作系统级复制，不做任何转码
    if (!QFile::copy(item.path, destination)) {
        error = QStringLiteral("无法复制 %1 到 %2。可能是磁盘空间不足或权限不足。")
                    .arg(item.info.fileName, QDir::toNativeSeparators(destination));
        return false;
    }
    // 复制成功即记为已处理；即使侧车日志写不进去（源目录只读），
    // 状态栏的"已处理"也应当反映本次真正完成的复制。
    if (!item.copiedDestinations.contains(destination)) {
        item.copiedDestinations.append(destination);
    }
    if (m_store) {
        if (!m_store->addLog(QStringLiteral("复制"), item.path, destination, colorName)) {
            appendLogLine(QStringLiteral("[警告] 分拣日志无法写入侧车文件（源目录只读？）"));
        }
        if (!m_store->logEntries().isEmpty()) {
            appendLogEntry(m_store->logEntries().last());
        }
    }
    return true;
}

void CullWindow::removeCopiedCopies(Item& item, const QString& keepDirectory)
{
    if (item.copiedDestinations.isEmpty()) {
        return;
    }
    const QFileInfo sourceInfo(item.path);
    // 这里还是修改前的标记，写进「删除」日志便于回溯原来落在哪一类
    const QString colorName = item.colorLabel.isEmpty()
                                  ? QString()
                                  : targetColorDisplayName(item.colorLabel);

    QStringList destinations = item.copiedDestinations;
    QStringList remaining;
    QStringList failed;
    for (const QString& path : destinations) {
        const QFileInfo destInfo(path);
        if (!isCopyOfSourceFile(destInfo, sourceInfo)) {
            // 记录指向的文件不像本程序复制出来的副本：不删文件，
            // 也不再把它当作已处理，避免侧车日志被改坏后误删。
            continue;
        }
        if (!keepDirectory.isEmpty() && destInfo.isFile()
            && isSameDirectory(destInfo.absolutePath(), keepDirectory)) {
            // 新标记仍然指向这个目录：副本依旧有效，保留下来，
            // 避免“先删掉再原样复制一遍”的无谓磁盘读写。
            remaining.append(path);
            continue;
        }
        if (destInfo.exists() && (!destInfo.isFile() || !QFile::remove(path))) {
            // 被其它程序占用或权限不足：保留记录，下次改标记还能再试
            remaining.append(path);
            failed.append(path);
            continue;
        }
        if (m_store) {
            // 文件即使已经不在（用户手动删过），也补一条删除日志，
            // 重启后重放日志才不会重新把它算成已处理。
            if (!m_store->addLog(QStringLiteral("删除"), item.path, path, colorName)) {
                appendLogLine(QStringLiteral("[警告] 删除记录无法写入侧车文件（源目录只读？）"));
            }
            if (!m_store->logEntries().isEmpty()) {
                appendLogEntry(m_store->logEntries().last());
            }
        }
    }
    item.copiedDestinations = remaining;

    if (!failed.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("无法删除副本"),
            QStringLiteral("以下副本没能删除，可能正被其它程序占用：\n%1")
                .arg(QDir::toNativeSeparators(failed.join(QLatin1Char('\n')))));
    }
}

QString CullWindow::uniqueDestination(const QString& targetDir, const QString& sourcePath,
                                      const QString& fileName, bool& alreadyExists) const
{
    alreadyExists = false;
    if (targetDir.isEmpty() || fileName.isEmpty()) {
        return QString();
    }
    const QDir dir(targetDir);
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    // 依次尝试原名、原名_1、原名_2…
    // 遇到已存在的文件时比较内容：完全一致说明同一张图已经复制过，直接跳过；
    // 只是名字相同而内容不同则是两张不同的照片，继续追加序号，绝不覆盖。
    for (int index = 0; index < 100000; ++index) {
        QString name = fileName;
        if (index > 0) {
            name = suffix.isEmpty()
                       ? QStringLiteral("%1_%2").arg(base).arg(index)
                       : QStringLiteral("%1_%2.%3").arg(base).arg(index).arg(suffix);
        }
        const QString candidate = dir.absoluteFilePath(name);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
        if (filesHaveSameContent(sourcePath, candidate)) {
            alreadyExists = true;
            return candidate;
        }
    }
    return QString();
}

bool CullWindow::ensureTargetDirectory(const QString& dir, QString& error)
{
    if (dir.trimmed().isEmpty()) {
        error = QStringLiteral("请先选择目标目录。");
        return false;
    }

    QFileInfo info(dir);
    if (!info.exists()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("目标目录不存在"),
            QStringLiteral("目录不存在：\n%1\n\n是否创建？")
                .arg(QDir::toNativeSeparators(dir)));
        if (answer != QMessageBox::Yes) {
            error = QStringLiteral("目标目录不存在。");
            return false;
        }
        if (!QDir().mkpath(info.absoluteFilePath())) {
            error = QStringLiteral("无法创建目标目录。");
            return false;
        }
        info.refresh();
    }
    if (!info.isDir()) {
        error = QStringLiteral("目标路径不是文件夹。");
        return false;
    }

    // 禁止把目标设成源目录：那样"复制"只会在原地生成副本，容易误操作
    const QString sourceDir = m_sourceEdit->text().trimmed();
    if (!sourceDir.isEmpty()
        && QFileInfo(sourceDir).absoluteFilePath() == info.absoluteFilePath()) {
        error = QStringLiteral("目标目录不能与源目录相同。");
        return false;
    }

    // 权限检查：Windows 的权限位不可靠，直接试写一个临时文件
    QTemporaryFile probe(QDir(info.absoluteFilePath())
                             .filePath(QStringLiteral(".rawedit_write_probe_XXXXXX")));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        error = QStringLiteral("目标目录没有写入权限。");
        return false;
    }
    probe.close();
    return true;
}

// ===========================================================================
//  预览
// ===========================================================================

void CullWindow::requestPreviewForCurrent(bool force)
{
    if (m_currentPath.isEmpty()) {
        m_preview->clearImage();
        return;
    }

    // 切换到别的图：上一张的 1:1 细节作废（它只对上一张有效）
    if (m_detailPath != m_currentPath) {
        m_detailPath.clear();
        m_detailTicket = 0;
    }

    const quint64 ticket = ++m_previewTicket;
    if (m_previewWorker != nullptr) {
        // 直接调用（原子量）：让排队中的旧任务尽快让位
        m_previewWorker->setLatestTicket(ticket);
    }

    if (!force) {
        if (QImage* cached = m_previewCache.object(m_currentPath)) {
            m_previewReadyPath = m_currentPath;
            m_preview->setImage(*cached);
            showStatus(QStringLiteral("预览已就绪（缓存）：%1")
                           .arg(QFileInfo(m_currentPath).fileName()));
            return;
        }
    }

    // 从缩略图顶上来的画面还不是真正的预览，不能据此请求 1:1 细节
    m_previewReadyPath.clear();
    // 缩略图先顶一下，避免切图瞬间画布一片空白。
    // 只换渲染选项时画布上已经是同一张图，塞小缩略图反而会重置缩放，
    // 所以保留视图的请求跳过占位。
    const bool preserveView = (m_preserveViewPath == m_currentPath);
    if (!preserveView) {
        if (Item* item = itemForPath(m_currentPath); item != nullptr && !item->thumbnail.isNull()) {
            m_preview->setImage(item->thumbnail);
        }
    }
    emit previewRequested(m_currentPath, kPreviewMaxDimension, ticket);
    showStatus(QStringLiteral("正在生成预览：%1")
                   .arg(QFileInfo(m_currentPath).fileName()));
}

void CullWindow::applyPreviewImage(const QString& path, const QImage& image,
                                   bool preserveView)
{
    if (image.isNull()) {
        return;
    }
    m_previewCache.insert(path, new QImage(image), 1);
    if (path == m_currentPath) {
        if (preserveView) {
            // 同一张图重新渲染（例如切换畸变校正）：只换像素，视点不动
            m_preview->setImagePreservingView(image);
        } else {
            m_preview->setImage(image);
        }
    }
}

void CullWindow::onPreviewReady(const QString& path, const QImage& image, quint64 ticket)
{
    // ---- 1:1 细节图 ----
    // 细节结果只替换画布上的像素，不写入 3072 预览缓存；视图比例与平移
    // 由 setImagePreservingView 保留，用户正在看的位置不会跳回适合窗口。
    if (m_detailTicket != 0 && ticket == m_detailTicket) {
        if (path != m_currentPath) {
            return;
        }
        m_preview->setImagePreservingView(image);
        showStatus(QStringLiteral("1:1 细节已就绪：%1 × %2 像素（原图分辨率）")
                       .arg(image.width())
                       .arg(image.height()));
        return;
    }

    // 过期结果（用户已经翻到别的图）直接丢弃
    if (ticket != m_previewTicket || path != m_currentPath) {
        return;
    }
    m_previewReadyPath = path;
    // 渲染选项变化后的重渲染：保留缩放与平移，用户不会从 1:1 被拽回适合窗口
    const bool preserveView = (m_preserveViewPath == path);
    m_preserveViewPath.clear();
    applyPreviewImage(path, image, preserveView);
    showStatus(QStringLiteral("预览已就绪：%1 × %2 像素")
                   .arg(image.width())
                   .arg(image.height()));

    // 用户可能在预览到达之前就已经滚轮放大了，这里补一次判断
    maybeRequestDetail();
}

void CullWindow::onPreviewFailed(const QString& path, const QString& message, quint64 ticket)
{
    if (m_detailTicket != 0 && ticket == m_detailTicket) {
        if (path != m_currentPath) {
            return;
        }
        // 细节失败不影响已经显示的快速预览；清掉状态，下次缩放还能重试
        m_detailPath.clear();
        m_detailTicket = 0;
        showStatus(QStringLiteral("1:1 细节生成失败：%1").arg(message));
        return;
    }

    if (ticket != m_previewTicket || path != m_currentPath) {
        return;
    }
    // 失败的这轮不再保留视图，下一次正常请求回到"适合窗口"
    m_preserveViewPath.clear();
    showStatus(QStringLiteral("预览失败：%1").arg(message));
    // 有缩略图时至少显示一张小图，避免画布空白
    if (Item* item = itemForPath(path); item != nullptr && !item->thumbnail.isNull()) {
        m_preview->setImage(item->thumbnail);
    }
}

void CullWindow::maybeRequestDetail()
{
    if (m_currentPath.isEmpty() || m_preview == nullptr) {
        return;
    }
    // 快速预览还没到：先等它，否则细节请求会排在它前面，画布长时间空白
    if (m_previewReadyPath != m_currentPath) {
        return;
    }
    // 这张图已经加载过细节，或已经有在途请求
    if (m_detailPath == m_currentPath) {
        return;
    }
    // 只有画布开始把预览像素放大时才值得付原图渲染的代价（约数秒）
    if (m_preview->screenPixelsPerImagePixel() < 1.0) {
        return;
    }

    // 与普通预览共用票据与工作线程：更大的票据会让排队中的旧预览让位，
    // 用户翻到下一张时 requestPreviewForCurrent 也会用新票据取消它。
    const quint64 ticket = ++m_previewTicket;
    m_detailTicket = ticket;
    m_detailPath = m_currentPath;
    if (m_previewWorker != nullptr) {
        m_previewWorker->setLatestTicket(ticket);
    }
    emit previewRequested(m_currentPath, kDetailMaxDimension, ticket);
    showStatus(QStringLiteral("正在生成 1:1 细节：%1（原图分辨率，稍慢属正常）")
                   .arg(QFileInfo(m_currentPath).fileName()));
}

// ===========================================================================
//  工作线程回调
// ===========================================================================

void CullWindow::onWorkerMetadata(const QString& path, const rawio::ImageFileInfo& info)
{
    Item* item = itemForPath(path);
    if (item == nullptr) {
        return;
    }
    // 探测结果里可能缺少文件大小（例如 exiv2 提前失败），保留扫描值
    const qint64 fallbackSize = item->info.fileSize;
    const bool wasRaw = item->info.isRaw;
    item->info = info;
    if (item->info.fileSize <= 0) {
        item->info.fileSize = fallbackSize;
    }
    item->info.isRaw = item->info.isRaw || wasRaw;
    item->metadataLoaded = true;
    refreshItemText(*item);

    if (path == m_currentPath) {
        updateInfoBar();
    }

    if (m_pendingMetadata > 0) {
        --m_pendingMetadata;
    }
    // 按拍摄时间排序 / 时间分组时，扫描阶段还没有 EXIF 拍摄时间；
    // 等全部元数据到齐后重建一次，分组与顺序才正确。
    if (m_pendingMetadata == 0
        && (m_sortCombo->currentIndex() == 1
            || (m_timeGroupCheck != nullptr && m_timeGroupCheck->isChecked()))) {
        rebuildList();
    }
}

void CullWindow::onWorkerThumbnail(const QString& path, const QImage& image)
{
    Item* item = itemForPath(path);
    if (item == nullptr) {
        return;
    }
    item->thumbnail = image;
    refreshItemIcon(*item);
}

void CullWindow::onWorkerThumbnailFailed(const QString& path, const QString& reason)
{
    // 缩略图失败不影响挑片：图标保留占位块，只在当前图片上提示原因
    if (path == m_currentPath) {
        showStatus(QStringLiteral("缩略图生成失败：%1").arg(reason));
    }
}

// ===========================================================================
//  F10：颜色标签 / CSV / 批量重命名 / 对比 / 构图视野
// ===========================================================================

void CullWindow::setCurrentColorLabel(const QString& label)
{
    Item* item = currentItem();
    if (item == nullptr) {
        return;
    }
    // 改标记 / 清标记后，原目标目录里的副本已经和当前标记对不上了：
    // 先删掉旧副本（比如重新标记会立即复制到新目录，旧目录不能留残影）。
    // 但新标记如果仍指向同一个目录，副本本来就是有效的，保留即可，
    // 避免出现「删掉旧副本 → 又复制一份相同文件」的冗余操作。
    // 重复确认同一个颜色不算改标记，不会触发删除。
    if (item->colorLabel != label && !item->copiedDestinations.isEmpty()) {
        const int newIndex = targetIndexForColor(label);
        const QString keepDirectory =
            newIndex >= 0 ? m_targetDirs.at(newIndex).path.trimmed() : QString();
        removeCopiedCopies(*item, keepDirectory);
    }
    item->colorLabel = label;
    bool saved = true;
    if (m_store) {
        saved = m_store->setColorLabel(item->relativePath, label);
    }
    updateColorLabelSelector(label);
    refreshItemText(*item);
    refreshItemIcon(*item);
    updateInfoBar();
    updateActionState();

    const int targetIndex = targetIndexForColor(label);
    QString status;
    if (label.isEmpty()) {
        status = QStringLiteral("已清除标记：%1").arg(item->info.fileName);
    } else if (targetIndex >= 0) {
        status = QStringLiteral("已标记为%1，将复制到 %2：%3")
                     .arg(targetColorDisplayName(label),
                          QDir::toNativeSeparators(m_targetDirs.at(targetIndex).path),
                          item->info.fileName);
    } else {
        status = QStringLiteral("已标记为%1（没有对应的目标目录）：%2")
                     .arg(targetColorDisplayName(label), item->info.fileName);
    }
    if (!saved) {
        status += QStringLiteral("（源目录不可写，标记仅本次有效）");
    }
    showStatus(status);
}

void CullWindow::exportCsv()
{
    if (m_items.isEmpty()) {
        showStatus(QStringLiteral("当前目录没有可导出的图片"));
        return;
    }

    // 默认文件名带上当天日期，方便同一天多次导出时区分（yyyyMMdd_pick.csv）
    const QString defaultPath = QDir(m_sourceEdit->text().trimmed())
        .filePath(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))
                  + QStringLiteral("_pick.csv"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出选片 CSV"), defaultPath,
        QStringLiteral("CSV (*.csv);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出 CSV"),
                             QStringLiteral("无法写入文件：%1").arg(path));
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "\xEF\xBB\xBF";  // BOM，Excel 打开中文不乱码
    // 原有列保持相对顺序：挑选状态插在文件名之后，EXIF 字段统一追加在后面。
    stream << QStringLiteral(
        "文件名,挑选状态,星级,颜色标记,目标目录,宽度,高度,拍摄时间,文件大小,"
        "相机,镜头,焦距(mm),光圈,快门,ISO,"
        "白平衡,色温(K),测光模式,对焦距离(m),"
        "纬度,经度,海拔(m)\n");
    // 数值字段：没有记录时留空，避免把 0 当成真实值。
    auto numberOrEmpty = [](double value, int precision) {
        return value > 0.0 ? QString::number(value, 'g', precision) : QString();
    };
    for (const Item& item : m_items) {
        auto escape = [](QString text) {
            text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(text);
        };
        const rawio::ImageFileInfo& info = item.info;
        // 挑选状态：与界面色板一致，设定了颜色标记即为「选中」，
        // 清除标记（最左边的「不选中」）后为「未选中」。
        const QString picked = item.colorLabel.isEmpty()
                                   ? QStringLiteral("未选中")
                                   : QStringLiteral("选中");
        const QString shutter = info.shutter > 0.0f
                                    ? rawio::formatShutterSpeed(info.shutter)
                                    : QString();
        stream << escape(info.fileName) << ','
               << escape(picked) << ','
               << item.star << ','
               << escape(item.colorLabel) << ','
               << escape(targetDirectoryForItem(item)) << ','
               << info.width << ','
               << info.height << ','
               // 拍摄时间用与信息栏一致的本地时间写法，避免 ISO 8601 的 "T"
               << escape(info.shotTime.isValid()
                             ? info.shotTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                             : QString()) << ','
               << info.fileSize << ','
               << escape(info.cameraModel) << ','
               << escape(info.lensModel) << ','
               << numberOrEmpty(info.focalLength, 4) << ','
               << numberOrEmpty(info.aperture, 3) << ','
               << escape(shutter) << ','
               << (info.iso > 0.0f ? QString::number(static_cast<int>(info.iso))
                                   : QString()) << ','
               << escape(info.whiteBalance) << ','
               << (info.whiteBalanceTemperature > 0.0f
                       ? QString::number(static_cast<int>(info.whiteBalanceTemperature + 0.5f))
                       : QString()) << ','
               << escape(info.meteringMode) << ','
               << numberOrEmpty(info.focusDistance, 4) << ','
               << (info.hasGps ? QString::number(info.latitude, 'f', 6) : QString()) << ','
               << (info.hasGps ? QString::number(info.longitude, 'f', 6) : QString()) << ','
               << (info.hasAltitude ? QString::number(info.altitude, 'f', 1) : QString())
               << '\n';
    }
    file.close();
    showStatus(QStringLiteral("已导出 CSV：%1").arg(QDir::toNativeSeparators(path)));
}

void CullWindow::renameSelection()
{
    const QList<QListWidgetItem*> selected = m_list->selectedItems();
    if (selected.isEmpty()) {
        showStatus(QStringLiteral("请先在缩略图中选择要重命名的图片"));
        return;
    }

    bool ok = false;
    const QString prefix = QInputDialog::getText(
        this, QStringLiteral("批量重命名"),
        QStringLiteral("在原文件名前添加前缀，例如输入“精选_”得到“精选_IMG_0001.CR3”："),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) {
        return;
    }
    if (prefix.trimmed().isEmpty()) {
        showStatus(QStringLiteral("没有输入前缀，未重命名"));
        return;
    }
    // 前缀会拼进文件名，先挡掉 Windows 不允许的字符，避免整批改名失败
    static const QRegularExpression invalidChars(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (invalidChars.match(prefix).hasMatch()) {
        QMessageBox::warning(
            this, QStringLiteral("批量重命名"),
            QStringLiteral("前缀不能包含 \\ / : * ? \" < > | 这些文件名非法字符。"));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("批量重命名"),
            QStringLiteral("将给 %1 个文件的文件名添加前缀“%2”，且无法自动撤销。继续？")
                .arg(selected.size())
                .arg(prefix)) != QMessageBox::Yes) {
        return;
    }

    const QString root = m_sourceEdit->text().trimmed();
    // 重命名后要重扫列表，记录当前图片的新路径，尽量停在原地
    QString keepPath = m_currentPath;
    int renamedCount = 0;
    for (QListWidgetItem* listItem : selected) {
        const int index = listItem->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_items.size()) {
            continue;
        }
        Item& item = m_items[index];
        const QFileInfo info(item.path);
        // 保留原文件名，只把前缀拼到最前面（不再用前缀整体替换文件名）
        const QString base = prefix + info.completeBaseName();
        QString newPath = info.absolutePath() + QLatin1Char('/') + base
            + QLatin1Char('.') + info.suffix();
        int suffix = 2;
        while (QFileInfo::exists(newPath)) {
            newPath = info.absolutePath() + QLatin1Char('/')
                + base + QStringLiteral("_%1").arg(suffix++)
                + QLatin1Char('.') + info.suffix();
        }
        if (newPath == item.path) {
            continue;
        }

        if (!QFile::rename(item.path, newPath)) {
            appendLogLine(QStringLiteral("[警告] 重命名失败：%1")
                              .arg(QDir::toNativeSeparators(item.path)));
            continue;
        }
        const QString oldRelative = item.relativePath;
        const QString newRelative = QDir(root).relativeFilePath(newPath);
        if (m_store) {
            m_store->renameEntry(oldRelative, newRelative);
        }
        if (item.path == keepPath) {
            keepPath = newPath;
        }
        item.path = newPath;
        item.relativePath = newRelative;
        item.info.fileName = QFileInfo(newPath).fileName();
        ++renamedCount;
    }

    if (renamedCount == 0) {
        showStatus(QStringLiteral("没有文件被重命名"));
        return;
    }
    scanDirectory();
    if (!keepPath.isEmpty()) {
        selectPath(keepPath);
    }
    showStatus(QStringLiteral("已为 %1 个文件添加前缀“%2”")
                   .arg(renamedCount)
                   .arg(prefix));
}

void CullWindow::createSelectionCopies()
{
    // 选中的图片逐个创建副本；没有多选时直接给当前图片创建，
    // 和"复制 & 下一张"的操作习惯保持一致。
    QVector<Item*> targets;
    const QList<QListWidgetItem*> selected = m_list->selectedItems();
    if (selected.isEmpty()) {
        if (Item* item = currentItem()) {
            targets.append(item);
        }
    } else {
        for (QListWidgetItem* listItem : selected) {
            const int index = listItem->data(Qt::UserRole).toInt();
            if (index >= 0 && index < m_items.size()) {
                targets.append(&m_items[index]);
            }
        }
    }
    if (targets.isEmpty()) {
        showStatus(QStringLiteral("请先选择要创建副本的图片"));
        return;
    }

    int succeeded = 0;
    QStringList failedNames;
    for (Item* item : targets) {
        const QString destination = duplicateDestination(item->path);
        if (!destination.isEmpty() && QFile::copy(item->path, destination)) {
            ++succeeded;
        } else {
            failedNames.append(item->info.fileName);
        }
    }

    // 立即重扫，新副本马上出现在缩略图网格里。
    // scanDirectory 自己会写状态栏，所以结果提示放在它之后。
    const QString keepPath = m_currentPath;
    if (succeeded > 0) {
        scanDirectory();
        // 重扫默认回到第一张；这里尽量留在原来正在看的那张图上
        if (!keepPath.isEmpty()) {
            selectPath(keepPath);
        }
    }
    QString status = succeeded > 0 ? QStringLiteral("已创建 %1 个副本").arg(succeeded)
                                   : QStringLiteral("创建副本失败");
    if (!failedNames.isEmpty()) {
        status += QStringLiteral("，失败 %1 个：%2")
                      .arg(static_cast<int>(failedNames.size()))
                      .arg(failedNames.join(QStringLiteral("、")));
    }
    showStatus(status);
}

QString CullWindow::duplicateDestination(const QString& sourcePath) const
{
    const QFileInfo info(sourcePath);
    if (!info.isFile()) {
        return QString();
    }
    const QString dir = info.absolutePath();
    const QString suffix = info.suffix().isEmpty()
                               ? QString()
                               : QLatin1Char('.') + info.suffix();
    // 沿用 Windows 资源管理器的习惯命名："原名 - 副本"，重复创建时追加编号
    const QString base = dir + QLatin1Char('/') + info.completeBaseName()
                         + QStringLiteral(" - 副本");
    QString candidate = base + suffix;
    int index = 2;
    while (QFileInfo::exists(candidate)) {
        candidate = base + QStringLiteral(" (%1)").arg(index++) + suffix;
    }
    return candidate;
}

void CullWindow::captureCompositionSample()
{
    if (m_currentPath.isEmpty()) {
        showStatus(QStringLiteral("请先选择一张图片"));
        return;
    }

    // 只渲染取景框内的画面，不抓控件：直接抓屏会把浮动面板、框线、
    // 压暗和辅助线一起截进画面（需求：截取构图框内区域）。
    const QImage sample = m_preview->compositionSample();
    if (sample.isNull()) {
        QMessageBox::warning(this, QStringLiteral("截取构图视野"),
                             QStringLiteral("当前画面无法截取，请重新选择图片后再试。"));
        return;
    }

    // 直接保存到原图所在目录：原名 + _composition_编号，避免覆盖原图。
    // 编号从 1 开始，同名文件已存在时依次递增。
    const QFileInfo info(m_currentPath);
    const QString base = info.absolutePath() + QLatin1Char('/')
        + info.completeBaseName() + QStringLiteral("_composition");
    QString path = base + QStringLiteral("_1.jpg");
    int index = 2;
    while (QFileInfo::exists(path)) {
        path = base + QStringLiteral("_%1.jpg").arg(index++);
    }

    // 固定按 JPG 保存；质量 92 兼顾体积与画面观感（默认 75 压缩痕迹偏重）
    if (!sample.save(path, "JPG", 92)) {
        QMessageBox::warning(this, QStringLiteral("截取构图视野"),
                             QStringLiteral("保存失败：%1\n请检查源目录是否可写。")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }

    // 立即把新文件加入列表：等目录监听触发全量重扫会重置取景框的缩放 /
    // 平移，连续截取时会打断构图操作。
    const QDir rootDir(m_sourceEdit->text().trimmed());
    Item item = makeItem(path, rootDir);
    const QString samplePath = item.path;
    m_indexByPath.insert(samplePath, m_items.size());
    m_items.append(item);
    rebuildList();
    emit metadataRequested(samplePath);
    emit thumbnailRequested(samplePath, kThumbnailSize);

    showStatus(QStringLiteral("构图视野已保存：%1")
                   .arg(QDir::toNativeSeparators(path)));
}

// ===========================================================================
//  日志
// ===========================================================================

void CullWindow::appendLogLine(const QString& line)
{
    appendLogHtml(line.toHtmlEscaped());
}

void CullWindow::appendLogEntry(const rawio::PickLogEntry& entry)
{
    appendLogHtml(logLineHtml(entry));
}

void CullWindow::appendLogHtml(const QString& html)
{
    // 用 insertHtml 而不是 append：append 是否按富文本解析取决于内容，
    // 这里明确按 HTML 插入，日志里的颜色圆点才能稳定上色。
    QTextCursor cursor(m_logView->document());
    cursor.movePosition(QTextCursor::End);
    // 文档里已经有内容时另起一段，保持每条日志一行的观感
    if (!m_logView->document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertHtml(html);
    m_logView->setTextCursor(cursor);
}

void CullWindow::reloadLogView()
{
    m_logView->clear();
    if (!m_store) {
        return;
    }
    const QVector<rawio::PickLogEntry>& entries = m_store->logEntries();
    // 只展示最近 200 条，避免长期使用的目录把日志面板撑爆
    const int count = static_cast<int>(entries.size());
    const int start = std::max(0, count - 200);
    for (int i = start; i < count; ++i) {
        appendLogEntry(entries.at(i));
    }
}

// ===========================================================================
//  键盘
// ===========================================================================

bool CullWindow::eventFilter(QObject* watched, QEvent* event)
{
    // 浮动色板显示期间接管键盘：← → 切换颜色、Esc 取消、松开空格确认。
    // 这个过滤器在弹出时安装到 qApp 上，因此焦点在任何控件上都能收到松手事件；
    // 注意 qApp 的全局过滤器拿到的 watched 是实际接收者，不能用 qApp 判断。
    if (m_spacePickerActive) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            switch (keyEvent->key()) {
            case Qt::Key_Left:
                moveSpaceColorPickerSelection(-1);
                break;
            case Qt::Key_Right:
                moveSpaceColorPickerSelection(1);
                break;
            case Qt::Key_Escape:
                finishSpaceColorPicker(false);
                break;
            default:
                break;
            }
            // 色板期间吞掉所有按键：等空格松开，也避免方向键挪动缩略图
            return true;
        }
        if (event->type() == QEvent::KeyRelease) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            // 「复制并下一张」可能被改到别的键上，按当前配置判断松手
            if (matchesShortcut(QStringLiteral("copyNext"), keyEvent)) {
                if (!keyEvent->isAutoRepeat()) {
                    // 松开即确认当前选项；最左边的「不选中」也在这里确认
                    finishSpaceColorPicker(true);
                }
                return true;
            }
            return false;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            // 点到色板外面：取消本次标记，点击本身照常生效
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QPoint globalPos = mouseEvent->globalPosition().toPoint();
            if (!m_colorPickerPopup->rect().contains(
                    m_colorPickerPopup->mapFromGlobal(globalPos))) {
                finishSpaceColorPicker(false);
            }
            return false;
        }
        if (event->type() == QEvent::ApplicationDeactivate) {
            finishSpaceColorPicker(false);
            return false;
        }
    } else if (m_spacePressActive) {
        // 已标记图片按下「复制并下一张」后的等待期：松手复制，按住满 1 秒由定时器弹板。
        // 等待期间吞掉按键（包括自动重复），避免改变当前图片。
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelSpacePress();
                showStatus(QStringLiteral("已取消"));
            }
            return true;
        }
        if (event->type() == QEvent::KeyRelease) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (matchesShortcut(QStringLiteral("copyNext"), keyEvent)) {
                if (!keyEvent->isAutoRepeat()) {
                    handleSpaceReleased();
                }
                return true;
            }
            return false;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            // 等待期间点了别处：取消这次复制，点击本身照常生效
            cancelSpacePress();
            return false;
        }
        if (event->type() == QEvent::ApplicationDeactivate) {
            cancelSpacePress();
            return false;
        }
    }

    // 压暗滑杆与主界面参数栏保持同一套规则：
    // 未点中时不响应滚轮（避免扫过左侧参数区误改），双击恢复默认值。
    if (watched == m_dimSlider) {
        if (event->type() == QEvent::Wheel) {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            if (!isSliderWheelArmed(m_dimSlider)) {
                forwardWheelToParentScrollArea(m_dimSlider, wheelEvent);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            disarmSliderWheel(m_dimSlider);
            m_dimSlider->setValue(kDimDefault);
            return true;
        }
    }

    // 缩略图列表可视宽度变化（窗口缩放、滚动条出现 / 消失）后，
    // 重新计算时间分组标题行的宽度，保证它始终独占一整行。
    if (m_list != nullptr && watched == m_list->viewport()
        && event->type() == QEvent::Resize) {
        updateGroupHeaderWidths();
    }

    // 构图 / 设置浮层贴着预览左上角：画面尺寸变化后重新贴边，避免留下空隙或被裁
    if (watched == m_preview && event->type() == QEvent::Resize) {
        if (m_compositionPanel != nullptr && m_compositionPanel->isVisible()) {
            positionCompositionPanel();
        }
        if (m_settingsPanel != nullptr && m_settingsPanel->isVisible()) {
            positionSettingsPanel();
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        // 日志是只读文本框：用鼠标点进去复制文字后，方向键仍应继续翻图，
        // 而不是移动文本光标。这里只接管单方向键；Shift / Ctrl + 方向键
        // 等组合留给 QTextEdit 自己选择文字。
        if (watched == m_logView) {
            const int key = keyEvent->key();
            if ((key == Qt::Key_Left || key == Qt::Key_Right
                 || key == Qt::Key_Up || key == Qt::Key_Down)
                && handleKeyPress(keyEvent)) {
                return true;
            }
        } else if (handleKeyPress(keyEvent)) {
            // 方向键一律由窗口处理：QListWidget 自带的网格导航会把
            // ↑ ↓ 当成前后切换，和 ← → 语义混淆，因此不再交给列表。
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void CullWindow::keyPressEvent(QKeyEvent* event)
{
    if (handleKeyPress(event)) {
        return;
    }
    QMainWindow::keyPressEvent(event);
}

bool CullWindow::handleKeyPress(QKeyEvent* event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (modifiers & Qt::ControlModifier) {
        if (key >= Qt::Key_1 && key <= Qt::Key_5) {
            // 需求 §四 同时把 1~5 分配给构图比例与星级，二者冲突。
            // 这里：数字键 = 星级，Ctrl+数字 = 构图比例，并在控件提示中说明。
            setCompositionEnabled(true);
            m_aspectCombo->setCurrentIndex(key - Qt::Key_1);
            showStatus(QStringLiteral("构图比例：%1")
                           .arg(m_aspectCombo->currentText()));
            return true;
        }
        return false;
    }
    if (modifiers & (Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }

    // 设置面板里可以改键的动作：按当前配置匹配，命中后走同一个执行入口。
    // 已被清除的动作这里不会命中，也就彻底不响应键盘。
    for (const ShortcutAction& action : shortcutActions()) {
        const QString id = QString::fromLatin1(action.id);
        if (matchesShortcut(id, event)) {
            triggerShortcutAction(id);
            return true;
        }
    }

    switch (key) {
    case Qt::Key_Escape:
        // 浮层优先：面板开着时 Esc 只收面板，不退出全屏
        if (m_settingsPanel != nullptr && m_settingsPanel->isVisible()) {
            setSettingsPanelVisible(false);
            return true;
        }
        if (m_compositionPanel != nullptr && m_compositionPanel->isVisible()) {
            setCompositionPanelVisible(false);
            return true;
        }
        if (m_fullScreen) {
            setFullScreenMode(false);
            return true;
        }
        return false;
    case Qt::Key_0:
        setCurrentStar(0);
        return true;
    case Qt::Key_1:
    case Qt::Key_2:
    case Qt::Key_3:
    case Qt::Key_4:
    case Qt::Key_5: {
        // 与点击星星保持一致：再按一次相同数字 = 取消评级，
        // 不必再去找 0 键。
        const int star = key - Qt::Key_0;
        const Item* item = currentItem();
        setCurrentStar(item != nullptr && item->star == star ? 0 : star);
        return true;
    }
    default:
        break;
    }
    return false;
}

void CullWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);

    // 窗口显示时 Qt 会把初始焦点交给焦点链里的第一个控件，
    // 左栏的只读控件（例如分拣日志）可能恰好在最前面，方向键就切不了图。
    // 延后到本轮事件处理结束再明确把焦点交给缩略图列表，
    // 保证一进来 ↑ ↓ ← → 就能直接开始挑片。
    QTimer::singleShot(0, this, [this]() {
        if (m_list != nullptr && m_list->isVisible()) {
            m_list->setFocus(Qt::OtherFocusReason);
        }
    });
}

void CullWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    // 第一次激活时，Qt 可能把默认焦点给了焦点链里的第一个控件
    // （左栏某个只读控件），这里无条件纠正到缩略图列表。
    // 之后的激活只在焦点为空或落在只读日志上时才纠正，
    // 不强行打断用户正在操作的下拉框 / 输入框。
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        const bool firstActivation = !m_initialFocusApplied;
        m_initialFocusApplied = true;
        QWidget* focus = QApplication::focusWidget();
        if (firstActivation || focus == nullptr || focus == m_logView) {
            QTimer::singleShot(0, this, [this]() {
                if (m_list != nullptr && m_list->isVisible()) {
                    m_list->setFocus(Qt::OtherFocusReason);
                }
            });
        }
    }
}

void CullWindow::closeEvent(QCloseEvent* event)
{
    // 关闭窗口时收起浮动色板 / 取消还没松手的 Space，并移除 qApp 上的事件过滤器
    finishSpaceColorPicker(false);
    cancelSpacePress();
    saveSettings();
    QMainWindow::closeEvent(event);
}

}  // namespace rawui
