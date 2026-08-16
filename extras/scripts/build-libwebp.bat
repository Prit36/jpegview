@echo off

setlocal
REM this builds libwebp and replaces the libs in the JPEGView src folder

SET XSRC_DIR=%~dp0..\..\src\JPEGView\libwebp
SET XLIB_DIR=%~dp0..\third_party\libwebp
SET XOUT_DIR=%~dp0libwebp

IF EXIST "%XOUT_DIR%" (
	rd /s /q "%XOUT_DIR%"
)

call :BUILD_COPY_WEBP x86 lib
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_WEBP x64 lib64
IF ERRORLEVEL 1 exit /b 1

xcopy "%XLIB_DIR%\src\webp\*.h" "%XSRC_DIR%\include\webp\" /Y /C /D
IF ERRORLEVEL 1 exit /b 1

rd /s /q "%XOUT_DIR%" 2>nul

echo === HEADER FILES SYNCHRONIZED ===

exit /b 0





:BUILD_COPY_WEBP

REM so the environments don't pollute each other
setlocal

mkdir "%XOUT_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XLIB_DIR%"

REM ARCH= is optional, but for some reason, building with VS2017 in some instances it's needed
::nmake.exe /f Makefile.vc CFG=release-static RTLIBCFG=static OBJDIR="%XOUT_DIR%"
nmake.exe /f Makefile.vc ARCH=%1 CFG=release-static RTLIBCFG=static OBJDIR="%XOUT_DIR%"
IF ERRORLEVEL 1 exit /b 1

popd

REM copy the libs over
REM error checking if a copy fails... throws error to caller
copy /y "%XOUT_DIR%\release-static\%1\lib\libwebp.lib" "%XSRC_DIR%\%2\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XOUT_DIR%\release-static\%1\lib\libwebpdemux.lib" "%XSRC_DIR%\%2\"
IF ERRORLEVEL 1 exit /b 1


exit /b 0
