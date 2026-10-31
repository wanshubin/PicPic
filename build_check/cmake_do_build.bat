@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\furywan\Desktop\RAW
set PATH=%PATH%;C:\Users\furywan\AppData\Roaming\Python\Python313\site-packages\cmake\data\bin
cmake --build build_verify --config Release --parallel 8
