@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 exit /b 1
pushd "%~dp0..\.."
if not exist scratch\perf_random mkdir scratch\perf_random
cl /LD /EHsc /O2 /arch:AVX2 /std:c++17 /nologo /Fo:scratch\perf_random\ /I src\JPEGView /I src\JPEGView\libdeflate\include benchmarks\pngbench\fastpng_test_bridge.cpp src\JPEGView\FastPng.cpp /link /LIBPATH:src\JPEGView\libdeflate\lib64 libdeflate.lib /OUT:scratch\perf_random\fastpng_test.dll /IMPLIB:scratch\perf_random\fastpng_test.lib
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%
