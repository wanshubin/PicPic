// ============================================================================
//  libraw 桩头文件 —— 仅用于离线语法检查（不参与正式构建）
//
//  目的：在没有安装 libraw 的环境下验证 RawLoader.cpp 的语法正确性。
//  正式构建时由 CMake 通过 RAWCore_LIBRAW_HEADER 指向真实的 libraw 头文件。
//  本文件不进入 CMakeLists 的源文件列表。
// ============================================================================
#pragma once

#include <string>

typedef unsigned short ushort;

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

struct libraw_image_sizes_t {
    ushort raw_height = 0, raw_width = 0;
    ushort height = 0, width = 0;
    ushort iheight = 0, iwidth = 0;
    ushort top_margin = 0, left_margin = 0;
};

struct libraw_iparams_t {
    char make[64] = {0};
    char model[64] = {0};
    char cdesc[8] = {0};
    ushort raw_height = 0;
};

struct libraw_colordata_t {
    float rgb_cam[4][3] = {};
    float cam_mul[4] = {0, 0, 0, 0};     // 相机记录的 as-shot 白平衡系数
    unsigned black = 0;                  // 统一黑电平
    unsigned cblack[4102] = {0};         // 分通道黑电平（前 4 项为 R/G1/B/G2）
    float maximum = 0.0f;                // 白电平（饱和值）
};

struct libraw_imgother_t {
    float iso_speed = 0, shutter = 0, aperture = 0, focal_len = 0, focus_distance = 0;
};

struct libraw_lensinfo_t {
    char Lens[128] = {0};
};

struct libraw_rawdata_t {
    void* raw_alloc = nullptr;
    ushort* raw_image = nullptr;   // 一维扁平缓冲：raw_width * raw_height
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
    libraw_rawdata_t rawdata;
    libraw_output_params_t params;
    struct { ushort raw_height = 0; } ioparams;
};

class LibRaw {
public:
    libraw_data_t imgdata;
    int open_file(const char*) { return 0; }
    int unpack() { return 0; }
    void recycle() {}
    void set_progress_handler(int (*)(void*, LibRaw_progress, int, int), void*) {}
};
