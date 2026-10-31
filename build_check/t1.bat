@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW
cl /nologo /EHsc /c /I build_check\fakedeps\include /Fo:build_check\fakedeps\ build_check\fakedeps\stub_lcms2.cpp 2>&1
