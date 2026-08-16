@echo off

setlocal

REM I've built jpegview releases a few times already, and I have to say it's almost a bit painful... as it's easy to forget one step vs another.
REM While I have a long todo list, scripting is always the least painless approach...
REM Goal: compiles the JPEGView in release configuration.  Doing it manually for releases is very error prone
REM /sylikc

REM set vars
SET JPV_ROOT=%~dp0..\..
SET XOUT_BASE=%~dp0jpegview-release
SET XRAW_OUT=%XOUT_BASE%\raw_out

REM so we don't end up building multiple releases together accidentally
IF EXIST "%XOUT_BASE%" (
	echo OUTPUT directory already exists, please delete and try again
	exit /b 1
)

REM detect version from source
SET JPV_VER=
echo ~ Resource:
findstr /l /c:JPEGVIEW_VERSION "%JPV_ROOT%\src\JPEGView\resource.h"
echo ~ rc:
findstr /l /i /c:FILEVERSION /c:PRODUCTVERSION "%JPV_ROOT%\src\JPEGView\JPEGView.rc"


SET JPV_VER=%~1
echo ~ Command Line: %JPV_VER%

if /I "%JPV_VER%" EQU "" (
	echo Argument must be target release version e.g. 1.0.34
	exit /b 1
)




SET YESNO=
SET /P YESNO="Have you made all the final code edits? y/[n] "
IF /I "%YESNO%" NEQ "y" (
	echo Aborted!
	exit /b 1
)





REM detect tools
SET X7ZA=C:\Program Files\7-Zip\7z.exe

IF NOT EXIST "%X7ZA%" (
	echo ERROR: 7-zip not found in standard location
	exit /b 1
)

REM needed to build libjpegturbo in the XP build
echo + Looking up NASM
where nasm.exe
IF ERRORLEVEL 1 (
	echo ERROR: NASM.exe not found on path
	exit /b 1
)

REM needed for XP build to run a custom script
echo + Looking up Python
python.exe -V
IF ERRORLEVEL 1 (
	echo ERROR: Python.exe not found on path
	exit /b 1
)






REM if you use "OUTDIR" this variable will be picked up by msbuild and weird things happen, some files pop out in OUTDIR instead of where you normally expect it
REM (JPEGView.Setup ends up there, JPEGView works as expected)


IF EXIST "%XOUT_DIR%" (
	echo ERROR: Output dir exists, please archive and retry
	exit /b 1
)


echo + Creating %XOUT_BASE% ...

mkdir "%XOUT_BASE%"
mkdir "%XRAW_OUT%"

echo ------------------------------------------------------
echo Creating release %JPV_VER% in folder %XOUT_BASE%
echo ------------------------------------------------------



echo + Cleaning obj/bin dirs ...
REM Delete * in the x86 and x64 Release directories (so config files get recopied for sure)
rd /s /q "%JPV_ROOT%\src\JPEGView\bin" 2>nul
rd /s /q "%JPV_ROOT%\src\JPEGView\obj" 2>nul

rd /s /q "%JPV_ROOT%\src\JPEGView.Setup\bin" 2>nul
rd /s /q "%JPV_ROOT%\src\JPEGView.Setup\obj" 2>nul

rd /s /q "%JPV_ROOT%\src\WICLoader\obj" 2>nul


REM MANUAL WAY:
REM Open VS: Set Configuration to Release/x64
REM Build Clean
REM Build All


REM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo + MsBuild for x86 and x64 ...

call :BUILD_COPY_JPV x86 Win32 32
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JPV x64 x64 64
IF ERRORLEVEL 1 exit /b 1

REM Copy HowToInstall.txt and HowToInstall_ru.txt to OUTPUT
copy "%JPV_ROOT%\HowToInstall.txt" "%XRAW_OUT%"
copy "%JPV_ROOT%\HowToInstall_ru.txt" "%XRAW_OUT%"
REM I didn't realize this was never in any releases
copy "%JPV_ROOT%\CHANGELOG.txt" "%XRAW_OUT%"

REM run in OUTPUT, remove intermediates
del /s "%XRAW_OUT%\*.exp" "%XRAW_OUT%\*.lib" "%XRAW_OUT%\*.pdb"


REM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

REM ZIP up flat from RAW OUTPUT
pushd "%XRAW_OUT%"

REM NOTE: these filenames should not change... the pattern and naming of these are expected by package managers like Scoop, and others on the web
REM the folder names JPEGView32 and JPEGView64 are also consistent with the original author's release style, which is expected by the same package managers
"%X7ZA%" a -tzip -mx9 ..\JPEGView_%JPV_VER%.zip .
IF ERRORLEVEL 1 exit /b 1
"%X7ZA%" a -t7z -mx9 ..\JPEGView_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1

pushd JPEGView32
"%X7ZA%" a -t7z -mx9 ..\..\JPEGView32_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1
popd

pushd JPEGView64
"%X7ZA%" a -t7z -mx9 ..\..\JPEGView64_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1
popd

popd

REM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

REM generate SHA256SUMS
pushd "%XOUT_BASE%"
python.exe "%~dp0sha256sum.py" "JPEGView*" > SHA256SUMS
IF ERRORLEVEL 1 exit /b 1
popd

REM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo + Autogenerate last chunk of changelog ...

SET JPV_ROOT=..\..
python.exe -c "from pathlib import Path; from util_common import get_all_text_between; print(get_all_text_between(Path(r'%JPV_ROOT%\CHANGELOG.txt'), '\[%JPV_VER%', '\['))"

exit /b 0


REM -----------------------------------------------------------------------------------------------------------

:BUILD_COPY_JPV

setlocal

SET XARCH=%~1
SET XPLATFORM=%~2
SET XBIT=%~3

call "%~dp0vs-init.bat" %XARCH%

pushd "%JPV_ROOT%\src"

REM build like with github actions, via command line
msbuild.exe /property:Platform=%XPLATFORM% /property:configuration="Release" JPEGView.sln
IF ERRORLEVEL 1 exit /b 1

popd

REM from JPEGView\x64\Release to OUTPUT\JPEGView64
move "%JPV_ROOT%\src\JPEGView\bin\%XARCH%\Release" "%XRAW_OUT%\JPEGView%XBIT%"
IF ERRORLEVEL 1 exit /b 1

REM move the wix installer
move "%JPV_ROOT%\src\JPEGView.Setup\bin\%XARCH%\Release\en-us\JPEGView.Setup.msi" "%XOUT_BASE%\JPEGView%XBIT%_en-us_%JPV_VER%.msi"
IF ERRORLEVEL 1 exit /b 1

exit /b 0

