@echo off
rem Starts EventStudio with the OpenEB 5.2 runtime environment (DLLs, HAL plugins).
rem Optional argument: a recording to open (.raw, .hdf5, .dat).
setlocal
call "%~dp0..\openeb_env.bat" || exit /b 1
cd /d "%~dp0"
rem Waits for EventStudio: double-clicked, this window is hidden while it runs and closed when it ends
rem (src/app/launch_console.cpp). From a terminal, the prompt comes back when EventStudio closes.
"%~dp0build\Release\EventStudio.exe" %*
