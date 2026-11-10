#include "core/latency_monitor.h"

#include <algorithm>

namespace evs {

namespace {
constexpr timestamp kCurrentWindowUs = 500'000;
constexpr timestamp kRecentWindowUs  = 10'000'000;
constexpr timestamp kBackwardJumpUs  = 1'000; // camera time going back more than this: new time base
} // namespace

LatencyMonitor::LatencyMonitor(timestamp warmup_us, timestamp history_us, timestamp bin_us)
    : warmup_us_(warmup_us), history_us_(history_us), bin_us_(std::max<timestamp>(bin_us, 1)) {}

void LatencyMonitor::add(timestamp camera_us, timestamp pc_us, std::int64_t unix_us) {
    if (camera_us < 0)
        return; // nothing decoded yet
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_reference_ && camera_us + kBackwardJumpUs < last_camera_) {
        // The camera restarted its time base: the old reference is meaningless.
        has_reference_ = false;
        recent_.clear();
        bins_.clear();
        max_delay_ = 0;
        samples_   = 0;
    }
    const timestamp offset = pc_us - camera_us;
    if (!has_reference_) {
        has_reference_ = true;
        min_offset_    = offset;
        reference_pc_  = pc_us;
        first_pc_      = pc_us;
    } else if (offset < min_offset_) {
        min_offset_   = offset;
        reference_pc_ = pc_us;
    }
    const timestamp delay = offset - min_offset_;
    last_camera_          = camera_us;
    last_pc_              = pc_us;
    last_delay_           = delay;
    ++samples_;
    if (logging_)
        log_.push_back({pc_us, unix_us, camera_us, delay});
    if (pc_us - first_pc_ < warmup_us_)
        return;

    max_delay_ = std::max(max_delay_, delay);
    recent_.push_back({pc_us, unix_us, camera_us, delay});
    while (!recent_.empty() && recent_.front().pc_us < pc_us - kRecentWindowUs)
        recent_.pop_front();

    const timestamp index = pc_us / bin_us_;
    if (bins_.empty() || bins_.back().index != index)
        bins_.push_back({index, delay});
    else
        bins_.back().max_delay_us = std::max(bins_.back().max_delay_us, delay);
    const timestamp n = history_us_ / bin_us_;
    while (!bins_.empty() && bins_.front().index <= index - n)
        bins_.pop_front();
}

LatencyMonitor::Stats LatencyMonitor::stats(timestamp pc_now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.samples = samples_;
    if (!has_reference_)
        return s;
    s.since_reset_s   = (pc_now_us - first_pc_) * 1e-6;
    s.reference_age_s = (pc_now_us - reference_pc_) * 1e-6;
    if (last_pc_ - first_pc_ < warmup_us_)
        return s;
    s.valid = true;

    timestamp current = -1, recent = 0;
    for (const auto &smp : recent_) {
        if (smp.pc_us >= pc_now_us - kCurrentWindowUs)
            current = std::max(current, smp.delay_us);
        if (smp.pc_us >= pc_now_us - kRecentWindowUs)
            recent = std::max(recent, smp.delay_us);
    }
    if (current < 0) // no buffer for a while: the newest data we have keeps getting older
        current = last_delay_ + std::max<timestamp>(0, pc_now_us - last_pc_);
    s.current_ms    = current * 1e-3;
    s.recent_max_ms = std::max(recent, current) * 1e-3;
    s.max_ms        = std::max(max_delay_, current) * 1e-3;
    return s;
}

std::vector<float> LatencyMonitor::history_ms(timestamp pc_now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const timestamp n = history_us_ / bin_us_;
    std::vector<float> values(static_cast<size_t>(std::max<timestamp>(n, 0)), 0.f);
    const timestamp now_index = pc_now_us / bin_us_;
    for (const auto &b : bins_) {
        const timestamp k = n - 1 - (now_index - b.index);
        if (k >= 0 && k < n)
            values[static_cast<size_t>(k)] = static_cast<float>(b.max_delay_us * 1e-3);
    }
    return values;
}

void LatencyMonitor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    has_reference_ = false;
    recent_.clear();
    bins_.clear();
    max_delay_  = 0;
    last_delay_ = 0;
    samples_    = 0;
}

void LatencyMonitor::set_logging(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    logging_ = on;
    if (!on)
        log_.clear();
}

std::vector<LatencyMonitor::Sample> LatencyMonitor::take_log() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Sample> out;
    out.swap(log_);
    return out;
}

} // namespace evs
