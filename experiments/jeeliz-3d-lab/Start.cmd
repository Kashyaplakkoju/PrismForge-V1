@echo off
setlocal
cd /d "%~dp0"
set "LAB_PYTHONW="
for /f "delims=" %%P in ('where pythonw.exe 2^>nul') do if not defined LAB_PYTHONW set "LAB_PYTHONW=%%P"
if defined LAB_PYTHONW (
  start "" /b "%LAB_PYTHONW%" "%~dp0server.py" start --open-browser
  exit /b 0
)
set "LAB_PYTHON="
for /f "delims=" %%P in ('where python.exe 2^>nul') do if not defined LAB_PYTHON set "LAB_PYTHON=%%P"
if not defined LAB_PYTHON (
  echo Python is unavailable. This lab uses an existing Python installation.
  pause
  exit /b 1
)
start "" /b "%LAB_PYTHON%" "%~dp0server.py" start --open-browser
exit /b 0
