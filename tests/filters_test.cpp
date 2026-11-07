#include <gtest/gtest.h>

#include "core/filters.h"
#include "test_utils.h"

using namespace evs;
using evs::test::ev;

namespace {
std::vector<EventCD> run(TrailFilter &f, const std::vector<EventCD> &in) {
    std::vector<EventCD> out;
    f.process(in.data(), in.data() + in.size(), out);
    return out;
}
std::vector<timestamp> times(const std::vector<EventCD> &v) {
    std::vector<timestamp> t;
    for (const auto &e : v)
        t.push_back(e.t);
    return t;
}
} // namespace

// ------------------------------------------------------------------------------------------------ polarity

TEST(PolarityFilter, KeepsRequestedPolarities) {
    const std::vector<EventCD> in = {ev(0, 0, 1, 1), ev(1, 0, 0, 2), ev(2, 0, 1, 3), ev(3, 0, 0, 4)};
    PolarityFilter f;
    std::vector<EventCD> out;

    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_EQ(out.size(), 4u);

    out.clear();
    f.set(true, false);
    f.process(in.data(), in.data() + in.size(), out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].p, 1);
    EXPECT_EQ(out[1].t, 3);

    out.clear();
    f.set(false, true);
    f.process(in.data(), in.data() + in.size(), out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].p, 0);

    out.clear();
    f.set(false, false);
    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_TRUE(out.empty());
}

// ------------------------------------------------------------------------------------------------ pixel mask

TEST(PixelMaskFilter, DropsMaskedPixelsOnly) {
    PixelMaskFilter f(4, 3);
    auto mask      = std::make_shared<std::vector<uint8_t>>(12, 0);
    (*mask)[1 * 4 + 2] = 1; // pixel (2, 1)
    f.set_mask(mask);
    const std::vector<EventCD> in = {ev(2, 1, 1, 1), ev(1, 2, 0, 2), ev(2, 1, 0, 3), ev(3, 2, 1, 4)};
    std::vector<EventCD> out;
    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_EQ(times(out), (std::vector<timestamp>{2, 4}));
}

TEST(PixelMaskFilter, EmptyOrInvalidMaskPassesEverything) {
    PixelMaskFilter f(4, 3);
    const std::vector<EventCD> in = {ev(2, 1, 1, 1), ev(1, 2, 0, 2)};
    std::vector<EventCD> out;
    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_EQ(out.size(), 2u);

    f.set_mask(std::make_shared<std::vector<uint8_t>>(5, 1)); // wrong size: ignored
    out.clear();
    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_EQ(out.size(), 2u);
}

// ------------------------------------------------------------------------------------------------ trail filter

TEST(TrailFilter, TrailKeepsFirstEventOfEachBurst) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::Trail);
    f.set_threshold_us(100);
    // burst at t=0,50,150 (each within 100 of the previous), then a gap > 100, then a polarity change
    const auto out = run(f, {ev(1, 1, 1, 0), ev(1, 1, 1, 50), ev(1, 1, 1, 150), ev(1, 1, 1, 400), ev(1, 1, 0, 420),
                             ev(1, 1, 0, 430), ev(1, 1, 1, 440)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 400, 420, 440}));
}

TEST(TrailFilter, ThresholdIsInclusiveAndMeasuredFromPreviousEvent) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::Trail);
    f.set_threshold_us(100);
    // 0 -> 100 (dt == threshold: same burst) -> 201 (dt = 101: new burst)
    const auto out = run(f, {ev(0, 0, 1, 0), ev(0, 0, 1, 100), ev(0, 0, 1, 201)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 201}));
}

TEST(TrailFilter, PixelsAreIndependent) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::Trail);
    f.set_threshold_us(1000);
    const auto out = run(f, {ev(0, 0, 1, 0), ev(1, 0, 1, 10), ev(0, 1, 1, 20), ev(0, 0, 1, 30), ev(1, 0, 1, 40)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 10, 20}));
}

TEST(TrailFilter, StcCutTrailKeepsOnlySecondEvent) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::StcCutTrail);
    f.set_threshold_us(100);
    // isolated event at 0 (next one 500 later) is dropped; burst 500,550,600,650 -> only 550 kept
    const auto out = run(f, {ev(2, 2, 0, 0), ev(2, 2, 0, 500), ev(2, 2, 0, 550), ev(2, 2, 0, 600), ev(2, 2, 0, 650)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{550}));
}

TEST(TrailFilter, StcKeepTrailKeepsSecondAndFollowing) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::StcKeepTrail);
    f.set_threshold_us(100);
    const auto out = run(f, {ev(2, 2, 0, 0), ev(2, 2, 0, 500), ev(2, 2, 0, 550), ev(2, 2, 0, 600), ev(2, 2, 1, 610),
                             ev(2, 2, 1, 700)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{550, 600, 700}));
}

TEST(TrailFilter, SupportsSubMillisecondThresholds) {
    // 100 us is below the 1 ms hardware minimum of the IMX636 but valid in software.
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::Trail);
    f.set_threshold_us(100);
    const auto out = run(f, {ev(0, 0, 1, 0), ev(0, 0, 1, 90), ev(0, 0, 1, 250), ev(0, 0, 1, 340)});
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 250}));
}

TEST(TrailFilter, ResetForgetsHistory) {
    TrailFilter f(8, 8);
    f.set_mode(TrailMode::Trail);
    f.set_threshold_us(1000);
    run(f, {ev(0, 0, 1, 0)});
    f.reset();
    EXPECT_EQ(run(f, {ev(0, 0, 1, 10)}).size(), 1u);
}

// ------------------------------------------------------------------------------------------------ refractory

TEST(RefractoryFilter, DropsEventsWithinPeriodWhateverThePolarity) {
    RefractoryFilter f(4, 4);
    f.set_period_us(100);
    const std::vector<EventCD> in = {ev(0, 0, 1, 0),   ev(0, 0, 0, 50),  ev(0, 0, 1, 99),
                                     ev(0, 0, 1, 100), ev(1, 0, 1, 101), ev(0, 0, 0, 150)};
    std::vector<EventCD> out;
    f.process(in.data(), in.data() + in.size(), out);
    // dropped events do not extend the period: 100 is exactly one period after 0
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 100, 101}));
}

// ------------------------------------------------------------------------------------------------ ERC emulation

TEST(ErcEmulator, BudgetFollowsRateAndPeriod) {
    ErcEmulator erc;
    erc.set_rate(10'000'000);
    erc.set_period_us(200);
    EXPECT_EQ(erc.budget_per_window(), 2000u); // IMX636: rate / 5000
    erc.set_rate(1'000'000);
    erc.set_period_us(1000);
    EXPECT_EQ(erc.budget_per_window(), 1000u);
}

TEST(ErcEmulator, PassesEverythingUnderBudget) {
    ErcEmulator erc;
    erc.set_rate(50'000); // 10 events per 200 us
    erc.set_period_us(200);
    std::vector<EventCD> in, out;
    for (int i = 0; i < 10; ++i)
        in.push_back(ev(i, 0, 1, i * 10));
    in.push_back(ev(0, 1, 1, 250)); // next window
    erc.process(in.data(), in.data() + in.size(), out);
    EXPECT_EQ(out.size(), 10u); // the event of the second window is held
    erc.flush(out);
    EXPECT_EQ(out.size(), 11u);
}

TEST(ErcEmulator, CapsEachWindowAndSpreadsTheKeptEvents) {
    ErcEmulator erc;
    erc.set_rate(50'000); // 10 events per 200 us window
    erc.set_period_us(200);
    std::vector<EventCD> in, out;
    for (int i = 0; i < 100; ++i) // 100 events in [0, 200)
        in.push_back(ev(i % 50, i / 50, 1, i * 2));
    for (int i = 0; i < 5; ++i) // 5 events in [200, 400)
        in.push_back(ev(i, 5, 0, 200 + i));
    erc.process(in.data(), in.data() + in.size(), out);
    erc.flush(out);
    ASSERT_EQ(out.size(), 15u);
    // kept events of the first window are evenly spread: one every 10 input events
    for (int k = 0; k < 10; ++k)
        EXPECT_EQ(out[k].t, (k * 10 + 9) * 2);
    for (int k = 10; k < 15; ++k)
        EXPECT_EQ(out[k].t, 200 + (k - 10));
}

TEST(ErcEmulator, ZeroRateDropsEverything) {
    ErcEmulator erc;
    erc.set_rate(0);
    std::vector<EventCD> in = {ev(0, 0, 1, 0), ev(0, 0, 1, 300)}, out;
    erc.process(in.data(), in.data() + in.size(), out);
    erc.flush(out);
    EXPECT_TRUE(out.empty());
}

// ------------------------------------------------------------------------------------------------ pipeline

TEST(FilterPipeline, DisabledPipelineIsIdentity) {
    FilterPipeline p(64, 64);
    FilterConfig c;
    p.set_config(c);
    EXPECT_FALSE(p.is_active());
    const auto in   = test::random_events(1000, 64, 64, 1);
    const auto &out = p.process(in.data(), in.data() + in.size());
    ASSERT_EQ(out.size(), in.size());
    EXPECT_EQ(out.back().t, in.back().t);
    EXPECT_EQ(p.stats().in, 1000u);
    EXPECT_EQ(p.stats().out, 1000u);
}

TEST(FilterPipeline, ChainsMaskAndPolarity) {
    FilterPipeline p(4, 4);
    FilterConfig c;
    c.polarity_enabled = true;
    c.keep_on          = true;
    c.keep_off         = false;
    p.set_config(c);
    auto mask = std::make_shared<std::vector<uint8_t>>(16, 0);
    (*mask)[0] = 1;
    p.set_mask(mask);
    EXPECT_TRUE(p.is_active());
    const std::vector<EventCD> in = {ev(0, 0, 1, 1), ev(1, 0, 1, 2), ev(1, 0, 0, 3), ev(2, 2, 1, 4)};
    const auto &out               = p.process(in.data(), in.data() + in.size());
    EXPECT_EQ(times(out), (std::vector<timestamp>{2, 4}));

    c.mask_enabled = false;
    p.set_config(c);
    const auto &out2 = p.process(in.data(), in.data() + in.size());
    EXPECT_EQ(times(out2), (std::vector<timestamp>{1, 2, 4}));
}

TEST(FilterPipeline, DisablingErcReleasesHeldEventsInOrder) {
    FilterPipeline p(16, 16);
    FilterConfig c;
    c.erc_enabled   = true;
    c.erc_rate      = 1'000'000'000;
    c.erc_period_us = 200;
    p.set_config(c);
    const std::vector<EventCD> a = {ev(0, 0, 1, 10), ev(1, 0, 1, 20)};
    EXPECT_TRUE(p.process(a.data(), a.data() + a.size()).empty()); // held in the open window

    c.erc_enabled = false;
    p.set_config(c);
    const std::vector<EventCD> b = {ev(2, 0, 1, 30)};
    const auto &out              = p.process(b.data(), b.data() + b.size());
    EXPECT_EQ(times(out), (std::vector<timestamp>{10, 20, 30}));
}

TEST(FilterPipeline, FlushOutputsErcTail) {
    FilterPipeline p(16, 16);
    FilterConfig c;
    c.erc_enabled = true;
    p.set_config(c);
    const std::vector<EventCD> a = {ev(0, 0, 1, 10)};
    EXPECT_TRUE(p.process(a.data(), a.data() + a.size()).empty());
    EXPECT_EQ(p.flush().size(), 1u);
    EXPECT_EQ(p.stats().out, 1u);
}

TEST(FilterPipeline, TrailThenPolarityOrder) {
    // The trail filter sees both polarities before the polarity filter: an OFF event breaks an ON burst
    // even when OFF events are filtered out afterwards.
    FilterPipeline p(4, 4);
    FilterConfig c;
    c.trail_enabled      = true;
    c.trail_mode         = TrailMode::Trail;
    c.trail_threshold_us = 100;
    c.polarity_enabled   = true;
    c.keep_off           = false;
    p.set_config(c);
    const std::vector<EventCD> in = {ev(0, 0, 1, 0), ev(0, 0, 0, 10), ev(0, 0, 1, 20)};
    const auto &out               = p.process(in.data(), in.data() + in.size());
    EXPECT_EQ(times(out), (std::vector<timestamp>{0, 20}));
}
