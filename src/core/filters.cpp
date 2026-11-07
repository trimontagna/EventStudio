#include "core/filters.h"

#include <algorithm>
#include <limits>

namespace evs {

// ------------------------------------------------------------------------------------------------ PolarityFilter

void PolarityFilter::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) const {
    if (keep_on_ && keep_off_) {
        out.insert(out.end(), begin, end);
        return;
    }
    if (!keep_on_ && !keep_off_)
        return;
    const short wanted = keep_on_ ? 1 : 0;
    for (auto it = begin; it != end; ++it)
        if (it->p == wanted)
            out.push_back(*it);
}

// ------------------------------------------------------------------------------------------------ PixelMaskFilter

PixelMaskFilter::PixelMaskFilter(int width, int height) : width_(width), height_(height) {}

void PixelMaskFilter::set_mask(std::shared_ptr<const std::vector<uint8_t>> mask) {
    if (mask && !mask->empty() && mask->size() != static_cast<size_t>(width_) * height_)
        mask.reset();
    mask_ = std::move(mask);
}

void PixelMaskFilter::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) const {
    if (!mask_ || mask_->empty()) {
        out.insert(out.end(), begin, end);
        return;
    }
    const uint8_t *m = mask_->data();
    for (auto it = begin; it != end; ++it) {
        if (it->x >= width_ || it->y >= height_ || !m[it->y * width_ + it->x])
            out.push_back(*it);
    }
}

// ------------------------------------------------------------------------------------------------ TrailFilter

const char *to_string(TrailMode mode) {
    switch (mode) {
    case TrailMode::Trail:
        return "TRAIL";
    case TrailMode::StcCutTrail:
        return "STC_CUT_TRAIL";
    case TrailMode::StcKeepTrail:
        return "STC_KEEP_TRAIL";
    }
    return "?";
}

TrailFilter::TrailFilter(int width, int height) :
    width_(width), height_(height), state_(static_cast<size_t>(width) * height) {}

void TrailFilter::reset() {
    std::fill(state_.begin(), state_.end(), PixelState());
}

void TrailFilter::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) {
    for (auto it = begin; it != end; ++it) {
        if (it->x >= width_ || it->y >= height_)
            continue;
        PixelState &s       = state_[it->y * width_ + it->x];
        const bool same_run = s.last_p == it->p && it->t - s.last_t <= threshold_;
        s.burst             = same_run ? static_cast<uint16_t>(std::min<int>(s.burst + 1, 0xFFFF)) : 1;
        s.last_t            = it->t;
        s.last_p            = it->p;

        bool keep = false;
        switch (mode_) {
        case TrailMode::Trail:
            keep = s.burst == 1;
            break;
        case TrailMode::StcCutTrail:
            keep = s.burst == 2;
            break;
        case TrailMode::StcKeepTrail:
            keep = s.burst >= 2;
            break;
        }
        if (keep)
            out.push_back(*it);
    }
}

// ------------------------------------------------------------------------------------------------ ActivityFilter

ActivityFilter::ActivityFilter(int width, int height) : width_(width), height_(height) {
    reset();
}

void ActivityFilter::reset() {
    const size_t n = static_cast<size_t>(width_) * height_;
    for (int p = 0; p < 2; ++p) {
        last_[p].assign(n, std::numeric_limits<timestamp>::min() / 2);
        released_[p].assign(n, std::numeric_limits<timestamp>::min() / 2);
        last_seq_[p].assign(n, -1);
    }
    queue_.clear();
    base_seq_ = 0;
}

void ActivityFilter::set_look_ahead(bool look_ahead) {
    if (look_ahead != look_ahead_)
        reset();
    look_ahead_ = look_ahead;
}

void ActivityFilter::finalize_front(std::vector<EventCD> &out) {
    const Pending &front = queue_.front();
    const EventCD &e     = front.ev;
    const int x = e.x, y = e.y, p = e.p ? 1 : 0;
    const timestamp since = e.t - window_, until = e.t + window_;
    const int x0 = std::max(0, x - radius_), x1 = std::min(width_ - 1, x + radius_);
    const int y0 = std::max(0, y - radius_), y1 = std::min(height_ - 1, y + radius_);
    int support = 0;
    for (int yy = y0; yy <= y1 && support < min_neighbors_; ++yy) {
        for (int xx = x0; xx <= x1 && support < min_neighbors_; ++xx) {
            if (xx == x && yy == y)
                continue;
            const size_t i = static_cast<size_t>(yy) * width_ + xx;
            for (int q = 0; q < 2; ++q) {
                if (same_polarity_ && q != p)
                    continue;
                // earlier events of the neighbour: the latest one already released
                bool active = released_[q][i] >= since;
                // later events: still pending (all pending events are >= e.t); the oldest one decides
                if (!active) {
                    int64_t s = last_seq_[q][i], oldest = -1;
                    while (s >= base_seq_) {
                        oldest = s;
                        s      = queue_[static_cast<size_t>(s - base_seq_)].prev;
                    }
                    active = oldest >= 0 && queue_[static_cast<size_t>(oldest - base_seq_)].ev.t <= until;
                }
                if (active) {
                    ++support;
                    break;
                }
            }
        }
    }
    released_[p][static_cast<size_t>(y) * width_ + x] = e.t;
    if (support >= min_neighbors_)
        out.push_back(e);
    queue_.pop_front();
    ++base_seq_;
}

void ActivityFilter::flush(std::vector<EventCD> &out) {
    while (!queue_.empty())
        finalize_front(out);
}

void ActivityFilter::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) {
    if (look_ahead_) {
        for (auto it = begin; it != end; ++it) {
            if (it->x >= width_ || it->y >= height_)
                continue;
            // the front event's future neighbourhood is complete once an event later than front + window arrives
            while (!queue_.empty() && queue_.front().ev.t + window_ < it->t)
                finalize_front(out);
            const int p    = it->p ? 1 : 0;
            const size_t i = static_cast<size_t>(it->y) * width_ + it->x;
            queue_.push_back({*it, last_seq_[p][i]});
            last_seq_[p][i] = base_seq_ + static_cast<int64_t>(queue_.size()) - 1;
        }
        return;
    }
    for (auto it = begin; it != end; ++it) {
        if (it->x >= width_ || it->y >= height_)
            continue;
        const int x = it->x, y = it->y, p = it->p ? 1 : 0;
        const timestamp since = it->t - window_;
        const int x0 = std::max(0, x - radius_), x1 = std::min(width_ - 1, x + radius_);
        const int y0 = std::max(0, y - radius_), y1 = std::min(height_ - 1, y + radius_);
        int support = 0;
        for (int yy = y0; yy <= y1 && support < min_neighbors_; ++yy) {
            const size_t row = static_cast<size_t>(yy) * width_;
            for (int xx = x0; xx <= x1; ++xx) {
                if (xx == x && yy == y)
                    continue;
                const size_t i = row + xx;
                const bool active =
                    same_polarity_ ? last_[p][i] >= since : (last_[0][i] >= since || last_[1][i] >= since);
                if (active && ++support >= min_neighbors_)
                    break;
            }
        }
        last_[p][static_cast<size_t>(y) * width_ + x] = it->t;
        if (support >= min_neighbors_)
            out.push_back(*it);
    }
}

// ------------------------------------------------------------------------------------------------ RefractoryFilter

RefractoryFilter::RefractoryFilter(int width, int height) : width_(width), height_(height) {
    reset();
}

void RefractoryFilter::reset() {
    last_kept_.assign(static_cast<size_t>(width_) * height_, std::numeric_limits<timestamp>::min());
}

void RefractoryFilter::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) {
    for (auto it = begin; it != end; ++it) {
        if (it->x >= width_ || it->y >= height_)
            continue;
        timestamp &last = last_kept_[it->y * width_ + it->x];
        if (last == std::numeric_limits<timestamp>::min() || it->t - last >= period_) {
            last = it->t;
            out.push_back(*it);
        }
    }
}

// ------------------------------------------------------------------------------------------------ ErcEmulator

uint64_t ErcEmulator::budget_per_window() const {
    return static_cast<uint64_t>(static_cast<double>(rate_) * static_cast<double>(period_) / 1e6 + 0.5);
}

void ErcEmulator::reset() {
    window_.clear();
    has_window_ = false;
}

void ErcEmulator::emit_window(std::vector<EventCD> &out) {
    const uint64_t n      = window_.size();
    const uint64_t budget = budget_per_window();
    if (n <= budget) {
        out.insert(out.end(), window_.begin(), window_.end());
    } else if (budget > 0) {
        // Keep event i iff floor((i+1)*budget/n) > floor(i*budget/n): exactly `budget` events, evenly spread.
        for (uint64_t i = 0; i < n; ++i)
            if ((i + 1) * budget / n > i * budget / n)
                out.push_back(window_[i]);
    }
    window_.clear();
}

void ErcEmulator::process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) {
    for (auto it = begin; it != end; ++it) {
        const timestamp idx = it->t >= 0 ? it->t / period_ : (it->t - period_ + 1) / period_;
        if (!has_window_) {
            window_index_ = idx;
            has_window_   = true;
        } else if (idx > window_index_) {
            emit_window(out);
            window_index_ = idx;
        }
        window_.push_back(*it); // late (out of order) events are counted in the current window
    }
}

void ErcEmulator::flush(std::vector<EventCD> &out) {
    emit_window(out);
    has_window_ = false;
}

// ------------------------------------------------------------------------------------------------ FilterPipeline

FilterPipeline::FilterPipeline(int width, int height) :
    width_(width),
    height_(height),
    mask_filter_(width, height),
    activity_(width, height),
    refractory_(width, height),
    trail_(width, height) {
    set_config(FilterConfig());
}

void FilterPipeline::set_config(const FilterConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config.erc_enabled && !config_.erc_enabled)
        erc_.reset();
    config_ = config;
    refractory_.set_period_us(config.refractory_us);
    if (config.activity_look_ahead != activity_.look_ahead()) {
        // switching mode restarts the filter: the events held by the look-ahead (at most one window) are dropped
        activity_.set_look_ahead(config.activity_look_ahead);
    }
    activity_.set_window_us(config.activity_window_us);
    activity_.set_radius(config.activity_radius);
    activity_.set_min_neighbors(config.activity_min_neighbors);
    activity_.set_same_polarity(config.activity_same_polarity);
    trail_.set_mode(config.trail_mode);
    trail_.set_threshold_us(config.trail_threshold_us);
    polarity_.set(config.keep_on, config.keep_off);
    erc_.set_rate(config.erc_rate);
    erc_.set_period_us(config.erc_period_us);
}

FilterConfig FilterPipeline::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void FilterPipeline::set_mask(std::shared_ptr<const std::vector<uint8_t>> mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    mask_ = mask;
    mask_filter_.set_mask(std::move(mask));
}

void FilterPipeline::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    activity_.reset();
    refractory_.reset();
    trail_.reset();
    erc_.reset();
    erc_was_enabled_      = false;
    activity_was_enabled_ = false;
    stats_                = Stats();
}

bool FilterPipeline::has_mask_locked() const {
    return config_.mask_enabled && mask_ && !mask_->empty() &&
           std::any_of(mask_->begin(), mask_->end(), [](uint8_t v) { return v != 0; });
}

bool FilterPipeline::is_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_mask_locked() || config_.activity_enabled || config_.refractory_enabled || config_.trail_enabled ||
           (config_.polarity_enabled && !(config_.keep_on && config_.keep_off)) || config_.erc_enabled;
}

const std::vector<EventCD> &FilterPipeline::process(const EventCD *begin, const EventCD *end) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.in += static_cast<uint64_t>(end - begin);
    return run_chain_locked(begin, end, false);
}

const std::vector<EventCD> &FilterPipeline::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return run_chain_locked(nullptr, nullptr, true);
}

timestamp FilterPipeline::holding_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    timestamp hold = 0;
    if (config_.activity_enabled && config_.activity_look_ahead)
        hold += config_.activity_window_us;
    if (config_.erc_enabled)
        hold += config_.erc_period_us;
    return hold;
}

const std::vector<EventCD> &FilterPipeline::run_chain_locked(const EventCD *begin, const EventCD *end,
                                                             bool flush_all) {
    std::vector<EventCD> *bufs[2] = {&a_, &b_};
    int next                      = 0;
    const EventCD *cb = begin, *ce = end;
    bool ran = false;
    auto run = [&](auto &&stage) {
        std::vector<EventCD> &dst = *bufs[next];
        dst.clear();
        stage(cb, ce, dst);
        cb = dst.data();
        ce = dst.data() + dst.size();
        next ^= 1;
        ran = true;
    };

    if (config_.mask_enabled && mask_ && !mask_->empty())
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) { mask_filter_.process(b, e, o); });
    if (config_.activity_enabled) {
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) {
            activity_.process(b, e, o);
            if (flush_all)
                activity_.flush(o);
        });
        activity_was_enabled_ = true;
    } else if (activity_was_enabled_) {
        // Activity filter just got disabled: release the events it still held (they precede the current ones).
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) {
            activity_.flush(o);
            o.insert(o.end(), b, e);
        });
        activity_was_enabled_ = false;
    }
    if (config_.refractory_enabled)
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) { refractory_.process(b, e, o); });
    if (config_.trail_enabled)
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) { trail_.process(b, e, o); });
    if (config_.polarity_enabled && !(config_.keep_on && config_.keep_off))
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) { polarity_.process(b, e, o); });
    if (config_.erc_enabled) {
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) {
            erc_.process(b, e, o);
            if (flush_all)
                erc_.flush(o);
        });
        erc_was_enabled_ = true;
    } else if (erc_was_enabled_) {
        // ERC just got disabled: release the events it still held (they precede the current ones).
        run([&](const EventCD *b, const EventCD *e, std::vector<EventCD> &o) {
            erc_.flush(o);
            o.insert(o.end(), b, e);
        });
        erc_was_enabled_ = false;
    }

    if (!ran) {
        a_.assign(begin, end);
        stats_.out += a_.size();
        return a_;
    }
    std::vector<EventCD> &result = *bufs[next ^ 1];
    stats_.out += result.size();
    return result;
}

FilterPipeline::Stats FilterPipeline::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace evs
