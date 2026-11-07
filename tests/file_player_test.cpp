#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "io/file_player.h"
#include "io/sample_launcher.h"
#include "test_utils.h"

using namespace evs;
using namespace std::chrono_literals;

namespace {

const char *kFile = "blinking_gen4_with_ext_triggers.raw";

template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

class FilePlayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!test::has_data_file(kFile))
            GTEST_SKIP() << "OpenEB test data not found";
        player_.set_events_callback([this](const EventCD *b, const EventCD *e) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (b != e) {
                if (!events_.empty() && b->t < events_.back().t)
                    ++order_violations_;
                events_.insert(events_.end(), b, e);
            }
        });
        player_.set_seek_callback([this](timestamp t) {
            std::lock_guard<std::mutex> lock(mutex_);
            seeks_.push_back(t);
            events_.clear();
        });
        player_.set_raw_bytes_callback([this](size_t n) { bytes_ += n; });
        std::string error;
        ASSERT_TRUE(player_.open(test::data_file(kFile), &error)) << error;
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }
    FilePlayer player_;
    std::mutex mutex_;
    std::vector<EventCD> events_;
    std::vector<timestamp> seeks_;
    int order_violations_ = 0;
    std::atomic<size_t> bytes_{0};
};

} // namespace

TEST_F(FilePlayerTest, ReportsGeometryAndEncoding) {
    EXPECT_EQ(player_.width(), 1280);
    EXPECT_EQ(player_.height(), 720);
    EXPECT_EQ(player_.encoding(), "EVT3");
    EXPECT_FALSE(player_.is_playing());
}

TEST_F(FilePlayerTest, PlaysWholeFileAtMaxSpeed) {
    const auto expected = test::read_all_events(test::data_file(kFile), true).size();
    player_.set_speed(0);
    player_.play();
    ASSERT_TRUE(wait_for([&] { return player_.at_end(); }, 60s));
    EXPECT_EQ(count(), expected);
    EXPECT_EQ(order_violations_, 0);
    EXPECT_FALSE(player_.is_playing());
    EXPECT_GT(bytes_.load(), 0u);
}

TEST_F(FilePlayerTest, PauseStopsDelivery) {
    player_.set_speed(1.0);
    player_.play();
    ASSERT_TRUE(wait_for([&] { return count() > 0; }, 5s));
    player_.pause();
    std::this_thread::sleep_for(100ms);
    const size_t paused_count = count();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(count(), paused_count);
    player_.play();
    EXPECT_TRUE(wait_for([&] { return count() > paused_count; }, 5s));
}

TEST_F(FilePlayerTest, RealTimePacing) {
    player_.set_speed(1.0);
    player_.play();
    ASSERT_TRUE(wait_for([&] { return count() > 0; }, 5s));
    const timestamp t0 = player_.current_time();
    std::this_thread::sleep_for(500ms);
    const timestamp played = player_.current_time() - t0;
    player_.pause();
    // ~500 ms of recording in 500 ms of wall time (generous bounds for a loaded machine)
    EXPECT_GT(played, 300'000);
    EXPECT_LT(played, 700'000);
}

TEST_F(FilePlayerTest, DoubleSpeed) {
    player_.set_speed(2.0);
    player_.play();
    ASSERT_TRUE(wait_for([&] { return count() > 0; }, 5s));
    const timestamp t0 = player_.current_time();
    std::this_thread::sleep_for(500ms);
    const timestamp played = player_.current_time() - t0;
    player_.pause();
    EXPECT_GT(played, 700'000);
    EXPECT_LT(played, 1'300'000);
}

TEST_F(FilePlayerTest, SeekWhilePausedShowsTarget) {
    timestamp min_t = 0, max_t = 0;
    ASSERT_TRUE(wait_for([&] { return player_.seek_range(min_t, max_t); }, 30s)) << "RAW index not built";
    ASSERT_GT(max_t, 1'000'000);
    const timestamp target = (min_t + max_t) / 2;
    player_.request_seek(target, 10'000);
    ASSERT_TRUE(wait_for([&] { return player_.current_time() >= target - 1000; }, 10s));
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(player_.is_playing());
    std::lock_guard<std::mutex> lock(mutex_);
    ASSERT_EQ(seeks_.size(), 1u);
    ASSERT_FALSE(events_.empty());
    // preroll delivered (paused): events around [target - 10 ms, target], nothing far beyond the target
    EXPECT_LE(events_.front().t, target);
    EXPECT_GE(events_.front().t, target - 200'000);
    EXPECT_LE(events_.back().t, target + 2000);
}

TEST_F(FilePlayerTest, QueriesFromAnotherThreadNeverBlockPlayback) {
    // Regression: the UI polls seek_range() / status() every frame while the worker is paced inside a read.
    // This used to deadlock (player mutex vs reader lock).
    player_.set_speed(1.0);
    player_.play();
    std::atomic<bool> done{false};
    std::atomic<double> worst_ms{0};
    std::thread poller([&] {
        const auto end = std::chrono::steady_clock::now() + 1500ms;
        while (std::chrono::steady_clock::now() < end) {
            const auto t0   = std::chrono::steady_clock::now();
            timestamp a = 0, b = 0;
            player_.seek_range(a, b);
            (void)player_.status();
            (void)player_.width();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst_ms        = std::max(worst_ms.load(), ms);
            std::this_thread::sleep_for(1ms);
        }
        done = true;
    });
    if (!wait_for([&] { return done.load(); }, 10s)) {
        std::fprintf(stderr, "DEADLOCK: player queries blocked during playback\n");
        std::_Exit(1); // cannot recover a deadlocked thread: fail the whole run loudly
    }
    poller.join();
    EXPECT_LT(worst_ms.load(), 50.0);
    EXPECT_GT(player_.current_time(), 500'000); // playback kept going
    player_.pause();
}

TEST_F(FilePlayerTest, LoopRewinds) {
    player_.set_loop(true);
    player_.set_speed(0);
    player_.play();
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            return !seeks_.empty();
        },
        60s));
    EXPECT_TRUE(player_.is_playing());
    player_.pause();
}

TEST_F(FilePlayerTest, PlayAtEndRestarts) {
    player_.set_speed(0);
    player_.play();
    ASSERT_TRUE(wait_for([&] { return player_.at_end(); }, 60s));
    player_.play();
    ASSERT_TRUE(wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            return !seeks_.empty() && !events_.empty();
        },
        30s));
    player_.pause();
}

TEST(FilePlayerErrors, OpenMissingFileFails) {
    FilePlayer p;
    std::string error;
    EXPECT_FALSE(p.open("C:/nope/missing.raw", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(p.is_open());
}

TEST(FilePlayerErrors, OpenUnsupportedExtensionFails) {
    const auto path = test::temp_dir() / "file.txt";
    { std::ofstream(path) << "hello"; }
    FilePlayer p;
    std::string error;
    EXPECT_FALSE(p.open(path, &error));
    EXPECT_NE(error.find("Unsupported"), std::string::npos);
}

// ------------------------------------------------------------------------------------------------ sample launcher

TEST(SampleLauncher, CommandLines) {
    const auto &samples = known_samples();
    auto find           = [&](const std::string &exe) {
        return *std::find_if(samples.begin(), samples.end(), [&](const SampleInfo &s) { return s.executable == exe; });
    };
    const std::filesystem::path dir = "C:/bin";
    EXPECT_EQ(build_sample_command_line(find("metavision_viewer.exe"), dir, "D:/rec.raw"),
              L"\"C:/bin\\metavision_viewer.exe\" -i \"D:/rec.raw\"");
    EXPECT_EQ(build_sample_command_line(find("metavision_viewer.exe"), dir, ""), L"\"C:/bin\\metavision_viewer.exe\"");
    EXPECT_EQ(build_sample_command_line(find("metavision_sdk_get_started.exe"), dir, "D:/a.raw"),
              L"\"C:/bin\\metavision_sdk_get_started.exe\" \"D:/a.raw\"");
    EXPECT_EQ(build_sample_command_line(find("metavision_file_info.exe"), dir, "D:/a.raw"),
              L"cmd.exe /k \"\"C:/bin\\metavision_file_info.exe\" -i \"D:/a.raw\"\"");
}

TEST(SampleLauncher, RefusesFileOnlySampleWithoutFile) {
    const auto &samples = known_samples();
    auto it = std::find_if(samples.begin(), samples.end(), [](const SampleInfo &s) { return !s.supports_camera; });
    ASSERT_NE(it, samples.end());
    std::string error;
    EXPECT_FALSE(launch_sample(*it, "C:/bin", "", &error));
    EXPECT_FALSE(error.empty());
}

TEST(SampleLauncher, FindsOpenEBSamples) {
    const auto dir = find_samples_dir();
    if (dir.empty())
        GTEST_SKIP() << "OpenEB samples not installed";
    EXPECT_TRUE(std::filesystem::exists(dir / "metavision_viewer.exe"));
}

TEST_F(FilePlayerTest, DeliversExternalTriggers) {
    std::atomic<size_t> triggers{0};
    FilePlayer p;
    p.set_triggers_callback([&](const EventExtTrigger *b, const EventExtTrigger *e) { triggers += e - b; });
    std::string error;
    ASSERT_TRUE(p.open(test::data_file(kFile), &error)) << error;
    p.set_speed(0);
    p.play();
    ASSERT_TRUE(wait_for([&] { return p.at_end(); }, 60s));
    EXPECT_GT(triggers.load(), 0u);
    EXPECT_EQ(triggers.load(), test::read_all_triggers(test::data_file(kFile), true).size());
}
