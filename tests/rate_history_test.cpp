#include <gtest/gtest.h>

#include "core/rate_history.h"

using namespace evs;

TEST(RateHistory, ConstantRateFillsTheGraphAndTheMean) {
    RateHistory h(10'000'000, 100'000);
    for (timestamp t = 0; t < 20'000'000; t += 1000) // 500 events / ms -> 5e5 / s
        h.add(500, t);
    const auto v = h.per_second(20'000'000);
    ASSERT_EQ(v.size(), 100u);
    for (float x : v)
        EXPECT_NEAR(x, 5e5, 1e3);
    EXPECT_NEAR(h.mean_per_second(20'000'000, 10'000'000), 5e5, 1e3);
}

TEST(RateHistory, GraphLeavesOutTheBinInProgress) {
    RateHistory h(1'000'000, 100'000);
    for (timestamp t = 0; t < 1'000'000; t += 1000)
        h.add(100, t);
    h.add(1'000'000, 1'000'500); // a burst in the bin in progress
    const auto v = h.per_second(1'001'000);
    EXPECT_NEAR(v.back(), 1e5, 1e3); // last complete bin [0.9 s, 1.0 s)
}

TEST(RateHistory, MeanOfAShortHistoryIsNotDiluted) {
    RateHistory h(60'000'000, 100'000);
    for (timestamp t = 5'000'000; t < 7'000'000; t += 1000) // 2 s at 2e6 / s, starting at 5 s
        h.add(2000, t);
    EXPECT_NEAR(h.mean_per_second(6'999'000, 10'000'000), 2e6, 0.02e6);
}

TEST(RateHistory, MeanFollowsTheWindow) {
    RateHistory h(60'000'000, 100'000);
    for (timestamp t = 0; t < 10'000'000; t += 1000)
        h.add(1000, t); // 1e6 / s for 10 s
    for (timestamp t = 10'000'000; t < 20'000'000; t += 1000)
        h.add(3000, t); // then 3e6 / s for 10 s
    EXPECT_NEAR(h.mean_per_second(19'999'000, 10'000'000), 3e6, 0.03e6);
    EXPECT_NEAR(h.mean_per_second(14'999'000, 10'000'000), 2e6, 0.03e6); // half and half
}

TEST(RateHistory, QuietSourceDecaysToZero) {
    RateHistory h(10'000'000, 100'000);
    for (timestamp t = 0; t < 1'000'000; t += 1000)
        h.add(100, t);
    EXPECT_GT(h.mean_per_second(1'000'000, 10'000'000), 0);
    EXPECT_EQ(h.mean_per_second(30'000'000, 10'000'000), 0);
    const auto v = h.per_second(30'000'000);
    for (float x : v)
        EXPECT_EQ(x, 0.f);
}

TEST(RateHistory, GoingBackInTimeStartsAgain) {
    RateHistory h(10'000'000, 100'000);
    for (timestamp t = 5'000'000; t < 6'000'000; t += 1000)
        h.add(100, t);
    h.add(100, 0); // recording looped
    EXPECT_NEAR(h.mean_per_second(0, 10'000'000), 100.0 * 1e6, 1); // one sample, 1 us span
    for (timestamp t = 1000; t < 1'000'000; t += 1000)
        h.add(100, t);
    EXPECT_NEAR(h.mean_per_second(999'000, 10'000'000), 1e5, 1e3);
}

TEST(RateHistory, ResetEmpties) {
    RateHistory h;
    h.add(10, 0);
    EXPECT_FALSE(h.empty());
    h.reset();
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(h.mean_per_second(100, 10'000'000), 0);
}
