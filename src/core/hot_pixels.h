#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"

namespace evs {

// Per-pixel event counts over a time span (input of the hot pixel detection).
class PixelActivity {
public:
    PixelActivity(int width, int height);
    void add(const EventCD *begin, const EventCD *end);
    void reset();

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<uint32_t> &counts() const { return counts_; }
    uint64_t total() const { return total_; }
    // Time span covered by the events (us), 0 before two events were seen.
    timestamp duration_us() const { return has_events_ ? last_t_ - first_t_ : 0; }

private:
    int width_, height_;
    std::vector<uint32_t> counts_;
    uint64_t total_  = 0;
    bool has_events_ = false;
    timestamp first_t_ = 0, last_t_ = 0;
};

struct HotPixelParams {
    // A pixel is hot when its rate is at least `min_rate_hz` AND at least `local_ratio` times the count of its
    // third-most-active neighbour (taken as >= 1 event). The local comparison keeps busy regions of a real scene
    // (e.g. a seeded flow, including their edges and corners) from being flagged while isolated pixels and adjacent
    // pairs are; the absolute minimum ignores quiet pixels.
    double min_rate_hz = 20.0;
    double local_ratio = 10.0;
    size_t max_pixels  = 100000;
};

struct HotPixel {
    uint32_t x, y;
    double rate_hz;
    double ratio; // count / count of the third-most-active neighbour
};

// Hot pixels sorted by decreasing rate.
std::vector<HotPixel> detect_hot_pixels(const PixelActivity &activity, const HotPixelParams &params);

} // namespace evs
