#include "core/rate_meter.h"

#include <algorithm>

namespace evs {

RateMeter::RateMeter(timestamp window_us, int bins) :
    window_us_(std::max<timestamp>(window_us, 1)), bin_us_(std::max<timestamp>(window_us / std::max(bins, 1), 1)) {}

void RateMeter::add(double amount, timestamp now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (first_us_ < 0 || now_us < first_us_)
        first_us_ = now_us;
    const timestamp idx = now_us / bin_us_;
    if (!bins_.empty() && bins_.back().index == idx)
        bins_.back().sum += amount;
    else if (!bins_.empty() && idx < bins_.back().index)
        bins_.back().sum += amount; // time went backward (e.g. seek without reset): count it in the latest bin
    else
        bins_.push_back({idx, amount});
    total_ += amount;
    drop_old_locked(now_us);
}

void RateMeter::drop_old_locked(timestamp now_us) const {
    const timestamp oldest = (now_us - window_us_) / bin_us_;
    while (!bins_.empty() && bins_.front().index <= oldest)
        bins_.pop_front();
}

double RateMeter::rate_per_second(timestamp now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    drop_old_locked(now_us);
    if (bins_.empty())
        return 0;
    double sum = 0;
    for (const auto &b : bins_)
        sum += b.sum;
    // Covered span: the complete bins of the window plus the elapsed part of the current bin, but never more than
    // the time elapsed since the first sample (so that a meter started 100 ms ago is not averaged over 1 s).
    const timestamp current_start = (now_us / bin_us_) * bin_us_;
    timestamp span                = (window_us_ - bin_us_) + (now_us - current_start + 1);
    span                          = std::min(span, now_us - first_us_ + 1);
    span                          = std::max<timestamp>(span, 1);
    return sum * 1e6 / static_cast<double>(span);
}

double RateMeter::total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
}

void RateMeter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    bins_.clear();
    total_    = 0;
    first_us_ = -1;
}

} // namespace evs
