@echo off
setlocal
powershell -NoProfile -File "%~dp0Build-Windows.ps1" %*
exit /b %errorlevel%
