@echo off
setlocal EnableDelayedExpansion
rem Double-click to build the release APK. Result: release\QuestProCameraService.apk
rem plus its SHA-256, ready to attach to a GitHub release. Full log: release\build.log
cd /d "%~dp0"
title Quest Pro Camera Service - release build
set "OUT=%~dp0release"
if not exist "%OUT%" mkdir "%OUT%"
set "LOG=%OUT%\build.log"

call :find_jdk
if not defined JAVA_HOME (
    echo Java 17 or newer was not found. Install Android Studio, or a JDK from adoptium.net, then run this again.
    goto fail
)
echo Using Java: %JAVA_HOME%
echo Building the headset binaries and the release APK. The first build takes a few minutes...

call "%~dp0gradlew.bat" assembleRelease --console=plain > "%LOG%" 2>&1
if errorlevel 1 (
    echo.
    echo The build failed. Last lines of release\build.log:
    echo ----------------------------------------------------------------
    powershell -NoProfile -Command "Get-Content -LiteralPath '%LOG%' -Tail 15"
    echo ----------------------------------------------------------------
    goto fail
)

set "APK=app\build\outputs\apk\release\app-release.apk"
if not exist "%APK%" (
    echo The build finished but %APK% is missing. See release\build.log.
    goto fail
)
set "VERSION=unknown"
for /f "tokens=2 delims== " %%v in ('findstr /r /c:"versionName *=" app\build.gradle.kts') do set "VERSION=%%~v"
copy /y "%APK%" "%OUT%\QuestProCameraService.apk" >nul
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%OUT%\QuestProCameraService.apk" SHA256') do if not defined HASH set "HASH=%%h"
> "%OUT%\QuestProCameraService.apk.sha256.txt" echo %HASH%  QuestProCameraService.apk (version %VERSION%)

echo.
echo Done: release\QuestProCameraService.apk (version %VERSION%)
echo SHA-256: %HASH%
start "" explorer "%OUT%"
pause
exit /b 0

:find_jdk
rem Gradle needs Java 17+. Try JAVA_HOME, Android Studio's bundled JDK, then installed JDKs, newest first.
set "CANDIDATES="
if defined JAVA_HOME set "CANDIDATES="%JAVA_HOME%""
set "JAVA_HOME="
for %%d in (%CANDIDATES% "%ProgramFiles%\Android\Android Studio\jbr" "%LOCALAPPDATA%\Programs\Android Studio\jbr") do (
    if not defined JAVA_HOME call :try_jdk "%%~d"
)
rem One dir per folder: dir with several patterns prints nothing if any folder is missing.
for %%r in ("%ProgramFiles%\Java" "%ProgramFiles%\Eclipse Adoptium" "%ProgramFiles%\Microsoft") do (
    if exist "%%~r" for /f "delims=" %%d in ('dir /b /ad /o-n "%%~r\jdk*" 2^>nul') do (
        if not defined JAVA_HOME call :try_jdk "%%~r\%%d"
    )
)
exit /b 0

:try_jdk
if not exist "%~1\bin\java.exe" exit /b 1
set "SPEC="
for /f "tokens=2 delims==" %%v in ('^""%~1\bin\java.exe" -XshowSettings:properties -version 2^>^&1 ^| findstr /c:"java.specification.version"^"') do set "SPEC=%%v"
set "SPEC=%SPEC: =%"
if not defined SPEC exit /b 1
for /f "tokens=1 delims=." %%m in ("%SPEC%") do set "MAJOR=%%m"
if "%MAJOR%"=="1" exit /b 1
if %MAJOR% LSS 17 exit /b 1
set "JAVA_HOME=%~1"
exit /b 0

:fail
echo.
echo Build did not finish.
pause
exit /b 1
