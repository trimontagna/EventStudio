#pragma once

#include <vector>

namespace evs {

// "Detent" of a slider drag: the value stops at given points (e.g. the edges of a manufacturer's recommended
// range) and the drag has to push on by `width` (value units) past the point to go through. Moving back toward the
// side the drag came from releases it at once. A drag starting exactly on a point passes it freely, so a second
// drag from the stop goes straight through.
class SliderDetent {
public:
    // value: the slider value before this frame's input; raw: the value under the mouse. Returns the value to use.
    int apply(int value, int raw, const std::vector<int> &points, int width);
    void release() { holding_ = false; } // end of the drag
    bool holding() const { return holding_; }
    int point() const { return point_; }

private:
    bool holding_ = false;
    int point_    = 0;
    int dir_      = 0; // +1: the drag pushes through upward
    int settled_  = 0; // smallest push past the point seen while holding
};

} // namespace evs
