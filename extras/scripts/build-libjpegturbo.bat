@echo off

setlocal
REM this builds libjpeg-turbo and replaces the libs in the JPEGView src folder

SET XSRC_DIR=%~dp0..\..\src
SET XLIB_DIR=%~dp0..\third_party\libjpeg-turbo
SET XOUT_DIR=%~dp0libjpeg-turbo

IF EXIST "%XOUT_DIR%" (
	rd /s /q "%XOUT_DIR%"
)

call :BUILD_COPY_JPEGT x86 lib
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JPEGT x64 lib64
IF ERRORLEVEL 1 exit /b 1

rd /s /q "%XOUT_DIR%" 2>nul

echo === HEADER FILES SYNCHRONIZED ===

exit /b 0





:BUILD_COPY_JPEGT

REM so the environments don't pollute each other
setlocal

SET XBUILD_DIR=%XOUT_DIR%\%1

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XBUILD_DIR%"
cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
nmake.exe
IF ERRORLEVEL 1 exit /b 1

IF NOT EXIST "%XBUILD_DIR%\turbojpeg-static.lib" (
	echo ERROR: turbojpeg-static.lib was not produced
	exit /b 1
)
IF ERRORLEVEL 1 exit /b 1

popd

REM copy the libs over
REM error checking if a copy fails... throws error to caller
copy /y "%XBUILD_DIR%\turbojpeg-static.lib" "%XSRC_DIR%\JPEGView\libjpeg-turbo\%~2\"
IF ERRORLEVEL 1 exit /b 1

copy /y "%XLIB_DIR%\src\jerror.h" "%XSRC_DIR%\JPEGView\libjpeg-turbo\include\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XLIB_DIR%\src\jmorecfg.h" "%XSRC_DIR%\JPEGView\libjpeg-turbo\include\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XLIB_DIR%\src\jpeglib.h" "%XSRC_DIR%\JPEGView\libjpeg-turbo\include\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XLIB_DIR%\src\turbojpeg.h" "%XSRC_DIR%\JPEGView\libjpeg-turbo\include\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\jconfig.h" "%XSRC_DIR%\JPEGView\libjpeg-turbo\include\"
IF ERRORLEVEL 1 exit /b 1

exit /b 0
