// ============================================================================
//  FrameCalibration.cpp —— 暗场帧 / 平场帧的加载、缓存与应用
// ============================================================================

#include "FrameCalibration.h"

#include "RawLoader.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

namespace rawcore {

namespace {

// ---------------------------------------------------------------------------
//  OpenMP 并行循环宏
//
//  与 PipelineOps.cpp 中的同名宏保持一致：关闭 OpenMP 时退化为串行 for。
// ---------------------------------------------------------------------------
#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  define RAWCORE_PARALLEL_FOR(INDEX, COUNT) \
     _Pragma("omp parallel for schedule(static)") for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#else
#  define RAWCORE_PARALLEL_FOR(INDEX, COUNT) for (int INDEX = 0; INDEX < (COUNT); ++INDEX)
#endif

/// 平场帧逐像素增益的夹取范围。
/// 下限防止把暗角极端处放成噪声，上限防止把帧上的尘埃（接近 0）放大成亮斑。
constexpr float kMinFlatGain = 0.25f;
constexpr float kMaxFlatGain = 4.0f;

/// 帧像素值的下限，避免除零
constexpr float kMinFrameValue = 1e-4f;

/// 缓存最多保留的帧数（一张 24MP 拜耳帧约 96 MB，不宜多留）
constexpr std::size_t kMaxCachedFrames = 4;

struct CachedFrame {
    std::string path;
    std::filesystem::file_time_type modified;
    std::shared_ptr<const CalibrationFrame> frame;
};

std::mutex& cacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<CachedFrame>& frameCache()
{
    static std::vector<CachedFrame> cache;
    return cache;
}

}  // namespace

ErrorInfo buildCalibrationFrame(BayerImage bayer,
                                float whiteLevel,
                                CalibrationFrame& out)
{
    out = CalibrationFrame{};
    if (bayer.isEmpty()) {
        return ErrorInfo::make(ErrorCode::InvalidArgument, "标定帧数据为空");
    }

    // 与管线中的 applyWhiteLevelClip 保持一致：按饱和白电平归一化到 0~1，
    // 保证帧与主图处于同一数值基准。
    const float inverseWhite = (whiteLevel > 1e-6f) ? (1.0f / whiteLevel) : 1.0f;
    const std::size_t count = bayer.sampleCount();
    float* data = bayer.data.data();

    RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
    {
        const float value = data[i] * inverseWhite;
        data[i] = (value > 0.0f) ? (value < 1.0f ? value : 1.0f) : 0.0f;
    }

    // 统计全帧均值与三通道均值。通道归属由帧自身的排列决定，
    // 与主图排列无关（逐像素校正只关心空间位置）。
    double sum = 0.0;
    double channelSum[3] = {0.0, 0.0, 0.0};
    long long channelCount[3] = {0, 0, 0};
    const int width = bayer.width;
    const int height = bayer.height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float value = bayer.at(x, y);
            sum += value;
            const int channel = std::min(2, std::max(0, bayer.channelAt(x, y)));
            channelSum[channel] += value;
            ++channelCount[channel];
        }
    }

    out.mean = (count > 0) ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    for (int c = 0; c < 3; ++c) {
        out.channelMean[static_cast<std::size_t>(c)] =
            (channelCount[c] > 0)
                ? static_cast<float>(channelSum[c] / static_cast<double>(channelCount[c]))
                : 0.0f;
    }
    out.bayer = std::move(bayer);
    return ErrorInfo::ok();
}

std::shared_ptr<const CalibrationFrame> loadCalibrationFrame(
    const std::string& path,
    std::string& errorMessage)
{
    errorMessage.clear();
    if (path.empty()) {
        errorMessage = "未指定标定帧文件";
        return nullptr;
    }

    std::error_code errorCode;
    const std::filesystem::path filePath =
        std::filesystem::weakly_canonical(std::filesystem::path(path), errorCode);
    const std::string cacheKey = (errorCode ? path : filePath.string());
    const std::filesystem::file_time_type modified =
        std::filesystem::last_write_time(filePath, errorCode);
    const bool hasModified = !errorCode;

    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        for (const CachedFrame& entry : frameCache()) {
            if (entry.path == cacheKey && (!hasModified || entry.modified == modified)) {
                return entry.frame;
            }
        }
    }

    // 解码放在锁外，避免阻塞其它线程查缓存
    RawLoader loader;
    RawSettings settings;
    RawDecodeResult raw;
    const ErrorInfo loadError = loader.load(cacheKey, settings, raw);
    if (!loadError.isOk()) {
        errorMessage = loadError.message.empty() ? "标定帧解码失败" : loadError.message;
        return nullptr;
    }

    auto frame = std::make_shared<CalibrationFrame>();
    const ErrorInfo buildError =
        buildCalibrationFrame(std::move(raw.bayer), raw.whiteLevel, *frame);
    if (!buildError.isOk()) {
        errorMessage = buildError.message;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        std::vector<CachedFrame>& cache = frameCache();

        // 同一路径的旧版本先移除（文件被替换的情况）
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->path == cacheKey) {
                cache.erase(it);
                break;
            }
        }
        if (cache.size() >= kMaxCachedFrames) {
            cache.erase(cache.begin());
        }
        cache.push_back(CachedFrame{cacheKey, modified, frame});
    }

    return frame;
}

void clearCalibrationFrameCache()
{
    std::lock_guard<std::mutex> lock(cacheMutex());
    frameCache().clear();
}

void applyDarkFrameSubtraction(BayerImage& bayer,
                               const CalibrationFrame& dark,
                               float strength)
{
    if (bayer.isEmpty() || dark.isEmpty()) {
        return;
    }
    const float amount = std::min(1.0f, std::max(0.0f, strength));
    if (amount <= 1e-4f) {
        return;
    }

    const std::size_t count = bayer.sampleCount();
    float* data = bayer.data.data();

    if (dark.matches(bayer.width, bayer.height)) {
        const float* frame = dark.bayer.data.data();
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
        {
            const float corrected = data[i] - amount * frame[i];
            data[i] = (corrected > 0.0f) ? corrected : 0.0f;
        }
        return;
    }

    // 尺寸不一致：只按通道均值扣除整体暗底抬升。
    // 两个排列不同也无妨——这里只关心"某个像素属于 R/G/B 中的哪个"。
    for (int c = 0; c < 3; ++c) {
        const float delta = amount * dark.channelMean[static_cast<std::size_t>(c)];
        if (delta <= 1e-7f) {
            continue;
        }
        RAWCORE_PARALLEL_FOR(y, bayer.height)
        {
            for (int x = 0; x < bayer.width; ++x) {
                if (bayer.channelAt(x, y) != c) {
                    continue;
                }
                float& pixel = bayer.at(x, y);
                pixel = std::max(0.0f, pixel - delta);
            }
        }
    }
}

void applyFlatFrameCorrection(BayerImage& bayer,
                              const CalibrationFrame& flat,
                              float strength)
{
    if (bayer.isEmpty() || flat.isEmpty()) {
        return;
    }
    const float amount = std::min(1.0f, std::max(0.0f, strength));
    if (amount <= 1e-4f) {
        return;
    }
    const float reference = std::max(kMinFrameValue, flat.mean);

    if (flat.matches(bayer.width, bayer.height)) {
        const float* frame = flat.bayer.data.data();
        const std::size_t count = bayer.sampleCount();
        float* data = bayer.data.data();
        RAWCORE_PARALLEL_FOR(i, static_cast<int>(count))
        {
            const float frameValue = std::max(kMinFrameValue, frame[i]);
            const float rawGain = reference / frameValue;
            const float gain =
                std::min(kMaxFlatGain, std::max(kMinFlatGain, rawGain));
            data[i] *= (1.0f + amount * (gain - 1.0f));
        }
        return;
    }

    // 尺寸不一致：退化为按通道均值校正，只用于消除平场偏色（不校正渐晕）。
    for (int c = 0; c < 3; ++c) {
        const float channelReference = std::max(
            kMinFrameValue, flat.channelMean[static_cast<std::size_t>(c)]);
        if (std::abs(static_cast<double>(channelReference) - 1.0) <= 1e-9) {
            continue;
        }
        const float rawGain = reference / channelReference;
        const float gain = std::min(kMaxFlatGain, std::max(kMinFlatGain, rawGain));
        const float blend = 1.0f + amount * (gain - 1.0f);
        if (std::abs(blend - 1.0f) <= 1e-6f) {
            continue;
        }
        RAWCORE_PARALLEL_FOR(y, bayer.height)
        {
            for (int x = 0; x < bayer.width; ++x) {
                if (bayer.channelAt(x, y) != c) {
                    continue;
                }
                bayer.at(x, y) *= blend;
            }
        }
    }
}

}  // namespace rawcore
