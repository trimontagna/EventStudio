#include <gtest/gtest.h>

#include "core/rate_meter.h"

using namespace evs;

TEST(RateMeter, ConstantRate) {
    RateMeter m(1'000'000, 10);
    // 1000 units every 1 ms during 3 s -> 1e6 / s
    for (timestamp t = 0; t < 3'000'000; t += 1000)
        m.add(1000, t);
    EXPECT_NEAR(m.rate_per_second(2'999'000), 1e6, 0.02e6);
    EXPECT_DOUBLE_EQ(m.total(), 3000.0 * 1000);
}

TEST(RateMeter, ShortHistoryIsNotDilutedByTheWindow) {
    RateMeter m(1'000'000, 10);
    for (timestamp t = 0; t < 200'000; t += 1000) // 200 ms at 2000 units / ms
        m.add(2000, t);
    EXPECT_NEAR(m.rate_per_second(199'000), 2e6, 0.05e6);
}

TEST(RateMeter, OldSamplesExpire) {
    RateMeter m(1'000'000, 10);
    m.add(5000, 0);
    EXPECT_GT(m.rate_per_second(500'000), 0);
    EXPECT_EQ(m.rate_per_second(5'000'000), 0);
}

TEST(RateMeter, RateDropsAfterStreamStops) {
    RateMeter m(1'000'000, 10);
    for (timestamp t = 0; t < 2'000'000; t += 1000)
        m.add(1, t);
    const double running = m.rate_per_second(2'000'000);
    const double later   = m.rate_per_second(2'500'000);
    EXPECT_NEAR(running, 1000, 30);
    EXPECT_NEAR(later, 500, 75); // one 100 ms bin of granularity
}

TEST(RateMeter, Reset) {
    RateMeter m;
    m.add(10, 0);
    m.reset();
    EXPECT_EQ(m.total(), 0);
    EXPECT_EQ(m.rate_per_second(10), 0);
}
