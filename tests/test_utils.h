#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/types.h"

namespace evs::test {

// Directory of the OpenEB test datasets (C:/Dev/OpenEB_tests/openeb), configured at build time.
std::filesystem::path data_file(const std::string &name);
bool has_data_file(const std::string &name);

// Fresh, empty temporary directory for the current test.
std::filesystem::path temp_dir();

// Decodes every CD event of an event file (RAW / HDF5 / DAT) with the OpenEB readers.
std::vector<EventCD> read_all_events(const std::filesystem::path &path, bool time_shift = false);

// Decodes every external trigger event of an event file.
std::vector<EventExtTrigger> read_all_triggers(const std::filesystem::path &path, bool time_shift = false);

// Decodes a RAW file with the OpenEB robust (grammar-checking) EVT3 decoder, counting every protocol violation
// reported by the decoder (any format).
std::vector<EventCD> read_raw_strict(const std::filesystem::path &path, int &violations);

// Sorts by (t, y, x, p): canonical order to compare streams whose events may be reordered within a timestamp.
void canonical_sort(std::vector<EventCD> &events);
bool same_events(std::vector<EventCD> a, std::vector<EventCD> b, std::string *diff = nullptr);

// Random time-ordered events with bursts sharing timestamps and dense rows (to exercise vectorized encodings).
std::vector<EventCD> random_events(size_t count, int width, int height, uint32_t seed, timestamp t0 = 0,
                                   timestamp max_dt = 50);

inline EventCD ev(int x, int y, int p, timestamp t) {
    return EventCD(static_cast<unsigned short>(x), static_cast<unsigned short>(y), static_cast<short>(p), t);
}

} // namespace evs::test
