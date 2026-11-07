@echo off
rem Configures, builds and tests EventStudio against the OpenEB 5.2 build tree (see C:\Dev\openeb_env.bat).
rem Usage: build.bat [target]   (default: all)
setlocal
call "%~dp0..\openeb_env.bat" || exit /b 1

set "SRC=%~dp0."
set "BUILD=%~dp0build"
set "OPENEB_BUILD=%OPENEB_SRC_DIR%\build"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=ALL_BUILD"

if not exist "%BUILD%\CMakeCache.txt" (
    cmake -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_SRC_DIR%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_MANIFEST_MODE=OFF -DVCPKG_TARGET_TRIPLET=x64-windows ^
        -DCMAKE_BUILD_TYPE=Release -DOPENEB_BUILD_DIR="%OPENEB_BUILD%" ^
        -DMetavisionSDK_DIR="%OPENEB_BUILD%\generated\share\cmake\MetavisionSDKCMakePackagesFilesDir" ^
        -Dhdf5_ecf_DIR="%OPENEB_BUILD%\sdk\modules\stream\cpp\3rdparty\hdf5_ecf" ^
        -DMetavisionHAL_DIR="%OPENEB_BUILD%\generated\share\cmake\MetavisionHALCMakePackagesFilesDir" ^
        -DEVS_TEST_DATA_DIR="C:/Dev/OpenEB_tests/openeb" || exit /b 1
)
cmake --build "%BUILD%" --config Release --parallel --target %TARGET% || exit /b 1
echo EVENTSTUDIO BUILD OK
