@echo off
setlocal
cd /d "%~dp0"
set "LAB_PYTHON="
for /f "delims=" %%P in ('where python.exe 2^>nul') do if not defined LAB_PYTHON set "LAB_PYTHON=%%P"
if not defined LAB_PYTHON (
  echo Python is unavailable. This lab uses an existing Python installation.
  pause
  exit /b 1
)
"%LAB_PYTHON%" "%~dp0server.py" stop
set "LAB_STOP_EXIT=%errorlevel%"
if not "%LAB_STOP_EXIT%"=="0" pause
exit /b %LAB_STOP_EXIT%
