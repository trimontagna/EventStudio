#include <fstream>

#include <gtest/gtest.h>

#include "core/mask_set.h"
#include "test_utils.h"

using namespace evs;

namespace {
Polygon rect(float x0, float y0, float x1, float y1) {
    return Polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
}
} // namespace

TEST(MaskSet, BitmapIsUnionOfEnabledShapes) {
    MaskSet set;
    set.add(rect(0, 0, 4, 4));
    set.add(rect(2, 2, 6, 6));
    set.add(rect(10, 10, 12, 12)).enabled = false;
    const auto bitmap = set.build_bitmap(16, 16);
    ASSERT_EQ(bitmap.size(), 256u);
    EXPECT_EQ(std::count(bitmap.begin(), bitmap.end(), uint8_t(1)), 16 + 16 - 4);
    EXPECT_EQ(bitmap[11 * 16 + 11], 0);
}

TEST(MaskSet, EmptyWhenNothingMasked) {
    MaskSet set;
    EXPECT_TRUE(set.build_bitmap(16, 16).empty());
    set.add(rect(0, 0, 4, 4)).enabled = false;
    EXPECT_TRUE(set.build_bitmap(16, 16).empty());
    set.add(Polygon({{0, 0}, {5, 5}})); // open polyline: ignored
    EXPECT_TRUE(set.build_bitmap(16, 16).empty());
}

TEST(MaskSet, MaskedPixelsListsCoordinatesRowMajor) {
    MaskSet set;
    set.add(rect(3, 1, 5, 3)); // pixels (3,1) (4,1) (3,2) (4,2)
    const auto px = set.masked_pixels(8, 8);
    ASSERT_EQ(px.size(), 4u);
    EXPECT_EQ(px[0], std::make_pair(3u, 1u));
    EXPECT_EQ(px[1], std::make_pair(4u, 1u));
    EXPECT_EQ(px[2], std::make_pair(3u, 2u));
    EXPECT_EQ(px[3], std::make_pair(4u, 2u));
}

TEST(MaskSet, DefaultNamesAreUnique) {
    MaskSet set;
    EXPECT_EQ(set.add(rect(0, 0, 1, 1)).name, "Mask 1");
    EXPECT_EQ(set.add(rect(0, 0, 1, 1)).name, "Mask 2");
    set.remove(0);
    EXPECT_EQ(set.add(rect(0, 0, 1, 1)).name, "Mask 3");
    EXPECT_FALSE(set.remove(42));
}

TEST(MaskSet, SaveLoadRoundTrip) {
    MaskSet set;
    set.add(rect(1.25f, 2.5f, 100.125f, 50.75f), "Hot pixels near the lens");
    auto &s  = set.add(Polygon({{10, 10}, {20, 12.5f}, {15, 30}}), "tri");
    s.enabled = false;
    const auto path = test::temp_dir() / "masks.txt";
    std::string error;
    ASSERT_TRUE(set.save(path, &error)) << error;

    MaskSet loaded;
    ASSERT_TRUE(loaded.load(path, &error)) << error;
    EXPECT_TRUE(loaded == set);
    EXPECT_EQ(loaded.shapes()[0].name, "Hot pixels near the lens");
    EXPECT_FALSE(loaded.shapes()[1].enabled);
}

TEST(MaskSet, LoadRejectsInvalidFiles) {
    const auto dir = test::temp_dir();
    {
        std::ofstream(dir / "bad.txt") << "mask 1 a\npt 1 nope\n";
    }
    MaskSet set;
    set.add(rect(0, 0, 1, 1));
    std::string error;
    EXPECT_FALSE(set.load(dir / "bad.txt", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(set.shapes().size(), 1u); // unchanged on failure
    EXPECT_FALSE(set.load(dir / "missing.txt", &error));
}

// ------------------------------------------------------------------------------------------------ categories

TEST(MaskSet, RoiKeepsOnlyInside) {
    MaskSet set;
    set.roi()     = RoiRect{true, RoiMode::Roi, 2, 1, 3, 2}; // pixels x 2..4, y 1..2
    const auto bm = set.build_bitmap(8, 4);
    ASSERT_EQ(bm.size(), 32u);
    EXPECT_EQ(std::count(bm.begin(), bm.end(), uint8_t(1)), 32 - 6);
    EXPECT_EQ(bm[1 * 8 + 2], 0);
    EXPECT_EQ(bm[2 * 8 + 4], 0);
    EXPECT_EQ(bm[2 * 8 + 5], 1);
    EXPECT_EQ(bm[0], 1);
}

TEST(MaskSet, RoniMasksInside) {
    MaskSet set;
    set.roi()     = RoiRect{true, RoiMode::Roni, 2, 1, 3, 2};
    const auto px = set.masked_pixels(8, 4);
    ASSERT_EQ(px.size(), 6u);
    EXPECT_EQ(px.front(), std::make_pair(2u, 1u));
    EXPECT_EQ(px.back(), std::make_pair(4u, 2u));
}

TEST(MaskSet, DisabledOrEmptyRoiMasksNothing) {
    MaskSet set;
    set.roi() = RoiRect{false, RoiMode::Roi, 2, 1, 3, 2};
    EXPECT_TRUE(set.build_bitmap(8, 4).empty());
    set.roi() = RoiRect{true, RoiMode::Roi, 2, 1, 0, 2};
    EXPECT_TRUE(set.build_bitmap(8, 4).empty());
}

TEST(MaskSet, RoiClampToSensor) {
    RoiRect r{true, RoiMode::Roi, -5, 700, 100, 50};
    r.clamp(1280, 720);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.width, 95);
    EXPECT_EQ(r.y, 700);
    EXPECT_EQ(r.height, 20);
}

TEST(MaskSet, PixelMasks) {
    MaskSet set;
    auto &px = set.pixels();
    EXPECT_TRUE(px.add(3, 1));
    EXPECT_FALSE(px.add(3, 1));
    EXPECT_TRUE(px.add(7, 3));
    EXPECT_TRUE(px.add(100, 100)); // outside the sensor: ignored in bitmaps
    auto bm = set.build_bitmap(8, 4);
    EXPECT_EQ(std::count(bm.begin(), bm.end(), uint8_t(1)), 2);
    EXPECT_EQ(bm[1 * 8 + 3], 1);
    EXPECT_TRUE(px.remove(3, 1));
    EXPECT_FALSE(px.remove(3, 1));
    px.enabled = false;
    EXPECT_TRUE(set.build_bitmap(8, 4).empty());
}

TEST(MaskSet, CategoriesSelectWhatIsMasked) {
    MaskSet set;
    set.add(rect(0, 0, 2, 2));                            // 4 px
    set.pixels().add(5, 3);                               // 1 px
    set.roi() = RoiRect{true, RoiMode::Roni, 6, 0, 2, 1}; // 2 px
    auto count = [&](unsigned cats) {
        const auto bm = set.build_bitmap(8, 4, cats);
        return std::count(bm.begin(), bm.end(), uint8_t(1));
    };
    EXPECT_EQ(count(kMaskSoftware), 4);
    EXPECT_EQ(count(kMaskPixels), 1);
    EXPECT_EQ(count(kMaskRoi), 2);
    EXPECT_EQ(count(kMaskAll), 7);
    EXPECT_EQ(count(kMaskSoftware | kMaskPixels), 5);
}

TEST(MaskSet, SaveLoadAllCategories) {
    MaskSet set;
    set.add(rect(1, 2, 30, 40), "poly");
    set.roi() = RoiRect{true, RoiMode::Roni, 10, 20, 300, 200};
    set.pixels().add(5, 6);
    set.pixels().add(1279, 719);
    set.pixels().enabled = false;
    const auto path      = test::temp_dir() / "all.masks";
    std::string error;
    ASSERT_TRUE(set.save(path, &error)) << error;
    MaskSet loaded;
    ASSERT_TRUE(loaded.load(path, &error)) << error;
    EXPECT_TRUE(loaded == set);
}

TEST(MaskSet, LoadsVersion1Files) {
    const auto path = test::temp_dir() / "v1.txt";
    {
        std::ofstream(path) << "# EventStudio masks v1\nmask 1 old\npt 0 0\npt 4 0\npt 4 4\n";
    }
    MaskSet set;
    std::string error;
    ASSERT_TRUE(set.load(path, &error)) << error;
    ASSERT_EQ(set.shapes().size(), 1u);
    EXPECT_FALSE(set.roi().enabled);
    EXPECT_TRUE(set.pixels().pixels.empty());
}
