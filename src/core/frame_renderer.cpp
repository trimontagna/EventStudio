#include "core/frame_renderer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace evs {

namespace {
constexpr timestamp kNever = std::numeric_limits<timestamp>::min() / 4;

inline uint8_t lerp8(uint8_t a, uint8_t b, float f) {
    return static_cast<uint8_t>(std::clamp(a + (static_cast<float>(b) - a) * f + 0.5f, 0.f, 255.f));
}
inline void put(uint8_t *px, Rgb c) {
    px[0] = c.r;
    px[1] = c.g;
    px[2] = c.b;
    px[3] = 255;
}
inline Rgb lerp(Rgb a, Rgb b, float f) {
    return {lerp8(a.r, b.r, f), lerp8(a.g, b.g, f), lerp8(a.b, b.b, f)};
}
} // namespace

const char *to_string(PreviewMode mode) {
    switch (mode) {
    case PreviewMode::Accumulation:
        return "CD accumulation";
    case PreviewMode::TimeDecay:
        return "Time decay";
    case PreviewMode::TimeSurface:
        return "Time surface";
    case PreviewMode::Histogram:
        return "Event frame: histogram";
    case PreviewMode::Difference:
        return "Event frame: difference";
    case PreviewMode::Integration:
        return "Events integration";
    default:
        return "?";
    }
}

const char *to_string(Palette palette) {
    switch (palette) {
    case Palette::Dark:
        return "Dark";
    case Palette::Light:
        return "Light";
    case Palette::Gray:
        return "Gray";
    default:
        return "?";
    }
}

// Colors of the Metavision "Dark" / "Light" / "Gray" color palettes.
Rgb palette_background(Palette p) {
    switch (p) {
    case Palette::Light:
        return {255, 255, 255};
    case Palette::Gray:
        return {128, 128, 128};
    default:
        return {30, 37, 52};
    }
}
Rgb palette_on(Palette p) {
    switch (p) {
    case Palette::Light:
        return {64, 126, 201};
    case Palette::Gray:
        return {255, 255, 255};
    default:
        return {216, 223, 236};
    }
}
Rgb palette_off(Palette p) {
    switch (p) {
    case Palette::Light:
        return {30, 37, 52};
    case Palette::Gray:
        return {0, 0, 0};
    default:
        return {64, 126, 201};
    }
}

Rgb colormap_jet(float v) {
    v           = std::clamp(v, 0.f, 1.f);
    auto chan   = [](float x) { return static_cast<uint8_t>(255.f * std::clamp(1.5f - std::abs(x), 0.f, 1.f)); };
    const float s = 4.f * v;
    return {chan(s - 3.f), chan(s - 2.f), chan(s - 1.f)};
}

FrameRenderer::FrameRenderer(int width, int height) : width_(width), height_(height) {
    reset();
}

void FrameRenderer::set_settings(const PreviewSettings &settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool mode_changed = settings.mode != settings_.mode;
    settings_               = settings;
    settings_.accumulation_us = std::max<timestamp>(1, settings_.accumulation_us);
    settings_.decay_us        = std::max<timestamp>(1, settings_.decay_us);
    settings_.histo_saturation = std::max(1, settings_.histo_saturation);
    if (mode_changed) {
        window_.clear();
        window_begin_ = 0;
        std::fill(level_.begin(), level_.end(), 0.f);
        std::fill(level_t_.begin(), level_t_.end(), kNever);
    }
}

PreviewSettings FrameRenderer::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void FrameRenderer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = static_cast<size_t>(width_) * height_;
    last_t_.assign(n, kNever);
    last_p_.assign(n, 0);
    level_.assign(n, 0.f);
    level_t_.assign(n, kNever);
    window_.clear();
    window_begin_ = 0;
    last_ts_      = 0;
}

bool FrameRenderer::uses_window() const {
    return settings_.mode == PreviewMode::Histogram || settings_.mode == PreviewMode::Difference;
}

timestamp FrameRenderer::last_timestamp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_ts_;
}

void FrameRenderer::trim_window_locked() {
    const timestamp limit = last_ts_ - settings_.accumulation_us;
    while (window_begin_ < window_.size() && window_[window_begin_].t <= limit)
        ++window_begin_;
    if (window_.size() - window_begin_ > kMaxWindowEvents)
        window_begin_ = window_.size() - kMaxWindowEvents;
    if (window_begin_ > 1'000'000 && window_begin_ * 2 > window_.size()) {
        window_.erase(window_.begin(), window_.begin() + static_cast<std::ptrdiff_t>(window_begin_));
        window_begin_ = 0;
    }
}

void FrameRenderer::ingest(const EventCD *begin, const EventCD *end) {
    if (begin == end)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool integrate = settings_.mode == PreviewMode::Integration;
    const float tau      = static_cast<float>(settings_.decay_us);
    for (auto it = begin; it != end; ++it) {
        if (it->x >= width_ || it->y >= height_)
            continue;
        const size_t i = static_cast<size_t>(it->y) * width_ + it->x;
        last_t_[i]     = it->t;
        last_p_[i]     = static_cast<int8_t>(it->p);
        if (integrate) {
            float &v = level_[i];
            if (level_t_[i] != kNever)
                v *= std::exp(-static_cast<float>(it->t - level_t_[i]) / tau);
            v           = std::clamp(v + (it->p ? settings_.integration_step : -settings_.integration_step), -1.f, 1.f);
            level_t_[i] = it->t;
        }
    }
    last_ts_ = std::max(last_ts_, (end - 1)->t);
    if (uses_window()) {
        window_.insert(window_.end(), begin, end);
        trim_window_locked();
    }
}

void FrameRenderer::render(timestamp now, std::vector<uint8_t> &rgba) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = static_cast<size_t>(width_) * height_;
    rgba.resize(n * 4);
    const Rgb bg = palette_background(settings_.palette), on = palette_on(settings_.palette),
              off   = palette_off(settings_.palette);
    const timestamp acc = settings_.accumulation_us;
    const float tau     = static_cast<float>(settings_.decay_us);
    uint8_t *px         = rgba.data();

    switch (settings_.mode) {
    case PreviewMode::Accumulation:
        for (size_t i = 0; i < n; ++i, px += 4)
            put(px, now - last_t_[i] < acc ? (last_p_[i] ? on : off) : bg);
        break;

    case PreviewMode::TimeDecay:
        for (size_t i = 0; i < n; ++i, px += 4) {
            const timestamp age = std::max<timestamp>(0, now - last_t_[i]);
            const float f       = age > 8 * settings_.decay_us ? 0.f : std::exp(-static_cast<float>(age) / tau);
            put(px, f < 1.f / 255 ? bg : lerp(bg, last_p_[i] ? on : off, f));
        }
        break;

    case PreviewMode::TimeSurface: {
        const Rgb ts_bg{0, 0, 0};
        for (size_t i = 0; i < n; ++i, px += 4) {
            const timestamp age = now - last_t_[i];
            put(px, age < acc ? colormap_jet(1.f - static_cast<float>(std::max<timestamp>(age, 0)) / acc) : ts_bg);
        }
        break;
    }

    case PreviewMode::Histogram:
    case PreviewMode::Difference: {
        std::vector<int16_t> on_count(n, 0), off_count(n, 0);
        for (size_t k = window_begin_; k < window_.size(); ++k) {
            const EventCD &ev = window_[k];
            if (ev.t <= now - acc || ev.t > now)
                continue;
            const size_t i = static_cast<size_t>(ev.y) * width_ + ev.x;
            int16_t &c     = ev.p ? on_count[i] : off_count[i];
            if (c < INT16_MAX)
                ++c;
        }
        const float sat = static_cast<float>(settings_.histo_saturation);
        if (settings_.mode == PreviewMode::Histogram) {
            for (size_t i = 0; i < n; ++i, px += 4) {
                const float fon = std::min(on_count[i] / sat, 1.f), foff = std::min(off_count[i] / sat, 1.f);
                // Hue from the ON/OFF ratio, intensity from the dominant count.
                const Rgb hue = lerp(off, on, fon / std::max(fon + foff, 1e-6f));
                put(px, lerp(bg, hue, std::max(fon, foff)));
            }
        } else {
            for (size_t i = 0; i < n; ++i, px += 4) {
                const float f = std::clamp((on_count[i] - off_count[i]) / sat, -1.f, 1.f);
                put(px, f >= 0 ? lerp(bg, on, f) : lerp(bg, off, -f));
            }
        }
        break;
    }

    case PreviewMode::Integration:
        for (size_t i = 0; i < n; ++i, px += 4) {
            float v = 0;
            if (level_t_[i] != kNever)
                v = level_[i] * std::exp(-static_cast<float>(std::max<timestamp>(0, now - level_t_[i])) / tau);
            const uint8_t g = static_cast<uint8_t>(std::clamp(127.5f + 127.5f * v, 0.f, 255.f));
            put(px, Rgb{g, g, g});
        }
        break;

    default:
        std::fill(rgba.begin(), rgba.end(), uint8_t(0));
    }
}

} // namespace evs
