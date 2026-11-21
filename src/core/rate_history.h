#pragma once

#include <deque>
#include <mutex>
#include <vector>

#include "core/types.h"

namespace evs {

// Amount (e.g. events) per time bin over a history window, for a rate graph and a rolling mean. Time is given by
// the caller in microseconds (PC clock for a live camera, recording time for a file). Thread-safe: add() is called
// on the source thread, the rest on the UI thread.
class RateHistory {
public:
    explicit RateHistory(timestamp history_us = 60'000'000, timestamp bin_us = 100'000);

    void add(double amount, timestamp now_us);
    // Amount per second of every complete bin of the history window ending at now_us, oldest first (the bin in
    // progress is left out, it would read low). Empty bins and bins before the first sample are 0.
    std::vector<float> per_second(timestamp now_us) const;
    // Mean amount per second over the last window_us ending at now_us, averaged over the time since the first
    // sample if that is shorter; 0 when nothing was added.
    double mean_per_second(timestamp now_us, timestamp window_us) const;
    bool empty() const;
    timestamp history_window_us() const { return history_us_; }
    timestamp bin_us() const { return bin_us_; }
    void reset();

private:
    struct Bin {
        timestamp index;
        double sum;
    };

    const timestamp history_us_, bin_us_;
    mutable std::mutex mutex_;
    std::deque<Bin> bins_;
    timestamp first_us_ = -1, last_us_ = -1;
};

} // namespace evs
