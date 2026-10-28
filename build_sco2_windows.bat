@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :build_msvc

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
            goto :build_msvc
        )
    )
)

if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    goto :build_msvc
)

echo [ERROR] Visual Studio 2022 Build Tools (MSVC x64) not found.
exit /b 1

:build_msvc
cd /d "%~dp0"
cl /nologo /O2 /EHsc /utf-8 /std:c++17 /MT sco2.cpp user32.lib gdi32.lib shell32.lib /link /SUBSYSTEM:WINDOWS /Fe:sco2.exe
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)
if exist sco2.obj del /q sco2.obj
echo [OK] Built sco2.exe successfully.
exit /b 0
