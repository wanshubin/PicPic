// ============================================================================
//  DcpProfile.cpp —— DCP（TIFF）解析实现
//
//  只实现读取所需的 TIFF 子集：IFD0 + 常见数值类型。
//  DCP 的标签数据都是非压缩的，因此不需要引入 libtiff。
// ============================================================================

#include "DcpProfile.h"

#include "LinearMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace rawcore {

namespace {

// ---------------------------------------------------------------------------
//  TIFF 基础结构
// ---------------------------------------------------------------------------

enum TiffType : std::uint16_t {
    kTypeByte = 1,
    kTypeAscii = 2,
    kTypeShort = 3,
    kTypeLong = 4,
    kTypeRational = 5,
    kTypeSByte = 6,
    kTypeUndefined = 7,
    kTypeSShort = 8,
    kTypeSLong = 9,
    kTypeSRational = 10,
    kTypeFloat = 11,
    kTypeDouble = 12,
};

std::size_t tiffTypeSize(std::uint16_t type)
{
    switch (type) {
    case kTypeByte:
    case kTypeAscii:
    case kTypeSByte:
    case kTypeUndefined:
        return 1;
    case kTypeShort:
    case kTypeSShort:
        return 2;
    case kTypeLong:
    case kTypeSLong:
    case kTypeFloat:
        return 4;
    case kTypeRational:
    case kTypeSRational:
    case kTypeDouble:
        return 8;
    default:
        return 0;
    }
}

/// 一个 IFD 目录项
struct TiffEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t dataOffset = 0;  ///< 数据在文件中的绝对偏移
    std::size_t byteSize = 0;
};

/// 带边界检查的 TIFF 读取器。
class TiffReader {
public:
    TiffReader(const std::vector<std::uint8_t>& bytes, bool littleEndian)
        : m_bytes(bytes)
        , m_little(littleEndian)
    {
    }

    bool isLittleEndian() const { return m_little; }

    bool inRange(std::size_t offset, std::size_t length) const
    {
        return offset <= m_bytes.size() && length <= m_bytes.size() - offset;
    }

    std::uint16_t u16(std::size_t offset) const
    {
        const std::uint8_t* p = m_bytes.data() + offset;
        if (m_little) {
            return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
        }
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
    }

    std::uint32_t u32(std::size_t offset) const
    {
        const std::uint8_t* p = m_bytes.data() + offset;
        if (m_little) {
            return static_cast<std::uint32_t>(p[0])
                | (static_cast<std::uint32_t>(p[1]) << 8)
                | (static_cast<std::uint32_t>(p[2]) << 16)
                | (static_cast<std::uint32_t>(p[3]) << 24);
        }
        return (static_cast<std::uint32_t>(p[0]) << 24)
            | (static_cast<std::uint32_t>(p[1]) << 16)
            | (static_cast<std::uint32_t>(p[2]) << 8)
            | static_cast<std::uint32_t>(p[3]);
    }

    std::int32_t i32(std::size_t offset) const
    {
        return static_cast<std::int32_t>(u32(offset));
    }

    float f32(std::size_t offset) const
    {
        const std::uint32_t raw = u32(offset);
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    double f64(std::size_t offset) const
    {
        std::uint64_t raw = 0;
        if (m_little) {
            for (int i = 7; i >= 0; --i) {
                raw = (raw << 8) | m_bytes[offset + static_cast<std::size_t>(i)];
            }
        } else {
            for (int i = 0; i < 8; ++i) {
                raw = (raw << 8) | m_bytes[offset + static_cast<std::size_t>(i)];
            }
        }
        double value = 0.0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    /// 读取 IFD0 的全部目录项。任何越界都视为文件损坏。
    bool readIfd0(std::vector<TiffEntry>& entries) const
    {
        if (m_bytes.size() < 8) {
            return false;
        }
        if (!(m_bytes[0] == 'I' && m_bytes[1] == 'I')
            && !(m_bytes[0] == 'M' && m_bytes[1] == 'M')) {
            return false;
        }
        const std::uint32_t ifdOffset = u32(4);
        if (!inRange(ifdOffset, 2)) {
            return false;
        }
        const std::uint16_t count = u16(ifdOffset);
        const std::size_t entriesStart = static_cast<std::size_t>(ifdOffset) + 2;
        if (!inRange(entriesStart, static_cast<std::size_t>(count) * 12)) {
            return false;
        }

        entries.clear();
        entries.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t offset = entriesStart + static_cast<std::size_t>(i) * 12;
            TiffEntry entry;
            entry.tag = u16(offset);
            entry.type = u16(offset + 2);
            entry.count = u32(offset + 4);
            const std::size_t unit = tiffTypeSize(entry.type);
            if (unit == 0) {
                continue;  // 未知类型直接跳过，不影响其余标签
            }
            entry.byteSize = unit * static_cast<std::size_t>(entry.count);
            entry.dataOffset = (entry.byteSize <= 4) ? offset + 8 : u32(offset + 8);
            if (!inRange(entry.dataOffset, entry.byteSize)) {
                continue;  // 越界标签跳过
            }
            entries.push_back(entry);
        }
        return true;
    }

    /// 读取数值型标签为 double 数组
    bool readNumbers(const TiffEntry& entry, std::vector<double>& output) const
    {
        output.clear();
        output.reserve(entry.count);
        const std::size_t unit = tiffTypeSize(entry.type);
        for (std::uint32_t i = 0; i < entry.count; ++i) {
            const std::size_t offset = entry.dataOffset + static_cast<std::size_t>(i) * unit;
            switch (entry.type) {
            case kTypeByte:
                output.push_back(m_bytes[offset]);
                break;
            case kTypeSByte:
                output.push_back(static_cast<std::int8_t>(m_bytes[offset]));
                break;
            case kTypeShort:
                output.push_back(u16(offset));
                break;
            case kTypeSShort:
                output.push_back(static_cast<std::int16_t>(u16(offset)));
                break;
            case kTypeLong:
                output.push_back(u32(offset));
                break;
            case kTypeSLong:
                output.push_back(i32(offset));
                break;
            case kTypeRational:
            case kTypeSRational: {
                const double numerator = (entry.type == kTypeRational)
                    ? static_cast<double>(u32(offset))
                    : static_cast<double>(i32(offset));
                const double denominator = (entry.type == kTypeRational)
                    ? static_cast<double>(u32(offset + 4))
                    : static_cast<double>(i32(offset + 4));
                output.push_back(denominator != 0.0 ? numerator / denominator : 0.0);
                break;
            }
            case kTypeFloat:
                output.push_back(f32(offset));
                break;
            case kTypeDouble:
                output.push_back(f64(offset));
                break;
            default:
                return false;
            }
        }
        return true;
    }

    std::string readAscii(const TiffEntry& entry) const
    {
        const char* begin = reinterpret_cast<const char*>(m_bytes.data() + entry.dataOffset);
        std::size_t length = entry.byteSize;
        // 截断到第一个 NUL，避免带出填充字节
        for (std::size_t i = 0; i < entry.byteSize; ++i) {
            if (begin[i] == '\0') {
                length = i;
                break;
            }
        }
        return std::string(begin, length);
    }

private:
    const std::vector<std::uint8_t>& m_bytes;
    bool m_little = true;
};

/// 从标签表里取第一个指定 tag
const TiffEntry* findEntry(const std::unordered_map<std::uint16_t, TiffEntry>& entries,
                           std::uint16_t tag)
{
    auto it = entries.find(tag);
    return it == entries.end() ? nullptr : &it->second;
}

/// 读取 3x3 矩阵标签（SRATIONAL 或 FLOAT 都接受）
bool readMatrixTag(const TiffReader& reader,
                   const std::unordered_map<std::uint16_t, TiffEntry>& entries,
                   std::uint16_t tag,
                   Matrix3x3& output)
{
    const TiffEntry* entry = findEntry(entries, tag);
    if (entry == nullptr) {
        return false;
    }
    std::vector<double> values;
    if (!reader.readNumbers(*entry, values) || values.size() < 9) {
        return false;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            const double value = values[row * 3 + col];
            if (!std::isfinite(value)) {
                return false;
            }
            output[row][col] = static_cast<float>(value);
        }
    }
    return true;
}

/// 读取单值整型标签
bool readIntTag(const TiffReader& reader,
                const std::unordered_map<std::uint16_t, TiffEntry>& entries,
                std::uint16_t tag,
                int& output)
{
    const TiffEntry* entry = findEntry(entries, tag);
    if (entry == nullptr) {
        return false;
    }
    std::vector<double> values;
    if (!reader.readNumbers(*entry, values) || values.empty()) {
        return false;
    }
    output = static_cast<int>(values.front());
    return true;
}

/// 读取 Hue/Sat 表
bool readHueSatMap(const TiffReader& reader,
                   const std::unordered_map<std::uint16_t, TiffEntry>& entries,
                   std::uint16_t dimsTag,
                   std::uint16_t dataTag,
                   std::uint16_t encodingTag,
                   DcpHueSatMap& output)
{
    const TiffEntry* dimsEntry = findEntry(entries, dimsTag);
    const TiffEntry* dataEntry = findEntry(entries, dataTag);
    if (dimsEntry == nullptr || dataEntry == nullptr) {
        return false;
    }

    std::vector<double> dims;
    if (!reader.readNumbers(*dimsEntry, dims) || dims.size() < 2) {
        return false;
    }
    const int hue = static_cast<int>(dims[0]);
    const int sat = static_cast<int>(dims[1]);
    const int val = (dims.size() >= 3) ? static_cast<int>(dims[2]) : 1;
    if (hue <= 0 || sat <= 0 || val <= 0) {
        return false;
    }

    std::vector<double> values;
    if (!reader.readNumbers(*dataEntry, values)) {
        return false;
    }
    const std::size_t needed = static_cast<std::size_t>(hue)
        * static_cast<std::size_t>(sat) * static_cast<std::size_t>(val) * 3u;
    if (values.size() < needed) {
        return false;
    }

    output.hueDivisions = hue;
    output.satDivisions = sat;
    output.valDivisions = val;
    output.data.resize(needed);
    for (std::size_t i = 0; i < needed; ++i) {
        output.data[i] = static_cast<float>(values[i]);
    }

    // DNG 规范规定每个节点的第一个值（色相偏移）以**度**为单位，
    // 值域约为 -180..+180；Hue/Sat 表与 Look 表都遵守该规则。
    // 渲染层在归一化色相 [0,1) 上做插值，因此解析时统一除以 360。
    // 若漏掉这一步，几度到上百度的偏移会被当成"几圈色相"，
    // 画面会出现大面积错色与彩色噪点。
    for (std::size_t i = 0; i + 2 < needed; i += 3) {
        output.data[i] /= 360.0f;
    }

    // 明度轴的索引编码：0 = 线性，1 = sRGB（暗部索引精度更高）
    int encoding = 0;
    if (readIntTag(reader, entries, encodingTag, encoding)) {
        output.srgbEncoding = (encoding == 1);
    }
    return true;
}

/// 读取色调曲线
bool readToneCurve(const TiffReader& reader,
                   const std::unordered_map<std::uint16_t, TiffEntry>& entries,
                   std::uint16_t tag,
                   DcpToneCurve& output)
{
    const TiffEntry* entry = findEntry(entries, tag);
    if (entry == nullptr) {
        return false;
    }
    std::vector<double> values;
    if (!reader.readNumbers(*entry, values) || values.size() < 4) {
        return false;
    }
    output.coordinates.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        output.coordinates[i] = static_cast<float>(values[i]);
    }
    return true;
}

/// 3x3 行列式，用于发现明显无效的标定矩阵
float determinant3x3(const Matrix3x3& m)
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
        - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

}  // namespace

// ---------------------------------------------------------------------------
//  光源编码 → 标称色温
// ---------------------------------------------------------------------------

float dcpIlluminantTemperature(int code)
{
    // 对应 EXIF LightSource / DNG CalibrationIlluminant 的常用取值。
    // 未列出的编码返回 0，由调用方决定是否退回默认端点。
    switch (code) {
    case 1:  return 5500.0f;   // Daylight
    case 2:  return 4150.0f;   // Fluorescent（标称值，差异较大）
    case 3:  return 2856.0f;   // Tungsten
    case 4:  return 5500.0f;   // Flash
    case 9:  return 5500.0f;   // Fine weather
    case 10: return 6500.0f;   // Cloudy
    case 11: return 7500.0f;   // Shade
    case 12: return 4400.0f;   // Daylight fluorescent
    case 13: return 5000.0f;   // Day white fluorescent
    case 14: return 4200.0f;   // Cool white fluorescent
    case 15: return 4000.0f;   // White fluorescent
    case 17: return 2856.0f;   // Standard light A
    case 18: return 4874.0f;   // Standard light B
    case 19: return 6774.0f;   // Standard light C
    case 20: return 5503.0f;   // D55
    case 21: return 6504.0f;   // D65
    case 22: return 7504.0f;   // D75
    case 23: return 5003.0f;   // D50
    case 24: return 3200.0f;   // ISO studio tungsten
    default: return 0.0f;
    }
}

bool DcpProfile::daylightMatrix(Matrix3x3& output) const
{
    if (hasColorMatrix2) {
        output = colorMatrix2;
        return true;
    }
    if (hasColorMatrix1) {
        output = colorMatrix1;
        return true;
    }
    return false;
}

bool DcpProfile::cameraToXyzMatrix(bool second, Matrix3x3& output) const
{
    // DCP 的 ColorMatrix 是 DNG 规范定义的 “XYZ → 相机原生 RGB”，
    // 而渲染管线需要 “相机原生 RGB → XYZ”，因此必须求逆。
    // 直接把 ColorMatrix 填进 cam2xyz 会让整个画面的色彩完全错乱。
    Matrix3x3 xyzToCamera = colorMatrix1;
    bool available = hasColorMatrix1;
    if (second && hasColorMatrix2) {
        xyzToCamera = colorMatrix2;
        available = true;
    } else if (!available && hasColorMatrix2) {
        xyzToCamera = colorMatrix2;
        available = true;
    }
    if (!available) {
        return false;
    }

    const float det = determinant3x3(xyzToCamera);
    if (!std::isfinite(det) || std::fabs(det) < 1e-9f) {
        return false;
    }
    return math::invert(xyzToCamera, output);
}

// ---------------------------------------------------------------------------
//  解析入口
// ---------------------------------------------------------------------------

DcpProfile parseDcpProfileFromBytes(const std::vector<std::uint8_t>& bytes)
{
    DcpProfile profile;

    if (bytes.size() < 8) {
        profile.error = "DCP 文件过短，不是有效的 TIFF 结构。";
        return profile;
    }

    const bool little = (bytes[0] == 'I' && bytes[1] == 'I');
    if (!little && !(bytes[0] == 'M' && bytes[1] == 'M')) {
        profile.error = "DCP 文件缺少 TIFF 字节序标识。";
        return profile;
    }

    TiffReader reader(bytes, little);
    // DCP 使用 TIFF 结构，但魔数是 DNG 规范规定的 0x4352（ASCII "CR"），
    // 而不是普通 TIFF 的 42。部分工具导出的 DCP 仍写 42，两者都接受。
    const std::uint16_t magic = reader.u16(2);
    if (magic != 42 && magic != 0x4352) {
        profile.error = "DCP 文件的 TIFF 魔数不正确（应为 0x4352 或 42）。";
        return profile;
    }

    std::vector<TiffEntry> entryList;
    if (!reader.readIfd0(entryList)) {
        profile.error = "DCP 的 IFD 目录损坏或越界。";
        return profile;
    }

    // 同名标签只保留第一个（规范允许重复，但实际文件不会出现）
    std::unordered_map<std::uint16_t, TiffEntry> entries;
    for (const TiffEntry& entry : entryList) {
        entries.emplace(entry.tag, entry);
    }

    // ---- 名称 ----
    if (const TiffEntry* entry = findEntry(entries, 50936)) {  // ProfileName
        profile.profileName = reader.readAscii(*entry);
    } else if (const TiffEntry* entry = findEntry(entries, 269)) {  // DocumentName
        profile.profileName = reader.readAscii(*entry);
    }
    if (profile.profileName.empty()) {
        if (const TiffEntry* entry = findEntry(entries, 285)) {  // PageName
            profile.profileName = reader.readAscii(*entry);
        }
    }

    if (const TiffEntry* entry = findEntry(entries, 50942)) {  // ProfileCopyright
        profile.profileCopyright = reader.readAscii(*entry);
    }

    // UniqueCameraModel：部分 DCP（尤其是相机厂商自带配置）会带上，
    // 配置文件库用它做机型自动匹配；缺失时由文件名 / ProfileName 兜底。
    if (const TiffEntry* entry = findEntry(entries, 50708)) {
        profile.uniqueCameraModel = reader.readAscii(*entry);
    }

    // ---- 标定光源 ----
    readIntTag(reader, entries, 50778, profile.illuminant1Code);  // CalibrationIlluminant1
    readIntTag(reader, entries, 50779, profile.illuminant2Code);  // CalibrationIlluminant2
    profile.illuminant1Temperature = dcpIlluminantTemperature(profile.illuminant1Code);
    profile.illuminant2Temperature = dcpIlluminantTemperature(profile.illuminant2Code);

    // ---- ColorMatrix（DNG 定义：XYZ → 相机 RGB）----
    profile.hasColorMatrix1 = readMatrixTag(reader, entries, 50721, profile.colorMatrix1);
    profile.hasColorMatrix2 = readMatrixTag(reader, entries, 50722, profile.colorMatrix2);

    if (!profile.hasColorMatrix1 && !profile.hasColorMatrix2) {
        profile.error = "DCP 中缺少 ColorMatrix1 / ColorMatrix2，无法用于相机标定。";
        return profile;
    }

    // 单矩阵配置时，把另一组也视为同一矩阵，便于上层统一处理
    if (profile.hasColorMatrix1 && !profile.hasColorMatrix2) {
        profile.colorMatrix2 = profile.colorMatrix1;
        profile.illuminant2Code = profile.illuminant1Code;
        profile.illuminant2Temperature = profile.illuminant1Temperature;
    } else if (!profile.hasColorMatrix1 && profile.hasColorMatrix2) {
        profile.colorMatrix1 = profile.colorMatrix2;
        profile.illuminant1Code = profile.illuminant2Code;
        profile.illuminant1Temperature = profile.illuminant2Temperature;
    }

    // ---- ForwardMatrix（白平衡后的相机 RGB → XYZ D50，仅解析）----
    profile.hasForwardMatrix1 = readMatrixTag(reader, entries, 50964, profile.forwardMatrix1);
    profile.hasForwardMatrix2 = readMatrixTag(reader, entries, 50965, profile.forwardMatrix2);

    // ---- Hue/Sat 表 ----
    // HueSatMap1/2 共用 ProfileHueSatMapEncoding（51107），
    // LookTable 使用独立的 ProfileLookTableEncoding（51108）。
    readHueSatMap(reader, entries, 50937, 50938, 51107, profile.hueSatMap1);
    readHueSatMap(reader, entries, 50937, 50939, 51107, profile.hueSatMap2);

    // ---- 默认 Look 表 ----
    readHueSatMap(reader, entries, 50981, 50982, 51108, profile.lookTable);

    // ---- 色调曲线 ----
    readToneCurve(reader, entries, 50940, profile.toneCurve);

    // ---- 数据合理性检查 ----
    const std::vector<std::pair<const char*, float>> determinants = {
        {"ColorMatrix1", determinant3x3(profile.colorMatrix1)},
        {"ColorMatrix2", determinant3x3(profile.colorMatrix2)},
    };
    for (const auto& item : determinants) {
        if (!std::isfinite(item.second) || std::fabs(item.second) < 1e-9f) {
            profile.warnings.push_back(
                std::string(item.first) + " 接近奇异，用它做颜色转换可能得到异常结果。");
        }
    }

    if (profile.illuminant1Code == 0 && profile.illuminant2Code == 0) {
        profile.warnings.push_back("DCP 未声明标定光源，已按标准 A（2856K）与 D65（6504K）处理。");
    } else if (profile.illuminant1Temperature <= 0.0f && profile.illuminant1Code != 0) {
        profile.warnings.push_back("无法识别 CalibrationIlluminant1 编码，双光源插值将退回默认端点。");
    }

    profile.valid = true;
    return profile;
}

DcpProfile loadDcpProfile(const std::string& filePath)
{
    DcpProfile profile;

    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        profile.error = "无法打开 DCP 文件：" + filePath;
        return profile;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0 || size > 64LL * 1024 * 1024) {
        profile.error = "DCP 文件大小异常，已拒绝读取：" + filePath;
        return profile;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size))) {
        profile.error = "读取 DCP 文件失败：" + filePath;
        return profile;
    }

    profile = parseDcpProfileFromBytes(bytes);
    if (!profile.valid && !profile.error.empty()) {
        profile.error = filePath + "：" + profile.error;
    }
    return profile;
}

}  // namespace rawcore
