@echo off
REM ===========================================================================
REM  Offline syntax check - verifies core/ compiles without libraw/lcms2/Qt.
REM  Uses stub headers in build_check\stubs. NOT for production builds.
REM ===========================================================================
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW

if not exist build_check\obj mkdir build_check\obj

cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /c ^
  /I src /I build_check\stubs ^
  /DRAWCore_LIBRAW_HEADER=\"libraw/libraw.h\" ^
  /DRAWCore_HAS_LENSFUN=0 ^
  /DRAWCore_HAS_OPENMP=0 ^
  /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  src\core\Types.cpp ^
  src\core\LinearMath.cpp ^
  src\core\EditContext.cpp ^
  src\core\ColorCalibration.cpp ^
  src\core\PipelineOps.cpp ^
  src\core\RawLoader.cpp ^
  src\core\ColorManagement.cpp ^
  src\core\LensCorrection.cpp ^
  src\core\PipelineRenderer.cpp ^
  /Fo:build_check\obj\ 2>&1
