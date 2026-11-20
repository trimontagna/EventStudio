#include "io/file_player.h"

#include <algorithm>

#include <metavision/sdk/stream/event_file_reader.h>
#include <metavision/sdk/stream/raw_event_file_reader.h>

namespace evs {

namespace {
constexpr timestamp kSliceUs = 1000; // granularity of the real-time pacing (event time)
}

FilePlayer::FilePlayer() = default;

FilePlayer::~FilePlayer() {
    close();
}

bool FilePlayer::open(const std::filesystem::path &path, std::string *error) {
    close();
    auto src = EventFileSource::open(path, true, error);
    if (!src)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = std::move(src);
        register_reader_callbacks();
        stop_       = false;
        playing_    = false;
        at_end_     = false;
        current_t_  = 0;
        pace_valid_ = false;
        seek_valid_ = false;
        last_seek_refresh_ = {};
        seek_request_.reset();
        preview_until_.reset();
        target_.reset();
        status_.clear();
    }
    thread_ = std::thread(&FilePlayer::run, this);
    return true;
}

void FilePlayer::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    source_.reset();
    playing_ = false;
}

void FilePlayer::register_reader_callbacks() {
    source_->reader().add_read_callback([this](const EventCD *b, const EventCD *e) { deliver(b, e); });
    source_->reader().add_read_callback([this](const EventExtTrigger *b, const EventExtTrigger *e) {
        if (triggers_cb_ && !abort_buffer_ && b != e)
            triggers_cb_(b, e);
    });
    if (raw_cb_ && source_->raw_reader())
        source_->raw_reader()->add_raw_read_callback(
            [this](const std::uint8_t *b, const std::uint8_t *e) { raw_cb_(static_cast<size_t>(e - b)); });
}

bool FilePlayer::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ != nullptr;
}

void FilePlayer::play() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!source_)
            return;
        playing_ = true;
        target_.reset();
    }
    cv_.notify_all();
}

void FilePlayer::pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = false;
    }
    cv_.notify_all();
}

void FilePlayer::set_speed(double speed) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        speed_      = speed;
        pace_valid_ = false;
    }
    cv_.notify_all();
}

double FilePlayer::speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speed_;
}

void FilePlayer::request_seek(timestamp t, timestamp preroll_us) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_request_  = std::max<timestamp>(0, t - std::max<timestamp>(0, preroll_us));
        preview_until_ = t;
        target_        = t;
    }
    cv_.notify_all();
}

bool FilePlayer::advance_to(timestamp t) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!source_ || playing_ || at_end_ || seek_request_ || t <= (target_ ? *target_ : current_t_.load()))
            return false;
        preview_until_ = t;
        target_        = t;
    }
    cv_.notify_all();
    return true;
}

timestamp FilePlayer::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !playing_ && target_ ? *target_ : current_t_.load();
}

bool FilePlayer::target_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !playing_ && target_ && (seek_request_ || preview_until_);
}

bool FilePlayer::target_reached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !playing_ && target_ && !seek_request_ && !preview_until_;
}

bool FilePlayer::seek_range(timestamp &min_t, timestamp &max_t) const {
    if (!seek_valid_)
        return false;
    min_t = seek_min_;
    max_t = seek_max_;
    return true;
}

void FilePlayer::refresh_seek_range() {
    // The range is fixed once the index is built: only poll until then, at most every 100 ms.
    const auto now = std::chrono::steady_clock::now();
    if (seek_valid_ || now - last_seek_refresh_ < std::chrono::milliseconds(100))
        return;
    last_seek_refresh_ = now;
    timestamp min_t = 0, max_t = 0;
    if (source_->seek_range(min_t, max_t)) {
        seek_min_   = min_t;
        seek_max_   = max_t;
        seek_valid_ = true;
    }
}

int FilePlayer::width() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ ? source_->width() : kDefaultWidth;
}

int FilePlayer::height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ ? source_->height() : kDefaultHeight;
}

std::string FilePlayer::encoding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ ? source_->encoding() : std::string();
}

std::map<std::string, std::string> FilePlayer::metadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ ? source_->metadata() : std::map<std::string, std::string>();
}

std::filesystem::path FilePlayer::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ ? source_->path() : std::filesystem::path();
}

std::string FilePlayer::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void FilePlayer::set_status(std::string s) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(s);
}

FilePlayer::Wait FilePlayer::wait_until_due(timestamp t, timestamp &limit) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (stop_ || seek_request_)
            return Wait::Abort;
        if (preview_until_) {
            if (t <= *preview_until_) {
                limit = std::min(limit, *preview_until_ + 1); // up to the target exactly, not a slice beyond
                return Wait::Proceed;                         // seek preroll: no pacing
            }
            preview_until_.reset();
            pace_valid_ = false;
        }
        if (!playing_) {
            pace_valid_ = false;
            cv_.wait(lock);
            continue;
        }
        if (speed_ <= 0)
            return Wait::Proceed;
        const auto now = std::chrono::steady_clock::now();
        if (!pace_valid_ || t < pace_t_) {
            pace_valid_ = true;
            pace_t_     = t;
            pace_wall_  = now;
            return Wait::Proceed;
        }
        const auto due = pace_wall_ + std::chrono::microseconds(static_cast<int64_t>((t - pace_t_) / speed_));
        if (now >= due) {
            // Decoding cannot keep up: re-anchor instead of bursting to catch up.
            if (now - due > std::chrono::milliseconds(250)) {
                pace_t_    = t;
                pace_wall_ = now;
            }
            return Wait::Proceed;
        }
        cv_.wait_until(lock, due);
    }
}

void FilePlayer::deliver(const EventCD *begin, const EventCD *end) {
    if (abort_buffer_ || begin == end)
        return;
    for (const EventCD *s = begin; s != end;) {
        timestamp limit = s->t + kSliceUs;
        if (wait_until_due(s->t, limit) == Wait::Abort) {
            abort_buffer_ = true;
            return;
        }
        const EventCD *e = s;
        while (e != end && e->t < limit)
            ++e;
        if (events_cb_)
            events_cb_(s, e);
        current_t_ = (e - 1)->t;
        s          = e;
    }
}

bool FilePlayer::rewind(std::unique_lock<std::mutex> &lock) {
    timestamp min_t = 0, max_t = 0;
    const bool can_seek = source_->seek_range(min_t, max_t);
    lock.unlock();
    bool ok = false;
    if (can_seek) {
        try {
            ok = source_->reader().seek(min_t);
        } catch (...) {}
    }
    lock.lock();
    if (!ok) {
        // Index not ready: reopen the file to start over.
        std::string error;
        auto path = source_->path();
        lock.unlock();
        auto src = EventFileSource::open(path, true, &error);
        lock.lock();
        if (!src) {
            status_ = error;
            return false;
        }
        source_ = std::move(src);
        register_reader_callbacks();
        seek_valid_        = false;
        last_seek_refresh_ = {};
        min_t              = 0;
    }
    at_end_     = false;
    current_t_  = min_t;
    pace_valid_ = false;
    target_.reset();
    lock.unlock();
    if (seek_cb_)
        seek_cb_(min_t);
    lock.lock();
    return true;
}

void FilePlayer::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        const auto has_work = [&] { return stop_ || seek_request_ || playing_ || (preview_until_ && !at_end_); };
        while (!has_work()) {
            if (seek_valid_) {
                cv_.wait(lock, has_work);
            } else {
                cv_.wait_for(lock, std::chrono::milliseconds(100), has_work);
                lock.unlock();
                refresh_seek_range();
                lock.lock();
            }
        }
        if (stop_)
            break;

        if (seek_request_) {
            const timestamp t = *seek_request_;
            seek_request_.reset();
            lock.unlock();
            bool ok = false;
            try {
                ok = source_->reader().seek(t);
            } catch (...) {}
            lock.lock();
            if (ok) {
                at_end_     = false;
                current_t_  = t;
                pace_valid_ = false;
                status_.clear();
                lock.unlock();
                if (seek_cb_)
                    seek_cb_(t);
                lock.lock();
            } else {
                preview_until_.reset();
                target_.reset();
                status_ = "Seek unavailable (the RAW index is still being built)";
            }
            continue;
        }

        if (at_end_) {
            // play() pressed at the end of the file: start over
            if (!rewind(lock)) {
                playing_ = false;
                continue;
            }
        }

        abort_buffer_ = false;
        lock.unlock();
        bool more = false;
        try {
            more = source_->reader().read();
            refresh_seek_range();
        } catch (const std::exception &e) {
            set_status(std::string("Read error: ") + e.what());
        }
        lock.lock();
        if (!more && !abort_buffer_) {
            preview_until_.reset();
            if (loop_ && !stop_ && playing_) {
                if (!rewind(lock))
                    playing_ = false;
            } else {
                at_end_  = true;
                playing_ = false;
            }
        }
    }
}

} // namespace evs
