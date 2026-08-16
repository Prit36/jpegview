@echo off

REM Routine to detect and initialize Visual Studio MSVC build environment
REM Supports Visual Studio 2026, Visual Studio 2022, and earlier installations

IF /I "%~1" EQU "" (
	echo ERROR: Pass in an architecture to be passed to vcvarsall.bat, e.g. x64 or x86
	exit /b 1
)

SET XVS_VCVARS_BAT=
SET VSWHERE_EXE=

SET "PF86=%ProgramFiles(x86)%"
SET "PF64=%ProgramFiles%"

IF DEFINED PF86 IF EXIST "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" SET "VSWHERE_EXE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
IF NOT DEFINED VSWHERE_EXE IF DEFINED PF64 IF EXIST "%PF64%\Microsoft Visual Studio\Installer\vswhere.exe" SET "VSWHERE_EXE=%PF64%\Microsoft Visual Studio\Installer\vswhere.exe"

IF NOT DEFINED VSWHERE_EXE GOTO FALLBACK_SEARCH

FOR /F "usebackq tokens=*" %%I IN (`"%VSWHERE_EXE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) DO (
	IF EXIST "%%I\VC\Auxiliary\Build\vcvarsall.bat" SET "XVS_VCVARS_BAT=%%I\VC\Auxiliary\Build\vcvarsall.bat"
)

:FALLBACK_SEARCH
IF DEFINED XVS_VCVARS_BAT GOTO FOUND_VCVARS

IF NOT DEFINED PF64 GOTO CHECK_PF86
FOR /F "usebackq tokens=*" %%I IN (`dir /b /on /s "%PF64%\Microsoft Visual Studio\vcvarsall.bat" 2^>nul`) DO SET "XVS_VCVARS_BAT=%%I"

:CHECK_PF86
IF DEFINED XVS_VCVARS_BAT GOTO FOUND_VCVARS
IF NOT DEFINED PF86 GOTO CHECK_DONE
FOR /F "usebackq tokens=*" %%I IN (`dir /b /on /s "%PF86%\Microsoft Visual Studio\vcvarsall.bat" 2^>nul`) DO SET "XVS_VCVARS_BAT=%%I"

:CHECK_DONE
IF NOT DEFINED XVS_VCVARS_BAT (
	echo ERROR: Visual Studio C++ toolset not found!
	echo Please install Visual Studio 2026 or 2022 with the "Desktop development with C++" workload.
	exit /b 1
)

:FOUND_VCVARS
echo == Initializing Visual Studio Environment: %XVS_VCVARS_BAT% (%~1) ==
call "%XVS_VCVARS_BAT%" %~1

IF EXIST "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" (
	SET "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
)
IF EXIST "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja" (
	SET "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
)
IF EXIST "%LOCALAPPDATA%\bin\NASM" (
	SET "PATH=%LOCALAPPDATA%\bin\NASM;%PATH%"
)
IF EXIST "C:\Program Files\NASM" (
	SET "PATH=C:\Program Files\NASM;%PATH%"
)
IF EXIST "%LOCALAPPDATA%\Programs\Python\Python312" (
	SET "PATH=%LOCALAPPDATA%\Programs\Python\Python312;%LOCALAPPDATA%\Programs\Python\Python312\Scripts;%PATH%"
)
IF EXIST "C:\Program Files\Python312" (
	SET "PATH=C:\Program Files\Python312;C:\Program Files\Python312\Scripts;%PATH%"
)
IF EXIST "C:\Program Files\7-Zip" (
	SET "PATH=C:\Program Files\7-Zip;%PATH%"
)

SET XVS_VCVARS_BAT=
SET VSWHERE_EXE=
SET PF86=
SET PF64=

exit /b 0
