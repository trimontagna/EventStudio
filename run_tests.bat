@echo off
rem Runs the EventStudio unit tests in the OpenEB environment. Extra arguments go to the test binary
rem (e.g. run_tests.bat --gtest_filter=Polygon.*).
setlocal
call "%~dp0..\openeb_env.bat" || exit /b 1
"%~dp0build\Release\evs_tests.exe" %*
