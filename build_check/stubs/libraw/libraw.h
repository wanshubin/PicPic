// ============================================================================
//  libraw 桩头文件 —— 仅用于离线语法检查（不参与正式构建）
//
//  目的：在没有安装 libraw 的环境下验证 RawLoader.cpp 的语法正确性。
//  正式构建时由 CMake 通过 RAWCore_LIBRAW_HEADER 指向真实的 libraw 头文件。
//  本文件不进入 CMakeLists 的源文件列表。
// ============================================================================
#pragma once

#include <ctime>
#include <string>

typedef unsigned short ushort;

// 与真实 libraw 一致的缩略图数量上限
#define LIBRAW_THUMBNAIL_MAXCOUNT 8

// 内嵌预览的图像类型
enum LibRaw_image_formats {
    LIBRAW_IMAGE_JPEG = 1,
    LIBRAW_IMAGE_BITMAP = 2,
    LIBRAW_IMAGE_JPEGXL = 3,
};

enum {
    LIBRAW_SUCCESS = 0,
    LIBRAW_UNSPECIFIED_ERROR = -1,
    LIBRAW_FILE_UNSUPPORTED = -2,
    LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE = -4,
    LIBRAW_OUT_OF_ORDER_CALL = -5,
    LIBRAW_NO_THUMBNAIL = -6,
    LIBRAW_UNSUPPORTED_THUMBNAIL = -7,
    LIBRAW_INPUT_CLOSED = -8,
    LIBRAW_INSUFFICIENT_MEMORY = -9,
    LIBRAW_IO_ERROR = -10,
    LIBRAW_CANCELLED_BY_CALLBACK = -11,
    LIBRAW_BAD_CROP = -12,
    LIBRAW_TOO_BIG = -13,
    LIBRAW_MEMPOOL_OVERFLOW = -14
};

enum LibRaw_progress { LIBRAW_PROGRESS_START = 0 };

// 与真实 libraw 保持一致的黑电平数组长度
#define LIBRAW_CBLACK_SIZE 4104

struct libraw_image_sizes_t {
    ushort raw_height = 0, raw_width = 0;
    ushort height = 0, width = 0;
    ushort iheight = 0, iwidth = 0;
    ushort top_margin = 0, left_margin = 0;
    int flip = 0;                        // EXIF 方向（dcraw 位域约定）
};

struct libraw_iparams_t {
    char make[64] = {0};
    char model[64] = {0};
    char cdesc[8] = {0};
    ushort raw_height = 0;
    unsigned filters = 0;                // CFA 排列位域（0 或 <1000 表示非标准拜耳）
    unsigned colors = 3;
    unsigned dng_version = 0;
    char xtrans[6][6] = {};              // X-Trans 6×6 通道表
};

struct libraw_colordata_t {
    float rgb_cam[4][3] = {};
    float cam_xyz[4][3] = {};            // XYZ → 相机通道（DNG ColorMatrix）
    float cam_mul[4] = {0, 0, 0, 0};     // 相机记录的 as-shot 白平衡系数
    float pre_mul[4] = {0, 0, 0, 0};     // 无 cam_mul 时 libraw 推算的基准增益
    unsigned black = 0;                  // 统一黑电平
    // 分通道黑电平：0..3 为分通道偏移，4..5 为重复区域尺寸，
    // 6.. 为重复区域内逐位置黑电平，末位被 libraw 用作项数记录
    unsigned cblack[LIBRAW_CBLACK_SIZE] = {0};
    float maximum = 0.0f;                // 白电平（饱和值）
    unsigned data_maximum = 0;
    // 厂商 / DNG 记录的 as-shot 色温与白平衡系数（第一行为 as-shot）
    float WBCT_Coeffs[64][5] = {};
    // DNG UniqueCameraModel / 厂商唯一机型名（用于配置文件库匹配）
    char UniqueCameraModel[64] = {0};
};

struct libraw_imgother_t {
    float iso_speed = 0, shutter = 0, aperture = 0, focal_len = 0, focus_distance = 0;
    std::time_t timestamp = 0;
};

/// 单条内嵌缩略图信息（与真实 libraw 字段顺序保持一致，桩只用到尺寸）
struct libraw_thumbnail_item_t {
    int tformat = 0;
    ushort twidth = 0, theight = 0, tflip = 0;
    unsigned tlength = 0;
    unsigned tmisc = 0;
};

struct libraw_thumbnail_list_t {
    int thumbcount = 0;
    libraw_thumbnail_item_t thumblist[LIBRAW_THUMBNAIL_MAXCOUNT];
};

/// 解码后的内嵌预览（JPEG / 位图）
struct libraw_processed_image_t {
    int type = 0;
    ushort height = 0, width = 0, colors = 0, bits = 0;
    unsigned int data_size = 0;
    unsigned char data[1] = {0};
};

struct libraw_lensinfo_t {
    char Lens[128] = {0};
};

struct libraw_rawdata_t {
    void* raw_alloc = nullptr;
    ushort* raw_image = nullptr;   // 一维扁平缓冲：raw_width * raw_height
    void* color3_image = nullptr;  // 线性 DNG 的三通道像素
    void* color4_image = nullptr;  // 线性 DNG 的四通道像素
    struct { ushort* raw_colors_visible = nullptr; } color;
};

struct libraw_output_params_t {
    int no_auto_bright = 0, use_camera_wb = 0, use_auto_wb = 0;
    int output_bps = 0, output_color = 0;
    double gamm[6] = {0};
    int no_auto_scale = 0, user_qual = 0, user_sat = 0;
    int cropbox[4] = {0, 0, 0, 0};
    int threshold = 0;
};

struct libraw_data_t {
    libraw_image_sizes_t sizes;
    libraw_iparams_t idata;
    libraw_colordata_t color;
    libraw_imgother_t other;
    libraw_lensinfo_t lens;
    libraw_thumbnail_list_t thumbs_list;
    libraw_rawdata_t rawdata;
    libraw_output_params_t params;
    struct { ushort raw_height = 0; } ioparams;
};

class LibRaw {
public:
    libraw_data_t imgdata;
    int open_file(const char*) { return 0; }
    // Windows 下 RawLoader 使用宽字符路径重载（见 openRawFile）
    int open_file(const wchar_t*) { return 0; }
    int unpack() { return 0; }
    int unpack_thumb() { return 0; }
    int unpack_thumb_ex(int) { return 0; }
    libraw_processed_image_t* dcraw_make_mem_thumb(int* error)
    {
        if (error != nullptr) {
            *error = 0;
        }
        return nullptr;
    }
    void dcraw_clear_mem(libraw_processed_image_t*) {}
    int COLOR(int, int) { return 0; }
    void recycle() {}
    void set_progress_handler(int (*)(void*, LibRaw_progress, int, int), void*) {}
};
