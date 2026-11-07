#pragma once

#include <deque>
#include <mutex>

#include "core/types.h"

namespace evs {

// Sliding-window rate estimator. Time is given by the caller in microseconds (wall clock for live sources,
// event time for recordings). Thread-safe.
class RateMeter {
public:
    explicit RateMeter(timestamp window_us = 1'000'000, int bins = 10);

    void add(double amount, timestamp now_us);
    // Amount per second over the last window ending at now_us (0 when nothing was recorded).
    double rate_per_second(timestamp now_us) const;
    double total() const;
    void reset();

private:
    struct Bin {
        timestamp index;
        double sum;
    };
    void drop_old_locked(timestamp now_us) const;

    timestamp window_us_, bin_us_;
    mutable std::mutex mutex_;
    mutable std::deque<Bin> bins_;
    double total_ = 0;
    timestamp first_us_ = -1;
};

} // namespace evs
