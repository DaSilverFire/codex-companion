@echo off
setlocal

set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
  echo Visual Studio developer command file not found: %VSDEVCMD% 1>&2
  exit /b 2
)

call "%VSDEVCMD%" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 exit /b %errorlevel%

if not defined COMPANION_NINJA_EXE (
  set "COMPANION_NINJA_EXE=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)
if not exist "%COMPANION_NINJA_EXE%" (
  echo Visual Studio Ninja executable is missing: %COMPANION_NINJA_EXE% 1>&2
  exit /b 2
)
"%COMPANION_NINJA_EXE%" --version >nul 2>nul
if errorlevel 1 (
  echo Visual Studio Ninja executable could not run: %COMPANION_NINJA_EXE% 1>&2
  exit /b 2
)
for %%I in ("%COMPANION_NINJA_EXE%") do set "PATH=%%~dpI;%PATH%"

if not defined QT_ROOT set "QT_ROOT=C:\Qt\6.11.1\msvc2022_64"
if not exist "%QT_ROOT%\bin\qtpaths.exe" (
  echo Qt 6.11.1 MSVC root is invalid: %QT_ROOT% 1>&2
  exit /b 2
)
set "QT_WEBSOCKETS_OVERLAY=%~dp0..\.deps\qt-6.11.1"
if not exist "%QT_WEBSOCKETS_OVERLAY%\bin\Qt6WebSockets.dll" (
  echo Qt WebSockets release runtime is missing. Run windows-native\tools\bootstrap-dependencies.ps1. 1>&2
  exit /b 2
)
if not exist "%QT_WEBSOCKETS_OVERLAY%\bin\Qt6WebSocketsd.dll" (
  echo Qt WebSockets debug runtime is missing. Run windows-native\tools\bootstrap-dependencies.ps1. 1>&2
  exit /b 2
)
if not exist "%QT_WEBSOCKETS_OVERLAY%\plugins\imageformats\qwebp.dll" (
  echo Qt WebP release plugin is missing. Run windows-native\tools\bootstrap-dependencies.ps1. 1>&2
  exit /b 2
)
if not exist "%QT_WEBSOCKETS_OVERLAY%\plugins\imageformats\qwebpd.dll" (
  echo Qt WebP debug plugin is missing. Run windows-native\tools\bootstrap-dependencies.ps1. 1>&2
  exit /b 2
)
set "PATH=%QT_WEBSOCKETS_OVERLAY%\bin;%QT_ROOT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%QT_WEBSOCKETS_OVERLAY%\plugins;%QT_ROOT%\plugins"

set "VCPKG_BASELINE=a9f0cd0345fb29cd227d802f1fd1917c28f8e5a3"
set "VCPKG_ROOT=%~dp0..\.deps\vcpkg\%VCPKG_BASELINE%\source"
set "VCPKG_INSTALLED_DIR=%~dp0..\.deps\vcpkg\%VCPKG_BASELINE%\installed"
set "FOUNDRY_ROOT=%~dp0..\.deps\foundry-local\1.2.1"

for %%T in (cl.exe link.exe rc.exe cmake.exe ninja.exe) do (
  where %%T >nul 2>nul
  if errorlevel 1 (
    echo Required native build command is unavailable: %%T 1>&2
    exit /b 2
  )
)

if "%~1"=="" (
  echo Usage: native-command.cmd command [arguments...] 1>&2
  exit /b 2
)

if /I "%~1"=="cmake" goto validate_dependencies
if /I "%~1"=="cmake.exe" goto validate_dependencies
if /I "%~1"=="ctest" goto validate_dependencies
if /I "%~1"=="ctest.exe" goto validate_dependencies
goto run_command

:validate_dependencies
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo Pinned vcpkg bootstrap is missing. Run windows-native\tools\bootstrap-foundry-local.ps1. 1>&2
  exit /b 2
)
if not exist "%VCPKG_INSTALLED_DIR%\vcpkg\status" (
  echo Pinned vcpkg installed status is missing. Run windows-native\tools\bootstrap-foundry-local.ps1. 1>&2
  exit /b 2
)
if not exist "%~dp0..\.deps\vcpkg\%VCPKG_BASELINE%\bootstrap-stamp.json" (
  echo Pinned vcpkg bootstrap stamp is missing. Run windows-native\tools\bootstrap-foundry-local.ps1. 1>&2
  exit /b 2
)
if not exist "%FOUNDRY_ROOT%\bootstrap-stamp.json" (
  echo Foundry Local bootstrap stamp is missing. Run windows-native\tools\bootstrap-foundry-local.ps1. 1>&2
  exit /b 2
)
if not exist "%FOUNDRY_ROOT%\source\sdk\cpp\src\model.cpp" (
  echo Foundry Local source payload is missing. Run windows-native\tools\bootstrap-foundry-local.ps1. 1>&2
  exit /b 2
)
if /I "%~1"=="cmake" if /I "%~2"=="--preset" goto run_cmake_configure
if /I "%~1"=="cmake.exe" if /I "%~2"=="--preset" goto run_cmake_configure
goto run_command

:run_cmake_configure
%* "-DCMAKE_MAKE_PROGRAM=%COMPANION_NINJA_EXE%"
exit /b %errorlevel%

:run_command
%*
exit /b %errorlevel%
