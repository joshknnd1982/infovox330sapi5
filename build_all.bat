@echo off
rem Convenience wrapper. The build itself lives in tools\build_all.ps1, which configures
rem and builds both architectures, stages output\, and compiles the installer into dist\.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_all.ps1" %*
exit /b %ERRORLEVEL%
