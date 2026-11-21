#include "core/rate_history.h"

#include <algorithm>

namespace evs {

RateHistory::RateHistory(timestamp history_us, timestamp bin_us) :
    history_us_(std::max<timestamp>(history_us, 1)), bin_us_(std::max<timestamp>(bin_us, 1)) {}

void RateHistory::add(double amount, timestamp now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_us_ >= 0 && now_us + bin_us_ < last_us_) {
        // Time went back by more than a bin (new time base, loop of a recording): start again.
        bins_.clear();
        first_us_ = -1;
        last_us_  = -1;
    }
    if (first_us_ < 0)
        first_us_ = now_us;
    last_us_ = std::max(last_us_, now_us);

    const timestamp index = now_us / bin_us_;
    if (!bins_.empty() && index <= bins_.back().index)
        bins_.back().sum += amount; // same bin, or slightly back in time: count it in the latest bin
    else
        bins_.push_back({index, amount});
    const timestamp n = history_us_ / bin_us_ + 1;
    while (!bins_.empty() && bins_.front().index <= bins_.back().index - n)
        bins_.pop_front();
}

std::vector<float> RateHistory::per_second(timestamp now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const timestamp n = std::max<timestamp>(history_us_ / bin_us_, 1);
    std::vector<float> values(static_cast<size_t>(n), 0.f);
    const timestamp last_complete = now_us / bin_us_ - 1;
    for (const auto &b : bins_) {
        const timestamp k = n - 1 - (last_complete - b.index);
        if (k < 0 || k >= n)
            continue;
        // The bin of the first sample only covers the time since that sample (at least a quarter bin, so that a
        // sample right at the end of a bin does not make a spike).
        const timestamp start = std::max(b.index * bin_us_, first_us_);
        const timestamp dur   = std::max((b.index + 1) * bin_us_ - start, bin_us_ / 4);
        values[static_cast<size_t>(k)] = static_cast<float>(b.sum * 1e6 / static_cast<double>(std::max<timestamp>(dur, 1)));
    }
    return values;
}

double RateHistory::mean_per_second(timestamp now_us, timestamp window_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bins_.empty() || first_us_ < 0)
        return 0;
    window_us                     = std::clamp<timestamp>(window_us, bin_us_, history_us_);
    const timestamp now_index     = now_us / bin_us_;
    const timestamp bins_in_window = window_us / bin_us_;
    double sum                    = 0;
    for (const auto &b : bins_)
        if (b.index > now_index - bins_in_window && b.index <= now_index)
            sum += b.sum;
    // Covered span: the complete bins of the window plus the elapsed part of the current one, but never more than
    // the time since the first sample (a history started 2 s ago is averaged over 2 s, not 10 s).
    timestamp span = (bins_in_window - 1) * bin_us_ + (now_us - now_index * bin_us_ + 1);
    span           = std::min(span, now_us - first_us_ + 1);
    span           = std::max<timestamp>(span, 1);
    return sum * 1e6 / static_cast<double>(span);
}

bool RateHistory::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bins_.empty();
}

void RateHistory::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    bins_.clear();
    first_us_ = -1;
    last_us_  = -1;
}

} // namespace evs
