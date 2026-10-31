@echo off
REM ===========================================================================
REM  Offline syntax check - verifies core/ compiles without libraw/lcms2/Qt.
REM  Uses stub headers in build_check\stubs. NOT for production builds.
REM
REM  Differences from check_core.bat:
REM    1. Locates Visual Studio automatically instead of hard-coding a path
REM    2. Derives the project root from this script's location
REM    3. Includes the new Lut3D.cpp in the check
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
  src\core\Lut3D.cpp ^
  src\core\PipelineOps.cpp ^
  src\core\RawLoader.cpp ^
  src\core\ColorManagement.cpp ^
  src\core\LensCorrection.cpp ^
  src\core\PipelineRenderer.cpp ^
  /Fo:build_check\obj\ 2>&1

exit /b %errorlevel%

:try
if defined VSPATH goto :eof
if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%~1"
goto :eof
