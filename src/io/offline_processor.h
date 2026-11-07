#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/filters.h"
#include "core/frame_renderer.h"
#include "core/types.h"

namespace evs {

enum class ExportFormat { RawEvt2, RawEvt21, RawEvt3, Hdf5, Csv, Avi, Images, Count };
const char *to_string(ExportFormat format);
const char *default_extension(ExportFormat format); // "" for Images (the output is a folder)

struct ExportJob {
    std::filesystem::path input, output;
    ExportFormat format = ExportFormat::RawEvt3;

    bool apply_filters = true;
    FilterConfig filters;
    std::shared_ptr<const std::vector<uint8_t>> mask; // width*height bitmap, may be null

    // Time range in recording time (first event ~ 0). end_us < 0: until the end.
    timestamp start_us = 0;
    timestamp end_us   = -1;

    // AVI / Images: frames rendered with these settings
    PreviewSettings preview;
    double video_fps = 30.0; // AVI only

    // Images: `output` is a folder receiving frame_000000.png ... and frames.csv (index, file, time range).
    // One image per preview.accumulation_us of recording time from start_us; frame i holds the events of
    // [start_us + i * period, start_us + (i + 1) * period).
};

struct ExportProgress {
    std::atomic<double> fraction{0.0};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> events_in{0}, events_out{0};
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    uint64_t events_in = 0, events_out = 0;
    uint64_t triggers  = 0; // external triggers written (not filtered; RAW / HDF5 / CSV "<name>_triggers.csv")
    uint64_t frames    = 0; // AVI / Images
    double seconds     = 0;
};

// Compact description of the image export settings, used in the default folder name, e.g.
// "dt-20ms_sERC-50M_masked": frame period, preview mode (if not the CD frame), time range, then the software filters
// in pipeline order and "masked" when `masked` (mask bitmap applied with at least one masked pixel).
std::string image_export_tag(const ExportJob &job, bool masked);
// <recording folder>/<recording name>_images_<tag>
std::filesystem::path default_image_folder(const std::filesystem::path &input, const std::string &tag);

// Reads the input file as fast as possible, applies the software filters and writes the result.
// Blocking: run it on a worker thread, `progress` may be polled / cancelled from another thread.
ExportResult run_export(const ExportJob &job, ExportProgress *progress = nullptr);

} // namespace evs
