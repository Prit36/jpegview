@echo off
REM Builds pngbench.exe against the REAL src/JPEGView PNG decode path.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
set ROOT=C:\Users\My_Home\Desktop\projects\jpegview
set SRC=%ROOT%\src\JPEGView
set LD=%SRC%\libdeflate
set OUTDIR=%~dp0
if "%OUTDIR:~-1%"=="\" set OUTDIR=%OUTDIR:~0,-1%
copy /Y "%SRC%\PNGWrapper.cpp" "%OUTDIR%\PNGWrapper_local.cpp" >nul
cl /EHsc /O2 /arch:AVX2 /std:c++17 /nologo /I "%OUTDIR%" /I "%SRC%" /I "%LD%\include" "%OUTDIR%\PNGWrapper_local.cpp" "%SRC%\FastPng.cpp" "%OUTDIR%\pngbench.cpp" /link /LIBPATH:"%LD%\lib64" libdeflate.lib /OUT:"%OUTDIR%\pngbench.exe"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK
