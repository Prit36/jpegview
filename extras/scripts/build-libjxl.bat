@echo off

setlocal
REM this builds libjxl and replaces the libs in the JPEGView src folder

REM for me it takes like 15min to build
echo NOTE: this takes a LONG time to build, don't be alarmed...
echo       if it looks like the build process hung... it didn't, it's just WAY SLOW!

SET XSRC_DIR=%~dp0..\..\src
SET XLIB_DIR=%~dp0..\third_party\libjxl
SET XOUT_DIR=%~dp0libjxl

IF EXIST "%XOUT_DIR%" (
	rd /s /q "%XOUT_DIR%"
)

call :BUILD_COPY_JXL x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JXL x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1

xcopy "%XLIB_DIR%\lib\include\jxl\*.h" "%XSRC_DIR%\JPEGView\libjxl\include\jxl\" /Y /C /D
IF ERRORLEVEL 1 exit /b 1

rd /s /q "%XOUT_DIR%" 2>nul

echo === HEADER FILES SYNCHRONIZED ===

exit /b 0





:BUILD_COPY_JXL

REM so the environments don't pollute each other
setlocal

SET XBUILD_ARCH=%~1
SET XPLATFORM=%~2
SET XJPV_ARCH_PATH=%~3

SET XBUILD_DIR=%XOUT_DIR%\%XBUILD_ARCH%

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XBUILD_ARCH%

pushd "%XBUILD_DIR%"

cmake.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_PLUGINS=OFF -A %XPLATFORM% "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%XPLATFORM% /p:configuration="Release" LIBJXL.sln /t:jxl_dec;jxl_threads
IF ERRORLEVEL 1 exit /b 1

popd

REM copy the binaries over
IF EXIST "%XBUILD_DIR%\lib\Release\jxl_dec.dll" (
	copy /y "%XBUILD_DIR%\lib\Release\jxl*.dll" "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\"
) ELSE (
	copy /y "%XBUILD_DIR%\Release\jxl*.dll" "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\"
)
IF ERRORLEVEL 1 exit /b 1

IF EXIST "%XBUILD_DIR%\third_party\brotli\Release\brotlicommon.dll" (
	copy /y "%XBUILD_DIR%\third_party\brotli\Release\brotli*.dll" "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\"
	IF ERRORLEVEL 1 exit /b 1
)

copy /y "%XBUILD_DIR%\lib\Release\jxl*.lib" "%XSRC_DIR%\JPEGView\libjxl\lib%XJPV_ARCH_PATH%\"
IF ERRORLEVEL 1 exit /b 1

xcopy "%XBUILD_DIR%\lib\include\jxl\*.h" "%XSRC_DIR%\JPEGView\libjxl\include\jxl\" /Y /C /D
IF ERRORLEVEL 1 exit /b 1
IF NOT EXIST "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\jxl_dec.dll" exit /b 1
IF NOT EXIST "%XSRC_DIR%\JPEGView\libjxl\lib%XJPV_ARCH_PATH%\jxl_dec.lib" exit /b 1

exit /b 0
