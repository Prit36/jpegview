@echo off
echo ======================================================
echo JPEGView Build Environment: Installing Visual C++ ATL
echo ======================================================
echo.
echo Requesting administrator privileges...
powershell -Command "Start-Process '%TEMP%\vs_buildtools.exe' -ArgumentList '--installPath \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\" --add Microsoft.VisualStudio.Component.VC.ATL --passive --norestart' -Verb RunAs -Wait"
echo.
echo Installation completed. You can now build JPEGView with full modern C++20 and Direct2D support.
pause
