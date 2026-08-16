@echo off
echo ================================================================
echo JPEGView Build Environment: Installing Visual C++ ATL Components
echo ================================================================
echo.

SET VSWHERE_EXE=
IF EXIST "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
	SET "VSWHERE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
) ELSE IF EXIST "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" (
	SET "VSWHERE_EXE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)

SET VS_INSTALL_PATH=
IF DEFINED VSWHERE_EXE (
	FOR /F "usebackq tokens=*" %%I IN (`"%VSWHERE_EXE%" -latest -products * -property installationPath`) DO (
		SET "VS_INSTALL_PATH=%%I"
	)
)

IF NOT DEFINED VS_INSTALL_PATH (
	IF EXIST "%ProgramFiles%\Microsoft Visual Studio\2026" SET "VS_INSTALL_PATH=%ProgramFiles%\Microsoft Visual Studio\2026\Community"
	IF EXIST "%ProgramFiles%\Microsoft Visual Studio\18" SET "VS_INSTALL_PATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"
	IF EXIST "%ProgramFiles%\Microsoft Visual Studio\2022" SET "VS_INSTALL_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
)

echo Detected Visual Studio Path: "%VS_INSTALL_PATH%"
echo.
echo Requesting administrator privileges to install Microsoft.VisualStudio.Component.VC.ATL...

IF EXIST "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vs_installer.exe" (
	powershell -Command "Start-Process '%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vs_installer.exe' -ArgumentList 'modify --installPath \"%VS_INSTALL_PATH%\" --add Microsoft.VisualStudio.Component.VC.ATL --passive --norestart' -Verb RunAs -Wait"
) ELSE (
	powershell -Command "Start-Process '%TEMP%\vs_buildtools.exe' -ArgumentList '--installPath \"%VS_INSTALL_PATH%\" --add Microsoft.VisualStudio.Component.VC.ATL --passive --norestart' -Verb RunAs -Wait"
)

echo.
echo Installation step completed. You can now build JPEGView with full modern C++23, Direct2D, and WTL support.
pause
