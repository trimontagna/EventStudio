#include <gtest/gtest.h>

#include "core/latency_monitor.h"

using evs::LatencyMonitor;
using evs::timestamp;

namespace {
// Camera time base starts at 0 when the PC clock reads 5 s; buffers every ms, normally 2 ms late.
constexpr timestamp kPcStart = 5'000'000;
} // namespace

TEST(LatencyMonitor, MeasuresDelayAboveTheFastestBuffer) {
    LatencyMonitor m(/*warmup*/ 100'000);
    timestamp pc = 0;
    for (timestamp cam = 0; cam <= 2'000'000; cam += 1000) {
        timestamp extra = 0;
        if (cam >= 1'200'000 && cam < 1'300'000)
            extra = 30'000; // 30 ms hiccup
        if (cam == 1'000'000)
            extra = -1'000; // one buffer faster than usual: becomes the reference
        pc = kPcStart + cam + 2'000 + extra;
        m.add(cam, pc, 0);
    }
    const auto s = m.stats(pc);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.current_ms, 1.0, 1e-9);    // back to normal: 1 ms above the fastest buffer
    EXPECT_NEAR(s.recent_max_ms, 31.0, 1e-9);
    EXPECT_NEAR(s.max_ms, 31.0, 1e-9);
    EXPECT_NEAR(s.reference_age_s, 1.0, 1e-3);
    EXPECT_EQ(s.samples, 2001u);
}

TEST(LatencyMonitor, WarmupOnlySetsTheReference) {
    LatencyMonitor m(/*warmup*/ 500'000);
    // Start-up backlog: the first 300 ms of data arrive in one burst at PC 5.3 s, then buffers flow with no delay.
    for (timestamp cam = 0; cam < 300'000; cam += 1000)
        m.add(cam, kPcStart + 300'000 + cam / 100, 0);
    EXPECT_FALSE(m.stats(kPcStart + 310'000).valid);
    timestamp pc = 0;
    for (timestamp cam = 300'000; cam <= 2'000'000; cam += 1000)
        m.add(cam, pc = kPcStart + cam, 0);
    const auto s = m.stats(pc);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.current_ms, 0.0, 1e-9);
    EXPECT_NEAR(s.max_ms, 0.0, 1e-9); // the 300 ms backlog is not reported as a delay
}

TEST(LatencyMonitor, StalledStreamShowsGrowingDelay) {
    LatencyMonitor m(0);
    for (timestamp cam = 0; cam <= 1'000'000; cam += 1000)
        m.add(cam, kPcStart + cam, 0);
    const timestamp last_pc = kPcStart + 1'000'000;
    EXPECT_NEAR(m.stats(last_pc).current_ms, 0.0, 1e-9);
    EXPECT_NEAR(m.stats(last_pc + 2'000'000).current_ms, 2000.0, 1e-9); // nothing for 2 s
}

TEST(LatencyMonitor, NewTimeBaseResetsTheReference) {
    LatencyMonitor m(0);
    for (timestamp cam = 0; cam <= 1'000'000; cam += 1000)
        m.add(cam, kPcStart + cam, 0);
    // Camera restarted: time base back to 0 at PC 20 s.
    timestamp pc = 0;
    for (timestamp cam = 0; cam <= 100'000; cam += 1000)
        m.add(cam, pc = 20'000'000 + cam, 0);
    const auto s = m.stats(pc);
    ASSERT_TRUE(s.valid);
    EXPECT_NEAR(s.current_ms, 0.0, 1e-9); // without the reset it would read ~14 s
    EXPECT_EQ(s.samples, 101u);
}

TEST(LatencyMonitor, HistoryKeepsTheMaxPerBin) {
    LatencyMonitor m(0, /*history*/ 1'000'000, /*bin*/ 100'000);
    timestamp pc = 0;
    for (timestamp cam = 0; cam < 1'000'000; cam += 1000) {
        const timestamp extra = cam == 850'000 ? 40'000 : 0;
        m.add(cam, pc = kPcStart + cam + extra, 0);
    }
    const auto h = m.history_ms(pc);
    ASSERT_EQ(h.size(), 10u);
    EXPECT_FLOAT_EQ(h[8], 40.f); // the hiccup lands in the second-to-last bin
    EXPECT_FLOAT_EQ(h[0], 0.f);
}

TEST(LatencyMonitor, LogKeepsEverySampleWhileEnabled) {
    LatencyMonitor m(1'000'000); // warm-up samples are logged too
    m.add(0, kPcStart, 111);
    m.set_logging(true);
    m.add(1000, kPcStart + 1500, 222);
    m.add(2000, kPcStart + 2000, 333);
    auto log = m.take_log();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].unix_us, 222);
    EXPECT_EQ(log[0].camera_us, 1000);
    EXPECT_EQ(log[0].delay_us, 500);
    EXPECT_EQ(log[1].delay_us, 0);
    EXPECT_TRUE(m.take_log().empty());
    m.set_logging(false);
    m.add(3000, kPcStart + 3000, 444);
    EXPECT_TRUE(m.take_log().empty());
}
