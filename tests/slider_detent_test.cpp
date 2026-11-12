#include <gtest/gtest.h>

#include "core/slider_detent.h"

using namespace evs;

namespace {
// Drags from `start` through the raw values, as the slider would report them frame after frame.
int drag(SliderDetent &d, int start, std::initializer_list<int> raws, const std::vector<int> &points, int width) {
    int v = start;
    for (int raw : raws)
        v = d.apply(v, raw, points, width);
    return v;
}
} // namespace

TEST(SliderDetent, MovesFreelyAwayFromThePoints) {
    SliderDetent d;
    EXPECT_EQ(drag(d, 0, {5, 10, 20}, {-50, 100}, 10), 20);
    EXPECT_FALSE(d.holding());
}

TEST(SliderDetent, StopsAtTheUpperLimitUntilPushedThrough) {
    SliderDetent d;
    const std::vector<int> pts = {-50, 100};
    EXPECT_EQ(drag(d, 90, {98, 104}, pts, 10), 100); // stops at the limit
    EXPECT_TRUE(d.holding());
    EXPECT_EQ(d.apply(100, 110, pts, 10), 100); // 6 past where it settled (104): held
    EXPECT_EQ(d.apply(100, 114, pts, 10), 114); // pushed 10 past the settled point: through
    EXPECT_FALSE(d.holding());
}

TEST(SliderDetent, StopsAtTheLowerLimitGoingDown) {
    SliderDetent d;
    const std::vector<int> pts = {-50, 100};
    EXPECT_EQ(drag(d, 0, {-40, -55}, pts, 10), -50);
    EXPECT_EQ(d.apply(-50, -60, pts, 10), -50);
    EXPECT_EQ(d.apply(-50, -65, pts, 10), -65);
}

TEST(SliderDetent, FastFlickStopsToo) {
    SliderDetent d;
    // one frame from inside to far outside: stops at the limit, then needs a further push
    EXPECT_EQ(d.apply(50, 140, {100}, 10), 100);
    EXPECT_EQ(d.apply(100, 145, {100}, 10), 100);
    EXPECT_EQ(d.apply(100, 150, {100}, 10), 150);
}

TEST(SliderDetent, MovingBackReleasesAtOnce) {
    SliderDetent d;
    EXPECT_EQ(d.apply(90, 103, {100}, 10), 100);
    EXPECT_EQ(d.apply(100, 95, {100}, 10), 95);
    EXPECT_FALSE(d.holding());
}

TEST(SliderDetent, ComingBackFromOutsideStopsAtTheLimit) {
    SliderDetent d;
    EXPECT_EQ(d.apply(130, 98, {100}, 10), 100);
    EXPECT_EQ(d.apply(100, 92, {100}, 10), 100);
    EXPECT_EQ(d.apply(100, 88, {100}, 10), 88);
}

TEST(SliderDetent, ADragStartingOnThePointPassesFreely) {
    SliderDetent d;
    EXPECT_EQ(d.apply(100, 103, {100}, 10), 103);
    EXPECT_FALSE(d.holding());
}

TEST(SliderDetent, ReleaseEndsTheHold) {
    SliderDetent d;
    EXPECT_EQ(d.apply(90, 101, {100}, 10), 100);
    d.release();
    EXPECT_FALSE(d.holding());
}

TEST(SliderDetent, JumpOverBothLimitsStopsAtTheFirst) {
    SliderDetent d;
    EXPECT_EQ(d.apply(-100, 200, {-50, 100}, 10), -50);
}
