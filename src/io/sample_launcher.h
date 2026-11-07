#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace evs {

// OpenEB sample applications that can be started from EventStudio on the current source.
struct SampleInfo {
    std::string name;        // menu label
    std::string executable;  // file name, e.g. metavision_viewer.exe
    std::string description;
    bool takes_input_flag;   // "-i <file>" (otherwise the file is the first positional argument)
    bool supports_camera;    // can run without an input file (live camera)
    bool console;            // console tool: keep its console open
};

const std::vector<SampleInfo> &known_samples();

// Directory holding the OpenEB sample executables: $EVS_OPENEB_BIN_DIR, the directory configured at build time,
// or the directory of metavision_viewer.exe found in PATH. Empty if none is found.
std::filesystem::path find_samples_dir();

// Command line used to start a sample (exposed for testing). Empty input = live camera.
std::wstring build_sample_command_line(const SampleInfo &sample, const std::filesystem::path &samples_dir,
                                       const std::filesystem::path &input);

bool launch_sample(const SampleInfo &sample, const std::filesystem::path &samples_dir,
                   const std::filesystem::path &input, std::string *error);

} // namespace evs
