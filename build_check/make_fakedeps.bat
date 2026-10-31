@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW
cl /nologo /EHsc /utf-8 /c /I build_check\fakedeps\include /Fo:build_check\fakedeps\ build_check\fakedeps\stub_empty.cpp build_check\fakedeps\stub_lcms2.cpp
lib /nologo /OUT:build_check\fakedeps\lib\raw.lib build_check\fakedeps\stub_empty.obj
lib /nologo /OUT:build_check\fakedeps\lib\lcms2.lib build_check\fakedeps\stub_lcms2.obj
