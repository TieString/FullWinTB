@echo off
setlocal
pushd "%~dp0"

if not defined VSCMD_VER (
    for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        call "%%V\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
where cl.exe >nul 2>&1 || (
    echo MSVC is required. Run this script from an x64 Native Tools Command Prompt.
    exit /b 1
)
if not defined FULLWINTB_BUILD_DIR set "FULLWINTB_BUILD_DIR=build-out"
cmake -S . -B "%FULLWINTB_BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release %* || exit /b 1
cmake --build "%FULLWINTB_BUILD_DIR%"
exit /b %errorlevel%
