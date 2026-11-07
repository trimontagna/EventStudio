#pragma once

#include <filesystem>
#include <string>

namespace evs::app {

// Native Windows dialogs. `filter` uses the Win32 format with '|' separators, e.g.
// "Event files|*.raw;*.hdf5;*.dat|All files|*.*". Return an empty path when cancelled.
std::filesystem::path open_file_dialog(const char *title, const char *filter);
std::filesystem::path save_file_dialog(const char *title, const char *filter, const std::filesystem::path &initial,
                                       const char *default_extension);
// Folder picker (existing folders), opened at `initial` or its closest existing parent.
std::filesystem::path pick_folder_dialog(const char *title, const std::filesystem::path &initial);

std::string to_utf8(const std::filesystem::path &path);
std::filesystem::path from_utf8(const std::string &text);

// %USERPROFILE%\Videos\EventStudio\<prefix>_YYYYMMDD_HHMMSS<extension>
std::filesystem::path default_output_path(const char *prefix, const char *extension);
std::string human_bytes(double bytes);
std::string human_rate(double per_second, const char *unit); // "12.3 Mev/s"
std::string format_time_us(long long us);                    // "00:01:23.456"

void reveal_in_explorer(const std::filesystem::path &path);

} // namespace evs::app
