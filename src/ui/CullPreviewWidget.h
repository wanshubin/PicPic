// ============================================================================
//  CullPreviewWidget.h —— 选图工作台的预览与构图辅助层
//
//  本控件只负责"看"，不参与任何渲染；图像由调用方以 QImage 传入。
//   - 支持拖动平移、滚轮缩放、仅预览旋转；切换图片由窗口的方向键负责；
//   - 「固定视野构图」开启后，在画布中央绘制固定比例的高亮框：
//     框的位置与大小只随控件尺寸变化，拖动 / 缩放只移动底层图像
//     （需求 §2.3）。框外画面按设定透明度压暗，框内可叠加构图线。
//   - 构图是纯视觉叠加层，不修改图像、不生成裁切文件（需求 §5.2）。
// ============================================================================

#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>

class QPainter;

namespace rawui {

class CullPreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit CullPreviewWidget(QWidget* parent = nullptr);

    // ---- 图像 ----

    /// 设置显示图像。新图像会重置缩放 / 平移，但保留当前旋转角度。
    void setImage(const QImage& image);
    void clearImage();
    bool hasImage() const { return !m_image.isNull(); }
    QSize imageSize() const { return m_image.size(); }

    // ---- 视图 ----

    /// 仅预览旋转，角度总是 0/90/180/270（需求 §3 MVP 3）
    void setRotation(int degrees);
    void rotateBy(int degrees);
    int rotation() const { return m_rotation; }

    /// 恢复"适合窗口"的缩放与位置
    void resetView();
    double zoom() const { return m_zoom; }

    /// 当前每个图像像素占用多少个设备像素。
    /// 大于 1 表示画布已经在放大插值，预览图的像素不够用了，
    /// 调用方据此决定是否请求原图分辨率的 1:1 细节。
    double screenPixelsPerImagePixel() const;

    /// 用同一张图的另一份像素替换当前图像，保留缩放、平移与旋转。
    /// 1:1 细节到达、切换内嵌 JPEG / 软件管线预览时使用：用户正在查看的
    /// 位置不能跳回"适合窗口"。新图尺寸或宽高比略有差异时，平移量按显示
    /// 尺寸等比换算，不触发复位。
    void setImagePreservingView(const QImage& image);

    // ---- 构图框 ----

    void setCompositionEnabled(bool enabled);
    bool isCompositionEnabled() const { return m_composition; }

    /// 构图框宽高比（宽 / 高）
    void setFrameAspect(double aspect);
    double frameAspect() const { return m_aspect; }

    void setFrameColor(const QColor& color);
    QColor frameColor() const { return m_frameColor; }

    /// 框内左上角的比例文字是否显示（默认显示）
    void setRatioLabelVisible(bool visible);
    bool isRatioLabelVisible() const { return m_ratioLabelVisible; }

    /// 框外压暗透明度（40~100%，数值越大框外越黑）
    void setDimOpacity(int percent);
    int dimOpacity() const { return m_dimOpacity; }

    /// 一键开关暗化遮罩（框与辅助线保留）
    void setMaskVisible(bool visible);
    bool isMaskVisible() const { return m_maskVisible; }

    /// 是否显示三分法构图线
    void setGuides(bool thirds);

    /// 生成构图视野：只渲染画面本身，不含面板、框线、压暗与辅助线。
    /// 构图开启时取当前取景框内的区域，否则取整个画布可视区域。
    QImage compositionSample();

signals:

    /// 视图比例发生变化（滚轮缩放或复位）。1:1 细节按需加载用。
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    /// 旋转后的图像尺寸（90°/270° 交换宽高）
    QSizeF rotatedImageSize() const;
    /// 适合窗口的缩放比例
    double fitScale() const;
    /// 固定居中的构图框（控件坐标，大小与图片无关）
    QRectF frameRect() const;

    /// 构图模式下让图像刚好铺满取景框所需的最小缩放（相对适合窗口）。
    /// 取景框或图像为空时返回 kMinZoom。
    double minZoomCoveringFrame() const;

    /// 限制平移范围，保证图像不会完全被拖出画布
    void clampPan();
    /// 当前图像在控件中的显示尺寸
    QSizeF displayedImageSize() const;

    /// 构建显示方向的多级缓存（mipmap 链）：下采样时取最接近的一层，
    /// 避免大比例缩小时双线性只采 4 个点造成的摩尔纹。
    /// 换图或旋转时重建。
    void rebuildMipLevels();

    /// 按当前缩放 / 平移绘制底层图像（控件坐标，叠加层由调用方另画）
    void paintImageLayer(QPainter& painter);

    void paintGuides(QPainter& painter, const QRectF& frame);

    QImage m_image;
    /// m_mips[0] 为旋转后的原图，之后逐层减半
    QVector<QImage> m_mips;
    int m_rotation = 0;
    double m_zoom = 1.0;
    QPointF m_pan;

    bool m_composition = false;
    double m_aspect = 3.0 / 2.0;
    QColor m_frameColor{0, 0, 0};
    int m_dimOpacity = 75;
    bool m_maskVisible = true;
    bool m_ratioLabelVisible = true;

    bool m_guideThirds = false;

    bool m_dragging = false;
    QPointF m_dragStart;
    QPointF m_dragStartPan;
};

}  // namespace rawui
