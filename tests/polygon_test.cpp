#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "core/polygon.h"

using namespace evs;

namespace {
Polygon rect(float x0, float y0, float x1, float y1) {
    return Polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
}
constexpr float kPi = 3.14159265358979f;
} // namespace

TEST(Polygon, ContainsUsesEvenOddRule) {
    const Polygon square = rect(0, 0, 10, 10);
    EXPECT_TRUE(square.contains({5, 5}));
    EXPECT_FALSE(square.contains({-1, 5}));
    EXPECT_FALSE(square.contains({5, 11}));

    // Concave "U" shape: the notch is outside.
    const Polygon u({{0, 0}, {3, 0}, {3, 5}, {6, 5}, {6, 0}, {9, 0}, {9, 9}, {0, 9}});
    EXPECT_TRUE(u.contains({1, 1}));
    EXPECT_FALSE(u.contains({4.5f, 2}));
    EXPECT_TRUE(u.contains({4.5f, 7}));
}

TEST(Polygon, DegeneratePolygonsContainNothing) {
    EXPECT_FALSE(Polygon().contains({0, 0}));
    EXPECT_FALSE(Polygon({{0, 0}, {10, 10}}).contains({5, 5}));
    std::vector<uint8_t> bitmap;
    Polygon({{0, 0}, {10, 10}}).rasterize(bitmap, 20, 20);
    EXPECT_TRUE(bitmap.empty());
}

TEST(Polygon, RectangleOnPixelBoundariesCoversExactPixels) {
    const Polygon r = rect(2, 3, 12, 8); // pixels x in [2,11], y in [3,7]
    EXPECT_EQ(r.pixel_count(20, 20), 50u);
    std::vector<uint8_t> bitmap(400, 0);
    r.rasterize(bitmap, 20, 20);
    EXPECT_EQ(bitmap[3 * 20 + 2], 1);
    EXPECT_EQ(bitmap[7 * 20 + 11], 1);
    EXPECT_EQ(bitmap[8 * 20 + 11], 0);
    EXPECT_EQ(bitmap[3 * 20 + 12], 0);
    EXPECT_EQ(bitmap[2 * 20 + 2], 0);
}

TEST(Polygon, RasterizationMatchesPointInPolygon) {
    // Random (possibly concave or self-intersecting) polygons: every pixel center is classified the same way.
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> coord(-10.f, 70.f);
    std::uniform_int_distribution<int> npts(3, 12);
    const int w = 64, h = 48;
    for (int trial = 0; trial < 200; ++trial) {
        Polygon poly;
        const int n = npts(rng);
        for (int i = 0; i < n; ++i)
            poly.add_point({coord(rng), coord(rng) * 0.75f});
        std::vector<uint8_t> bitmap(w * h, 0);
        poly.rasterize(bitmap, w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                ASSERT_EQ(bitmap[y * w + x] != 0, poly.contains({x + 0.5f, y + 0.5f}))
                    << "trial " << trial << " pixel " << x << "," << y;
    }
}

TEST(Polygon, RasterizationClipsToImage) {
    const Polygon r = rect(-5, -5, 3, 2); // only pixels x in [0,2], y in [0,1] are in the image
    EXPECT_EQ(r.pixel_count(10, 10), 6u);
    const Polygon outside = rect(20, 20, 30, 30);
    EXPECT_EQ(outside.pixel_count(10, 10), 0u);
}

TEST(Polygon, CentroidAndArea) {
    const Polygon r = rect(0, 0, 4, 2);
    EXPECT_FLOAT_EQ(std::abs(r.signed_area()), 8.f);
    const Vec2 c = r.centroid();
    EXPECT_FLOAT_EQ(c.x, 2.f);
    EXPECT_FLOAT_EQ(c.y, 1.f);
    // Non-uniform vertex density does not bias the area centroid.
    const Polygon r2({{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {4, 2}, {0, 2}});
    EXPECT_NEAR(r2.centroid().x, 2.f, 1e-5f);
}

TEST(Polygon, TranslateScaleRotate) {
    Polygon r = rect(0, 0, 2, 2);
    r.translate({10, 5});
    EXPECT_FLOAT_EQ(r.points()[0].x, 10.f);
    EXPECT_FLOAT_EQ(r.points()[0].y, 5.f);

    Polygon s = rect(0, 0, 2, 2);
    s.scale(2.f, 3.f, {1, 1});
    EXPECT_FLOAT_EQ(s.points()[0].x, -1.f);
    EXPECT_FLOAT_EQ(s.points()[0].y, -2.f);
    EXPECT_FLOAT_EQ(s.points()[2].x, 3.f);
    EXPECT_FLOAT_EQ(s.points()[2].y, 4.f);
    EXPECT_FLOAT_EQ(std::abs(s.signed_area()), 24.f);

    Polygon q = rect(0, 0, 4, 2);
    q.rotate(kPi / 2, q.centroid());
    Vec2 mn, mx;
    q.bounds(mn, mx);
    EXPECT_NEAR(mn.x, 1.f, 1e-5f);
    EXPECT_NEAR(mx.x, 3.f, 1e-5f);
    EXPECT_NEAR(mn.y, -1.f, 1e-5f);
    EXPECT_NEAR(mx.y, 3.f, 1e-5f);
    EXPECT_NEAR(std::abs(q.signed_area()), 8.f, 1e-4f);
}

TEST(Polygon, RotationPreservesPixelCountApproximately) {
    Polygon r = rect(100, 100, 200, 150);
    const auto before = r.pixel_count(400, 400);
    r.rotate(kPi / 6, r.centroid());
    const auto after = r.pixel_count(400, 400);
    EXPECT_NEAR(static_cast<double>(after), static_cast<double>(before), before * 0.02);
}

TEST(Polygon, EditPoints) {
    Polygon p = rect(0, 0, 10, 10);
    p.insert_point(1, {5, -2});
    ASSERT_EQ(p.size(), 5u);
    EXPECT_FLOAT_EQ(p.points()[1].x, 5.f);
    EXPECT_TRUE(p.remove_point(1));
    EXPECT_EQ(p.size(), 4u);
    EXPECT_TRUE(p.remove_point(0));
    EXPECT_FALSE(p.remove_point(0)); // cannot go below a triangle
    EXPECT_FALSE(p.remove_point(10));
    p.move_point(0, {42, 43});
    EXPECT_FLOAT_EQ(p.points()[0].x, 42.f);
}

TEST(Polygon, NearestVertexAndEdge) {
    const Polygon p = rect(0, 0, 10, 10);
    EXPECT_EQ(p.nearest_vertex({9.5f, 0.4f}, 1.f), 1);
    EXPECT_EQ(p.nearest_vertex({5, 5}, 1.f), -1);
    Vec2 proj;
    EXPECT_EQ(p.nearest_edge({5, 0.5f}, 1.f, &proj), 0);
    EXPECT_FLOAT_EQ(proj.x, 5.f);
    EXPECT_FLOAT_EQ(proj.y, 0.f);
    EXPECT_EQ(p.nearest_edge({0.3f, 5}, 1.f), 3); // closing edge (3 -> 0)
    EXPECT_EQ(p.nearest_edge({5, 5}, 1.f), -1);
}
