// lcms2 桩实现：只保证能链接，不做任何真实色彩运算。
// 仅用于离线验证工程的编译/链接流程，切勿用于实际构建。
#include "lcms2.h"
#include <cstdint>
#include <cstring>

extern "C" {
cmsInt32Number cmsGetEncodedCMMversion(void) { return 2140; }
cmsHPROFILE cmsCreateRGBProfile(const cmsCIExyY*, const cmsCIExyYTRIPLE*, cmsToneCurve* const[3]) { return (cmsHPROFILE)1; }
cmsHPROFILE cmsCreate_sRGBProfile(void) { return (cmsHPROFILE)2; }
cmsHPROFILE cmsCreateXYZProfile(void) { return (cmsHPROFILE)3; }
cmsHPROFILE cmsOpenProfileFromFile(const char*, const char*) { return nullptr; }
cmsBool cmsCloseProfile(cmsHPROFILE) { return 1; }
cmsToneCurve* cmsBuildGamma(void*, cmsFloat64Number) { return reinterpret_cast<cmsToneCurve*>(static_cast<std::uintptr_t>(4)); }
cmsToneCurve* cmsBuildParametricToneCurve(void*, cmsInt32Number, const cmsFloat64Number[]) { return reinterpret_cast<cmsToneCurve*>(static_cast<std::uintptr_t>(4)); }
void cmsFreeToneCurve(cmsToneCurve*) {}
cmsHTRANSFORM cmsCreateTransform(cmsHPROFILE, cmsUInt32Number, cmsHPROFILE, cmsUInt32Number, cmsUInt32Number, cmsUInt32Number) { return (cmsHTRANSFORM)5; }
cmsHTRANSFORM cmsCreateProofingTransform(cmsHPROFILE, cmsUInt32Number, cmsHPROFILE, cmsUInt32Number, cmsHPROFILE, cmsUInt32Number, cmsUInt32Number, cmsUInt32Number) { return (cmsHTRANSFORM)6; }
void cmsDeleteTransform(cmsHTRANSFORM) {}
void cmsDoTransform(cmsHTRANSFORM, const void* in, void* out, cmsUInt32Number size) {
    std::memcpy(out, in, static_cast<size_t>(size) * 3u * sizeof(float));
}
cmsBool cmsSaveProfileToMem(cmsHPROFILE, void*, cmsUInt32Number* bytesNeeded) {
    if (bytesNeeded) { *bytesNeeded = 0; }
    return 0;
}
}
