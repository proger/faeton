@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

call :ensure_msvc_env
if errorlevel 1 exit /b 2

where cl >nul 2>nul
if errorlevel 1 (
  echo cl.exe not found in PATH.
  echo Could not initialize MSVC environment.
  exit /b 2
)

pushd "%SCRIPT_DIR%"
echo Building faeton.exe...
set "RES_OBJ="
if exist "%SCRIPT_DIR%\faeton.rc" (
  rc /nologo /fo "%SCRIPT_DIR%\faeton.res" "%SCRIPT_DIR%\faeton.rc"
  if errorlevel 1 (
    popd
    exit /b 3
  )
  set "RES_OBJ=%SCRIPT_DIR%\faeton.res"
)
cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE main.cpp %RES_OBJ% user32.lib gdi32.lib d2d1.lib dwrite.lib shell32.lib /Fe:faeton.exe
set "BUILD_RC=%ERRORLEVEL%"
popd
if not "%BUILD_RC%"=="0" exit /b 3

echo Build complete: %SCRIPT_DIR%\faeton.exe
exit /b 0

:ensure_msvc_env
where cl >nul 2>nul
if not errorlevel 1 exit /b 0

set "VCVARS="
if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
  )
)

if not defined VCVARS exit /b 1
call "%VCVARS%" >nul 2>nul
where cl >nul 2>nul
if errorlevel 1 exit /b 1
exit /b 0
