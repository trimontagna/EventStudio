#include <gtest/gtest.h>

#include "core/frame_renderer.h"
#include "test_utils.h"

using namespace evs;
using evs::test::ev;

namespace {
Rgb pixel(const std::vector<uint8_t> &rgba, int w, int x, int y) {
    const uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    return {p[0], p[1], p[2]};
}
bool operator==(Rgb a, Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
void ingest(FrameRenderer &r, const std::vector<EventCD> &v) {
    r.ingest(v.data(), v.data() + v.size());
}
} // namespace

TEST(FrameRenderer, AccumulationShowsLastPolarityWithinWindow) {
    FrameRenderer r(8, 4);
    PreviewSettings s;
    s.mode            = PreviewMode::Accumulation;
    s.accumulation_us = 1000;
    r.set_settings(s);
    ingest(r, {ev(1, 1, 1, 100), ev(2, 1, 0, 900), ev(3, 1, 1, 1500), ev(3, 1, 0, 1600)});
    EXPECT_EQ(r.last_timestamp(), 1600);
    std::vector<uint8_t> rgba;
    r.render(1600, rgba);
    ASSERT_EQ(rgba.size(), 8u * 4 * 4);
    EXPECT_TRUE(pixel(rgba, 8, 1, 1) == palette_background(Palette::Dark)); // 1500 us old
    EXPECT_TRUE(pixel(rgba, 8, 2, 1) == palette_off(Palette::Dark));        // 700 us old
    EXPECT_TRUE(pixel(rgba, 8, 3, 1) == palette_off(Palette::Dark));        // last polarity wins
    EXPECT_TRUE(pixel(rgba, 8, 0, 0) == palette_background(Palette::Dark));
    EXPECT_EQ(rgba[3], 255);
}

TEST(FrameRenderer, PalettesChangeColors) {
    FrameRenderer r(2, 1);
    PreviewSettings s;
    s.palette = Palette::Gray;
    r.set_settings(s);
    ingest(r, {ev(0, 0, 1, 10)});
    std::vector<uint8_t> rgba;
    r.render(10, rgba);
    EXPECT_TRUE(pixel(rgba, 2, 0, 0) == (Rgb{255, 255, 255}));
    EXPECT_TRUE(pixel(rgba, 2, 1, 0) == (Rgb{128, 128, 128}));
}

TEST(FrameRenderer, TimeDecayFadesWithAge) {
    FrameRenderer r(2, 1);
    PreviewSettings s;
    s.mode     = PreviewMode::TimeDecay;
    s.decay_us = 1000;
    r.set_settings(s);
    ingest(r, {ev(0, 0, 1, 0), ev(1, 0, 1, 900)});
    std::vector<uint8_t> rgba;
    r.render(1000, rgba);
    const Rgb old = pixel(rgba, 2, 0, 0), recent = pixel(rgba, 2, 1, 0), bg = palette_background(Palette::Dark);
    EXPECT_GT(recent.r, old.r);
    EXPECT_GT(old.r, bg.r);
    r.render(100000, rgba);
    EXPECT_TRUE(pixel(rgba, 2, 0, 0) == bg);
}

TEST(FrameRenderer, TimeSurfaceMapsAgeToColor) {
    FrameRenderer r(3, 1);
    PreviewSettings s;
    s.mode            = PreviewMode::TimeSurface;
    s.accumulation_us = 1000;
    r.set_settings(s);
    ingest(r, {ev(0, 0, 1, 1000), ev(1, 0, 0, 100)});
    std::vector<uint8_t> rgba;
    r.render(1000, rgba);
    EXPECT_TRUE(pixel(rgba, 3, 0, 0) == colormap_jet(1.f));
    EXPECT_TRUE(pixel(rgba, 3, 1, 0) == colormap_jet(1.f - 900.f / 1000));
    EXPECT_TRUE(pixel(rgba, 3, 2, 0) == (Rgb{0, 0, 0}));
}

TEST(FrameRenderer, HistogramAndDifferenceCountWithinWindow) {
    FrameRenderer r(3, 1);
    PreviewSettings s;
    s.mode             = PreviewMode::Difference;
    s.accumulation_us  = 1000;
    s.histo_saturation = 2;
    r.set_settings(s);
    // pixel 0: 2 ON in window; pixel 1: 1 ON + 3 OFF; pixel 2: 5 ON but too old
    ingest(r, {ev(2, 0, 1, 10), ev(2, 0, 1, 11), ev(2, 0, 1, 12), ev(2, 0, 1, 13), ev(2, 0, 1, 14),
               ev(0, 0, 1, 1500), ev(1, 0, 1, 1600), ev(1, 0, 0, 1700), ev(1, 0, 0, 1800), ev(0, 0, 1, 1900),
               ev(1, 0, 0, 2000)});
    std::vector<uint8_t> rgba;
    r.render(2000, rgba);
    EXPECT_TRUE(pixel(rgba, 3, 0, 0) == palette_on(Palette::Dark));          // +2 -> saturated ON
    EXPECT_TRUE(pixel(rgba, 3, 1, 0) == palette_off(Palette::Dark));         // -2 -> saturated OFF
    EXPECT_TRUE(pixel(rgba, 3, 2, 0) == palette_background(Palette::Dark));  // outside the window

    s.mode = PreviewMode::Histogram;
    r.set_settings(s); // switching mode restarts the window
    ingest(r, {ev(0, 0, 1, 2100)});
    r.render(2100, rgba);
    const Rgb half = pixel(rgba, 3, 0, 0), bg = palette_background(Palette::Dark), on = palette_on(Palette::Dark);
    EXPECT_GT(half.r, bg.r);
    EXPECT_LT(half.r, on.r);
}

TEST(FrameRenderer, IntegrationRaisesAndLowersGrayLevel) {
    FrameRenderer r(2, 1);
    PreviewSettings s;
    s.mode     = PreviewMode::Integration;
    s.decay_us = 100000;
    r.set_settings(s);
    ingest(r, {ev(0, 0, 1, 0), ev(0, 0, 1, 10), ev(1, 0, 0, 10)});
    std::vector<uint8_t> rgba;
    r.render(20, rgba);
    EXPECT_GT(pixel(rgba, 2, 0, 0).r, 160);
    EXPECT_LT(pixel(rgba, 2, 1, 0).r, 128);
}

TEST(FrameRenderer, ResetClearsState) {
    FrameRenderer r(2, 1);
    ingest(r, {ev(0, 0, 1, 10)});
    r.reset();
    EXPECT_EQ(r.last_timestamp(), 0);
    std::vector<uint8_t> rgba;
    r.render(10, rgba);
    EXPECT_TRUE(pixel(rgba, 2, 0, 0) == palette_background(Palette::Dark));
}

TEST(FrameRenderer, IgnoresOutOfBoundsEvents) {
    FrameRenderer r(2, 2);
    ingest(r, {ev(5, 0, 1, 10), ev(0, 9, 1, 10)});
    std::vector<uint8_t> rgba;
    r.render(10, rgba);
    EXPECT_EQ(rgba.size(), 16u);
}

TEST(Colormap, JetEndpoints) {
    const Rgb lo = colormap_jet(0.f), hi = colormap_jet(1.f);
    EXPECT_EQ(lo.r, 0);
    EXPECT_GT(lo.b, 100);
    EXPECT_GT(hi.r, 100);
    EXPECT_EQ(hi.b, 0);
}
