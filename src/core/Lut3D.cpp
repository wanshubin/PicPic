// ============================================================================
//  Lut3D.cpp —— .cube 3D LUT 解析与应用实现
// ============================================================================

#include "Lut3D.h"

#include "LinearMath.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace rawcore {

namespace {

/// 去掉首尾空白（.cube 行可能带缩进与制表符）
std::string trim(const std::string& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();

    while (begin < end
           && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin
           && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// 读取一行中的三个浮点数，要求恰好三个（数据行格式）
bool readTriple(const std::string& line, float out[3])
{
    std::istringstream stream(line);
    if (!(stream >> out[0] >> out[1] >> out[2])) {
        return false;
    }

    // 不允许行尾还有多余内容，便于尽早暴露格式错误
    std::string extra;
    return !(stream >> extra);
}

// ---------------------------------------------------------------------------
//  解析结果缓存
//
//  拖动 LUT 强度滑块时每次渲染都会经过 LUT 阶段，
//  若每次都读盘解析一个大文件，交互会明显卡顿。
//  缓存键为路径，并用文件修改时间判断是否需要重新解析。
// ---------------------------------------------------------------------------

struct LutCacheEntry {
    std::int64_t stamp = -1;             ///< 文件修改时间戳；-1 表示无法读取
    std::shared_ptr<const Lut3D> lut;
};

/// 缓存条目上限。超过后整体清空，避免长时间运行下无限增长。
constexpr std::size_t kMaxCacheEntries = 16;

std::mutex g_lutCacheMutex;
std::unordered_map<std::string, LutCacheEntry> g_lutCache;

/// 取文件修改时间戳；失败返回 -1
std::int64_t fileTimestamp(const std::string& filePath)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(filePath, error);
    if (error) {
        return -1;
    }
    return static_cast<std::int64_t>(time.time_since_epoch().count());
}

}  // namespace

// ===========================================================================
//  解析
// ===========================================================================

ErrorInfo Lut3D::loadFromFile(const std::string& filePath, Lut3D& out)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        return ErrorInfo::make(ErrorCode::FileNotFound,
                               "无法打开 3D LUT 文件，请检查路径与访问权限",
                               "路径: " + filePath);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    if (!file.good() && !file.eof()) {
        return ErrorInfo::make(ErrorCode::FileCorrupted,
                               "读取 3D LUT 文件时发生错误，文件可能已损坏",
                               "路径: " + filePath);
    }

    return parseCubeText(buffer.str(), out);
}

ErrorInfo Lut3D::parseCubeText(const std::string& text, Lut3D& out)
{
    // 先在临时对象上解析，成功后才写回 out，避免留下半成品结果
    Lut3D parsed;

    std::vector<float> values;
    bool hasSize = false;
    std::array<float, 3> domainMin{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> domainMax{{1.0f, 1.0f, 1.0f}};

    std::istringstream input(text);
    std::string rawLine;
    int lineNumber = 0;

    while (std::getline(input, rawLine)) {
        ++lineNumber;

        // 兼容 Windows 换行
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.pop_back();
        }

        // 去掉行内注释
        const std::size_t hash = rawLine.find('#');
        if (hash != std::string::npos) {
            rawLine.erase(hash);
        }

        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }

        std::istringstream header(line);
        std::string token;
        header >> token;

        if (token == "TITLE") {
            continue;
        }
        if (token == "LUT_3D_SIZE") {
            int size = 0;
            if (!(header >> size)) {
                return ErrorInfo::make(ErrorCode::FileCorrupted,
                                       "3D LUT 的 LUT_3D_SIZE 行格式错误",
                                       "第 " + std::to_string(lineNumber) + " 行: " + line);
            }
            if (size < 2 || size > 256) {
                return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                                       "3D LUT 尺寸超出支持范围（2~256）",
                                       "LUT_3D_SIZE = " + std::to_string(size));
            }
            if (hasSize) {
                return ErrorInfo::make(ErrorCode::FileCorrupted,
                                       "3D LUT 重复定义了 LUT_3D_SIZE",
                                       "第 " + std::to_string(lineNumber) + " 行");
            }
            parsed.m_size = size;
            hasSize = true;
            continue;
        }
        if (token == "LUT_1D_SIZE" || token == "LUT_1D_INPUT_RANGE") {
            // 本软件当前只支持 3D LUT；1D LUT 请改用曲线面板实现
            return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                                   "暂不支持一维（1D）LUT，请使用 3D LUT 文件",
                                   "第 " + std::to_string(lineNumber) + " 行: " + line);
        }
        if (token == "DOMAIN_MIN") {
            float triple[3] = {0.0f, 0.0f, 0.0f};
            if (!(header >> triple[0] >> triple[1] >> triple[2])) {
                return ErrorInfo::make(ErrorCode::FileCorrupted,
                                       "3D LUT 的 DOMAIN_MIN 行格式错误",
                                       "第 " + std::to_string(lineNumber) + " 行: " + line);
            }
            domainMin = {{triple[0], triple[1], triple[2]}};
            continue;
        }
        if (token == "DOMAIN_MAX") {
            float triple[3] = {0.0f, 0.0f, 0.0f};
            if (!(header >> triple[0] >> triple[1] >> triple[2])) {
                return ErrorInfo::make(ErrorCode::FileCorrupted,
                                       "3D LUT 的 DOMAIN_MAX 行格式错误",
                                       "第 " + std::to_string(lineNumber) + " 行: " + line);
            }
            domainMax = {{triple[0], triple[1], triple[2]}};
            continue;
        }

        // 其余行按数据行处理：必须恰好三个数值
        if (!hasSize) {
            return ErrorInfo::make(ErrorCode::FileCorrupted,
                                   "3D LUT 数据出现在 LUT_3D_SIZE 之前，格式不正确",
                                   "第 " + std::to_string(lineNumber) + " 行: " + line);
        }

        float triple[3] = {0.0f, 0.0f, 0.0f};
        if (!readTriple(line, triple)) {
            return ErrorInfo::make(ErrorCode::FileCorrupted,
                                   "3D LUT 数据行格式错误，应为三个数值",
                                   "第 " + std::to_string(lineNumber) + " 行: " + line);
        }

        for (float value : triple) {
            if (!std::isfinite(value)) {
                return ErrorInfo::make(ErrorCode::FileCorrupted,
                                       "3D LUT 数据包含无效数值（NaN 或无穷大）",
                                       "第 " + std::to_string(lineNumber) + " 行");
            }
            values.push_back(value);
        }
    }

    if (!hasSize) {
        return ErrorInfo::make(ErrorCode::UnsupportedFormat,
                               "文件中没有 LUT_3D_SIZE，不是有效的 3D LUT",
                               "未找到 LUT_3D_SIZE 声明");
    }

    // DOMAIN 必须单调递增，否则归一化会出现除零或反向映射
    for (int c = 0; c < 3; ++c) {
        const std::size_t index = static_cast<std::size_t>(c);
        if (!(domainMax[index] > domainMin[index])) {
            return ErrorInfo::make(ErrorCode::FileCorrupted,
                                   "3D LUT 的 DOMAIN_MAX 必须大于 DOMAIN_MIN",
                                   "通道 " + std::to_string(c));
        }
    }

    if (values.size() != parsed.expectedSize()) {
        return ErrorInfo::make(ErrorCode::FileCorrupted,
                               "3D LUT 数据量不足或超出声明的尺寸",
                               "期望 " + std::to_string(parsed.expectedSize())
                               + " 个数值，实际 " + std::to_string(values.size()));
    }

    parsed.m_domainMin = domainMin;
    parsed.m_domainMax = domainMax;
    parsed.m_data = std::move(values);

    out = std::move(parsed);
    return ErrorInfo::ok();
}

std::shared_ptr<const Lut3D> Lut3D::loadCached(const std::string& filePath,
                                               ErrorInfo& error)
{
    error = ErrorInfo::ok();

    if (filePath.empty()) {
        error = ErrorInfo::make(ErrorCode::FileNotFound,
                                "已启用 3D LUT，但尚未选择 .cube 文件",
                                "LutSettings::path 为空");
        return nullptr;
    }

    const std::int64_t stamp = fileTimestamp(filePath);

    // 缓存命中：路径相同且文件未修改
    {
        std::lock_guard<std::mutex> lock(g_lutCacheMutex);
        const auto it = g_lutCache.find(filePath);
        if (it != g_lutCache.end() && it->second.lut && it->second.stamp == stamp) {
            return it->second.lut;
        }
    }

    auto lut = std::make_shared<Lut3D>();
    error = loadFromFile(filePath, *lut);
    if (!error.isOk()) {
        return nullptr;
    }

    // 仅在能读取修改时间时缓存，避免文件内容变化却一直命中旧结果
    if (stamp >= 0) {
        std::lock_guard<std::mutex> lock(g_lutCacheMutex);
        if (g_lutCache.size() >= kMaxCacheEntries) {
            g_lutCache.clear();
        }
        g_lutCache[filePath] = LutCacheEntry{stamp, lut};
    }

    return lut;
}

// ===========================================================================
//  应用
// ===========================================================================

float Lut3D::sample(int r, int g, int b, int channel) const
{
    const std::size_t index =
        (static_cast<std::size_t>(r) * static_cast<std::size_t>(m_size)
         + static_cast<std::size_t>(g)) * static_cast<std::size_t>(m_size)
        + static_cast<std::size_t>(b);
    return m_data[index * 3u + static_cast<std::size_t>(channel)];
}

void Lut3D::apply(ImageBuffer& image, float opacity) const
{
    if (image.isEmpty() || !isValid()) {
        return;
    }

    const float amount = math::clamp(opacity, 0.0f, 1.0f);
    if (amount <= 1e-4f) {
        return;
    }

    const int last = m_size - 1;
    const float lastIndex = static_cast<float>(last);

    // 每个通道的归一化参数：输入值 → 0~1 格点空间
    float invRange[3] = {1.0f, 1.0f, 1.0f};
    for (int c = 0; c < 3; ++c) {
        const std::size_t index = static_cast<std::size_t>(c);
        invRange[c] = 1.0f / (m_domainMax[index] - m_domainMin[index]);
    }

    float* data = image.data();
    const int pixels = static_cast<int>(image.pixelCount());

#if defined(RAWCore_HAS_OPENMP) && RAWCore_HAS_OPENMP
#  pragma omp parallel for schedule(static)
#endif
    for (int pixel = 0; pixel < pixels; ++pixel) {
        float* rgb = data + static_cast<std::size_t>(pixel) * 3u;

        // 先定位 8 个邻近格点与插值权重
        int low[3] = {0, 0, 0};
        int high[3] = {0, 0, 0};
        float frac[3] = {0.0f, 0.0f, 0.0f};

        for (int c = 0; c < 3; ++c) {
            const std::size_t index = static_cast<std::size_t>(c);
            // 线性值超出 LUT 定义域时夹取到边界（外推在高光处极易失控）
            const float normalized = math::clamp(
                (rgb[c] - m_domainMin[index]) * invRange[c], 0.0f, 1.0f);
            const float position = normalized * lastIndex;

            int lowIndex = static_cast<int>(position);
            if (lowIndex >= last) {
                lowIndex = last - 1;
            }
            low[c] = lowIndex;
            high[c] = lowIndex + 1;
            frac[c] = position - static_cast<float>(lowIndex);
        }

        const float fR = frac[0];
        const float fG = frac[1];
        const float fB = frac[2];

        for (int channel = 0; channel < 3; ++channel) {
            const float c000 = sample(low[0], low[1], low[2], channel);
            const float c001 = sample(low[0], low[1], high[2], channel);
            const float c010 = sample(low[0], high[1], low[2], channel);
            const float c011 = sample(low[0], high[1], high[2], channel);
            const float c100 = sample(high[0], low[1], low[2], channel);
            const float c101 = sample(high[0], low[1], high[2], channel);
            const float c110 = sample(high[0], high[1], low[2], channel);
            const float c111 = sample(high[0], high[1], high[2], channel);

            const float c00 = math::lerp(c000, c001, fB);
            const float c01 = math::lerp(c010, c011, fB);
            const float c10 = math::lerp(c100, c101, fB);
            const float c11 = math::lerp(c110, c111, fB);

            const float c0 = math::lerp(c00, c01, fG);
            const float c1 = math::lerp(c10, c11, fG);
            const float mapped = math::lerp(c0, c1, fR);

            // 按强度与原值混合
            rgb[channel] += (mapped - rgb[channel]) * amount;
        }
    }
}

}  // namespace rawcore
