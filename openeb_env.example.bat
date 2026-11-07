@echo off
rem OpenEB 5.2 environment used by build.bat, run_tests.bat and the application launchers.
rem Copy this file as openeb_env.bat into the folder that holds the repository, "openeb" (OpenEB 5.2 built in
rem openeb\build) and "vcpkg-2024.11.16" (OpenEB's dependencies), e.g. C:\Dev\openeb_env.bat. Edit the two paths
rem below if your folders are elsewhere.
set "OPENEB_SRC_DIR=%~dp0openeb"
set "VCPKG_SRC_DIR=%~dp0vcpkg-2024.11.16"

if not exist "%OPENEB_SRC_DIR%\build\bin\Release\metavision_hal.dll" (
    echo openeb_env: OpenEB build tree not found in "%OPENEB_SRC_DIR%\build" 1>&2
    exit /b 1
)
if not exist "%VCPKG_SRC_DIR%\installed\x64-windows\bin" (
    echo openeb_env: vcpkg installed tree not found in "%VCPKG_SRC_DIR%" 1>&2
    exit /b 1
)

set "PATH=%OPENEB_SRC_DIR%\build\bin\Release;%VCPKG_SRC_DIR%\installed\x64-windows\bin;%PATH%"
set "MV_HAL_PLUGIN_PATH=%OPENEB_SRC_DIR%\build\lib\metavision\hal\plugins"
set "HDF5_PLUGIN_PATH=%OPENEB_SRC_DIR%\build\lib\hdf5\plugin"
set "PYTHONPATH=%OPENEB_SRC_DIR%\build\py3\Release;%PYTHONPATH%"
exit /b 0
