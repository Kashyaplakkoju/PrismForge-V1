@echo off
setlocal
set "APP_DIR=%~dp0.."
if exist "%~dp0prismforge_source.dll" set "APP_DIR=%~dp0"
if exist "%~dp0dist\prismforge_source.dll" set "APP_DIR=%~dp0dist"
if not exist "%APP_DIR%\prismforge_vcam_installer.exe" (
  echo PrismForge camera installer not found. Build or unpack the development package first.
  exit /b 1
)
"%APP_DIR%\prismforge_vcam_installer.exe" register "%APP_DIR%\prismforge_source.dll" "%APP_DIR%\prismforge_cam.dll"
exit /b %ERRORLEVEL%
