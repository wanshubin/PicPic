@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW
if not exist build_check\lfobj mkdir build_check\lfobj
cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /c ^
  /I src /I build_check\stubs ^
  /DRAWCore_LIBRAW_HEADER=\"libraw/libraw.h\" ^
  /DRAWCore_HAS_LENSFUN=1 ^
  /DRAWCore_LENSFUN_HEADER=\"lensfun/lensfun.h\" ^
  /DRAWCore_HAS_OPENMP=0 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  src\core\LensCorrection.cpp ^
  src\core\PipelineRenderer.cpp ^
  /Fo:build_check\lfobj\ 2>&1
