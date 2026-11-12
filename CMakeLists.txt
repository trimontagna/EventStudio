cmake_minimum_required(VERSION 3.20)
project(EventStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /MP /Zi)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF) # PDBs for Release builds (crash / hang diagnostics)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# OpenEB from the build tree (see build.bat): MetavisionSDK_DIR / MetavisionHAL_DIR are passed on the command line.
find_package(MetavisionSDK COMPONENTS base core stream REQUIRED)
find_package(MetavisionHAL REQUIRED)
find_package(OpenCV COMPONENTS core imgproc imgcodecs videoio REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# Also usable as a subproject (add_subdirectory) providing evs_core, evs_io and imgui; the tests are then off.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_evs_top_level ON)
else()
    set(_evs_top_level OFF)
endif()
option(EVS_BUILD_TESTS "Build the unit tests" ${_evs_top_level})
option(EVS_BUILD_APP "Build the EventStudio application" ON)

# ---------------------------------------------------------------- core: no camera, no GUI
add_library(evs_core STATIC
    src/core/filters.cpp
    src/core/polygon.cpp
    src/core/mask_set.cpp
    src/core/frame_renderer.cpp
    src/core/rate_meter.cpp
    src/core/evt_encoder.cpp
    src/core/raw_file_writer.cpp
    src/core/hot_pixels.cpp
    src/core/ptv_planner.cpp
    src/core/latency_monitor.cpp
    src/core/rate_history.cpp
    src/core/slider_detent.cpp
    src/core/camera_settings.cpp
)
target_include_directories(evs_core PUBLIC src)
target_link_libraries(evs_core PUBLIC MetavisionSDK::base)

# ---------------------------------------------------------------- io: OpenEB camera / file access
add_library(evs_io STATIC
    src/io/live_camera.cpp
    src/io/file_player.cpp
    src/io/event_file_source.cpp
    src/io/offline_processor.cpp
    src/io/sample_launcher.cpp
)
target_include_directories(evs_io PUBLIC src)
target_link_libraries(evs_io PUBLIC evs_core MetavisionSDK::stream MetavisionSDK::core Metavision::HAL
                             opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio)
if(OPENEB_BUILD_DIR)
    file(TO_CMAKE_PATH "${OPENEB_BUILD_DIR}" _openeb_build_dir) # forward slashes: used in a C++ string literal
    target_compile_definitions(evs_io PRIVATE EVS_OPENEB_BIN_DIR="${_openeb_build_dir}/bin/Release")
endif()

# ---------------------------------------------------------------- Dear ImGui (vendored, v1.91.5-docking)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------- application
# (src/app/platform.cpp also serves other ImGui tools built on these libraries, e.g. EBIV Studio)
if(EVS_BUILD_APP)
    add_executable(EventStudio WIN32
        src/app/main.cpp
        src/app/app.cpp
        src/app/viewport.cpp
        src/app/mask_editor.cpp
        src/app/panels.cpp
        src/app/platform.cpp
    )
endif()
if(EVS_BUILD_APP)
    target_link_libraries(EventStudio PRIVATE evs_io imgui comdlg32 shell32 ole32)
    if(MSVC)
        # WIN32 executable but keep a regular main()
        target_link_options(EventStudio PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# ---------------------------------------------------------------- tests
if(EVS_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_executable(evs_tests
        tests/filters_test.cpp
        tests/polygon_test.cpp
        tests/mask_set_test.cpp
        tests/frame_renderer_test.cpp
        tests/rate_meter_test.cpp
        tests/ptv_test.cpp
        tests/latency_monitor_test.cpp
        tests/rate_history_test.cpp
        tests/slider_detent_test.cpp
        tests/camera_settings_test.cpp
        tests/evt_encoder_test.cpp
        tests/offline_processor_test.cpp
        tests/file_player_test.cpp
        tests/test_utils.cpp
    )
    target_link_libraries(evs_tests PRIVATE evs_io GTest::gtest GTest::gtest_main)
    target_compile_definitions(evs_tests PRIVATE
        EVS_TEST_DATA_DIR="${EVS_TEST_DATA_DIR}")
    include(GoogleTest)
    gtest_discover_tests(evs_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
endif()
