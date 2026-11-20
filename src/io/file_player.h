#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/types.h"
#include "io/event_file_source.h"

namespace evs {

// Plays an event file on a worker thread, paced against the wall clock (speed factor), with pause, seek and loop.
// All callbacks run on the worker thread.
class FilePlayer {
public:
    using EventsCallback   = std::function<void(const EventCD *, const EventCD *)>;
    using TriggersCallback = std::function<void(const EventExtTrigger *, const EventExtTrigger *)>;
    using SeekCallback     = std::function<void(timestamp)>;   // after a seek / rewind, before the next events
    using RawBytesCallback = std::function<void(size_t bytes)>; // RAW files only

    FilePlayer();
    ~FilePlayer();

    bool open(const std::filesystem::path &path, std::string *error);
    void close();
    bool is_open() const;

    // Must be set before open().
    void set_events_callback(EventsCallback cb) { events_cb_ = std::move(cb); }
    // External triggers, delivered as decoded (not paced).
    void set_triggers_callback(TriggersCallback cb) { triggers_cb_ = std::move(cb); }
    void set_seek_callback(SeekCallback cb) { seek_cb_ = std::move(cb); }
    void set_raw_bytes_callback(RawBytesCallback cb) { raw_cb_ = std::move(cb); }

    void play();
    void pause();
    bool is_playing() const { return playing_; }
    bool at_end() const { return at_end_; }

    // 1.0 = real time; <= 0: as fast as possible.
    void set_speed(double speed);
    double speed() const;
    void set_loop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }

    // Asynchronous; ignored while the RAW index is not ready (see seek_range()).
    // Never blocks: the range is cached by the worker thread (querying the reader from another thread would wait
    // for the reader's lock, held during a whole read).
    // Decoding restarts `preroll_us` before t so that a paused view shows the state at t (the events up to t are
    // delivered without pacing, then playback stays paused if it was).
    void request_seek(timestamp t, timestamp preroll_us = 0);
    bool seek_range(timestamp &min_t, timestamp &max_t) const;
    // Paused step forward to t without seeking: decoding carries on from where it stopped and delivers the events up
    // to t. False (nothing done) while playing, at the end, with a seek pending or t not ahead: request_seek() then.
    bool advance_to(timestamp t);

    // Time of the last delivered event.
    timestamp current_time() const { return current_t_; }
    // Time the view stands at: while paused after a seek / step, its target (exact, even between events), otherwise
    // current_time().
    timestamp position() const;
    // True once the events up to the target of the last seek / step are all delivered (paused): the view then
    // shows the state at position().
    bool target_reached() const;
    // True while paused with a seek / step whose events up to the target are still being delivered.
    bool target_pending() const;
    int width() const;
    int height() const;
    std::string encoding() const;
    std::map<std::string, std::string> metadata() const;
    std::filesystem::path path() const;
    std::string status() const;

private:
    enum class Wait { Proceed, Abort };
    void run();
    void deliver(const EventCD *begin, const EventCD *end);
    Wait wait_until_due(timestamp t, timestamp &limit);
    bool rewind(std::unique_lock<std::mutex> &lock);
    void set_status(std::string s);
    void refresh_seek_range(); // worker thread only, mutex_ not held
    void register_reader_callbacks();

    EventsCallback events_cb_;
    TriggersCallback triggers_cb_;
    SeekCallback seek_cb_;
    RawBytesCallback raw_cb_;

    std::unique_ptr<EventFileSource> source_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false}, playing_{false}, at_end_{false}, loop_{false};
    std::atomic<timestamp> current_t_{0};
    std::atomic<bool> seek_valid_{false};
    std::atomic<timestamp> seek_min_{0}, seek_max_{0};
    std::chrono::steady_clock::time_point last_seek_refresh_;
    double speed_ = 1.0;
    std::optional<timestamp> seek_request_;
    std::optional<timestamp> preview_until_;
    std::optional<timestamp> target_; // target of the last seek / step, until playback resumes
    bool abort_buffer_ = false;
    std::string status_;

    // pacing reference: event time pace_t_ was delivered at wall time pace_wall_
    bool pace_valid_ = false;
    timestamp pace_t_ = 0;
    std::chrono::steady_clock::time_point pace_wall_;
};

} // namespace evs
