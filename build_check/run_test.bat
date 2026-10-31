@echo off
REM ===========================================================================
REM  Local core test runner - builds and runs the core test programs.
REM
REM  Compiles with the stub headers/dependencies in build_check:
REM    - stubs/libraw, stubs/lcms2.h     : header-only stubs
REM    - fakedeps/stub_lcms2.cpp         : linkable lcms2 stubs
REM
REM  Runs:
REM    1. test_pipeline    - synthetic Bayer data through the render pipeline
REM    2. test_core_units  - temperature / curve / CCM / CSV unit tests
REM
REM  NOTE: keep this file ASCII-only; cmd.exe misparses UTF-8 comments.
REM ===========================================================================
setlocal

set "VSPATH="
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
call :try "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
call :try "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"

if not defined VSPATH (
    echo [ERROR] Visual Studio with C++ tools was not found
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

cd /d "%~dp0.."

if not exist build_check\testobj mkdir build_check\testobj

set "CORE_SOURCES=src\core\Types.cpp src\core\PipelineStages.cpp src\core\LinearMath.cpp src\core\EditContext.cpp src\core\ColorCalibration.cpp src\core\Lut3D.cpp src\core\PipelineOps.cpp src\core\RawLoader.cpp src\core\ColorManagement.cpp src\core\LensCorrection.cpp src\core\PipelineRenderer.cpp src\core\FrameCalibration.cpp src\core\MaskModel.cpp src\core\DcpProfile.cpp src\core\DcpApply.cpp"
set "STUB_SOURCES=build_check\fakedeps\stub_lcms2.cpp build_check\fakedeps\stub_empty.cpp"
set "CL_FLAGS=/nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj /O2 /I src /I build_check\stubs /DRAWCore_LIBRAW_HEADER=\"libraw/libraw.h\" /DRAWCore_HAS_LENSFUN=0 /DRAWCore_HAS_OPENMP=0 /DRAWEDIT_TEST_LENSFUN_DB=\"third_party/lensfun-db/version_1\" /DNOMINMAX /DWIN32_LEAN_AND_MEAN"

echo [1/4] Building test_pipeline.exe
cl %CL_FLAGS% build_check\test_pipeline.cpp %CORE_SOURCES% %STUB_SOURCES% ^
  /Fe:build_check\testobj\test_pipeline.exe /link user32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] test_pipeline build failed
    exit /b 1
)

echo [2/4] Building test_core_units.exe
cl %CL_FLAGS% build_check\test_core_units.cpp %CORE_SOURCES% %STUB_SOURCES% ^
  /Fe:build_check\testobj\test_core_units.exe /link user32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] test_core_units build failed
    exit /b 1
)

echo [3/4] Running test_pipeline.exe
build_check\testobj\test_pipeline.exe
if errorlevel 1 (
    echo [ERROR] test_pipeline failed
    exit /b 1
)

echo [4/4] Running test_core_units.exe
build_check\testobj\test_core_units.exe
if errorlevel 1 (
    echo [ERROR] test_core_units failed
    exit /b 1
)

echo [OK] all core tests passed
exit /b 0

:try
if defined VSPATH goto :eof
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%~1"
goto :eof
