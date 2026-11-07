#include <random>

#include <gtest/gtest.h>

#include "core/filters.h"
#include "core/hot_pixels.h"
#include "core/mask_set.h"
#include "core/ptv_planner.h"
#include "test_utils.h"

using namespace evs;
using evs::test::ev;

// ------------------------------------------------------------------------------------------------ PTV planner

TEST(PtvPlanner, UserSetupFullFrame) {
    PtvSetup s; // 50 mm over 1280 px, 20 m/s, 3 um particles, f/8, 532 nm, 0.001 ppp
    const auto e = estimate_ptv(s);
    EXPECT_NEAR(e.scale_um_per_px, 39.0625, 1e-9);
    EXPECT_NEAR(e.magnification, 0.1244, 1e-4);
    EXPECT_NEAR(e.velocity_px_per_us, 0.512, 1e-9);
    EXPECT_NEAR(e.image_diam_diff_um, 11.68, 0.01); // diffraction dominates the 0.37 um geometric image
    EXPECT_NEAR(e.image_diam_px, 2.40, 0.01);
    EXPECT_NEAR(e.pixel_dwell_us, 4.69, 0.02);
    EXPECT_NEAR(e.particles_in_roi, 921.6, 1e-6);
    EXPECT_NEAR(e.total_event_rate, 2.27e9, 0.01e9);
    EXPECT_TRUE(e.exceeds_bandwidth);
    EXPECT_NEAR(e.max_ppp_for_bandwidth, 4.4e-4, 0.1e-4);
    EXPECT_TRUE(e.faster_than_pixel);
    EXPECT_NEAR(e.latency_blur_px, 51.2, 1e-6);
    EXPECT_DOUBLE_EQ(e.suggested_activity_window_us, 100.0);
}

TEST(PtvPlanner, SmallerApertureOrRoiReduceTheRate) {
    PtvSetup s;
    const double full = estimate_ptv(s).total_event_rate;
    s.f_number = 4;
    EXPECT_NEAR(estimate_ptv(s).total_event_rate / full, 0.5, 0.01); // image size ~ f-number when diffraction-limited
    s.f_number      = 8;
    s.roi_height_px = 360;
    EXPECT_NEAR(estimate_ptv(s).total_event_rate / full, 0.5, 1e-9);
}

TEST(PtvPlanner, SlowFlowIsWithinLimits) {
    PtvSetup s;
    s.velocity_m_s = 0.2;
    const auto e   = estimate_ptv(s);
    EXPECT_FALSE(e.exceeds_bandwidth);
    EXPECT_FALSE(e.faster_than_pixel);
    EXPECT_GT(e.suggested_activity_window_us, 4 * e.pixel_dwell_us - 1e-9);
}

// ------------------------------------------------------------------------------------------------ activity filter

TEST(ActivityFilter, CausalRemovesIsolatedEventsKeepsSupportedOnes) {
    ActivityFilter f(16, 16);
    f.set_look_ahead(false);
    f.set_window_us(100);
    std::vector<EventCD> out;
    const std::vector<EventCD> in = {
        ev(5, 5, 1, 0),    // no neighbour yet: dropped (but recorded)
        ev(6, 5, 1, 10),   // neighbour (5,5) 10 us ago: kept
        ev(12, 12, 1, 20), // isolated: dropped
        ev(7, 6, 0, 50),   // diagonal neighbour (6,5): kept (any polarity)
        ev(5, 5, 1, 300),  // neighbours too old: dropped
    };
    f.process(in.data(), in.data() + in.size(), out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].t, 10);
    EXPECT_EQ(out[1].t, 50);
}

TEST(ActivityFilter, LookAheadKeepsTheFirstEventOfANeighbourhood) {
    ActivityFilter f(16, 16);
    f.set_window_us(100);
    std::vector<EventCD> out;
    const std::vector<EventCD> in = {
        ev(5, 5, 1, 0),    // supported by the later (6,5): kept
        ev(6, 5, 1, 10),   // supported by (5,5): kept
        ev(12, 12, 1, 20), // isolated: dropped
        ev(7, 6, 0, 50),   // supported by (6,5): kept
        ev(5, 5, 1, 300),  // neighbours too old, none later: dropped
        ev(9, 9, 1, 1000),
    };
    f.process(in.data(), in.data() + in.size(), out);
    // events are released once the window after them has passed
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].t, 0);
    EXPECT_EQ(out[1].t, 10);
    EXPECT_EQ(out[2].t, 50);
    const std::vector<EventCD> late = {ev(9, 10, 1, 1050)}; // future support for (9,9)
    f.process(late.data(), late.data() + late.size(), out);
    f.flush(out);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[3].t, 1000);
    EXPECT_EQ(out[4].t, 1050);
}

TEST(ActivityFilter, LookAheadWindowIsSymmetric) {
    ActivityFilter f(16, 16);
    f.set_window_us(100);
    std::vector<EventCD> out;
    const std::vector<EventCD> in = {ev(1, 1, 1, 0), ev(2, 1, 1, 101), ev(8, 8, 1, 500), ev(9, 8, 1, 600)};
    f.process(in.data(), in.data() + in.size(), out);
    f.flush(out);
    ASSERT_EQ(out.size(), 2u); // 0 / 101 are 101 us apart: not neighbours in time; 500 / 600 are
    EXPECT_EQ(out[0].t, 500);
    EXPECT_EQ(out[1].t, 600);
}

TEST(ActivityFilter, SamePolarityAndMinNeighbours) {
    ActivityFilter f(16, 16);
    f.set_look_ahead(false);
    f.set_window_us(100);
    f.set_same_polarity(true);
    std::vector<EventCD> out;
    std::vector<EventCD> in = {ev(5, 5, 1, 0), ev(6, 5, 0, 10)};
    f.process(in.data(), in.data() + in.size(), out);
    EXPECT_TRUE(out.empty()); // opposite polarity does not count

    ActivityFilter g(16, 16);
    g.set_look_ahead(false);
    g.set_window_us(100);
    g.set_min_neighbors(2);
    out.clear();
    in = {ev(5, 5, 1, 0), ev(6, 6, 1, 1), ev(5, 6, 1, 2)};
    g.process(in.data(), in.data() + in.size(), out);
    ASSERT_EQ(out.size(), 1u); // only the third event has two active neighbours
    EXPECT_EQ(out[0].t, 2);
}

TEST(ActivityFilter, RadiusExtendsTheNeighbourhood) {
    ActivityFilter f(16, 16);
    f.set_window_us(100);
    std::vector<EventCD> out;
    const std::vector<EventCD> in = {ev(5, 5, 1, 0), ev(7, 5, 1, 1)};
    f.process(in.data(), in.data() + in.size(), out);
    f.flush(out);
    EXPECT_TRUE(out.empty());
    ActivityFilter g(16, 16);
    g.set_window_us(100);
    g.set_radius(2);
    g.process(in.data(), in.data() + in.size(), out);
    g.flush(out);
    EXPECT_EQ(out.size(), 2u);
}

TEST(ActivityFilter, SyntheticParticleTrackInNoise) {
    // 2.4 px particle at 0.512 px/us (the user's setup) crossing the sensor, plus uniform background noise.
    const int w = 1280, h = 720;
    std::mt19937 rng(3);
    std::vector<EventCD> events;
    for (int x = 10; x < 1270; ++x) {
        const timestamp t_on = static_cast<timestamp>(x / 0.512);
        for (int y = 359; y <= 361; ++y) {
            std::uniform_int_distribution<int> jitter(0, 20); // pixel response spread
            events.push_back(ev(x, y, 1, t_on + jitter(rng)));
            events.push_back(ev(x, y, 0, t_on + 5 + jitter(rng)));
        }
    }
    const size_t track = events.size();
    // background noise: 1 Hz per pixel over the 2.5 ms of the track -> ~2300 events
    std::uniform_int_distribution<int> rx(0, w - 1), ry(0, h - 1), rp(0, 1);
    std::uniform_int_distribution<timestamp> rt(0, 2500);
    for (int i = 0; i < 2300; ++i)
        events.push_back(ev(rx(rng), ry(rng), rp(rng), rt(rng)));
    std::stable_sort(events.begin(), events.end(), [](const EventCD &a, const EventCD &b) { return a.t < b.t; });

    auto run = [&](bool look_ahead, size_t &kept_track, size_t &kept_noise, double &mean_shift) {
        ActivityFilter f(w, h);
        f.set_window_us(100);
        f.set_look_ahead(look_ahead);
        std::vector<EventCD> out;
        f.process(events.data(), events.data() + events.size(), out);
        f.flush(out);
        kept_track = kept_noise = 0;
        for (const auto &e : out)
            (e.y >= 359 && e.y <= 361 && e.x >= 10 && e.x < 1270) ? ++kept_track : ++kept_noise;
        // first ON event of each column: arrival time estimate, compared with the true arrival x / v
        std::vector<timestamp> first(w, -1);
        for (const auto &e : out)
            if (e.p == 1 && e.y >= 359 && e.y <= 361 && first[e.x] < 0)
                first[e.x] = e.t;
        double shift = 0;
        int n        = 0;
        for (int x = 10; x < 1270; ++x)
            if (first[x] >= 0) {
                shift += static_cast<double>(first[x] - static_cast<timestamp>(x / 0.512));
                ++n;
            }
        mean_shift = shift / std::max(1, n);
    };
    size_t track_la, noise_la, track_c, noise_c;
    double shift_la, shift_c;
    run(true, track_la, noise_la, shift_la);
    run(false, track_c, noise_c, shift_c);
    std::printf("[ info ] track %zu events, noise 2300: look-ahead kept %zu track / %zu noise, arrival shift %.1f us; "
                "causal kept %zu track / %zu noise, arrival shift %.1f us\n",
                track, track_la, noise_la, shift_la, track_c, noise_c, shift_c);
    EXPECT_GT(track_la, track * 999 / 1000); // look-ahead keeps (almost) the whole track
    EXPECT_GT(track_c, track * 90 / 100);    // causal drops the first arrivals of each neighbourhood...
    EXPECT_LT(track_c, track_la);
    EXPECT_GT(shift_c, shift_la); // ...which delays the measured arrival times
    EXPECT_LT(noise_la, 25u);     // ~1 % of the noise survives (chance coincidences / noise next to the track)
    EXPECT_LT(noise_c, 25u);
}

TEST(FilterPipeline, CausalActivityStage) {
    FilterPipeline p(16, 16);
    FilterConfig c;
    c.activity_enabled    = true;
    c.activity_window_us  = 100;
    c.activity_look_ahead = false;
    p.set_config(c);
    EXPECT_TRUE(p.is_active());
    EXPECT_EQ(p.holding_time(), 0);
    const std::vector<EventCD> in = {ev(1, 1, 1, 0), ev(2, 1, 1, 5), ev(9, 9, 1, 6)};
    const auto &out               = p.process(in.data(), in.data() + in.size());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].t, 5);
}

TEST(FilterPipeline, LookAheadActivityIsFlushedThroughLaterStages) {
    FilterPipeline p(16, 16);
    FilterConfig c;
    c.activity_enabled   = true;
    c.activity_window_us = 100;
    c.polarity_enabled   = true;
    c.keep_off           = false;
    c.erc_enabled        = true;
    c.erc_period_us      = 200;
    p.set_config(c);
    EXPECT_EQ(p.holding_time(), 300);
    const std::vector<EventCD> in = {ev(1, 1, 1, 0), ev(2, 1, 0, 5), ev(9, 9, 1, 6), ev(3, 1, 1, 7)};
    EXPECT_TRUE(p.process(in.data(), in.data() + in.size()).empty()); // all held
    const auto &out = p.flush();
    ASSERT_EQ(out.size(), 2u); // supported ON events (1,1) and (3,1); the OFF one is removed by the polarity stage
    EXPECT_EQ(out[0].t, 0);
    EXPECT_EQ(out[1].t, 7);
    EXPECT_EQ(p.stats().out, 2u);
}

TEST(FilterPipeline, DisablingLookAheadReleasesHeldEvents) {
    FilterPipeline p(16, 16);
    FilterConfig c;
    c.activity_enabled = true;
    p.set_config(c);
    const std::vector<EventCD> a = {ev(1, 1, 1, 0), ev(2, 1, 1, 5)};
    EXPECT_TRUE(p.process(a.data(), a.data() + a.size()).empty());
    c.activity_enabled = false;
    p.set_config(c);
    const std::vector<EventCD> b = {ev(9, 9, 1, 50)};
    const auto &out              = p.process(b.data(), b.data() + b.size());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[2].t, 50);
}

// ------------------------------------------------------------------------------------------------ hot pixels

TEST(HotPixels, DetectsPixelsMuchMoreActiveThanTheirNeighbours) {
    PixelActivity act(64, 48);
    std::vector<EventCD> events;
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> rx(0, 63), ry(0, 47);
    // 2 s of background: ~1 event / pixel / s, plus a busy region (a "flow") at 50 Hz / px and two hot pixels
    for (timestamp t = 0; t < 2'000'000; t += 300) {
        events.push_back(ev(rx(rng), ry(rng), 1, t));
        if (t % 3000 == 0)
            events.push_back(ev(10, 10, 1, t)); // 333 Hz hot pixel
        if (t % 15000 == 0)
            events.push_back(ev(50, 40, 0, t)); // 67 Hz hot pixel
    }
    for (timestamp t = 0; t < 2'000'000; t += 20000) // busy 8x8 region, 50 Hz per pixel
        for (int y = 30; y < 38; ++y)
            for (int x = 20; x < 28; ++x)
                events.push_back(ev(x, y, 1, t + x + y));
    std::stable_sort(events.begin(), events.end(), [](const EventCD &a, const EventCD &b) { return a.t < b.t; });
    act.add(events.data(), events.data() + events.size());
    EXPECT_NEAR(static_cast<double>(act.duration_us()), 2e6, 1e3);

    HotPixelParams params;
    params.min_rate_hz = 20;
    params.local_ratio = 10;
    const auto hot     = detect_hot_pixels(act, params);
    ASSERT_EQ(hot.size(), 2u); // the busy region is not flagged (its neighbours are as active)
    EXPECT_EQ(hot[0].x, 10u);
    EXPECT_EQ(hot[0].y, 10u);
    EXPECT_NEAR(hot[0].rate_hz, 333, 5);
    EXPECT_EQ(hot[1].x, 50u);
    EXPECT_EQ(hot[1].y, 40u);

    params.min_rate_hz = 100; // only the hottest one
    EXPECT_EQ(detect_hot_pixels(act, params).size(), 1u);
}

TEST(HotPixels, EmptyOrResetActivity) {
    PixelActivity act(8, 8);
    EXPECT_TRUE(detect_hot_pixels(act, HotPixelParams()).empty());
    const std::vector<EventCD> in = {ev(1, 1, 1, 0), ev(1, 1, 1, 1000)};
    act.add(in.data(), in.data() + in.size());
    EXPECT_EQ(act.total(), 2u);
    act.reset();
    EXPECT_EQ(act.total(), 0u);
    EXPECT_EQ(act.duration_us(), 0);
}

// ------------------------------------------------------------------------------------------------ software pixels

TEST(MaskSet, SoftwarePixelsBelongToTheSoftwareCategory) {
    MaskSet set;
    set.software_pixels().add(2, 3);
    set.pixels().add(4, 1);
    auto bm = set.build_bitmap(8, 4, kMaskSoftware);
    EXPECT_EQ(std::count(bm.begin(), bm.end(), uint8_t(1)), 1);
    EXPECT_EQ(bm[3 * 8 + 2], 1);
    bm = set.build_bitmap(8, 4, kMaskPixels);
    EXPECT_EQ(bm[1 * 8 + 4], 1);

    const auto path = test::temp_dir() / "spx.masks";
    ASSERT_TRUE(set.save(path));
    MaskSet loaded;
    ASSERT_TRUE(loaded.load(path));
    EXPECT_TRUE(loaded == set);
}
