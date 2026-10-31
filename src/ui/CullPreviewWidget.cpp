// ============================================================================
//  CullPreviewWidget.cpp —— 选图预览与构图叠加层实现
// ============================================================================

#include "CullPreviewWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rawui {

namespace {

/// 透明素材中性灰棋盘（ui_color.md：禁用黑白棋盘）
void paintNeutralCheckerboard(QPainter& painter, const QRectF& rect)
{
    constexpr int kCell = 12;
    painter.save();
    painter.setClipRect(rect);
    painter.fillRect(rect, QColor(0x44, 0x44, 0x44));
    const int left = static_cast<int>(std::floor(rect.left()));
    const int top = static_cast<int>(std::floor(rect.top()));
    const int right = static_cast<int>(std::ceil(rect.right()));
    const int bottom = static_cast<int>(std::ceil(rect.bottom()));
    for (int y = top; y < bottom; y += kCell) {
        for (int x = left; x < right; x += kCell) {
            if ((((x - left) / kCell) + ((y - top) / kCell)) % 2 == 1) {
                painter.fillRect(QRect(x, y, kCell, kCell), QColor(0x50, 0x50, 0x50));
            }
        }
    }
    painter.restore();
}

/// 画布留白：构图框边线不会被控件边缘裁掉
constexpr double kCanvasMargin = 10.0;
/// 缩放范围（相对"适合窗口"）
constexpr double kMinZoom = 0.10;
constexpr double kMaxZoom = 10.0;
/// 拖动时至少保留可见的图像边缘像素
constexpr double kVisibleEdge = 60.0;

double clampDouble(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}

/// 把宽高比格式化成与预设下拉一致的写法（3:2 / 16:9 / 1.85 : 1），
/// 避免框内左上角显示 "1.5 : 1"、下拉里却是 "3:2" 的不一致。
QString formatAspectLabel(double aspect)
{
    struct Preset {
        double value;
        const char* label;
    };
    static const Preset kLabels[] = {
        {3.0 / 2.0, "3:2"},
        {4.0 / 3.0, "4:3"},
        {16.0 / 9.0, "16:9"},
        {1.0, "1:1"},
        {2.39, "2.39:1"},
        {2.0 / 3.0, "2:3"},
        {3.0 / 4.0, "3:4"},
        {4.0 / 5.0, "4:5"},
        {9.0 / 16.0, "9:16"},
    };
    for (const Preset& preset : kLabels) {
        if (std::abs(aspect - preset.value) < 1e-3) {
            return QString::fromLatin1(preset.label);
        }
    }
    return QStringLiteral("%1 : 1").arg(QString::number(aspect, 'g', 3));
}

}  // namespace

CullPreviewWidget::CullPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setAutoFillBackground(false);
}

// ===========================================================================
//  图像与视图
// ===========================================================================

void CullPreviewWidget::setImage(const QImage& image)
{
    m_image = image;
    m_mips.clear();
    rebuildMipLevels();
    resetView();
    update();
}

void CullPreviewWidget::clearImage()
{
    m_image = QImage();
    m_mips.clear();
    resetView();
    update();
}

void CullPreviewWidget::setRotation(int degrees)
{
    int normalized = degrees % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    // 只保留四个直角方向，避免任意角度插值带来的模糊
    normalized = (normalized / 90) * 90;
    if (m_rotation == normalized) {
        return;
    }
    m_rotation = normalized;
    // 旋转已烘焙进 mip 链，角度变了必须重建
    m_mips.clear();
    rebuildMipLevels();
    resetView();
}

void CullPreviewWidget::rotateBy(int degrees)
{
    setRotation(m_rotation + degrees);
}

void CullPreviewWidget::resetView()
{
    const double previousZoom = m_zoom;
    m_zoom = 1.0;
    // 构图模式要求取景框内始终是图像本身：例如竖构图配横框时，
    // "适合窗口"铺不满框，需要放大到刚好盖住取景框的比例
    if (m_composition) {
        m_zoom = std::max(m_zoom, minZoomCoveringFrame());
    }
    m_pan = QPointF(0.0, 0.0);
    update();
    if (std::abs(previousZoom - m_zoom) > 1e-6) {
        emit zoomChanged(m_zoom);
    }
}

double CullPreviewWidget::screenPixelsPerImagePixel() const
{
    if (m_image.isNull()) {
        return 1.0;
    }
    // paintEvent 里的 scale 是"逻辑像素 / 图像像素"，再乘 dpr 才是
    // 屏幕上真实的设备像素比；判断预览是否已经被放大插值必须用后者。
    return fitScale() * m_zoom * devicePixelRatioF();
}

void CullPreviewWidget::setImagePreservingView(const QImage& image)
{
    if (image.isNull()) {
        return;
    }
    if (m_image.isNull()) {
        setImage(image);
        return;
    }

    const QSizeF oldSize = rotatedImageSize();
    const double oldFit = fitScale();
    const double oldZoom = m_zoom;

    m_image = image;
    const QSizeF newSize = rotatedImageSize();
    const double newFit = fitScale();

    // 构图模式下图像尺寸变化可能让原来的缩放盖不满取景框，先补足最小缩放
    if (m_composition) {
        const double minZoom = minZoomCoveringFrame();
        if (m_zoom < minZoom) {
            m_zoom = minZoom;
            emit zoomChanged(m_zoom);
        }
    }

    // 保留视点：平移量是屏幕像素，按"图像在画布上的显示尺寸"等比换算，
    // 新图仍停在原来看的相对位置。
    // 不能只比较宽高比：相机内嵌 JPEG 与软件管线的像素尺寸往往略有差异
    // （例如 1616×1080 对 3000×2000），宽高比差千分之几，直接复位会让
    // 用户在切换预览模式时被拽回"适合窗口"。
    const QSizeF oldDisplayed(oldSize.width() * oldFit * oldZoom,
                              oldSize.height() * oldFit * oldZoom);
    const QSizeF newDisplayed(newSize.width() * newFit * m_zoom,
                              newSize.height() * newFit * m_zoom);
    if (oldDisplayed.width() > 0.0 && oldDisplayed.height() > 0.0) {
        m_pan = QPointF(
            m_pan.x() * newDisplayed.width() / oldDisplayed.width(),
            m_pan.y() * newDisplayed.height() / oldDisplayed.height());
    }

    // 只重建 mip，缩放与平移沿用换算后的值，用户视点不会跳。
    m_mips.clear();
    rebuildMipLevels();
    clampPan();
    update();
}

// ===========================================================================
//  构图框
// ===========================================================================

void CullPreviewWidget::setCompositionEnabled(bool enabled)
{
    if (m_composition == enabled) {
        return;
    }
    m_composition = enabled;
    // 开启构图时回到适合窗口，避免画面还停在上一轮的放大位置
    if (enabled) {
        resetView();
    }
    update();
}

void CullPreviewWidget::setFrameAspect(double aspect)
{
    if (!(aspect > 0.05)) {
        return;
    }
    m_aspect = aspect;
    // 框尺寸变了，原来合法的缩放 / 平移可能已经露出图像边缘
    if (!m_image.isNull()) {
        if (m_composition) {
            const double minZoom = minZoomCoveringFrame();
            if (m_zoom < minZoom) {
                m_zoom = minZoom;
                emit zoomChanged(m_zoom);
            }
        }
        clampPan();
    }
    update();
}

void CullPreviewWidget::setFrameColor(const QColor& color)
{
    if (!color.isValid()) {
        return;
    }
    m_frameColor = color;
    update();
}

void CullPreviewWidget::setRatioLabelVisible(bool visible)
{
    if (m_ratioLabelVisible == visible) {
        return;
    }
    m_ratioLabelVisible = visible;
    update();
}

void CullPreviewWidget::setDimOpacity(int percent)
{
    // 压暗范围 40%~100%：100% 时框外完全变黑，突出取景框内的画面
    m_dimOpacity = std::clamp(percent, 0, 100);
    update();
}

void CullPreviewWidget::setMaskVisible(bool visible)
{
    m_maskVisible = visible;
    update();
}

void CullPreviewWidget::setGuides(bool thirds)
{
    m_guideThirds = thirds;
    update();
}

// ===========================================================================
//  几何计算
// ===========================================================================

QSizeF CullPreviewWidget::rotatedImageSize() const
{
    if (m_image.isNull()) {
        return QSizeF(0.0, 0.0);
    }
    const QSizeF size(m_image.width(), m_image.height());
    return (m_rotation % 180 == 0) ? size : QSizeF(size.height(), size.width());
}

double CullPreviewWidget::fitScale() const
{
    const QSizeF imageSize = rotatedImageSize();
    if (imageSize.isEmpty()) {
        return 1.0;
    }
    const double availableWidth = std::max(1.0, width() - 2.0 * kCanvasMargin);
    const double availableHeight = std::max(1.0, height() - 2.0 * kCanvasMargin);
    return std::min(availableWidth / imageSize.width(),
                    availableHeight / imageSize.height());
}

QRectF CullPreviewWidget::frameRect() const
{
    // 固定视野：取景框的大小只由画布尺寸和所选比例决定，
    // 与图片尺寸、缩放、平移都无关。滚轮缩放 / 拖动时框保持不动，
    // 就像透过固定取景器看画面，框外区域始终压暗。
    const double availableWidth = std::max(1.0, width() - 2.0 * kCanvasMargin);
    const double availableHeight = std::max(1.0, height() - 2.0 * kCanvasMargin);
    if (!(m_aspect > 0.05)) {
        return QRectF();
    }

    // 取景框最多占画布可用区域的 82%，四周留出余量便于观察框外画面
    constexpr double kFrameFill = 0.82;
    double frameWidth = 0.0;
    double frameHeight = 0.0;
    if (m_aspect >= availableWidth / availableHeight) {
        // 目标更宽：受画布宽度限制
        frameWidth = availableWidth * kFrameFill;
        frameHeight = frameWidth / m_aspect;
    } else {
        frameHeight = availableHeight * kFrameFill;
        frameWidth = frameHeight * m_aspect;
    }
    return QRectF((width() - frameWidth) / 2.0,
                  (height() - frameHeight) / 2.0,
                  frameWidth, frameHeight);
}

double CullPreviewWidget::minZoomCoveringFrame() const
{
    const QSizeF imageSize = rotatedImageSize();
    const QRectF frame = frameRect();
    if (imageSize.isEmpty() || frame.isEmpty()) {
        return kMinZoom;
    }
    const double fit = fitScale();
    if (!(fit > 0.0)) {
        return kMinZoom;
    }
    // displayedSize = imageSize * fit * zoom，要求两个方向都盖住取景框
    const double need = std::max(frame.width() / (imageSize.width() * fit),
                                 frame.height() / (imageSize.height() * fit));
    if (need > kMaxZoom) {
        // 极端长宽比（如全景长条）即使放到最大也铺不满，退回普通缩放范围
        return kMinZoom;
    }
    return std::max(kMinZoom, need);
}

QSizeF CullPreviewWidget::displayedImageSize() const
{
    const QSizeF imageSize = rotatedImageSize();
    const double scale = fitScale() * m_zoom;
    return QSizeF(imageSize.width() * scale, imageSize.height() * scale);
}

void CullPreviewWidget::clampPan()
{
    const QSizeF displayed = displayedImageSize();

    // 固定视野模式：取景框内必须始终是图片本身，不能让图像边缘以外的
    // 空白被拖进框里。框和图像都居中，所以可移动余量就是两者尺寸差的
    // 一半；图像在该方向上比框还小时无法铺满，只能固定在中间。
    if (m_composition) {
        const QRectF frame = frameRect();
        const double freeX = (displayed.width() - frame.width()) / 2.0;
        const double freeY = (displayed.height() - frame.height()) / 2.0;
        m_pan.setX(freeX > 0.0 ? clampDouble(m_pan.x(), -freeX, freeX) : 0.0);
        m_pan.setY(freeY > 0.0 ? clampDouble(m_pan.y(), -freeY, freeY) : 0.0);
        return;
    }

    const double maxX = std::max(0.0, (displayed.width() + width()) / 2.0 - kVisibleEdge);
    const double maxY = std::max(0.0, (displayed.height() + height()) / 2.0 - kVisibleEdge);
    m_pan.setX(clampDouble(m_pan.x(), -maxX, maxX));
    m_pan.setY(clampDouble(m_pan.y(), -maxY, maxY));
}

void CullPreviewWidget::rebuildMipLevels()
{
    m_mips.clear();
    if (m_image.isNull()) {
        return;
    }

    // 先把旋转烘焙进基准图：之后绘制只做缩放，不再旋转
    QImage base = m_image;
    if (m_rotation % 360 != 0) {
        QTransform transform;
        transform.rotate(m_rotation);
        base = m_image.transformed(transform, Qt::SmoothTransformation);
    }
    m_mips.append(base);

    // 逐层减半到长边 64px 左右。任意显示尺寸都能找到"不小于目标尺寸"的
    // 最小一层，每层最多再缩 2 倍，避免双线性大比例下采样产生的摩尔纹。
    QSize size = base.size();
    while (size.width() > 64 && size.height() > 64) {
        size = QSize(std::max(1, size.width() / 2), std::max(1, size.height() / 2));
        m_mips.append(m_mips.last().scaled(size, Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation));
    }
}

// ===========================================================================
//  绘制
// ===========================================================================

void CullPreviewWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    // ui_color.md：预览画布空白区标准中性灰 #333333
    painter.fillRect(rect(), QColor(0x33, 0x33, 0x33));

    if (m_image.isNull()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("在下方缩略图中选择一张图片"));
        return;
    }

    if (m_mips.isEmpty()) {
        rebuildMipLevels();
    }
    if (m_mips.isEmpty()) {
        return;
    }

    paintImageLayer(painter);

    if (!m_composition) {
        return;
    }

    const QRectF frame = frameRect();
    if (frame.isEmpty()) {
        return;
    }

    // ---- 框外压暗 ----
    if (m_maskVisible && m_dimOpacity > 0) {
        const QColor mask(0, 0, 0, m_dimOpacity * 255 / 100);
        const QRectF whole(rect());
        // 四条边框区域，避免对框内重复半透明填充
        painter.fillRect(QRectF(whole.left(), whole.top(), whole.width(), frame.top() - whole.top()), mask);
        painter.fillRect(QRectF(whole.left(), frame.bottom(), whole.width(), whole.bottom() - frame.bottom()), mask);
        painter.fillRect(QRectF(whole.left(), frame.top(), frame.left() - whole.left(), frame.height()), mask);
        painter.fillRect(QRectF(frame.right(), frame.top(), whole.right() - frame.right(), frame.height()), mask);
    }

    // ---- 框线与辅助线 ----
    // 框线固定 1px：保持取景框轻盈，不随缩放变粗。
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen borderPen(m_frameColor, 1.0);
    borderPen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(frame);

    paintGuides(painter, frame);

    // ---- 比例提示（可在参数区关闭）----
    if (m_ratioLabelVisible && m_aspect > 0.0) {
        painter.setPen(QColor(255, 255, 255, 200));
        painter.drawText(QPointF(frame.left() + 6.0, frame.top() + 16.0),
                         formatAspectLabel(m_aspect));
    }
}

void CullPreviewWidget::paintImageLayer(QPainter& painter)
{
    if (m_mips.isEmpty()) {
        return;
    }

    const double scale = fitScale() * m_zoom;
    const QSizeF displayed = displayedImageSize();

    // 按设备像素挑选 mip 层：用"不小于目标尺寸"的最小一层，
    // 保证绘制时最多只再缩 2 倍，从根源上避免摩尔纹。
    const double dpr = devicePixelRatioF();
    const QSizeF target(displayed.width() * dpr, displayed.height() * dpr);
    int level = 0;
    while (level + 1 < m_mips.size()) {
        const QSize next = m_mips.at(level + 1).size();
        if (next.width() >= target.width() && next.height() >= target.height()) {
            ++level;
        } else {
            break;
        }
    }
    const QImage& source = m_mips.at(level);
    // 该层相对原图缩小了若干倍，绘制时把缩放乘回去，屏幕尺寸保持不变
    const double levelScale =
        scale * (static_cast<double>(m_mips.first().width())
                 / std::max(1, source.width()));

    // 放大到源像素 2 倍以上时改用最近邻：看焦点、看像素细节更清楚，
    // 不会被双线性插值糊成一团
    const bool crispPixels = (scale * dpr) >= 2.0;

    if (source.hasAlphaChannel()) {
        paintNeutralCheckerboard(
            painter,
            QRectF(rect().center()
                       - QPointF(displayed.width() / 2.0, displayed.height() / 2.0),
                   displayed));
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, !crispPixels);
    painter.save();
    painter.translate(QPointF(rect().center()) + m_pan);
    painter.scale(levelScale, levelScale);
    painter.drawImage(QPointF(-source.width() / 2.0, -source.height() / 2.0), source);
    painter.restore();
}

QImage CullPreviewWidget::compositionSample()
{
    if (m_image.isNull()) {
        return QImage();
    }
    if (m_mips.isEmpty()) {
        rebuildMipLevels();
    }
    if (m_mips.isEmpty()) {
        return QImage();
    }

    // 构图开启时只取取景框内的画面；关闭时退回整个画布可视区域。
    // 注意：这里直接渲染底层图像，不走控件抓屏，避免把浮动面板、
    // 框线、压暗与辅助线一起带进导出结果。
    QRectF region = m_composition ? frameRect() : QRectF(rect());
    if (region.isEmpty()) {
        region = QRectF(rect());
    }
    if (region.isEmpty()) {
        return QImage();
    }

    // 按设备像素渲染，避免高分屏下导出图比屏幕看到的更模糊
    const double dpr = devicePixelRatioF();
    const QSize pixelSize(std::max(1, qRound(region.width() * dpr)),
                          std::max(1, qRound(region.height() * dpr)));
    QImage sample(pixelSize, QImage::Format_RGB32);
    sample.fill(QColor(0x33, 0x33, 0x33));

    QPainter painter(&sample);
    painter.scale(dpr, dpr);
    painter.translate(-region.topLeft());
    paintImageLayer(painter);
    painter.end();

    return sample;
}

void CullPreviewWidget::paintGuides(QPainter& painter, const QRectF& frame)
{
    if (!m_guideThirds) {
        return;
    }

    QColor guideColor = m_frameColor;
    guideColor.setAlpha(170);
    QPen guidePen(guideColor, 1.0);
    painter.setPen(guidePen);

    const double x1 = frame.left() + frame.width() / 3.0;
    const double x2 = frame.left() + frame.width() * 2.0 / 3.0;
    const double y1 = frame.top() + frame.height() / 3.0;
    const double y2 = frame.top() + frame.height() * 2.0 / 3.0;
    painter.drawLine(QPointF(x1, frame.top()), QPointF(x1, frame.bottom()));
    painter.drawLine(QPointF(x2, frame.top()), QPointF(x2, frame.bottom()));
    painter.drawLine(QPointF(frame.left(), y1), QPointF(frame.right(), y1));
    painter.drawLine(QPointF(frame.left(), y2), QPointF(frame.right(), y2));
}

// ===========================================================================
//  交互
// ===========================================================================

void CullPreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !m_image.isNull()) {
        m_dragging = true;
        m_dragStart = event->position();
        m_dragStartPan = m_pan;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CullPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        m_pan = m_dragStartPan + (event->position() - m_dragStart);
        clampPan();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void CullPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CullPreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CullPreviewWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // "适合窗口"的比例随控件尺寸变化：窗口放大后预览图也可能被拉大，
    // 需要重新判断是否该加载 1:1 细节。zoomChanged 的消费方是幂等的。
    if (!m_image.isNull()) {
        // 画布尺寸变化会改变取景框大小：构图模式下不能让图像缩得比框还小，
        // 同时需要重新约束平移范围
        if (m_composition) {
            const double minZoom = minZoomCoveringFrame();
            if (m_zoom < minZoom) {
                m_zoom = minZoom;
            }
        }
        clampPan();
        emit zoomChanged(m_zoom);
    }
}

void CullPreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_image.isNull()) {
        event->ignore();
        return;
    }

    // 滚轮统一用于缩放（构图模式与否都一样），围绕光标位置缩放。
    // 切换图片改用方向键（← → ↑ ↓），见 CullWindow::handleKeyPress。
    // 构图模式下不能把图像缩到比取景框还小，否则框内会露出画布底色
    const double minZoom = m_composition ? minZoomCoveringFrame() : kMinZoom;
    const double factor = std::pow(1.0015, static_cast<double>(event->angleDelta().y()));
    const double newZoom = clampDouble(m_zoom * factor, minZoom, kMaxZoom);
    if (std::abs(newZoom - m_zoom) < 1e-6) {
        event->accept();
        return;
    }

    // 保持光标下的图像内容不动
    const QPointF cursor = event->position();
    const QPointF center = QPointF(rect().center());
    const double ratio = newZoom / m_zoom;
    m_pan = (cursor - center) - (cursor - center - m_pan) * ratio;
    m_zoom = newZoom;
    clampPan();
    update();
    emit zoomChanged(m_zoom);
    event->accept();
}

}  // namespace rawui
