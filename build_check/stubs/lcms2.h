// ============================================================================
//  lcms2 桩头文件 —— 仅用于离线语法检查（不参与正式构建）
//  正式构建时使用系统/vcpkg 提供的真实 lcms2.h
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

typedef std::uint32_t cmsUInt32Number;
typedef int cmsInt32Number;
typedef float cmsFloat32Number;
typedef double cmsFloat64Number;
typedef void* cmsHPROFILE;
typedef void* cmsHTRANSFORM;
typedef void* cmsToneCurve;
typedef int cmsBool;

#define TYPE_RGB_FLT 0x00000001u
#define TYPE_RGB_16 0x00000002u
#define INTENT_PERCEPTUAL 0
#define INTENT_RELATIVE_COLORIMETRIC 1
#define INTENT_SATURATION 2
#define INTENT_ABSOLUTE_COLORIMETRIC 3
#define cmsFLAGS_BLACKPOINTCOMPENSATION 0x2000
#define cmsFLAGS_HIGHRESPRECALC 0x0400
#define cmsFLAGS_SOFTPROOFING 0x4000

struct cmsCIExyY {
    cmsFloat64Number x, y, Y;
};

struct cmsCIExyYTRIPLE {
    cmsCIExyY Red, Green, Blue;
};

#ifdef __cplusplus
extern "C" {
#endif

cmsInt32Number cmsGetEncodedCMMversion(void);
cmsHPROFILE cmsCreateRGBProfile(const cmsCIExyY* whitePoint,
                                const cmsCIExyYTRIPLE* primaries,
                                cmsToneCurve* const TransferFunction[3]);
cmsHPROFILE cmsCreate_sRGBProfile(void);
cmsHPROFILE cmsCreateXYZProfile(void);
cmsHPROFILE cmsOpenProfileFromFile(const char* fileName, const char* access);
cmsBool cmsCloseProfile(cmsHPROFILE hProfile);
cmsToneCurve* cmsBuildGamma(void* contextID, cmsFloat64Number gamma);
cmsToneCurve* cmsBuildParametricToneCurve(void* contextID, cmsInt32Number type,
                                          const cmsFloat64Number params[]);
void cmsFreeToneCurve(cmsToneCurve* curve);
cmsHTRANSFORM cmsCreateTransform(cmsHPROFILE input, cmsUInt32Number inputFormat,
                                 cmsHPROFILE output, cmsUInt32Number outputFormat,
                                 cmsUInt32Number intent, cmsUInt32Number flags);
cmsHTRANSFORM cmsCreateProofingTransform(cmsHPROFILE input, cmsUInt32Number inputFormat,
                                         cmsHPROFILE output, cmsUInt32Number outputFormat,
                                         cmsHPROFILE proofing,
                                         cmsUInt32Number intent,
                                         cmsUInt32Number proofingIntent,
                                         cmsUInt32Number flags);
void cmsDeleteTransform(cmsHTRANSFORM hTransform);
void cmsDoTransform(cmsHTRANSFORM transform, const void* inputBuffer,
                    void* outputBuffer, cmsUInt32Number size);
cmsBool cmsSaveProfileToMem(cmsHPROFILE hProfile, void* memPtr, cmsUInt32Number* bytesNeeded);

#ifdef __cplusplus
}
#endif
