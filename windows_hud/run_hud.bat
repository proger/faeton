@echo off
setlocal

rem Usage:
rem   run_hud.bat

call :ensure_msvc_env
if errorlevel 1 (
  echo Could not initialize MSVC environment.
  exit /b 4
)

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "EXE=%SCRIPT_DIR%\faeton.exe"

if not exist "%EXE%" (
  call "%SCRIPT_DIR%\build.bat"
  if errorlevel 1 exit /b 6
)

if not exist "%EXE%" (
  exit /b 6
)

taskkill /F /IM faeton.exe >nul 2>nul

echo Starting HUD...
"%EXE%"
set "RC=%ERRORLEVEL%"

exit /b %RC%

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
