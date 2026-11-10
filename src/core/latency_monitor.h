#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "core/types.h"

namespace evs {

// Estimates how far the camera stream lags behind the PC.
//
// Event cameras only send relative timestamps (EVT 2 / 2.1 / 3: microseconds since the sensor time base started,
// no absolute date), so the delay cannot be read from the data. Instead, every received buffer gives a pair
// (PC clock at reception, last camera timestamp decoded from it) and offset = PC time - camera time. The smallest
// offset seen since the reset is the reference (the fastest a buffer ever arrived); the delay of a buffer is its
// offset minus that reference. It is therefore the delay *in excess of the best case*: buffering in the camera / USB
// driver, a decoder or application falling behind. The constant part (exposure to first USB packet) is invisible
// without an external reference (e.g. a trigger input driven by the PC).
//
// Clock drift between the camera oscillator and the PC (typically 10-50 ppm) is not compensated: over long sessions
// the delay can creep by up to ~0.05 ms per second; reset() takes a new reference.
//
// Thread-safe: add() is called on the camera decoding thread, the rest on the UI thread.
class LatencyMonitor {
public:
    struct Sample {
        timestamp pc_us;        // PC steady clock at reception
        std::int64_t unix_us;   // PC system clock (UTC, us since 1970) at reception
        timestamp camera_us;    // camera time base (same as the event timestamps)
        timestamp delay_us;     // offset - reference
    };
    struct Stats {
        bool valid = false;        // a reference exists and the warm-up is over
        double current_ms = 0;     // max delay over the last 0.5 s (grows while no buffer arrives)
        double recent_max_ms = 0;  // max over the last 10 s
        double max_ms = 0;         // max since reset (after warm-up)
        double reference_age_s = 0; // time since the reference (minimum offset) was last lowered
        double since_reset_s = 0;
        std::uint64_t samples = 0;
    };

    // warmup_us: samples received right after a reset only set the reference (start-up backlog is not a delay).
    explicit LatencyMonitor(timestamp warmup_us = 1'000'000, timestamp history_us = 60'000'000,
                            timestamp bin_us = 100'000);

    void add(timestamp camera_us, timestamp pc_us, std::int64_t unix_us);
    Stats stats(timestamp pc_now_us) const;
    // Max delay (ms) per bin over the history window ending at pc_now_us, oldest first; empty bins are 0.
    std::vector<float> history_ms(timestamp pc_now_us) const;
    timestamp history_window_us() const { return history_us_; }
    void reset();

    // While logging, every sample (warm-up included) is kept until take_log().
    void set_logging(bool on);
    std::vector<Sample> take_log();

private:
    struct Bin {
        timestamp index;
        timestamp max_delay_us;
    };

    const timestamp warmup_us_, history_us_, bin_us_;
    mutable std::mutex mutex_;
    bool has_reference_ = false;
    timestamp min_offset_ = 0, reference_pc_ = 0, first_pc_ = 0;
    timestamp last_camera_ = 0, last_pc_ = 0, last_delay_ = 0, max_delay_ = 0;
    std::uint64_t samples_ = 0;
    std::deque<Sample> recent_; // samples of the last 10 s (after warm-up)
    std::deque<Bin> bins_;
    bool logging_ = false;
    std::vector<Sample> log_;
};

} // namespace evs
