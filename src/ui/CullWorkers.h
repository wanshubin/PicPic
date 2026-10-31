// ============================================================================
//  CullWorkers.h —— 选图工作台的后台任务
//
//  两类任务分别放在自己的线程里，互不阻塞：
//   - CullThumbnailWorker：元数据探测 + 缩略图。缩略图默认读 RAW 内嵌的
//     相机 JPEG（毫秒级），没有内嵌预览或用户勾选"缩略图展示为RAW"时才
//     走软件预览管线（较慢）。整目录几百张图也不至于把界面卡住。
//   - CullPreviewWorker：当前图片的大图预览。解码 + 渲染耗时较长，
//     用"票据"机制丢弃过期请求：用户连续按方向键时，排队中的旧任务
//     会立即返回，只有最新一张真正渲染。
//
//  线程归属：对象由 CullWindow 创建后 moveToThread，所有槽都在工作线程
//  执行；结果通过队列连接回到 UI 线程。
// ============================================================================

#pragma once

#include "io/ImageMetadata.h"

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

namespace rawcore {
class PipelineRenderer;
}

Q_DECLARE_METATYPE(rawio::ImageFileInfo)

namespace rawui {

/// 缩略图与元数据工作对象（跑在独立线程）
class CullThumbnailWorker : public QObject {
    Q_OBJECT

public:
    explicit CullThumbnailWorker(QObject* parent = nullptr);
    ~CullThumbnailWorker() override;

public slots:
    /// 是否所有缩略图都走软件渲染管线（色彩与主界面一致，但慢很多）
    void setUsePipelineThumbnails(bool enabled);

    /// 软件管线是否做镜头畸变校正（与主预览的开关保持一致）。
    /// 内嵌 JPEG 缩略图走相机自己的失真控制，只有管线缩略图受影响。
    void setLensCorrection(bool enabled);

    /// 探测单个文件的元数据（EXIF / 尺寸 / 大小）
    void requestMetadata(const QString& path);

    /// 生成缩略图。会先尝试内嵌 JPEG，失败且允许时回退到预览管线。
    void requestThumbnail(const QString& path, int maxSize);

signals:
    void metadataReady(const QString& path, const rawio::ImageFileInfo& info);
    void thumbnailReady(const QString& path, const QImage& image, bool fromPipeline);
    void thumbnailFailed(const QString& path, const QString& reason);

private:
    /// 走软件预览管线生成小图
    bool renderPipelineThumbnail(const QString& path, int maxSize,
                                 QImage& image, QString& error);

    bool m_usePipeline = false;
    /// 软件管线是否做镜头畸变校正（默认开启）
    bool m_lensCorrection = true;
    /// 只在工作线程里使用，重复创建代价高（lensfun 数据库）
    std::unique_ptr<rawcore::PipelineRenderer> m_renderer;
};

/// 大图预览工作对象（跑在独立线程）
class CullPreviewWorker : public QObject {
    Q_OBJECT

public:
    explicit CullPreviewWorker(QObject* parent = nullptr);
    ~CullPreviewWorker() override;

    /// 记录最新请求的票据。主线程**直接调用**（内部为原子量），
    /// 排队中的旧任务执行前会发现票据过期并立即返回。
    void setLatestTicket(quint64 ticket);

    /// 是否优先显示相机内嵌 JPEG。主线程**直接调用**（内部为原子量），
    /// 排队中的请求在解码前会读到最新值。
    void setUseEmbeddedJpeg(bool enabled);

public slots:
    /// 解码并渲染一张预览图
    void requestPreview(const QString& path, int maxDimension, quint64 ticket);

    /// 预览是否做镜头畸变校正（由界面开关决定）
    void setLensCorrection(bool enabled);

signals:
    void previewReady(const QString& path, const QImage& image, quint64 ticket);
    void previewFailed(const QString& path, const QString& message, quint64 ticket);

private:
    std::atomic<quint64> m_latestTicket{0};
    /// 渲染时读取；由队列连接的槽在渲染线程内写入
    std::atomic<bool> m_lensCorrection{true};
    /// 为 true 时 RAW 预览直接解码相机内嵌 JPEG（主线程写入）。
    /// 与界面开关的默认状态保持一致：默认开启。
    std::atomic<bool> m_useEmbeddedJpeg{true};
    std::unique_ptr<rawcore::PipelineRenderer> m_renderer;
};

}  // namespace rawui
