#include "core/hot_pixels.h"

#include <algorithm>

namespace evs {

PixelActivity::PixelActivity(int width, int height) :
    width_(width), height_(height), counts_(static_cast<size_t>(width) * height, 0) {}

void PixelActivity::add(const EventCD *begin, const EventCD *end) {
    if (begin == end)
        return;
    for (auto it = begin; it != end; ++it) {
        if (it->x < width_ && it->y < height_) {
            uint32_t &c = counts_[static_cast<size_t>(it->y) * width_ + it->x];
            if (c != UINT32_MAX)
                ++c;
        }
    }
    total_ += static_cast<uint64_t>(end - begin);
    if (!has_events_) {
        first_t_    = begin->t;
        has_events_ = true;
    }
    last_t_ = std::max(last_t_, (end - 1)->t);
}

void PixelActivity::reset() {
    std::fill(counts_.begin(), counts_.end(), 0u);
    total_      = 0;
    has_events_ = false;
    first_t_ = last_t_ = 0;
}

std::vector<HotPixel> detect_hot_pixels(const PixelActivity &activity, const HotPixelParams &params) {
    std::vector<HotPixel> hot;
    const timestamp duration = activity.duration_us();
    if (duration <= 0)
        return hot;
    const double seconds   = duration / 1e6;
    const double min_count = params.min_rate_hz * seconds;
    const int w = activity.width(), h = activity.height();
    const auto &counts = activity.counts();

    uint32_t neighbours[8];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t c = counts[static_cast<size_t>(y) * w + x];
            if (c < min_count || c == 0)
                continue;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if ((dx || dy) && xx >= 0 && yy >= 0 && xx < w && yy < h)
                        neighbours[n++] = counts[static_cast<size_t>(yy) * w + xx];
                }
            // Reference: third-highest neighbour. An isolated hot pixel (or a pair) stands out against it, while the
            // edges and corners of genuinely active regions (>= 3 active neighbours) do not.
            const int k = std::max(0, n - 3);
            std::nth_element(neighbours, neighbours + k, neighbours + n);
            const double reference = std::max<double>(1.0, neighbours[k]);
            const double ratio     = c / reference;
            if (ratio >= params.local_ratio)
                hot.push_back({static_cast<uint32_t>(x), static_cast<uint32_t>(y), c / seconds, ratio});
        }
    }
    std::sort(hot.begin(), hot.end(), [](const HotPixel &a, const HotPixel &b) { return a.rate_hz > b.rate_hz; });
    if (hot.size() > params.max_pixels)
        hot.resize(params.max_pixels);
    return hot;
}

} // namespace evs
