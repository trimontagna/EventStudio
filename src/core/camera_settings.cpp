#include "core/camera_settings.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace evs {

namespace {
std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
} // namespace

bool KeyValueFile::load(const std::filesystem::path &path) {
    values.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

bool KeyValueFile::save(const std::filesystem::path &path, std::string *error) const {
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error)
                *error = "cannot write " + path.u8string();
            return false;
        }
        std::istringstream h(header);
        std::string line;
        while (std::getline(h, line))
            out << "# " << line << '\n';
        for (const auto &[k, v] : values)
            out << k << '=' << v << '\n';
        if (!out) {
            if (error)
                *error = "cannot write " + path.u8string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + path.u8string() + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::string> KeyValueFile::get(const std::string &key) const {
    const auto it = values.find(key);
    if (it == values.end())
        return std::nullopt;
    return it->second;
}

long long KeyValueFile::get_int(const std::string &key, long long fallback) const {
    const auto v = get(key);
    if (!v || v->empty())
        return fallback;
    char *end         = nullptr;
    const long long r = std::strtoll(v->c_str(), &end, 10);
    return end && *end == '\0' ? r : fallback;
}

bool KeyValueFile::get_bool(const std::string &key, bool fallback) const {
    const auto v = get(key);
    if (!v)
        return fallback;
    if (*v == "1" || *v == "true")
        return true;
    if (*v == "0" || *v == "false")
        return false;
    return fallback;
}

void KeyValueFile::set(const std::string &key, const std::string &value) {
    values[key] = value;
}

std::optional<TrailMode> trail_mode_from_string(const std::string &text) {
    for (TrailMode m : {TrailMode::Trail, TrailMode::StcCutTrail, TrailMode::StcKeepTrail})
        if (text == to_string(m))
            return m;
    return std::nullopt;
}

KeyValueFile CameraSettings::to_file(const std::string &serial) const {
    KeyValueFile f;
    f.header = "EventStudio: settings last applied to camera " + serial +
               "\nRestored when this camera is opened again. Local to this PC (not part of the repository).";
    for (const auto &[name, value] : biases)
        f.set_int("bias." + name, value);
    if (has_erc) {
        f.set_bool("erc.enabled", erc_enabled);
        f.set_int("erc.rate", erc_rate);
    }
    if (has_trail) {
        f.set_bool("trail.enabled", trail_enabled);
        f.set("trail.mode", to_string(trail_mode));
        f.set_int("trail.threshold_us", trail_threshold_us);
    }
    for (const auto &[channel, enabled] : trigger_in)
        f.set_bool("trigger_in." + channel, enabled);
    return f;
}

CameraSettings CameraSettings::from_file(const KeyValueFile &f) {
    CameraSettings s;
    for (const auto &[key, value] : f.values) {
        if (key.rfind("bias.", 0) == 0) {
            const long long v = f.get_int(key, LLONG_MIN);
            if (v != LLONG_MIN)
                s.biases[key.substr(5)] = static_cast<int>(v);
        } else if (key.rfind("trigger_in.", 0) == 0) {
            s.trigger_in[key.substr(11)] = f.get_bool(key, false);
        }
    }
    if (f.get("erc.enabled")) {
        s.has_erc     = true;
        s.erc_enabled = f.get_bool("erc.enabled", false);
        s.erc_rate    = static_cast<uint32_t>(std::max(0LL, f.get_int("erc.rate", 0)));
    }
    if (f.get("trail.enabled")) {
        s.has_trail          = true;
        s.trail_enabled      = f.get_bool("trail.enabled", false);
        s.trail_mode         = trail_mode_from_string(f.get("trail.mode").value_or("")).value_or(TrailMode::StcCutTrail);
        s.trail_threshold_us = static_cast<uint32_t>(std::max(0LL, f.get_int("trail.threshold_us", 0)));
    }
    return s;
}

std::string camera_settings_file_name(const std::string &serial) {
    std::string name = serial.empty() ? "unknown" : serial;
    for (char &c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            c = '_';
    return name + ".ini";
}

} // namespace evs
