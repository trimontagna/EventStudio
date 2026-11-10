#include <gtest/gtest.h>

#include <fstream>

#include "core/camera_settings.h"
#include "test_utils.h"

using namespace evs;

TEST(CameraSettings, RoundTripsThroughAFile) {
    CameraSettings s;
    s.biases             = {{"bias_diff_on", 25}, {"bias_diff_off", -12}, {"bias_hpf", 0}};
    s.has_erc            = true;
    s.erc_enabled        = true;
    s.erc_rate           = 40'000'000;
    s.has_trail          = true;
    s.trail_enabled      = false;
    s.trail_mode         = TrailMode::StcKeepTrail;
    s.trail_threshold_us = 7000;
    s.trigger_in         = {{"Main", true}, {"Aux", false}};

    const auto path = test::temp_dir() / "sub" / camera_settings_file_name("00050423");
    std::string error;
    ASSERT_TRUE(s.to_file("00050423").save(path, &error)) << error;
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    KeyValueFile f;
    ASSERT_TRUE(f.load(path));
    const auto r = CameraSettings::from_file(f);
    EXPECT_EQ(r.biases, s.biases);
    EXPECT_TRUE(r.has_erc);
    EXPECT_TRUE(r.erc_enabled);
    EXPECT_EQ(r.erc_rate, 40'000'000u);
    EXPECT_TRUE(r.has_trail);
    EXPECT_FALSE(r.trail_enabled);
    EXPECT_EQ(r.trail_mode, TrailMode::StcKeepTrail);
    EXPECT_EQ(r.trail_threshold_us, 7000u);
    EXPECT_EQ(r.trigger_in, s.trigger_in);
}

TEST(CameraSettings, MissingSectionsAreReported) {
    KeyValueFile f;
    f.set_int("bias.bias_fo", 10);
    f.set("bias.broken", "abc");
    const auto r = CameraSettings::from_file(f);
    EXPECT_EQ(r.biases.size(), 1u);
    EXPECT_EQ(r.biases.at("bias_fo"), 10);
    EXPECT_FALSE(r.has_erc);
    EXPECT_FALSE(r.has_trail);
}

TEST(KeyValueFile, IgnoresCommentsAndBlankLinesAndTrims) {
    const auto path = test::temp_dir() / "a.ini";
    {
        std::ofstream out(path);
        out << "# comment\n\n  key = some value \r\nnoequals\nempty=\n";
    }
    KeyValueFile f;
    ASSERT_TRUE(f.load(path));
    EXPECT_EQ(f.get("key").value_or(""), "some value");
    EXPECT_EQ(f.get("empty").value_or("x"), "");
    EXPECT_FALSE(f.get("noequals"));
    EXPECT_EQ(f.get_int("key", -1), -1);
    EXPECT_FALSE(f.load(path.parent_path() / "missing.ini"));
    EXPECT_TRUE(f.values.empty());
}

TEST(CameraSettings, FileNameIsSanitized) {
    EXPECT_EQ(camera_settings_file_name("P50905:ab/c"), "P50905_ab_c.ini");
    EXPECT_EQ(camera_settings_file_name(""), "unknown.ini");
}
