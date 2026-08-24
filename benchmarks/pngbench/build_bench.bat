@echo off
REM Build the standalone PNG benchmark against the REAL JPEGView sources:
REM   - src\JPEGView\PNGWrapper.cpp  (copied here so "stdafx.h" resolves to our minimal stand-in)
REM   - src\JPEGView\FastPng.cpp     (the accelerated decode path)
REM Re-run after editing either file.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
set ROOT=C:\Users\My_Home\Desktop\projects\jpegview
set SRC=%ROOT%\src\JPEGView
set LIBPNG=%SRC%\libpng-apng
set ZLIBINC=%ROOT%\extras\third_party\libpng-apng.src-patch\zlib
set LD=%ROOT%\benchmarks\libdeflate-1.23
set OUTDIR=%~dp0
if "%OUTDIR:~-1%"=="\" set OUTDIR=%OUTDIR:~0,-1%
copy /Y "%SRC%\PNGWrapper.cpp" "%OUTDIR%\PNGWrapper_local.cpp" >nul
cl /EHsc /O2 /arch:AVX2 /std:c++17 /nologo /I "%OUTDIR%" /I "%SRC%" /I "%LIBPNG%\include" /I "%ZLIBINC%" /I "%LD%" "%OUTDIR%\PNGWrapper_local.cpp" "%SRC%\FastPng.cpp" "%OUTDIR%\pngbench.cpp" /link /LIBPATH:"%LIBPNG%\lib64" /LIBPATH:"%LD%" libpng16.lib zlib.lib libdeflate.lib /OUT:"%OUTDIR%\pngbench.exe"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK
endlocal
