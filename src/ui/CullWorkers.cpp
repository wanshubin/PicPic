// ============================================================================
//  CullWorkers.cpp —— 选图工作台后台任务实现
// ============================================================================

#include "CullWorkers.h"

#include "core/EditContext.h"
#include "core/PipelineRenderer.h"
#include "core/RawLoader.h"
#include "io/StandardImageLoader.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>

namespace rawui {

namespace {

/// 把管线输出的 RGB888 显示缓冲包成 QImage。
/// RenderResult::display8 在内存里是紧密排列的，包一层后 copy() 成
/// 独立缓冲，避免图像继续引用临时结果（结果析构后指针就悬空了）。
QImage imageFromDisplay(const rawcore::RenderResult& result)
{
    if (result.display8.empty() || result.renderWidth <= 0 || result.renderHeight <= 0) {
        return QImage();
    }
    const QImage view(result.display8.data(), result.renderWidth, result.renderHeight,
                      result.renderWidth * 3, QImage::Format_RGB888);
    return view.copy();
}

/// 缩放并保持宽高比；maxSize 为长边上限
QImage scaleToFit(const QImage& image, int maxSize)
{
    if (image.isNull() || maxSize <= 0) {
        return image;
    }
    if (image.width() <= maxSize && image.height() <= maxSize) {
        return image;
    }
    return image.scaled(maxSize, maxSize, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

/// 读取 RAW 内嵌 JPEG 并解码成 QImage；没有内嵌 JPEG 或解码失败返回空图。
QImage loadEmbeddedPreviewImage(const QString& path, int maxDimension)
{
    rawcore::RawLoader loader;
    std::vector<std::uint8_t> jpegData;
    if (!loader.loadEmbeddedPreview(path.toStdString(), jpegData).isOk()
        || jpegData.empty()) {
        return QImage();
    }

    QBuffer buffer;
    buffer.setData(reinterpret_cast<const char*>(jpegData.data()),
                   static_cast<int>(jpegData.size()));
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "JPEG");
    // 内嵌 JPEG 按存储方向保存，靠 EXIF 方向标签还原竖拍
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid()
        && (size.width() > maxDimension || size.height() > maxDimension)) {
        // 让 JPEG 解码器直接按目标尺寸解码，省去完整解码后的二次缩放；
        // 内嵌图本来就比目标小时保持原尺寸，不做无意义的放大。
        reader.setScaledSize(size.scaled(maxDimension, maxDimension,
                                         Qt::KeepAspectRatio));
    }
    return reader.read();
}

/// 读取 RAW / JPG 并走软件预览管线渲染成 QImage。
///
/// 本应用只做选图，所有预览统一使用同一组默认参数：
/// as-shot 白平衡、相机原厂矩阵、观感影调。
bool renderWithPipeline(rawcore::PipelineRenderer& renderer, const QString& path,
                        int maxDimension, bool lensCorrection, QImage& image,
                        QString& error,
                        const rawcore::ProgressCallback& progress = nullptr)
{
    if (rawio::isStandardImagePath(path)) {
        // 普通位图不需要 RAW 管线，按目标尺寸解码即可。
        // 竖拍照片依赖 EXIF 方向标签，必须交给 Qt 应用旋转，否则会横躺。
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid()) {
            reader.setScaledSize(size.scaled(maxDimension, maxDimension,
                                             Qt::KeepAspectRatio));
        }
        image = reader.read();
        if (image.isNull()) {
            error = QStringLiteral("无法读取 JPG：%1").arg(reader.errorString());
            return false;
        }
        return true;
    }

    rawcore::RawLoader loader;
    rawcore::RawDecodeResult raw;
    const rawcore::ErrorInfo decodeError =
        loader.load(path.toStdString(), rawcore::RawSettings{}, raw);
    if (!decodeError.isOk()) {
        error = QString::fromStdString(decodeError.message);
        return false;
    }

    rawcore::EditContext context;
    context.resetToDefaults();
    context.sourceRawPath = path.toStdString();
    // as-shot 白平衡：基准白平衡归 RAW 标定层，创意偏移为 0（停在标定白点）。
    if (!raw.isRgbSource) {
        const bool hasAsShot = raw.metadata.asShotTemperature > 0.0f;
        context.baseWhiteBalance.enable = true;
        context.baseWhiteBalance.temperature =
            hasAsShot ? raw.metadata.asShotTemperature : 5500.0f;
        context.baseWhiteBalance.tint =
            hasAsShot ? raw.metadata.asShotTint : 0.0f;
        context.baseWhiteBalance.fromAsShot = hasAsShot;
        context.baseWhiteBalance.presentInProject = true;
    }
    context.whiteBalance = rawcore::WhiteBalanceSettings{};
    // 相机原厂矩阵作为颜色基准，不启用自定义 CCM
    context.ccm.cam2xyz = raw.cameraToXyz;
    context.ccm.enable = false;
    context.ccm.initializedFromCamera = raw.hasCameraMatrix;

    // 镜头畸变校正由界面开关决定。打开时按 EXIF 匹配镜头库：相机内嵌
    // JPEG 已按机内设置做过失真控制与相应裁切，RAW 若不校正，缩略图被
    // 管线预览替换时会出现可见的几何跳变（直线在画面边缘外鼓）。只开
    // 畸变：暗角交给预览影调自行平衡，色差由管线自己的彩边校正处理，
    // 避免重复校正，也省掉暗角 / 色差的高开销。
    // 找不到匹配镜头时渲染器会自动跳过并给出提示，不影响预览。
    if (lensCorrection) {
        context.lens.enable = true;
        context.lens.enableDistortion = true;
        context.lens.enableVignetting = false;
        context.lens.enableChromaticAberration = false;
    }

    rawcore::RenderOptions options;
    options.scale = rawcore::RenderScale::Preview;
    options.previewMaxDimension = maxDimension;
    options.previewTone = rawcore::PreviewToneMode::Look;
    options.applyLensCorrection = true;

    rawcore::RenderResult result;
    const rawcore::ErrorInfo renderError =
        renderer.render(raw, context, options, result, progress);
    if (!renderError.isOk()) {
        error = QString::fromStdString(renderError.message);
        return false;
    }
    image = imageFromDisplay(result);
    if (image.isNull()) {
        error = QStringLiteral("渲染未产生有效图像。");
        return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
//  CullThumbnailWorker
// ===========================================================================

CullThumbnailWorker::CullThumbnailWorker(QObject* parent)
    : QObject(parent)
{
}

CullThumbnailWorker::~CullThumbnailWorker() = default;

void CullThumbnailWorker::setUsePipelineThumbnails(bool enabled)
{
    m_usePipeline = enabled;
}

void CullThumbnailWorker::setLensCorrection(bool enabled)
{
    m_lensCorrection = enabled;
}

void CullThumbnailWorker::requestMetadata(const QString& path)
{
    rawio::ImageFileInfo info;
    rawio::probeImageFile(path, info);
    emit metadataReady(path, info);
}

void CullThumbnailWorker::requestThumbnail(const QString& path, int maxSize)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isRaw = rawcore::RawLoader::isSupportedExtension(path.toStdString());

    if (!m_usePipeline) {
        if (isRaw) {
            // 优先用相机内嵌的 JPEG：整目录挑片时这是唯一能接受的耗时
            rawcore::RawLoader loader;
            std::vector<std::uint8_t> jpegData;
            if (loader.loadEmbeddedPreview(path.toStdString(), jpegData).isOk()
                && !jpegData.empty()) {
                QBuffer buffer;
                buffer.setData(reinterpret_cast<const char*>(jpegData.data()),
                               static_cast<int>(jpegData.size()));
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer, "JPEG");
                // 内嵌 JPEG 按存储方向保存，靠 EXIF 方向标签还原竖拍
                reader.setAutoTransform(true);
                const QSize size = reader.size();
                if (size.isValid()) {
                    // 让 JPEG 解码器直接按缩放后的尺寸解码，省去完整解码
                    reader.setScaledSize(size.scaled(maxSize, maxSize,
                                                     Qt::KeepAspectRatio));
                }
                const QImage image = reader.read();
                if (!image.isNull()) {
                    emit thumbnailReady(path, image, false);
                    return;
                }
            }
        } else {
            QImageReader reader(path);
            // 普通 JPG 同样依赖 EXIF 方向标签
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.isValid()) {
                reader.setScaledSize(size.scaled(maxSize, maxSize,
                                                 Qt::KeepAspectRatio));
            }
            const QImage image = reader.read();
            if (!image.isNull()) {
                emit thumbnailReady(path, image, false);
                return;
            }
        }
    }

    // 用户勾选"缩略图展示为RAW"，或内嵌预览不可用：回退到软件预览管线
    QImage image;
    QString error;
    if (renderPipelineThumbnail(path, maxSize, image, error)) {
        emit thumbnailReady(path, image, true);
    } else {
        emit thumbnailFailed(path, error);
    }
}

bool CullThumbnailWorker::renderPipelineThumbnail(const QString& path, int maxSize,
                                                  QImage& image, QString& error)
{
    // 延迟创建渲染器：lensfun 数据库加载较慢，而多数缩略图用不到管线。
    // 这里已经在工作线程内，创建不会卡住界面。
    if (!m_renderer) {
        m_renderer = std::make_unique<rawcore::PipelineRenderer>();
    }
    // 多要一点像素再缩小，避免下采样倍数取整后缩略图明显发虚
    const int renderDimension = std::max(256, maxSize * 2);
    if (!renderWithPipeline(*m_renderer, path, renderDimension, m_lensCorrection,
                            image, error)) {
        return false;
    }
    image = scaleToFit(image, maxSize);
    return !image.isNull();
}

// ===========================================================================
//  CullPreviewWorker
// ===========================================================================

CullPreviewWorker::CullPreviewWorker(QObject* parent)
    : QObject(parent)
{
}

CullPreviewWorker::~CullPreviewWorker() = default;

void CullPreviewWorker::setLatestTicket(quint64 ticket)
{
    m_latestTicket.store(ticket, std::memory_order_relaxed);
}

void CullPreviewWorker::setUseEmbeddedJpeg(bool enabled)
{
    m_useEmbeddedJpeg.store(enabled, std::memory_order_relaxed);
}

void CullPreviewWorker::setLensCorrection(bool enabled)
{
    m_lensCorrection.store(enabled, std::memory_order_relaxed);
}

void CullPreviewWorker::requestPreview(const QString& path, int maxDimension,
                                       quint64 ticket)
{
    // 用户已经翻到别的图：这张不用渲染了，立即让出线程处理最新请求
    if (ticket < m_latestTicket.load(std::memory_order_relaxed)) {
        return;
    }

    // 内嵌 JPEG 模式：RAW 直接显示相机生成的 JPEG，速度快、
    // 色彩是相机机内处理后的结果，不需要跑软件管线。
    if (m_useEmbeddedJpeg.load(std::memory_order_relaxed)
        && rawcore::RawLoader::isSupportedExtension(path.toStdString())) {
        const QImage embedded = loadEmbeddedPreviewImage(path, maxDimension);
        if (!embedded.isNull()) {
            // 解码期间用户可能已经翻走，过期结果不跨线程传回
            if (ticket < m_latestTicket.load(std::memory_order_relaxed)) {
                return;
            }
            emit previewReady(path, embedded, ticket);
            return;
        }
        // 没有可用内嵌 JPEG：静默回退到软件管线，保证画面不空白
    }

    QImage image;
    QString error;
    // 延迟创建渲染器：第一次真正渲染时才加载 lensfun 数据库
    if (!m_renderer) {
        m_renderer = std::make_unique<rawcore::PipelineRenderer>();
    }

    // 渲染期间检查票据：1:1 细节要跑好几秒，用户已经翻到下一张时没必要
    // 把整幅 45MP 跑完再丢弃；在阶段边界请求取消，尽快让出线程。
    const rawcore::ProgressCallback progress =
        [this, ticket](rawcore::RenderStage, float) {
            if (ticket != m_latestTicket.load(std::memory_order_relaxed)
                && m_renderer != nullptr) {
                m_renderer->requestCancel();
            }
        };

    if (!renderWithPipeline(*m_renderer, path, maxDimension,
                            m_lensCorrection.load(std::memory_order_relaxed),
                            image, error, progress)) {
        // 因票据过期而被取消的任务不是错误，静默退出即可
        if (ticket != m_latestTicket.load(std::memory_order_relaxed)) {
            return;
        }
        emit previewFailed(path, error, ticket);
        return;
    }
    image = scaleToFit(image, maxDimension);
    // 渲染期间用户可能已经翻走了：再确认一次，避免把过期的大图跨线程传回 UI
    if (ticket != m_latestTicket.load(std::memory_order_relaxed)) {
        return;
    }
    emit previewReady(path, image, ticket);
}

}  // namespace rawui
