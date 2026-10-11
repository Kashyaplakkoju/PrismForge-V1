@echo off
setlocal
set "APP_DIR=%~dp0.."
if exist "%~dp0dist\adb\adb.exe" set "APP_DIR=%~dp0dist"
echo Installing PrismForge Studio development companion.
if not exist "%APP_DIR%\apk\prismforge-companion-debug.apk" (
  echo Development APK not found. Run scripts\build-android.ps1 first.
  exit /b 1
)
"%APP_DIR%\adb\adb.exe" install -r "%APP_DIR%\apk\prismforge-companion-debug.apk"
exit /b %ERRORLEVEL%
