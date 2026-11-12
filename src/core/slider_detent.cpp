#include "core/slider_detent.h"

#include <algorithm>
#include <cstdlib>

namespace evs {

int SliderDetent::apply(int value, int raw, const std::vector<int> &points, int width) {
    width = std::max(width, 1);
    if (holding_) {
        const int push = (raw - point_) * dir_;
        if (push < 0) { // back toward the side the drag came from
            holding_ = false;
            return raw;
        }
        settled_ = std::min(settled_, push);
        if (push - settled_ >= width) { // pushed through
            holding_ = false;
            return raw;
        }
        return point_;
    }
    // The first point met when moving from value to raw.
    bool found = false;
    int best   = 0;
    for (int p : points) {
        const bool met = (value < p && raw >= p) || (value > p && raw <= p);
        if (met && (!found || std::abs(p - value) < std::abs(best - value))) {
            best  = p;
            found = true;
        }
    }
    if (!found)
        return raw;
    holding_ = true;
    point_   = best;
    dir_     = raw > value ? 1 : -1;
    settled_ = (raw - best) * dir_;
    return best;
}

} // namespace evs
