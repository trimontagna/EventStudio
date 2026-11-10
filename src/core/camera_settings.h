#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "core/filters.h"

namespace evs {

// "key=value" text file (UTF-8), one entry per line, '#' starts a comment line. Keys are sorted when saved.
class KeyValueFile {
public:
    bool load(const std::filesystem::path &path); // false when missing / unreadable (the content is then empty)
    // Creates the parent folders; writes a temporary file then renames it, so a crash never leaves half a file.
    bool save(const std::filesystem::path &path, std::string *error) const;

    std::optional<std::string> get(const std::string &key) const;
    long long get_int(const std::string &key, long long fallback) const;
    bool get_bool(const std::string &key, bool fallback) const;
    void set(const std::string &key, const std::string &value);
    void set_int(const std::string &key, long long value) { set(key, std::to_string(value)); }
    void set_bool(const std::string &key, bool value) { set(key, value ? "1" : "0"); }

    std::map<std::string, std::string> values;
    std::string header; // written as '#' comment lines at the top
};

// Settings EventStudio gave a camera, restored the next time a camera with the same serial is opened (the sensor
// forgets them when it is closed / unplugged).
struct CameraSettings {
    std::map<std::string, int> biases;
    bool has_erc = false, erc_enabled = false;
    uint32_t erc_rate = 0; // events / s
    bool has_trail = false, trail_enabled = false;
    TrailMode trail_mode        = TrailMode::StcCutTrail;
    uint32_t trail_threshold_us = 0;
    std::map<std::string, bool> trigger_in; // channel name -> enabled

    KeyValueFile to_file(const std::string &serial) const;
    static CameraSettings from_file(const KeyValueFile &file);
};

// File name for a camera serial: characters outside [A-Za-z0-9._-] become '_' ("unknown" when empty).
std::string camera_settings_file_name(const std::string &serial);

std::optional<TrailMode> trail_mode_from_string(const std::string &text);

} // namespace evs
