#include "test_utils.h"

#include <algorithm>
#include <sstream>

#include <gtest/gtest.h>
#include <metavision/hal/device/device.h>
#include <metavision/hal/device/device_discovery.h>
#include <metavision/hal/facilities/i_events_stream_decoder.h>
#include <metavision/hal/utils/raw_file_config.h>
#include <metavision/sdk/stream/event_file_reader.h>
#include <metavision/sdk/stream/raw_event_file_reader.h>

#include "io/event_file_source.h"

namespace evs::test {

std::filesystem::path data_file(const std::string &name) {
    return std::filesystem::path(EVS_TEST_DATA_DIR) / name;
}

bool has_data_file(const std::string &name) {
    std::error_code ec;
    return std::filesystem::exists(data_file(name), ec);
}

std::filesystem::path temp_dir() {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name() : "evs_test";
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    auto dir = std::filesystem::temp_directory_path() / "EventStudioTests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<EventCD> read_all_events(const std::filesystem::path &path, bool time_shift) {
    std::string error;
    auto src = EventFileSource::open(path, time_shift, &error);
    EXPECT_TRUE(src) << error;
    std::vector<EventCD> events;
    if (!src)
        return events;
    src->reader().add_read_callback(
        [&](const EventCD *b, const EventCD *e) { events.insert(events.end(), b, e); });
    while (src->reader().read()) {}
    return events;
}

std::vector<EventExtTrigger> read_all_triggers(const std::filesystem::path &path, bool time_shift) {
    std::string error;
    auto src = EventFileSource::open(path, time_shift, &error);
    EXPECT_TRUE(src) << error;
    std::vector<EventExtTrigger> triggers;
    if (!src)
        return triggers;
    src->reader().add_read_callback(
        [&](const EventExtTrigger *b, const EventExtTrigger *e) { triggers.insert(triggers.end(), b, e); });
    // CD callback needed for the RAW reader to decode
    src->reader().add_read_callback([](const EventCD *, const EventCD *) {});
    while (src->reader().read()) {}
    return triggers;
}

std::vector<EventCD> read_raw_strict(const std::filesystem::path &path, int &violations) {
    violations = 0;
    _putenv_s("MV_FLAGS_EVT3_ROBUST_DECODER", "1");
    Metavision::RawFileConfig config;
    config.do_time_shifting_ = false;
    auto device              = Metavision::DeviceDiscovery::open_raw_file(path, config);
    _putenv_s("MV_FLAGS_EVT3_ROBUST_DECODER", "");
    std::vector<EventCD> events;
    EXPECT_TRUE(device);
    if (!device)
        return events;
    if (auto *decoder = device->get_facility<Metavision::I_EventsStreamDecoder>()) {
        try {
            decoder->add_protocol_violation_callback([&](Metavision::DecoderProtocolViolation v) {
                if (++violations <= 5)
                    ADD_FAILURE() << "protocol violation: " << v;
            });
        } catch (const std::exception &) {} // EVT 2.x decoders do not report violations
    }
    {
        Metavision::RAWEventFileReader reader(*device, path);
        reader.add_read_callback([&](const EventCD *b, const EventCD *e) { events.insert(events.end(), b, e); });
        while (reader.read()) {}
    }
    return events;
}

void canonical_sort(std::vector<EventCD> &events) {
    std::sort(events.begin(), events.end(), [](const EventCD &a, const EventCD &b) {
        if (a.t != b.t)
            return a.t < b.t;
        if (a.y != b.y)
            return a.y < b.y;
        if (a.x != b.x)
            return a.x < b.x;
        return a.p < b.p;
    });
}

bool same_events(std::vector<EventCD> a, std::vector<EventCD> b, std::string *diff) {
    canonical_sort(a);
    canonical_sort(b);
    if (a.size() != b.size()) {
        if (diff)
            *diff = "size " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        // still report the first difference below
    }
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].p != b[i].p || a[i].t != b[i].t) {
            if (diff) {
                std::ostringstream ss;
                ss << (diff->empty() ? "" : *diff + ", ") << "first difference at #" << i << ": (" << a[i].x << ","
                   << a[i].y << "," << a[i].p << "," << a[i].t << ") vs (" << b[i].x << "," << b[i].y << ","
                   << b[i].p << "," << b[i].t << ")";
                *diff = ss.str();
            }
            return false;
        }
    }
    return a.size() == b.size();
}

std::vector<EventCD> random_events(size_t count, int width, int height, uint32_t seed, timestamp t0,
                                   timestamp max_dt) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> rx(0, width - 1), ry(0, height - 1), rp(0, 1), mode(0, 9);
    std::uniform_int_distribution<timestamp> rdt(0, max_dt);
    std::vector<EventCD> events;
    events.reserve(count);
    timestamp t = t0;
    while (events.size() < count) {
        t += rdt(rng);
        const int m = mode(rng);
        if (m < 3) {
            // dense run on a row: same timestamp, consecutive-ish x (vector material)
            const int y = ry(rng), p = rp(rng);
            int x       = rx(rng);
            const int n = 1 + mode(rng) * 6;
            for (int k = 0; k < n && x < width && events.size() < count; ++k, x += 1 + (mode(rng) == 0))
                events.push_back(ev(x, y, p, t));
        } else if (m == 3 && !events.empty()) {
            // exact duplicate of the previous event
            EventCD d = events.back();
            d.t       = t;
            events.push_back(d);
            events.push_back(d);
        } else {
            events.push_back(ev(rx(rng), ry(rng), rp(rng), t));
        }
    }
    return events;
}

} // namespace evs::test
