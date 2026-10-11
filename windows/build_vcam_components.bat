@echo off
setlocal
rem Build only. This script never registers or installs a camera.
set "SCRIPT_DIR=%~dp0"
if defined VSCMD_VER goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio C++ build tools are required.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
:build
if not exist "%SCRIPT_DIR%..\dist" mkdir "%SCRIPT_DIR%..\dist"
if not exist "%SCRIPT_DIR%x64\CameraComponents" mkdir "%SCRIPT_DIR%x64\CameraComponents"
pushd "%SCRIPT_DIR%x64\CameraComponents"
cl /nologo /O2 /std:c++17 /EHsc /MD /LD "%SCRIPT_DIR%src\mfsource\DllMain.cpp" "%SCRIPT_DIR%src\mfsource\MediaSource.cpp" "%SCRIPT_DIR%src\mfsource\MediaStream.cpp" "%SCRIPT_DIR%src\mfsource\FrameServer.cpp" /Fe:prismforge_source.dll /link /DEF:"%SCRIPT_DIR%src\mfsource\prismforge_source.def" mfplat.lib mf.lib mfuuid.lib mfsensorgroup.lib ole32.lib advapi32.lib
if errorlevel 1 goto failed
cl /nologo /O2 /std:c++17 /EHsc /MD "%SCRIPT_DIR%src\installer\vcam_installer.cpp" /Fe:prismforge_vcam_installer.exe /link /MANIFEST /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" /MANIFEST:EMBED mfplat.lib mf.lib mfuuid.lib mfsensorgroup.lib ole32.lib advapi32.lib user32.lib
if errorlevel 1 goto failed
copy /y prismforge_source.dll "%SCRIPT_DIR%..\dist\prismforge_source.dll" >nul
if errorlevel 1 goto failed
copy /y prismforge_vcam_installer.exe "%SCRIPT_DIR%..\dist\prismforge_vcam_installer.exe" >nul
if errorlevel 1 goto failed
popd
echo Camera components built successfully. Nothing was installed.
exit /b 0
:failed
popd
exit /b 1
