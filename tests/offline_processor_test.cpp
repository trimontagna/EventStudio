#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include "core/mask_set.h"
#include "core/raw_file_writer.h"
#include "io/offline_processor.h"
#include "test_utils.h"

using namespace evs;

namespace {

const char *kEvt3File  = "blinking_gen4_with_ext_triggers.raw";
const char *kEvt21File = "standup_evt21-legacy.raw";
const char *kHdf5File  = "blinking_gen4_with_ext_triggers.hdf5";

class OfflineProcessor : public ::testing::Test {
protected:
    void SetUp() override {
        if (!test::has_data_file(kEvt3File))
            GTEST_SKIP() << "OpenEB test data not found";
        input_ = test::data_file(kEvt3File);
        dir_   = test::temp_dir();
    }
    // Decoded source with time shifting, as the processor reads it.
    const std::vector<EventCD> &source() {
        if (source_.empty())
            source_ = test::read_all_events(input_, true);
        return source_;
    }
    ExportJob job(ExportFormat format, const std::string &name) {
        ExportJob j;
        j.input  = input_;
        j.output = dir_ / name;
        j.format = format;
        return j;
    }
    std::filesystem::path input_, dir_;
    std::vector<EventCD> source_;
};

} // namespace

TEST(ExportFormat, Extensions) {
    EXPECT_STREQ(default_extension(ExportFormat::RawEvt21), ".raw");
    EXPECT_STREQ(default_extension(ExportFormat::Hdf5), ".hdf5");
    EXPECT_STREQ(default_extension(ExportFormat::Avi), ".avi");
}

TEST_F(OfflineProcessor, UnfilteredRawExportIsLossless) {
    for (auto format : {ExportFormat::RawEvt3, ExportFormat::RawEvt21, ExportFormat::RawEvt2}) {
        auto j          = job(format, std::string("copy_") + std::to_string(static_cast<int>(format)) + ".raw");
        j.apply_filters = false;
        const auto res  = run_export(j);
        ASSERT_TRUE(res.ok) << res.error;
        EXPECT_EQ(res.events_in, source().size());
        EXPECT_EQ(res.events_out, source().size());
        std::string diff;
        EXPECT_TRUE(test::same_events(source(), test::read_all_events(j.output, false), &diff))
            << to_string(format) << ": " << diff;
    }
}

TEST_F(OfflineProcessor, ConvertsEvt21LegacyRecordingToEvt3) {
    if (!test::has_data_file(kEvt21File))
        GTEST_SKIP();
    auto j          = job(ExportFormat::RawEvt3, "standup_evt3.raw");
    j.input         = test::data_file(kEvt21File);
    j.apply_filters = false;
    j.end_us        = 2'000'000;
    const auto res  = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    auto expected = test::read_all_events(j.input, true);
    expected.erase(std::remove_if(expected.begin(), expected.end(), [](const EventCD &e) { return e.t > 2'000'000; }),
                   expected.end());
    std::string diff;
    EXPECT_TRUE(test::same_events(expected, test::read_all_events(j.output, false), &diff)) << diff;
}

TEST_F(OfflineProcessor, PolarityFilterKeepsOnlyOnEvents) {
    auto j                     = job(ExportFormat::RawEvt3, "on.raw");
    j.filters.polarity_enabled = true;
    j.filters.keep_off         = false;
    const auto res             = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto out = test::read_all_events(j.output, false);
    const auto on_count =
        std::count_if(source().begin(), source().end(), [](const EventCD &e) { return e.p == 1; });
    EXPECT_EQ(static_cast<int64_t>(out.size()), on_count);
    EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](const EventCD &e) { return e.p == 1; }));
}

TEST_F(OfflineProcessor, MaskRemovesEventsInsidePolygon) {
    MaskSet masks;
    masks.add(Polygon({{300, 200}, {900, 150}, {1000, 600}, {250, 500}}));
    const auto bitmap = std::make_shared<std::vector<uint8_t>>(masks.build_bitmap(1280, 720));
    auto j            = job(ExportFormat::RawEvt21, "masked.raw");
    j.mask            = bitmap;
    const auto res    = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto out = test::read_all_events(j.output, false);
    for (const auto &e : out)
        ASSERT_EQ((*bitmap)[e.y * 1280 + e.x], 0) << e.x << "," << e.y;
    const auto expected = std::count_if(source().begin(), source().end(),
                                        [&](const EventCD &e) { return (*bitmap)[e.y * 1280 + e.x] == 0; });
    EXPECT_EQ(static_cast<int64_t>(out.size()), expected);
    EXPECT_LT(out.size(), source().size());
}

TEST_F(OfflineProcessor, TimeRangeIsApplied) {
    auto j          = job(ExportFormat::Csv, "range.csv");
    j.apply_filters = false;
    j.start_us      = 500'000;
    j.end_us        = 900'000;
    const auto res  = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto expected = std::count_if(source().begin(), source().end(),
                                        [](const EventCD &e) { return e.t >= 500'000 && e.t <= 900'000; });
    EXPECT_EQ(static_cast<int64_t>(res.events_out), expected);

    std::ifstream in(j.output);
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "x,y,p,t");
    int64_t lines = 0;
    while (std::getline(in, line)) {
        ++lines;
        const auto t = std::stoll(line.substr(line.rfind(',') + 1));
        ASSERT_GE(t, 500'000);
        ASSERT_LE(t, 900'000);
    }
    EXPECT_EQ(lines, expected);
}

TEST_F(OfflineProcessor, TrailAndErcReduceTheEventCount) {
    auto j                       = job(ExportFormat::RawEvt3, "trail_erc.raw");
    j.filters.trail_enabled      = true;
    j.filters.trail_mode         = TrailMode::Trail;
    j.filters.trail_threshold_us = 100;
    j.filters.erc_enabled        = true;
    j.filters.erc_rate           = 1'000'000;
    const auto res               = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_LT(res.events_out, res.events_in);
    // ERC cap: at most rate * period per 200 us window
    const auto out = test::read_all_events(j.output, false);
    std::map<timestamp, int> per_window;
    for (const auto &e : out)
        ++per_window[e.t / 200];
    for (const auto &[w, n] : per_window)
        ASSERT_LE(n, 200) << "window " << w;
}

TEST_F(OfflineProcessor, Hdf5Export) {
    auto j          = job(ExportFormat::Hdf5, "out.hdf5");
    j.apply_filters = false;
    const auto res  = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto out = test::read_all_events(j.output, false);
    EXPECT_EQ(out.size(), source().size());
}

TEST_F(OfflineProcessor, Hdf5Input) {
    if (!test::has_data_file(kHdf5File))
        GTEST_SKIP();
    auto j          = job(ExportFormat::RawEvt3, "from_hdf5.raw");
    j.input         = test::data_file(kHdf5File);
    j.apply_filters = false;
    const auto res  = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.events_out, test::read_all_events(j.input, true).size());
}

TEST_F(OfflineProcessor, AviExport) {
    auto j            = job(ExportFormat::Avi, "out.avi");
    j.apply_filters   = false;
    j.end_us          = 1'000'000;
    j.video_fps       = 25;
    const auto res    = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_NEAR(static_cast<double>(res.frames), 25.0, 2.0);
    cv::VideoCapture cap(j.output.string());
    ASSERT_TRUE(cap.isOpened());
    cv::Mat frame;
    ASSERT_TRUE(cap.read(frame));
    EXPECT_EQ(frame.cols, 1280);
    EXPECT_EQ(frame.rows, 720);
}

// Synthetic EVT3 recording (the image tests do not need the OpenEB datasets).
static std::filesystem::path write_synthetic_raw(const std::filesystem::path &dir, const std::vector<EventCD> &events,
                                                 int width, int height) {
    const auto path = dir / "synthetic.raw";
    RawFileWriter writer;
    std::string error;
    EXPECT_TRUE(writer.open(path, EvtFormat::Evt3, width, height, {}, &error)) << error;
    writer.write(events.data(), events.data() + events.size());
    writer.close();
    return path;
}

TEST(ImageSequenceExport, PartitionsTheRecording) {
    const int w = 640, h = 480;
    const auto events = test::random_events(40'000, w, h, 7, 0, 60); // ~130 ms (about 9 events per time step)
    const auto dir    = test::temp_dir();
    ExportJob j;
    j.input                   = write_synthetic_raw(dir, events, w, h);
    j.output                  = dir / "images";
    j.format                  = ExportFormat::Images;
    j.apply_filters           = false;
    j.end_us                  = 100'000;
    j.preview.accumulation_us = 20'000;
    const auto res            = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    // [0, 20 ms) ... [80 ms, 100 ms), plus [100 ms, 120 ms) when an event is exactly at the range end
    ASSERT_GE(res.frames, 5u);
    ASSERT_LE(res.frames, 6u);

    const auto source = test::read_all_events(j.input, true);
    ASSERT_EQ(source.size(), events.size());
    std::ifstream index(j.output / "frames.csv");
    ASSERT_TRUE(index.is_open());
    std::string line;
    std::getline(index, line);
    EXPECT_EQ(line, "index,file,t_begin_us,t_end_us");
    const Rgb bg      = palette_background(Palette::Dark);
    uint64_t rows     = 0;
    size_t lit_total  = 0;
    while (std::getline(index, line)) {
        unsigned long long i = 0;
        char name[64]        = {};
        long long t0 = 0, t1 = 0;
        ASSERT_EQ(std::sscanf(line.c_str(), "%llu,%63[^,],%lld,%lld", &i, name, &t0, &t1), 4) << line;
        EXPECT_EQ(i, rows);
        EXPECT_EQ(t0, static_cast<long long>(rows) * 20'000);
        EXPECT_EQ(t1, t0 + 20'000);
        const cv::Mat img = cv::imread((j.output / name).string(), cv::IMREAD_COLOR);
        ASSERT_FALSE(img.empty()) << name;
        ASSERT_EQ(img.cols, w);
        ASSERT_EQ(img.rows, h);
        // CD frame: a pixel is lit exactly when it has an event in [t0, t1) (and within the export range)
        std::vector<uint8_t> active(static_cast<size_t>(w) * h, 0);
        for (const auto &e : source)
            if (e.t >= t0 && e.t < t1 && e.t <= j.end_us)
                active[static_cast<size_t>(e.y) * w + e.x] = 1;
        size_t mismatches = 0;
        for (int y = 0; y < img.rows; ++y)
            for (int x = 0; x < img.cols; ++x) {
                const auto px  = img.at<cv::Vec3b>(y, x);
                const bool lit = !(px[0] == bg.b && px[1] == bg.g && px[2] == bg.r);
                mismatches += lit != (active[static_cast<size_t>(y) * w + x] != 0);
                lit_total += lit;
            }
        EXPECT_EQ(mismatches, 0u) << name;
        ++rows;
    }
    EXPECT_EQ(rows, res.frames);
    EXPECT_GT(lit_total, 0u);
}

TEST(ImageSequenceExport, ReportsUnwritableFolder) {
    const auto events = test::random_events(1'000, 64, 64, 3);
    const auto dir    = test::temp_dir();
    ExportJob j;
    j.input  = write_synthetic_raw(dir, events, 64, 64);
    j.output = dir / "blocker";
    std::ofstream(j.output) << "x"; // a file where the folder should be
    j.format       = ExportFormat::Images;
    const auto res = run_export(j);
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

TEST(ImageExportTag, DescribesTheSettingsCompactly) {
    ExportJob j;
    j.preview.accumulation_us = 20'000;
    EXPECT_EQ(image_export_tag(j, false), "dt-20ms");
    j.filters.erc_enabled = true;
    j.filters.erc_rate    = 50'000'000;
    EXPECT_EQ(image_export_tag(j, true), "dt-20ms_sERC-50M_masked");
    j.filters.mask_enabled = false; // masks not applied
    EXPECT_EQ(image_export_tag(j, true), "dt-20ms_sERC-50M");
    j.apply_filters = false; // unfiltered export: no filter tags at all
    EXPECT_EQ(image_export_tag(j, true), "dt-20ms");

    ExportJob k;
    k.preview.accumulation_us    = 1'500;
    k.preview.mode               = PreviewMode::Histogram;
    k.preview.palette            = Palette::Gray;
    k.start_us                   = 1'500'000;
    k.end_us                     = 3'000'000;
    k.filters.activity_enabled   = true;
    k.filters.activity_window_us = 500;
    k.filters.activity_radius    = 2;
    k.filters.activity_min_neighbors = 3;
    k.filters.activity_look_ahead    = false;
    k.filters.refractory_enabled = true;
    k.filters.refractory_us      = 1'000;
    k.filters.trail_enabled      = true;
    k.filters.trail_mode         = TrailMode::StcKeepTrail;
    k.filters.trail_threshold_us = 2'000;
    k.filters.polarity_enabled   = true;
    k.filters.keep_off           = false;
    k.filters.erc_enabled        = true;
    k.filters.erc_rate           = 2'500'000;
    k.filters.erc_period_us      = 500;
    EXPECT_EQ(image_export_tag(k, false), "dt-1.5ms_histo-sat4_gray_t1.5-3s_sAct-500us-r2-n3-causal_sRefr-1ms_"
                                          "sSTCkeep-2ms_ONonly_sERC-2.5M-w500us");
    k.preview.accumulation_us = 2'000'000;
    k.preview.mode            = PreviewMode::TimeDecay;
    k.preview.palette         = Palette::Dark;
    k.end_us                  = -1;
    k.apply_filters           = false;
    EXPECT_EQ(image_export_tag(k, false), "dt-2s_decay-20ms_t1.5s-end");

    EXPECT_EQ(default_image_folder("D:/data/run 1.raw", "dt-20ms_masked"),
              std::filesystem::path("D:/data/run 1_images_dt-20ms_masked"));
}

TEST_F(OfflineProcessor, CancelStopsEarly) {
    ExportProgress progress;
    progress.cancel = true;
    auto j          = job(ExportFormat::RawEvt3, "cancel.raw");
    const auto res  = run_export(j, &progress);
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.cancelled);
}

TEST_F(OfflineProcessor, ProgressReachesOne) {
    ExportProgress progress;
    auto j         = job(ExportFormat::RawEvt3, "progress.raw");
    const auto res = run_export(j, &progress);
    ASSERT_TRUE(res.ok);
    EXPECT_DOUBLE_EQ(progress.fraction, 1.0);
    EXPECT_EQ(progress.events_in, res.events_in);
}

TEST(OfflineProcessorErrors, ReportsMissingInput) {
    ExportJob j;
    j.input        = "C:/does/not/exist.raw";
    j.output       = test::temp_dir() / "x.raw";
    const auto res = run_export(j);
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.empty());
}

// ------------------------------------------------------------------------------------------------ triggers / masks

TEST_F(OfflineProcessor, ExternalTriggersAreKeptInAllFormats) {
    const auto expected = test::read_all_triggers(input_, true);
    ASSERT_FALSE(expected.empty()) << "test file should contain triggers";
    for (auto format : {ExportFormat::RawEvt3, ExportFormat::RawEvt21, ExportFormat::RawEvt2, ExportFormat::Hdf5}) {
        auto j = job(format, std::string("trig_") + std::to_string(static_cast<int>(format)) +
                                 default_extension(format));
        j.filters.polarity_enabled = true; // filters apply to CD events only
        j.filters.keep_off         = false;
        j.filters.erc_enabled      = true;
        j.filters.erc_rate         = 2'000'000;
        const auto res             = run_export(j);
        ASSERT_TRUE(res.ok) << res.error;
        EXPECT_EQ(res.triggers, expected.size());
        const auto got = test::read_all_triggers(j.output, false);
        ASSERT_EQ(got.size(), expected.size()) << to_string(format);
        for (size_t i = 0; i < got.size(); ++i) {
            ASSERT_EQ(got[i].t, expected[i].t) << to_string(format) << " #" << i;
            ASSERT_EQ(got[i].p, expected[i].p) << to_string(format) << " #" << i;
            ASSERT_EQ(got[i].id, expected[i].id) << to_string(format) << " #" << i;
        }
    }
}

TEST_F(OfflineProcessor, CsvExportWritesTriggerFile) {
    const auto expected = test::read_all_triggers(input_, true);
    auto j              = job(ExportFormat::Csv, "with_triggers.csv");
    j.end_us            = 2'000'000;
    const auto res      = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto in_range =
        std::count_if(expected.begin(), expected.end(), [](const EventExtTrigger &t) { return t.t <= 2'000'000; });
    EXPECT_EQ(static_cast<int64_t>(res.triggers), in_range);
    std::ifstream in(dir_ / "with_triggers_triggers.csv");
    ASSERT_TRUE(in.is_open());
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "p,id,t");
    int64_t lines = 0;
    while (std::getline(in, line))
        ++lines;
    EXPECT_EQ(lines, in_range);
}

TEST_F(OfflineProcessor, RoiAndPixelMasksAreEmulated) {
    MaskSet masks;
    masks.roi() = RoiRect{true, RoiMode::Roi, 400, 200, 300, 250};
    masks.pixels().add(450, 300);
    auto j         = job(ExportFormat::RawEvt3, "roi.raw");
    j.mask         = std::make_shared<std::vector<uint8_t>>(masks.build_bitmap(1280, 720, kMaskPixels | kMaskRoi));
    const auto res = run_export(j);
    ASSERT_TRUE(res.ok) << res.error;
    const auto out = test::read_all_events(j.output, false);
    ASSERT_FALSE(out.empty());
    for (const auto &e : out) {
        ASSERT_TRUE(masks.roi().inside(e.x, e.y)) << e.x << "," << e.y;
        ASSERT_FALSE(e.x == 450 && e.y == 300);
    }
}
