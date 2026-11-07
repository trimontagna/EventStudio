#include "io/event_file_source.h"

#include <algorithm>
#include <cctype>

#include <metavision/hal/device/device.h>
#include <metavision/hal/device/device_discovery.h>
#include <metavision/hal/facilities/i_geometry.h>
#include <metavision/hal/utils/raw_file_config.h>
#include <metavision/sdk/stream/dat_event_file_reader.h>
#include <metavision/sdk/stream/hdf5_event_file_reader.h>
#include <metavision/sdk/stream/raw_event_file_reader.h>

namespace evs {

bool parse_geometry(const std::string &text, int &width, int &height) {
    const auto x = text.find('x');
    if (x == std::string::npos)
        return false;
    try {
        const int w = std::stoi(text.substr(0, x));
        const int h = std::stoi(text.substr(x + 1));
        if (w <= 0 || h <= 0)
            return false;
        width  = w;
        height = h;
        return true;
    } catch (...) { return false; }
}

std::unique_ptr<EventFileSource> EventFileSource::open(const std::filesystem::path &path, bool time_shift,
                                                       std::string *error) {
    std::unique_ptr<EventFileSource> src(new EventFileSource());
    src->path_ = path;
    std::error_code ec;
    src->file_size_ = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error)
            *error = "Cannot access " + path.string();
        return nullptr;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

    try {
        if (ext == ".raw") {
            Metavision::RawFileConfig config;
            config.do_time_shifting_ = time_shift;
            config.build_index_      = true;
            src->device_             = Metavision::DeviceDiscovery::open_raw_file(path, config);
            if (!src->device_) {
                if (error)
                    *error = "No HAL plugin could open " + path.string();
                return nullptr;
            }
            auto raw         = std::make_unique<Metavision::RAWEventFileReader>(*src->device_, path);
            src->raw_reader_ = raw.get();
            src->reader_     = std::move(raw);
            if (auto *geometry = src->device_->get_facility<Metavision::I_Geometry>()) {
                src->width_  = geometry->get_width();
                src->height_ = geometry->get_height();
            }
        } else if (ext == ".hdf5" || ext == ".h5") {
            src->reader_ = std::make_unique<Metavision::HDF5EventFileReader>(path, time_shift);
        } else if (ext == ".dat") {
            src->reader_ = std::make_unique<Metavision::DATEventFileReader>(path);
        } else {
            if (error)
                *error = "Unsupported file type '" + ext + "' (expected .raw, .hdf5 or .dat)";
            return nullptr;
        }

        for (const auto &[k, v] : src->reader_->get_metadata_map())
            src->metadata_[k] = v;
    } catch (const std::exception &e) {
        if (error)
            *error = std::string("Cannot open ") + path.string() + ": " + e.what();
        return nullptr;
    }

    if (!src->raw_reader_) {
        auto it = src->metadata_.find("geometry");
        if (it != src->metadata_.end())
            parse_geometry(it->second, src->width_, src->height_);
    }

    if (ext == ".raw") {
        auto it = src->metadata_.find("format");
        if (it != src->metadata_.end())
            src->encoding_ = it->second.substr(0, it->second.find(';'));
        else if ((it = src->metadata_.find("evt")) != src->metadata_.end())
            src->encoding_ = it->second == "3.0" ? "EVT3" : it->second == "2.1" ? "EVT21" : "EVT2";
        else
            src->encoding_ = "RAW";
    } else {
        src->encoding_ = ext == ".dat" ? "DAT" : "HDF5";
    }
    return src;
}

EventFileSource::~EventFileSource() {
    // The reader references the device: destroy it first.
    reader_.reset();
    device_.reset();
}

bool EventFileSource::seek_range(timestamp &min_t, timestamp &max_t) const {
    try {
        return reader_->seekable() && reader_->get_seek_range(min_t, max_t);
    } catch (...) { return false; }
}

} // namespace evs
