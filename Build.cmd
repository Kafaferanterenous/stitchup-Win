@echo off
setlocal
set ROOT=%~dp0
cd /d "%ROOT%"

set "MSVCROOT="
for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath') do set "MSVCROOT=%%i"
if not defined MSVCROOT (
  echo ERROR: Microsoft C++ Build Tools not found. Install them via the Visual Studio
  echo Build Tools installer and select the "Desktop development with C++" workload.
  exit /b 1
)
call "%MSVCROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1

if not exist dist mkdir dist
copy /y "build\Stitchup.exe" "dist\Stitchup.exe" >nul
copy /y "third_party\pdfium\bin\pdfium.dll" "dist\pdfium.dll" >nul
echo.
echo Built: dist\Stitchup.exe + dist\pdfium.dll  (portable PDF editor)
start "" "dist\Stitchup.exe" --self-test