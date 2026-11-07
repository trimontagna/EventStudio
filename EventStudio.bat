@echo off
rem Starts EventStudio with the OpenEB 5.2 runtime environment (DLLs, HAL plugins).
rem Optional argument: a recording to open (.raw, .hdf5, .dat).
setlocal
call "%~dp0..\openeb_env.bat" || exit /b 1
cd /d "%~dp0"
start "" "%~dp0build\Release\EventStudio.exe" %*
