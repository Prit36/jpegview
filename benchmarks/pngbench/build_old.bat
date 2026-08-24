@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
set ROOT=C:\Users\My_Home\Desktop\projects\jpegview
set SRC=%ROOT%\src\JPEGView
set LIBPNG=%SRC%\libpng-apng
set ZLIBINC=%ROOT%\extras\third_party\libpng-apng.src-patch\zlib
set LD=%ROOT%\benchmarks\libdeflate-1.23
set OUTDIR=%~dp0
if "%OUTDIR:~-1%"=="\" set OUTDIR=%OUTDIR:~0,-1%
cl /EHsc /O2 /arch:AVX2 /std:c++17 /nologo /I "%OUTDIR%" /I "%SRC%" /I "%LIBPNG%\include" /I "%ZLIBINC%" /I "%LD%" "%OUTDIR%\PNGWrapper_old_local.cpp" "%SRC%\FastPng.cpp" "%OUTDIR%\pngbench.cpp" /link /LIBPATH:"%LIBPNG%\lib64" /LIBPATH:"%LD%" libpng16.lib zlib.lib libdeflate.lib /OUT:"%OUTDIR%\pngbench_old.exe"
