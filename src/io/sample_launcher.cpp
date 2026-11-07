#include "io/sample_launcher.h"

#include <cstdlib>

#include <windows.h>

namespace evs {

const std::vector<SampleInfo> &known_samples() {
    static const std::vector<SampleInfo> samples = {
        {"Viewer", "metavision_viewer.exe", "Displays events from a camera or a file", true, true, false},
        {"Time surface", "metavision_time_surface.exe", "Displays the time surface of the events", true, true, false},
        {"Events integration", "metavision_events_integration.exe", "Gray-level images from integrated events", true,
         true, false},
        {"Event frame generation", "metavision_event_frame_generation.exe",
         "Histogram and difference event frames (file only)", true, false, false},
        {"Filtering", "metavision_filtering.exe", "ROI and polarity filters", true, true, false},
        {"Composed viewer", "metavision_composed_viewer.exe", "Unfiltered and filtered events side by side", true, true,
         false},
        {"SDK get started", "metavision_sdk_get_started.exe", "Step-by-step example with statistics", false, true,
         false},
        {"Active pixel detection", "metavision_active_pixel_detection.exe", "Detects and masks active pixels", true,
         true, false},
        {"Dummy radar", "metavision_dummy_radar.exe", "Radar-like display of the most active object", true, true,
         false},
        {"Camera stream slicer", "metavision_camera_stream_slicer.exe", "Slices the stream by count or duration", true,
         true, false},
        {"File info", "metavision_file_info.exe", "Prints information about the file", true, false, true},
    };
    return samples;
}

std::filesystem::path find_samples_dir() {
    const auto has_viewer = [](const std::filesystem::path &dir) {
        std::error_code ec;
        return !dir.empty() && std::filesystem::exists(dir / "metavision_viewer.exe", ec);
    };
    if (const char *env = std::getenv("EVS_OPENEB_BIN_DIR"); env && has_viewer(env))
        return env;
#ifdef EVS_OPENEB_BIN_DIR
    if (has_viewer(EVS_OPENEB_BIN_DIR))
        return EVS_OPENEB_BIN_DIR;
#endif
    wchar_t buffer[MAX_PATH];
    if (SearchPathW(nullptr, L"metavision_viewer.exe", nullptr, MAX_PATH, buffer, nullptr) > 0)
        return std::filesystem::path(buffer).parent_path();
    return {};
}

std::wstring build_sample_command_line(const SampleInfo &sample, const std::filesystem::path &samples_dir,
                                       const std::filesystem::path &input) {
    const std::wstring exe = (samples_dir / sample.executable).wstring();
    std::wstring cmd       = L"\"" + exe + L"\"";
    if (!input.empty())
        cmd += (sample.takes_input_flag ? L" -i \"" : L" \"") + input.wstring() + L"\"";
    if (sample.console)
        cmd = L"cmd.exe /k \"" + cmd + L"\"";
    return cmd;
}

bool launch_sample(const SampleInfo &sample, const std::filesystem::path &samples_dir,
                   const std::filesystem::path &input, std::string *error) {
    if (samples_dir.empty()) {
        if (error)
            *error = "OpenEB samples not found (set EVS_OPENEB_BIN_DIR)";
        return false;
    }
    if (input.empty() && !sample.supports_camera) {
        if (error)
            *error = sample.name + " needs an input file";
        return false;
    }
    std::wstring cmd = build_sample_command_line(sample, samples_dir, input);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = samples_dir.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si,
                        &pi)) {
        if (error)
            *error = "Cannot start " + sample.executable + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace evs
