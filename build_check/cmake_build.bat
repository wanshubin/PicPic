@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW
set PATH=%PATH%;C:\Users\furywan\AppData\Roaming\Python\Python313\site-packages\cmake\data\bin
cmake -S . -B build_verify -G "Visual Studio 16 2019" -A x64 ^
  -DCMAKE_PREFIX_PATH="C:/Qt-aqt/6.5.3/msvc2019_64" ^
  -DLIBRAW_ROOT="C:/Users/furywan/Desktop/RAW/build_check/fakedeps" ^
  -DLCMS2_ROOT="C:/Users/furywan/Desktop/RAW/build_check/fakedeps" ^
  -DRAWCore_HAS_LENSFUN=OFF ^
  -DCMAKE_BUILD_TYPE=Release
