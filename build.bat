@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"
"D:\Softwares\CLion 2025.1.4\bin\ninja\win\x64\ninja.exe" -C cmake-build-release-visual-studio
echo.
echo === Build done, running test ===
pwsh -NoProfile -Command "cd '%~dp0cmake-build-release-visual-studio'; .\corehold_test.exe"
