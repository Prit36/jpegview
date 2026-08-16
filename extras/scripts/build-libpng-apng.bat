@echo off

setlocal
REM this builds libpng and replaces the libs in the JPEGView src folder
REM zlib does not need to be built separately, as it is part of the libpng build process

REM FYI: building zlib separately with CMake deletes zconf.h in the source directory
REM because the makefiles to build are in whatever directory you built them
REM so when libpng builds, it'll break... so we no longer build zlib separately
REM https://github.com/madler/zlib/issues/133

REM https://stackoverflow.com/questions/10906554/how-do-i-revert-my-changes-to-a-git-submodule
REM NOTE: this modifies files in the submodule... to reset do:
REM  $ git submodule deinit -f -- extras/third_party/libpng-apng.src-patch/libpng
REM  $ git submodule update --init -- extras/third_party/libpng-apng.src-patch/libpng

REM if zlib gets dirty after building
REM  $ git submodule deinit -f -- extras/third_party/libpng-apng.src-patch/zlib
REM  $ git submodule update --init -- extras/third_party/libpng-apng.src-patch/zlib


SET XSRC_DIR=%~dp0..\..\src
SET XLIB_DIR=%~dp0..\third_party\libpng-apng.src-patch\libpng

SET XOUT_DIR_32=%XLIB_DIR%\projects\vstudio\Release Library
SET XOUT_DIR_64=%XLIB_DIR%\projects\vstudio\x64\Release Library

SET XPATCH_DIR=%~dp0..\third_party\libpng-apng.src-patch

findstr /C:"libpng version 1.6.58" "%XLIB_DIR%\png.h" >nul
IF ERRORLEVEL 1 (
	echo ERROR: libpng source must be v1.6.58
	exit /b 1
)

IF NOT EXIST "%XPATCH_DIR%\libpng-apng\libpng-1.6.58-apng.patch" (
	echo ERROR: libpng 1.6.58 APNG patch is missing
	exit /b 1
)

REM this script doesn't "clean" you do it yourself

IF EXIST "%XOUT_DIR_32%" (
	rd /s /q "%XOUT_DIR_32%"
)

IF EXIST "%XOUT_DIR_64%" (
	rd /s /q "%XOUT_DIR_64%"
)

call :PATCH_PNG
IF ERRORLEVEL 1 exit /b 1


call :BUILD_PNG x86 Win32
IF ERRORLEVEL 1 exit /b 1
call :BUILD_PNG x64 x64
IF ERRORLEVEL 1 exit /b 1



REM copy the libs over
copy /y "%XOUT_DIR_32%\libpng16.lib" "%XSRC_DIR%\JPEGView\libpng-apng\lib\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XOUT_DIR_32%\zlib.lib" "%XSRC_DIR%\JPEGView\libpng-apng\lib\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XOUT_DIR_64%\libpng16.lib" "%XSRC_DIR%\JPEGView\libpng-apng\lib64\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XOUT_DIR_64%\zlib.lib" "%XSRC_DIR%\JPEGView\libpng-apng\lib64\"
IF ERRORLEVEL 1 exit /b 1

copy /y "%XLIB_DIR%\png.h" "%XSRC_DIR%\JPEGView\libpng-apng\include\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XLIB_DIR%\pngconf.h" "%XSRC_DIR%\JPEGView\libpng-apng\include\"
IF ERRORLEVEL 1 exit /b 1
IF EXIST "%XOUT_DIR_64%\pnglibconf.h" (
	copy /y "%XOUT_DIR_64%\pnglibconf.h" "%XSRC_DIR%\JPEGView\libpng-apng\include\pnglibconf.h"
) ELSE (
	copy /y "%XLIB_DIR%\scripts\pnglibconf.h.prebuilt" "%XSRC_DIR%\JPEGView\libpng-apng\include\pnglibconf.h"
)
IF ERRORLEVEL 1 exit /b 1
copy /y "%~dp0..\third_party\libpng-apng.src-patch\zlib\zconf.h" "%XSRC_DIR%\JPEGView\libpng-apng\include\"
IF ERRORLEVEL 1 exit /b 1


echo === HEADER FILES SYNCHRONIZED ===


exit /b 0





:BUILD_PNG

REM so the environments don't pollute each other
setlocal

call "%~dp0vs-init.bat" %1

msbuild.exe /property:Platform=%2 /property:configuration="Release Library" /property:PlatformToolset=v143 "%XLIB_DIR%\projects\vstudio\vstudio.sln"
exit /b %ERRORLEVEL%





:PATCH_PNG
REM patch with apng and x64 support first
setlocal

REM NOTE: this is kinda hacky and depends on Git for Windows installed in a standard location,
REM and Git-Bash set up in a default way
REM
REM if someone wants to make it more generic, by all means,
REM but this is just meant to work for me, and for the GH runners

REM set up path to find the required bin files
SET PATH=%ProgramFiles%\Git\usr\bin;%PATH%

where.exe bash.exe 2>nul
IF ERRORLEVEL 1 (
	echo bash.exe not found in PATH
	exit /b 1
)

REM can't check where.exe's together as the ERRORLEVEL doesn't come back as 1 if at least one thing was found
where.exe patch.exe 2>nul
IF ERRORLEVEL 1 (
	echo bash.exe not found in PATH
	exit /b 1
)

pushd %XPATCH_DIR%
REM bash.exe is on the path somewhere, honor the path order
bash.exe -c "./patch-libpng.sh"
SET XERROR=%ERRORLEVEL%

popd
exit /b %XERROR%




REM **** I don't think there's need for this type of trickery, if we can just get it working with the above ****
REM this is where it gets super hacky...

SET BASH_EXE=
FOR /F "usebackq tokens=*" %%I IN (`where.exe bash.exe`) DO (
	SET BASH_EXE=%%I
)

SET PATCH_EXE=
FOR /F "usebackq tokens=*" %%I IN (`where.exe patch.exe`) DO (
	SET PATCH_EXE=%%I
)

echo Bash: %BASH_EXE%
echo Patch: %PATCH_EXE%

pushd %XPATCH_DIR%
REM because the script just calls "patch" we can fudge what that stands for by using an alias
"%BASH_EXE%" -c "alias patch='%PATCH_EXE:\=/%'; ./patch-libpng.sh"
SET XERROR=%ERRORLEVEL%

popd

exit /b %XERROR%
