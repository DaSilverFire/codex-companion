@echo off
setlocal EnableExtensions

cd /d "%~dp0\..\.."
if errorlevel 1 goto :failure

set "LOG=%USERPROFILE%\Desktop\Codex Companion 0.3.5 Rebuild.log"
set "NATIVE=windows-native\tools\native-command.cmd"
set "BUILD=work\build\windows-msvc-release"
set "STAGE=%BUILD%\windows-native\stage\Release"
set "RELEASE_ROOT=work\release\candidate-v035-current\portable"
set "CURRENT=%RELEASE_ROOT%\Codex Companion"
set "INCOMING=%RELEASE_ROOT%\Codex Companion.incoming"
set "PREVIOUS=%RELEASE_ROOT%\Codex Companion.previous"
set "COMPANION_NINJA_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

>"%LOG%" echo Codex Companion 0.3.5 native Qt rebuild
>>"%LOG%" echo Started %DATE% %TIME%
>>"%LOG%" echo Source %CD%

echo [1/6] Configuring native Qt release...
call "%NATIVE%" cmake --preset windows-msvc-release "-DCMAKE_MAKE_PROGRAM=%COMPANION_NINJA_EXE%" >>"%LOG%" 2>&1
if errorlevel 1 goto :failure

echo [2/6] Building focused account and mobile tests...
call "%NATIVE%" cmake --build --preset windows-msvc-release --target ^
  companion_codex_account_profiles ^
  companion_codex_discovery ^
  companion_codex_continuation ^
  companion_mobile_pairing ^
  companion_mobile_relay_pairing_bootstrap ^
  companion_ui_settings_view_model ^
  companion_foundation_app_lifecycle ^
  companion_packaging_cutover_automation >>"%LOG%" 2>&1
if errorlevel 1 goto :failure

echo [3/6] Running focused tests...
for %%T in (
  codex.account-profiles
  codex.discovery
  codex.continuation
  mobile.pairing
  mobile.relay-pairing-bootstrap
  ui.settings-view-model
  foundation.app-lifecycle
  packaging.cutover-automation
) do (
  call "%NATIVE%" ctest --test-dir "%BUILD%" --output-on-failure -R "^%%T$" >>"%LOG%" 2>&1
  if errorlevel 1 goto :failure
)

echo [4/6] Building and verifying the deployed app tree...
call "%NATIVE%" cmake --build --preset windows-msvc-release --target companion_stage_smoke >>"%LOG%" 2>&1
if errorlevel 1 goto :failure
if not exist "%STAGE%\bin\CodexCompanion.exe" (
  >>"%LOG%" echo Missing staged executable: %STAGE%\bin\CodexCompanion.exe
  goto :failure
)

echo [5/6] Preparing the atomic replacement...
if exist "%INCOMING%" rmdir /s /q "%INCOMING%" >>"%LOG%" 2>&1
robocopy "%STAGE%" "%INCOMING%" /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS >>"%LOG%" 2>&1
if errorlevel 8 goto :failure
if not exist "%INCOMING%\bin\CodexCompanion.exe" (
  >>"%LOG%" echo Incoming portable app is incomplete.
  goto :failure
)

echo [6/6] Replacing only Codex Companion and launching it...
call :stopCompanionForReplacement
if errorlevel 1 goto :failure
if exist "%PREVIOUS%" (
  rmdir /s /q "%PREVIOUS%" >>"%LOG%" 2>&1
  if errorlevel 1 goto :failure
)
if exist "%CURRENT%" (
  move "%CURRENT%" "%PREVIOUS%" >>"%LOG%" 2>&1
  if errorlevel 1 goto :restore
)
move "%INCOMING%" "%CURRENT%" >>"%LOG%" 2>&1
if errorlevel 1 goto :restore

start "" "%CURRENT%\bin\CodexCompanion.exe"
>>"%LOG%" echo Completed %DATE% %TIME%
echo.
echo Codex Companion 0.3.5 rebuilt and launched successfully.
echo The previous portable app is retained at:
echo   %PREVIOUS%
echo Log:
echo   %LOG%
pause
exit /b 0

:stopCompanionForReplacement
taskkill /IM CodexCompanion.exe /T /F >>"%LOG%" 2>&1
if not errorlevel 1 goto :waitForCompanionToStop

tasklist /FI "IMAGENAME eq CodexCompanion.exe" /NH 2>nul | "%SystemRoot%\System32\find.exe" /I "CodexCompanion.exe" >nul
if errorlevel 1 (
  >>"%LOG%" echo Codex Companion was not running; continuing replacement.
  exit /b 0
)

>>"%LOG%" echo Codex Companion is still running after taskkill failed.
exit /b 1

:waitForCompanionToStop
for /L %%I in (1,1,10) do (
  tasklist /FI "IMAGENAME eq CodexCompanion.exe" /NH 2>nul | "%SystemRoot%\System32\find.exe" /I "CodexCompanion.exe" >nul
  if errorlevel 1 (
    >>"%LOG%" echo Codex Companion stopped; continuing replacement.
    exit /b 0
  )
  "%SystemRoot%\System32\ping.exe" -n 2 127.0.0.1 >nul
)

>>"%LOG%" echo Codex Companion did not stop within the replacement timeout.
exit /b 1

:restore
>>"%LOG%" echo Replacement failed; attempting rollback.
if not exist "%CURRENT%" if exist "%PREVIOUS%" move "%PREVIOUS%" "%CURRENT%" >>"%LOG%" 2>&1

:failure
set "EXIT_CODE=%ERRORLEVEL%"
if "%EXIT_CODE%"=="0" set "EXIT_CODE=1"
>>"%LOG%" echo Failed %DATE% %TIME% with exit code %EXIT_CODE%.
echo.
echo Codex Companion was not replaced because build, test, staging, or copy failed.
echo Review this log:
echo   %LOG%
pause
exit /b %EXIT_CODE%
