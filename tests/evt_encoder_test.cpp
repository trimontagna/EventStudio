#include <cstring>
#include <fstream>

#include <gtest/gtest.h>

#include "core/evt_encoder.h"
#include "core/raw_file_writer.h"
#include "io/event_file_source.h"
#include "test_utils.h"

using namespace evs;
using evs::test::ev;

namespace {

const EvtFormat kFormats[] = {EvtFormat::Evt2, EvtFormat::Evt21, EvtFormat::Evt3};

std::filesystem::path write_raw(const std::vector<EventCD> &events, EvtFormat format, int w, int h,
                                const std::string &name, size_t chunk = 0) {
    const auto path = test::temp_dir() / name;
    RawFileWriter writer;
    std::string error;
    EXPECT_TRUE(writer.open(path, format, w, h, {}, &error)) << error;
    if (chunk == 0)
        chunk = events.size();
    for (size_t i = 0; i < events.size(); i += chunk) {
        const size_t n = std::min(chunk, events.size() - i);
        writer.write(events.data() + i, events.data() + i + n);
    }
    writer.close();
    EXPECT_EQ(writer.events_written(), events.size());
    return path;
}

std::string read_header(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::string header, line;
    while (std::getline(in, line) && !line.empty() && line[0] == '%') {
        header += line + "\n";
        if (line == "% end")
            break;
    }
    return header;
}

// Decodes with the robust decoder and fails on any protocol violation.
std::vector<EventCD> decode(const std::filesystem::path &path) {
    int violations    = 0;
    auto events       = test::read_raw_strict(path, violations);
    EXPECT_EQ(violations, 0) << "decoder protocol violations in " << path;
    return events;
}

class EncoderRoundTrip : public ::testing::TestWithParam<EvtFormat> {};

} // namespace

TEST(EvtFormat, Descriptors) {
    EXPECT_EQ(format_descriptor(EvtFormat::Evt3, 1280, 720), "EVT3;height=720;width=1280");
    EXPECT_EQ(format_descriptor(EvtFormat::Evt21, 1280, 720), "EVT21;height=720;width=1280;endianness=legacy");
    EXPECT_EQ(format_descriptor(EvtFormat::Evt2, 640, 480), "EVT2;height=480;width=640");
    EXPECT_EQ(evt_version(EvtFormat::Evt3), "3.0");
    EXPECT_EQ(evt_version(EvtFormat::Evt21), "2.1");
}

TEST(RawFileWriter, HeaderOverridesFormatFieldsAndKeepsMetadata) {
    const auto header = RawFileWriter::build_header(
        EvtFormat::Evt3, 1280, 720,
        {{"serial_number", "00052238"}, {"format", "EVT21;height=720;width=1280"}, {"evt", "2.1"}, {"end", ""}});
    EXPECT_NE(header.find("% format EVT3;height=720;width=1280\n"), std::string::npos);
    EXPECT_NE(header.find("% evt 3.0\n"), std::string::npos);
    EXPECT_NE(header.find("% serial_number 00052238\n"), std::string::npos);
    EXPECT_NE(header.find("% geometry 1280x720\n"), std::string::npos);
    EXPECT_EQ(header.find("2.1"), std::string::npos);
    EXPECT_EQ(header.rfind("% end\n"), header.size() - 6);
}

TEST_P(EncoderRoundTrip, RandomEventsDecodeIdentically) {
    const auto events = test::random_events(200'000, 1280, 720, 7, 1000, 40);
    const auto path   = write_raw(events, GetParam(), 1280, 720, "random.raw", 4096);
    EXPECT_NE(read_header(path).find(std::string("% format ") + format_descriptor(GetParam(), 1280, 720)),
              std::string::npos);
    const auto decoded = decode(path);
    std::string diff;
    EXPECT_TRUE(test::same_events(events, decoded, &diff)) << diff;
}

TEST_P(EncoderRoundTrip, ChunkingDoesNotChangeTheResult) {
    const auto events = test::random_events(20'000, 640, 480, 11, 0, 5);
    const auto a      = decode(write_raw(events, GetParam(), 640, 480, "a.raw", 1));
    const auto b      = decode(write_raw(events, GetParam(), 640, 480, "b.raw", 0));
    std::string diff;
    EXPECT_TRUE(test::same_events(a, b, &diff)) << diff;
    EXPECT_TRUE(test::same_events(events, a, &diff)) << diff;
}

TEST_P(EncoderRoundTrip, FullRowsAndImageBorders) {
    // Complete rows (vector material) up to the last column, both polarities, first and last row.
    std::vector<EventCD> events;
    for (int y : {0, 1, 719})
        for (int x = 0; x < 1280; ++x)
            events.push_back(ev(x, y, (x / 7) % 2, 5000));
    for (int x = 1240; x < 1280; x += 3)
        events.push_back(ev(x, 300, 1, 6000));
    const auto decoded = decode(write_raw(events, GetParam(), 1280, 720, "rows.raw"));
    std::string diff;
    EXPECT_TRUE(test::same_events(events, decoded, &diff)) << diff;
}

TEST_P(EncoderRoundTrip, LongGapsKeepAbsoluteTime) {
    // Gaps longer than the EVT 3.0 time range (2^24 us ~ 16.8 s) must not lose time loops.
    std::vector<EventCD> events = {ev(1, 1, 1, 100),         ev(2, 2, 0, 20'000'000),  ev(3, 3, 1, 20'000'001),
                                   ev(4, 4, 1, 70'123'456), ev(5, 5, 0, 200'000'000), ev(6, 6, 1, 200'004'096)};
    const auto decoded = decode(write_raw(events, GetParam(), 64, 64, "gaps.raw"));
    std::string diff;
    EXPECT_TRUE(test::same_events(events, decoded, &diff)) << diff;
}

TEST(Evt2xEncoder, TimeLoopAt2Pow34) {
    // EVT 2.x time high wraps every 2^34 us (~4.8 h); the decoders must see the loop.
    constexpr timestamp loop = timestamp(1) << 34;
    const std::vector<EventCD> events = {ev(1, 1, 1, loop - 1000), ev(2, 1, 0, loop + 500), ev(3, 1, 1, 2 * loop + 64),
                                         ev(4, 1, 1, 2 * loop + 100'000)};
    const auto decoded = decode(write_raw(events, EvtFormat::Evt21, 64, 64, "evt21.raw"));
    std::string diff;
    EXPECT_TRUE(test::same_events(events, decoded, &diff)) << diff;

    // The OpenEB EVT 2.0 decoder only checks for a loop on the last TIME_HIGH of each decoded chunk, which works on
    // dense sensor data only: check the encoded words instead (TH(max) immediately followed by TH(0) at each loop).
    auto enc = make_encoder(EvtFormat::Evt2, 64, 64);
    std::vector<uint8_t> out;
    enc->encode(events.data(), events.data() + events.size(), out);
    enc->flush(out);
    std::vector<uint32_t> th;
    for (size_t i = 0; i + 4 <= out.size(); i += 4) {
        uint32_t w;
        std::memcpy(&w, out.data() + i, 4);
        if ((w >> 28) == 0x8)
            th.push_back(w & 0x0FFFFFFF);
    }
    const uint32_t max = 0x0FFFFFFF;
    const std::vector<uint32_t> expected = {
        static_cast<uint32_t>(((loop - 1000) >> 6) & max), max, 0, static_cast<uint32_t>(500 >> 6), max, 0, 1,
        static_cast<uint32_t>(100'000 >> 6)};
    EXPECT_EQ(th, expected);
}

TEST_P(EncoderRoundTrip, OutOfOrderEventsAreClampedToLatestTime) {
    const std::vector<EventCD> events = {ev(1, 1, 1, 1000), ev(2, 1, 1, 900), ev(3, 1, 1, 1100)};
    const auto decoded                = decode(write_raw(events, GetParam(), 64, 64, "ooo.raw"));
    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_EQ(decoded[1].t, 1000);
}

TEST_P(EncoderRoundTrip, ReencodesRealRecording) {
    if (!test::has_data_file("gen4_evt3_hand.raw"))
        GTEST_SKIP() << "OpenEB test data not found";
    auto source = test::read_all_events(test::data_file("gen4_evt3_hand.raw"), true);
    source.resize(std::min<size_t>(source.size(), 3'000'000));
    ASSERT_FALSE(source.empty());
    const auto decoded = decode(write_raw(source, GetParam(), 1280, 720, "hand.raw", 1 << 16));
    std::string diff;
    EXPECT_TRUE(test::same_events(source, decoded, &diff)) << diff;
}

INSTANTIATE_TEST_SUITE_P(AllFormats, EncoderRoundTrip, ::testing::ValuesIn(kFormats),
                         [](const auto &info) { return std::string(to_string(info.param)); });

TEST(Evt3Encoder, VectorizesDenseRows) {
    std::vector<EventCD> events;
    for (int x = 0; x < 1280; ++x)
        events.push_back(ev(x, 10, 1, 42));
    auto enc = make_encoder(EvtFormat::Evt3, 1280, 720);
    std::vector<uint8_t> out;
    enc->encode(events.data(), events.data() + events.size(), out);
    enc->flush(out);
    // 40 blocks of 32 pixels: VECT_BASE_X + 40 * 3 words, plus TIME_HIGH, TIME_LOW, ADDR_Y.
    EXPECT_EQ(out.size(), (1 + 40 * 3 + 3) * 2u);
}

TEST(Evt21Encoder, PacksAlignedBlocksOf32Pixels) {
    std::vector<EventCD> events;
    for (int x = 0; x < 64; ++x)
        events.push_back(ev(x, 3, 0, 7));
    auto enc = make_encoder(EvtFormat::Evt21, 1280, 720);
    std::vector<uint8_t> out;
    enc->encode(events.data(), events.data() + events.size(), out);
    enc->flush(out);
    ASSERT_EQ(out.size(), 3 * 8u); // TIME_HIGH + 2 vectors
    uint64_t w;
    std::memcpy(&w, out.data() + 8, 8);
    EXPECT_EQ(w >> 32, 0xFFFFFFFFull);                // all 32 pixels valid
    EXPECT_EQ((w >> 28) & 0xF, 0x0u);                 // OFF
    EXPECT_EQ((w >> 11) & 0x7FF, 0u);                 // x base
    EXPECT_EQ(w & 0x7FF, 3u);                         // y
}

// ------------------------------------------------------------------------------------------------ triggers

TEST_P(EncoderRoundTrip, ExternalTriggersRoundTrip) {
    const auto events = test::random_events(50'000, 640, 480, 21, 0, 30);
    std::vector<EventExtTrigger> triggers;
    for (timestamp t = 17; t < events.back().t; t += 997)
        triggers.emplace_back(static_cast<short>((t / 997) % 2), t, static_cast<short>(t % 2 == 0 ? 0 : 6));
    const auto path = test::temp_dir() / "trig.raw";
    RawFileWriter writer;
    std::string error;
    ASSERT_TRUE(writer.open(path, GetParam(), 640, 480, {}, &error)) << error;
    // Triggers delivered after the CD events of their buffer, as decoders do.
    size_t ti = 0;
    for (size_t i = 0; i < events.size(); i += 4096) {
        const size_t n       = std::min<size_t>(4096, events.size() - i);
        writer.write(events.data() + i, events.data() + i + n);
        const timestamp last = events[i + n - 1].t;
        const size_t tstart  = ti;
        while (ti < triggers.size() && triggers[ti].t <= last)
            ++ti;
        writer.write_triggers(triggers.data() + tstart, triggers.data() + ti);
        writer.advance(last - 1);
    }
    writer.write_triggers(triggers.data() + ti, triggers.data() + triggers.size());
    writer.close();
    EXPECT_EQ(writer.triggers_written(), triggers.size());

    const auto decoded = test::read_all_triggers(path);
    ASSERT_EQ(decoded.size(), triggers.size());
    for (size_t i = 0; i < triggers.size(); ++i) {
        ASSERT_EQ(decoded[i].t, triggers[i].t) << i;
        ASSERT_EQ(decoded[i].p, triggers[i].p) << i;
        ASSERT_EQ(decoded[i].id, triggers[i].id) << i;
    }
    std::string diff;
    EXPECT_TRUE(test::same_events(events, decode(path), &diff)) << diff;
}

TEST(RawFileWriter, AdvanceOnlyEncodesUpToSafeTime) {
    const auto path = test::temp_dir() / "advance.raw";
    RawFileWriter writer;
    ASSERT_TRUE(writer.open(path, EvtFormat::Evt3, 64, 64));
    const std::vector<EventCD> cd = {ev(1, 1, 1, 100), ev(2, 1, 1, 200), ev(3, 1, 1, 300)};
    writer.write(cd.data(), cd.data() + cd.size());
    const auto header_bytes = writer.bytes_written();
    writer.advance(50);
    EXPECT_EQ(writer.bytes_written(), header_bytes);
    writer.advance(250);
    EXPECT_GT(writer.bytes_written(), header_bytes);
    // A trigger older than the safe time breaks the caller contract: it is stamped with the latest encoded time.
    const EventExtTrigger late(1, 150, 0);
    writer.write_triggers(&late, &late + 1);
    writer.close();
    const auto tr = test::read_all_triggers(path);
    ASSERT_EQ(tr.size(), 1u);
    EXPECT_EQ(tr[0].t, 200);
}
