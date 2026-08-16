@echo off

setlocal
REM this builds libheif and replaces the libs in the JPEGView src folder
REM it also builds libavif as both share the same dav1d libs

SET XSRC_DIR=%~dp0..\..\src\JPEGView\libheif
SET XLIB_DIR=%~dp0..\third_party\libheif
SET XLIBAVIF_DIR=%~dp0..\third_party\libavif
SET XAVIFSRC_DIR=%~dp0..\..\src\JPEGView\libavif
SET XOUT_DIR=%~dp0libheif_libavif

SET XVENV_DIR=%~dp0venv-meson

findstr /C:"version: '1.5.4'" "%XLIB_DIR%\dav1d\meson.build" >nul
IF ERRORLEVEL 1 (
	echo ERROR: dav1d source must be v1.5.4
	exit /b 1
)
findstr /C:"VERSION 1.0.18" "%XLIB_DIR%\libde265\CMakeLists.txt" >nul
IF ERRORLEVEL 1 (
	echo ERROR: libde265 source must be v1.0.18
	exit /b 1
)
findstr /C:"VERSION 1.23.1" "%XLIB_DIR%\libheif\CMakeLists.txt" >nul
IF ERRORLEVEL 1 (
	echo ERROR: libheif source must be v1.23.1
	exit /b 1
)
findstr /C:"VERSION 1.4.2" "%XLIBAVIF_DIR%\CMakeLists.txt" >nul
IF ERRORLEVEL 1 (
	echo ERROR: libavif source must be v1.4.2
	exit /b 1
)

IF EXIST "%XOUT_DIR%" (
	rd /s /q "%XOUT_DIR%"
)

IF EXIST "%LOCALAPPDATA%\bin\NASM" SET "PATH=%LOCALAPPDATA%\bin\NASM;%PATH%"
IF EXIST "C:\Program Files\NASM" SET "PATH=C:\Program Files\NASM;%PATH%"
IF EXIST "%LOCALAPPDATA%\Programs\Python\Python312" SET "PATH=%LOCALAPPDATA%\Programs\Python\Python312;%LOCALAPPDATA%\Programs\Python\Python312\Scripts;%PATH%"
IF EXIST "C:\Program Files\Python312" SET "PATH=C:\Program Files\Python312;C:\Program Files\Python312\Scripts;%PATH%"

echo + Looking up NASM
where nasm.exe
IF ERRORLEVEL 1 (
	echo NASM isn't found on path!
	exit /b 1
)

echo + Looking up Python
where python.exe
python -V
IF ERRORLEVEL 1 (
	echo Python isn't found on path!
	exit /b 1
)

echo + Create VENV
python.exe -m venv "%XVENV_DIR%"

echo + Activate VENV
call "%XVENV_DIR%\Scripts\activate.bat"

echo + Install Meson and Ninja inside VENV
python.exe -m pip install -U pip
python.exe -m pip install -U meson ninja


REM libavif only depends on dav1d
REM libheif depends on dav1d and libde

call :BUILD_COPY_DAV1D x86 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBAVIF x86 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBDE x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBHEIF x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1


call :BUILD_COPY_DAV1D x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBAVIF x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBDE x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBHEIF x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1




rd /s /q "%XOUT_DIR%" 2>nul

echo === HEADER FILES SYNCHRONIZED ===

exit /b 0




REM ===============================================================================================

:BUILD_COPY_DAV1D

REM so the environments don't pollute each other
setlocal

SET XBUILD_DIR=%XOUT_DIR%\dav1d-%1\build
SET XDIST_DIR=%XOUT_DIR%\dav1d-%1\dist

mkdir "%XBUILD_DIR%" 2>nul
mkdir "%XDIST_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XLIB_DIR%\dav1d"

REM or use --reconfigure flag
REM rd /s /q "%XLIB_DIR%\dav1d\build" 2>nul

REM part of the commands came from dav1d.cmd in libheif third-party


REM meson.exe from pip install venv\scripts
REM if you want to build static DLL do this:  meson setup --default-library=static --buildtype release --prefix "%XDIST_DIR%" "%XBUILD_DIR%"
meson setup --default-library=shared --buildtype release --prefix "%XDIST_DIR%" --libdir lib "%XBUILD_DIR%"
IF ERRORLEVEL 1 exit /b 1

REM ninja.exe from pip install venv\scripts
ninja -C "%XBUILD_DIR%"
IF ERRORLEVEL 1 exit /b 1
REM libheif and libavif consume the installed dav1d layout
ninja -C "%XBUILD_DIR%" install
IF ERRORLEVEL 1 exit /b 1

IF NOT EXIST "%XDIST_DIR%\bin\dav1d.dll" exit /b 1
IF EXIST "%XDIST_DIR%\lib\libdav1d.lib" (
	SET XDAV1D_LIB=%XDIST_DIR%\lib\libdav1d.lib
) ELSE IF EXIST "%XDIST_DIR%\lib\dav1d.lib" (
	SET XDAV1D_LIB=%XDIST_DIR%\lib\dav1d.lib
) ELSE (
	echo ERROR: dav1d import library was not produced
	exit /b 1
)

copy /y "%XDIST_DIR%\bin\dav1d.dll" "%XAVIFSRC_DIR%\bin%~2\"
IF ERRORLEVEL 1 exit /b 1

popd

exit /b 0




REM ===============================================================================================
:BUILD_COPY_LIBDE

REM so the environments don't pollute each other
setlocal

SET XLIB_DIR=%XLIB_DIR%\libde265

SET XBUILD_DIR=%XOUT_DIR%\libde-%1

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XBUILD_DIR%"
cmake.exe -DCMAKE_BUILD_TYPE=Release -A %2 "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%2 /p:configuration="Release" libde265.sln /t:de265
IF ERRORLEVEL 1 exit /b 1

IF NOT EXIST "%XBUILD_DIR%\libde265\Release\libde265.dll" exit /b 1
IF EXIST "%XBUILD_DIR%\libde265\Release\libde265.lib" (
	SET "XDE265_OUT_LIB=%XBUILD_DIR%\libde265\Release\libde265.lib"
) ELSE IF EXIST "%XBUILD_DIR%\libde265\Release\de265.lib" (
	SET "XDE265_OUT_LIB=%XBUILD_DIR%\libde265\Release\de265.lib"
) ELSE (
	echo ERROR: libde265 import library was not produced
	exit /b 1
)
IF NOT EXIST "%XBUILD_DIR%\libde265\de265-version.h" exit /b 1

copy /y "%XBUILD_DIR%\libde265\Release\libde265.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XDE265_OUT_LIB%" "%XSRC_DIR%\lib%~3\libde265.lib"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XDE265_OUT_LIB%" "%XSRC_DIR%\lib%~3\de265.lib"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\libde265\de265-version.h" "%XBUILD_DIR%\libde265\libde265-version.h" >nul
IF ERRORLEVEL 1 exit /b 1


popd

exit /b 0







REM ===============================================================================================

:BUILD_COPY_LIBHEIF

REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libheif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist
SET XDE265_DIR=%XLIB_DIR%\libde265

SET XDE265_BUILD=%XOUT_DIR%\libde-%1

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%

IF NOT EXIST "%XDE265_BUILD%\libde265\de265-version.h" exit /b 1
copy /y "%XDE265_BUILD%\libde265\de265-version.h" "%XDE265_DIR%\libde265" >nul
IF ERRORLEVEL 1 exit /b 1

IF EXIST "%XDAV1D_DIST%\lib\libdav1d.lib" (
	SET XDAV1D_LIB=%XDAV1D_DIST%\lib\libdav1d.lib
) ELSE IF EXIST "%XDAV1D_DIST%\lib\dav1d.lib" (
	SET XDAV1D_LIB=%XDAV1D_DIST%\lib\dav1d.lib
) ELSE (
	echo ERROR: dav1d import library was not found
	exit /b 1
)
IF EXIST "%XDE265_BUILD%\libde265\Release\libde265.lib" (
	SET XDE265_LIB=%XDE265_BUILD%\libde265\Release\libde265.lib
) ELSE IF EXIST "%XDE265_BUILD%\libde265\Release\de265.lib" (
	SET XDE265_LIB=%XDE265_BUILD%\libde265\Release\de265.lib
) ELSE (
	echo ERROR: libde265 import library was not found
	exit /b 1
)

pushd "%XBUILD_DIR%"

cmake.exe -DCMAKE_BUILD_TYPE=Release -A %2 -DCMAKE_PREFIX_PATH="%XDAV1D_DIST%;%XDE265_BUILD%" -DENABLE_PLUGIN_LOADING=OFF -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF -DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF -DDAV1D_LIBRARY="%XDAV1D_LIB%" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DLIBDE265_LIBRARY="%XDE265_LIB%" -DLIBDE265_INCLUDE_DIR="%XLIB_DIR%\libde265" "%XLIB_DIR%\libheif"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%2 /p:configuration="Release" libheif.sln /t:heif
IF ERRORLEVEL 1 exit /b 1

IF NOT EXIST "%XBUILD_DIR%\libheif\Release\heif.dll" exit /b 1
IF NOT EXIST "%XBUILD_DIR%\libheif\Release\heif.lib" exit /b 1

popd

copy /y "%XBUILD_DIR%\libheif\Release\heif.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\libheif\Release\heif.lib" "%XSRC_DIR%\lib%~3\"
IF ERRORLEVEL 1 exit /b 1
IF EXIST "%XBUILD_DIR%\libheif\heif_version.h" (
	copy /y "%XBUILD_DIR%\libheif\heif_version.h" "%XSRC_DIR%\include\libheif\heif_version.h"
) ELSE IF EXIST "%XBUILD_DIR%\heif_version.h" (
	copy /y "%XBUILD_DIR%\heif_version.h" "%XSRC_DIR%\include\libheif\heif_version.h"
) ELSE (
	exit /b 1
)
IF ERRORLEVEL 1 exit /b 1
copy /y "%XDE265_BUILD%\libde265\Release\libde265.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
IF EXIST "%XLIB_DIR%\libheif\libheif\api\libheif" (
	xcopy "%XLIB_DIR%\libheif\libheif\api\libheif\*.h" "%XSRC_DIR%\include\libheif\" /Y /C /D
) ELSE (
	xcopy "%XLIB_DIR%\libheif\api\libheif\*.h" "%XSRC_DIR%\include\libheif\" /Y /C /D
)
IF ERRORLEVEL 1 exit /b 1


exit /b 0






REM ===============================================================================================

:BUILD_COPY_LIBAVIF


REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libavif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist


mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%


pushd "%XBUILD_DIR%"

REM not sure if there's any diff using nmake vs ninja

IF EXIST "%XDAV1D_DIST%\lib\libdav1d.lib" (
	SET XDAV1D_LIB=%XDAV1D_DIST%\lib\libdav1d.lib
) ELSE IF EXIST "%XDAV1D_DIST%\lib\dav1d.lib" (
	SET XDAV1D_LIB=%XDAV1D_DIST%\lib\dav1d.lib
) ELSE (
	echo ERROR: dav1d import library was not found
	exit /b 1
)

cmake.exe -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%XDAV1D_DIST%" -DCMAKE_C_STANDARD=11 -DCMAKE_C_STANDARD_REQUIRED=ON -DBUILD_SHARED_LIBS=ON -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF -DAVIF_LIBYUV=OFF -DAVIF_CODEC_AOM=OFF -DAVIF_CODEC_DAV1D=SYSTEM -DAVIF_CODEC_LIBGAV1=OFF -DAVIF_CODEC_RAV1E=OFF -DAVIF_CODEC_SVT=OFF -DAVIF_ZLIBPNG=OFF -DDAV1D_LIBRARY="%XDAV1D_LIB%" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" "%XLIBAVIF_DIR%"
IF ERRORLEVEL 1 exit /b 1
::nmake.exe
ninja.exe
IF ERRORLEVEL 1 exit /b 1

IF NOT EXIST "%XBUILD_DIR%\avif.dll" exit /b 1
IF NOT EXIST "%XBUILD_DIR%\avif.lib" exit /b 1

popd

copy /y "%XBUILD_DIR%\avif.dll" "%XAVIFSRC_DIR%\bin%~2\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\avif.lib" "%XAVIFSRC_DIR%\lib%~2\"
IF ERRORLEVEL 1 exit /b 1
xcopy "%XLIBAVIF_DIR%\include\avif\*.h" "%XAVIFSRC_DIR%\include\avif\" /Y /C /D
IF ERRORLEVEL 1 exit /b 1




exit /b 0












REM !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
REM OLD CODE ROUTINES

REM ===============================================================================================

REM builds it the old way, not used at the moment, using qbnu's cmake way: https://github.com/qbnu/build-heif-avif-decoders/blob/main/.github/workflows/main.yml
:BUILD_COPY_LIBDE_BUILDBAT

REM so the environments don't pollute each other
setlocal

SET XLIB_DIR=%XLIB_DIR%\libde265

REM it builds into its own directory, clear it before proceeding
REM if upstream changes the build locations, then this will have to match what's in build.bat upstream
SET XBUILD_DIR=%XLIB_DIR%\bin_%1

rd /s /q "%XBUILD_DIR%"

REM build.bat has this, but it's fixed on some version which might not be what vs-init detects
call "%~dp0vs-init.bat" %1

pushd "%XLIB_DIR%"
call build.bat %1
IF ERRORLEVEL 1 exit /b 1


copy /y "%XBUILD_DIR%\libde265.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1


popd

exit /b 0

REM ===============================================================================================

REM this was the accompanying when using the BUILD.BAT way of libde
:BUILD_COPY_LIBHEIF_OLDLIBDE

REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libheif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist
SET XDE265_DIR=%XLIB_DIR%\libde265


mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%

pushd "%XBUILD_DIR%"


REM de265.h has an include
REM #include <libde265/de265-version.h>
REM which requires doing this since on windows there's no "install" which takes care of this
REM if you try to just include the extra directory, bad things happen since there are other .h files in there ... -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%\extra"
copy /y "%XDE265_DIR%\extra\libde265\de265-version.h" "%XDE265_DIR%\libde265"

REM for static dav1d:  cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\libdav1d.a" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DLIBDE265_LIBRARY="%XDE265_DIR%\bin_%XARCH%\lib\libde265.lib" -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%" "%XLIB_DIR%\libheif"
cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\dav1d.lib" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DLIBDE265_LIBRARY="%XDE265_DIR%\bin_%XARCH%\lib\libde265.lib" -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%" "%XLIB_DIR%\libheif"
IF ERRORLEVEL 1 exit /b 1
nmake.exe
IF ERRORLEVEL 1 exit /b 1

popd

copy /y "%XBUILD_DIR%\libheif\heif.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\libheif\heif.lib" "%XSRC_DIR%\lib%~3\"
IF ERRORLEVEL 1 exit /b 1


exit /b 0
