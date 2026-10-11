@echo off
setlocal
where cl >nul 2>&1
if errorlevel 1 (
    echo Run this from an x64 Visual Studio Developer Command Prompt.
    exit /b 1
)
set "ROOT=%~dp0..\.."
set "DEPS=%ROOT%\build\deps\wx-build\lib\vc_x64_dll"
set "WX_INCLUDE=%ROOT%\build\deps\wx-src\include"
set "VCPKG=C:\vcpkg\installed\x64-windows"
if not exist "%DEPS%\wxbase33u.lib" (
    if exist "%VCPKG%\lib\wxbase33u.lib" (
        set "DEPS=%VCPKG%\lib"
        set "WX_INCLUDE=%VCPKG%\include"
    ) else (
        echo Neither build\deps nor vcpkg wxWidgets found.
        exit /b 1
    )
)
if not exist "%ROOT%\windows\x64\Tests" mkdir "%ROOT%\windows\x64\Tests"
for %%f in ("%DEPS%\wxbase*u.lib") do set "WX_BASE=%%~nxf"
pushd "%ROOT%\windows\x64\Tests"
cl /nologo /std:c++17 /EHsc /I"%ROOT%\windows\src" "%~dp0rotation_test.cpp" /Fe:rotation_test.exe
if errorlevel 1 goto failed
rotation_test.exe
if errorlevel 1 goto failed
cl /nologo /std:c++17 /EHsc /MD /DWIN32_LEAN_AND_MEAN /DwxUSE_GUI=0 /DWXUSINGDLL /D_UNICODE /DUNICODE /I"%WX_INCLUDE%" /I"%DEPS%\mswu" /I"%ROOT%\windows\src" "%~dp0presets_test.cpp" /Fe:presets_test.exe /link "%WX_BASE%" /LIBPATH:"%DEPS%"
if errorlevel 1 goto failed
set "PATH=%DEPS%;%VCPKG%\bin;%PATH%"
presets_test.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
