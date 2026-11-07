#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "core/types.h"

namespace evs {

// Software counterparts of the sensor / ESP features.
// Every filter appends the events it keeps to `out`, preserving their order, so filters can be chained
// and their output fed straight to an encoder (which requires time-ordered events).

class PolarityFilter {
public:
    void set(bool keep_on, bool keep_off) {
        keep_on_  = keep_on;
        keep_off_ = keep_off;
    }
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) const;

private:
    bool keep_on_ = true, keep_off_ = true;
};

// Drops the events of the pixels flagged in a width*height bitmap (non-zero = masked).
class PixelMaskFilter {
public:
    PixelMaskFilter(int width, int height);
    // An empty bitmap masks nothing; otherwise it must hold width*height entries.
    void set_mask(std::shared_ptr<const std::vector<uint8_t>> mask);
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out) const;

private:
    int width_, height_;
    std::shared_ptr<const std::vector<uint8_t>> mask_;
};

// Same modes as the ESP Event Trail Filter (I_EventTrailFilterModule::Type).
enum class TrailMode { Trail, StcCutTrail, StcKeepTrail };
const char *to_string(TrailMode mode);

// A burst is a run of same-polarity events on one pixel, each event arriving at most `threshold` us after the
// previous one. A polarity change or a longer gap starts a new burst.
//  - Trail        : keeps the first event of each burst.
//  - StcCutTrail  : keeps the second event of each burst only (isolated events and the trail are dropped).
//  - StcKeepTrail : keeps the second and all following events of each burst (isolated events are dropped).
// Unlike the IMX636 hardware (1 ms .. 100 ms, 1 ms steps) any threshold >= 1 us is accepted.
class TrailFilter {
public:
    TrailFilter(int width, int height);
    void set_mode(TrailMode mode) { mode_ = mode; }
    void set_threshold_us(timestamp threshold) { threshold_ = threshold; }
    TrailMode mode() const { return mode_; }
    timestamp threshold_us() const { return threshold_; }
    void reset();
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out);

private:
    struct PixelState {
        timestamp last_t = 0;
        int16_t last_p   = -1; // -1: no event seen yet
        uint16_t burst   = 0;  // 1-based rank of the last event in its burst (saturated)
    };
    int width_, height_;
    TrailMode mode_      = TrailMode::StcCutTrail;
    timestamp threshold_ = 1000;
    std::vector<PixelState> state_;
};

// Spatio-temporal activity (neighbourhood) filter: keeps an event only if at least `min_neighbors` pixels of its
// (2 * radius + 1)^2 neighbourhood (the pixel itself excluded) had an event within the last `window` us
// (optionally of the same polarity). Isolated background-noise events are removed while the spatially correlated
// events of moving objects / particles are kept.
//  - causal mode: support from the last `window` us only. The first event of each object / neighbourhood has no
//    support and is dropped, which biases arrival times late when pixel response times jitter.
//  - look-ahead mode (default): support from neighbour events within +/- `window` us. Events are held for `window` us
//    (latency) until their future neighbourhood is known; flush() releases the held events.
class ActivityFilter {
public:
    ActivityFilter(int width, int height);
    void set_window_us(timestamp window) { window_ = window; }
    void set_radius(int radius) { radius_ = radius < 1 ? 1 : radius; }
    void set_min_neighbors(int n) { min_neighbors_ = n < 1 ? 1 : n; }
    void set_same_polarity(bool same) { same_polarity_ = same; }
    void set_look_ahead(bool look_ahead);
    bool look_ahead() const { return look_ahead_; }
    void reset();
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out);
    void flush(std::vector<EventCD> &out);

private:
    void finalize_front(std::vector<EventCD> &out);

    int width_, height_;
    timestamp window_   = 1000;
    int radius_         = 1;
    int min_neighbors_  = 1;
    bool same_polarity_ = false;
    bool look_ahead_    = true;
    std::vector<timestamp> last_[2]; // causal: per polarity, last event time of each pixel

    // look-ahead state
    struct Pending {
        EventCD ev;
        int64_t prev; // sequence number of the previous event of the same pixel & polarity
    };
    std::deque<Pending> queue_;
    int64_t base_seq_ = 0;               // sequence number of queue_.front()
    std::vector<int64_t> last_seq_[2];   // per polarity: sequence number of the latest event of each pixel
    std::vector<timestamp> released_[2]; // per polarity: time of the latest finalized event of each pixel
};

// Software refractory period: after an event is kept on a pixel, any event on that pixel (either polarity)
// within `period` us is dropped. This only approximates bias_refr (it cannot recover events the sensor never
// produced, it can only remove more).
class RefractoryFilter {
public:
    RefractoryFilter(int width, int height);
    void set_period_us(timestamp period) { period_ = period; }
    timestamp period_us() const { return period_; }
    void reset();
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out);

private:
    int width_, height_;
    timestamp period_ = 1000;
    std::vector<timestamp> last_kept_;
};

// Emulation of the Event Rate Controller: time is cut in windows of `period` us (200 us on the IMX636) and at most
// rate * period events are output per window. When a window overflows, the kept events are spread evenly
// over the window's events (deterministic decimation), approximating the hardware's spatio-temporal dropping.
// Events are buffered until their window closes, i.e. until an event of a later window arrives or flush().
class ErcEmulator {
public:
    void set_rate(uint64_t events_per_second) { rate_ = events_per_second; }
    void set_period_us(timestamp period) { period_ = period > 0 ? period : 1; }
    uint64_t rate() const { return rate_; }
    timestamp period_us() const { return period_; }
    uint64_t budget_per_window() const;
    void reset();
    void process(const EventCD *begin, const EventCD *end, std::vector<EventCD> &out);
    void flush(std::vector<EventCD> &out);

private:
    void emit_window(std::vector<EventCD> &out);

    uint64_t rate_    = 10'000'000;
    timestamp period_ = 200;
    timestamp window_index_ = 0;
    bool has_window_        = false;
    std::vector<EventCD> window_;
};

struct FilterConfig {
    bool mask_enabled = true;

    bool activity_enabled        = false;
    timestamp activity_window_us = 100;
    int activity_radius          = 1;
    int activity_min_neighbors   = 1;
    bool activity_same_polarity  = false;
    bool activity_look_ahead     = true;

    bool refractory_enabled = false;
    timestamp refractory_us = 1000;

    bool trail_enabled           = false;
    TrailMode trail_mode         = TrailMode::StcCutTrail;
    timestamp trail_threshold_us = 1000;

    bool polarity_enabled = false;
    bool keep_on          = true;
    bool keep_off         = true;

    bool erc_enabled        = false;
    uint64_t erc_rate       = 10'000'000;
    timestamp erc_period_us = 200;
};

// Thread-safe chain: mask -> activity -> refractory -> trail -> polarity -> ERC (the ESP order: pixel-level stages first, ERC
// last). Configuration may be changed from another thread while events are processed.
class FilterPipeline {
public:
    FilterPipeline(int width, int height);

    void set_config(const FilterConfig &config);
    FilterConfig config() const;
    void set_mask(std::shared_ptr<const std::vector<uint8_t>> mask);
    // Forgets per-pixel history (to call after a seek / source change).
    void reset();

    // Returns the kept events. The reference stays valid until the next call to process / flush.
    const std::vector<EventCD> &process(const EventCD *begin, const EventCD *end);
    // Outputs the events still held (activity look-ahead, ERC emulation).
    const std::vector<EventCD> &flush();
    // Upper bound of the time by which output events lag the input (us).
    timestamp holding_time() const;

    // True when at least one stage can drop events.
    bool is_active() const;

    struct Stats {
        uint64_t in = 0, out = 0;
    };
    Stats stats() const;

    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool has_mask_locked() const;
    const std::vector<EventCD> &run_chain_locked(const EventCD *begin, const EventCD *end, bool flush_all);

    int width_, height_;
    mutable std::mutex mutex_;
    FilterConfig config_;
    bool erc_was_enabled_      = false;
    bool activity_was_enabled_ = false;
    std::shared_ptr<const std::vector<uint8_t>> mask_;
    PixelMaskFilter mask_filter_;
    ActivityFilter activity_;
    RefractoryFilter refractory_;
    TrailFilter trail_;
    PolarityFilter polarity_;
    ErcEmulator erc_;
    std::vector<EventCD> a_, b_;
    Stats stats_;
};

} // namespace evs
