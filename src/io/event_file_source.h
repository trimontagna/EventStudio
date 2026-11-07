#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "core/types.h"

namespace Metavision {
class Device;
class EventFileReader;
class RAWEventFileReader;
} // namespace Metavision

namespace evs {

// An opened event file (RAW, HDF5 or DAT) read through the OpenEB EventFileReader API.
class EventFileSource {
public:
    // With time_shift, the first event of the recording is at t ~ 0 (as in the Metavision tools).
    static std::unique_ptr<EventFileSource> open(const std::filesystem::path &path, bool time_shift,
                                                 std::string *error);
    ~EventFileSource();

    Metavision::EventFileReader &reader() { return *reader_; }
    // nullptr when the file is not a RAW file.
    Metavision::RAWEventFileReader *raw_reader() { return raw_reader_; }

    const std::filesystem::path &path() const { return path_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // "EVT3", "EVT21", "EVT2", ... for RAW files; "HDF5" / "DAT" otherwise.
    const std::string &encoding() const { return encoding_; }
    const std::map<std::string, std::string> &metadata() const { return metadata_; }
    uint64_t file_size() const { return file_size_; }

    // False while the RAW index is being built (seeking is not possible yet).
    bool seek_range(timestamp &min_t, timestamp &max_t) const;

private:
    EventFileSource() = default;

    std::filesystem::path path_;
    std::unique_ptr<Metavision::Device> device_;
    std::unique_ptr<Metavision::EventFileReader> reader_;
    Metavision::RAWEventFileReader *raw_reader_ = nullptr;
    int width_ = kDefaultWidth, height_ = kDefaultHeight;
    std::string encoding_;
    std::map<std::string, std::string> metadata_;
    uint64_t file_size_ = 0;
};

// Parses "1280x720" (geometry field of RAW / HDF5 / DAT metadata).
bool parse_geometry(const std::string &text, int &width, int &height);

} // namespace evs
