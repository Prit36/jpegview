@echo off

REM Routine to detect and initialize Visual Studio MSVC build environment
REM Supports Visual Studio 2026, Visual Studio 2022, and earlier installations

IF /I "%~1" EQU "" (
	echo ERROR: Pass in an [arch] to be passed to vcvarsall.bat (e.g. x64, x86)
	exit /b 1
)

SET XVS_VCVARS_BAT=
SET VSWHERE_EXE=

REM 1. Look for vswhere.exe in standard Visual Studio Installer directories
IF EXIST "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
	SET "VSWHERE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
) ELSE IF EXIST "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" (
	SET "VSWHERE_EXE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)

REM 2. Use vswhere to find the latest Visual Studio installation with VC Tools
IF DEFINED VSWHERE_EXE (
	FOR /F "usebackq tokens=*" %%I IN (`"%VSWHERE_EXE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) DO (
		IF EXIST "%%I\VC\Auxiliary\Build\vcvarsall.bat" (
			SET "XVS_VCVARS_BAT=%%I\VC\Auxiliary\Build\vcvarsall.bat"
		)
	)
)

REM 3. Fallback discovery if vswhere did not locate vcvarsall.bat
IF NOT DEFINED XVS_VCVARS_BAT (
	REM Check 64-bit Program Files (VS 2022 / VS 2026 default)
	IF EXIST "%ProgramFiles%\Microsoft Visual Studio" (
		FOR /F "usebackq tokens=*" %%I IN (`dir /b /on /s "%ProgramFiles%\Microsoft Visual Studio\vcvarsall.bat" 2^>nul`) DO (
			SET "XVS_VCVARS_BAT=%%I"
		)
	)
)

IF NOT DEFINED XVS_VCVARS_BAT (
	REM Check 32-bit Program Files (VS 2019 / VS 2017 / BuildTools fallback)
	IF EXIST "%ProgramFiles(x86)%\Microsoft Visual Studio" (
		FOR /F "usebackq tokens=*" %%I IN (`dir /b /on /s "%ProgramFiles(x86)%\Microsoft Visual Studio\vcvarsall.bat" 2^>nul`) DO (
			SET "XVS_VCVARS_BAT=%%I"
		)
	)
)

IF NOT DEFINED XVS_VCVARS_BAT (
	echo ERROR: Visual Studio C++ toolset not found!
	echo Please install Visual Studio 2026 or 2022 with the "Desktop development with C++" workload.
	exit /b 1
)

echo == Initializing Visual Studio Environment: %XVS_VCVARS_BAT% (%~1) ==
call "%XVS_VCVARS_BAT%" %~1

SET XVS_VCVARS_BAT=
SET VSWHERE_EXE=

exit /b 0
