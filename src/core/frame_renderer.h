#pragma once

#include <mutex>
#include <vector>

#include "core/types.h"

namespace evs {

// Built-in preview modes (in-app versions of the OpenEB sample visualizations).
enum class PreviewMode {
    Accumulation, // CD frame: last polarity of the pixels active during the accumulation time (metavision_viewer)
    TimeDecay,    // exponential fading of the last event of each pixel
    TimeSurface,  // age of the last event, color-mapped over the accumulation time (metavision_time_surface)
    Histogram,    // ON / OFF event counts over the accumulation time (event_frame_generation, histo)
    Difference,   // ON - OFF count over the accumulation time (event_frame_generation, diff)
    Integration,  // leaky integration of polarities into a gray level (metavision_events_integration)
    Count
};
const char *to_string(PreviewMode mode);

enum class Palette { Dark, Light, Gray, Count };
const char *to_string(Palette palette);

struct PreviewSettings {
    PreviewMode mode          = PreviewMode::Accumulation;
    Palette palette           = Palette::Dark;
    timestamp accumulation_us = 10000;
    timestamp decay_us        = 20000;  // time constant for TimeDecay / Integration
    int histo_saturation      = 4;      // event count mapped to full intensity (Histogram / Difference)
    float integration_step    = 0.15f;  // contribution of one event to the integrated level (Integration)
};

// Keeps the minimal per-pixel state required by the preview modes and renders RGBA8 frames.
// ingest() and render() may be called from different threads.
class FrameRenderer {
public:
    FrameRenderer(int width, int height);

    void set_settings(const PreviewSettings &settings);
    PreviewSettings settings() const;
    void reset();

    void ingest(const EventCD *begin, const EventCD *end);
    timestamp last_timestamp() const;

    // Renders the view at time `now` into `rgba` (resized to width*height*4).
    void render(timestamp now, std::vector<uint8_t> &rgba) const;

    int width() const { return width_; }
    int height() const { return height_; }

    // Upper bound of the events kept for the window-based modes (Histogram / Difference).
    static constexpr size_t kMaxWindowEvents = 8'000'000;

private:
    bool uses_window() const;
    void trim_window_locked();

    int width_, height_;
    mutable std::mutex mutex_;
    PreviewSettings settings_;
    std::vector<timestamp> last_t_;
    std::vector<int8_t> last_p_;
    std::vector<float> level_;      // Integration
    std::vector<timestamp> level_t_;
    std::vector<EventCD> window_;   // Histogram / Difference
    size_t window_begin_ = 0;
    timestamp last_ts_   = 0;
};

// Color helpers shared with the UI.
struct Rgb {
    uint8_t r, g, b;
};
Rgb palette_background(Palette p);
Rgb palette_on(Palette p);
Rgb palette_off(Palette p);
Rgb colormap_jet(float v); // v in [0, 1]

} // namespace evs
