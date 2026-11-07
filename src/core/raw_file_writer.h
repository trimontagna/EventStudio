#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/evt_encoder.h"

namespace evs {

// Writes a Prophesee RAW file (text header + encoded events) readable by OpenEB / Metavision tools.
// Thread-safe: write(), write_triggers(), advance() and close() may be called from different threads.
//
// CD events and external triggers arrive as two separately time-ordered streams (a decoder may deliver a trigger
// after CD events that are later in time). Both are queued and merged in time order: events are encoded once their
// timestamp is <= the "safe time" given to advance() (the caller guarantees that no earlier event will arrive),
// and everything left is encoded by close().
class RawFileWriter {
public:
    ~RawFileWriter();

    // `metadata` entries are copied into the header (e.g. from the source recording); the format-related fields
    // ("format", "evt", "geometry") are always set from the arguments.
    bool open(const std::filesystem::path &path, EvtFormat format, int width, int height,
              const std::map<std::string, std::string> &metadata = {}, std::string *error = nullptr);
    void write(const EventCD *begin, const EventCD *end);
    void write_triggers(const EventExtTrigger *begin, const EventExtTrigger *end);
    // Encodes the queued events with t <= safe_t (triggers before CD events of the same timestamp).
    void advance(timestamp safe_t);
    void close();

    bool is_open() const;
    uint64_t events_written() const;   // CD events received
    uint64_t triggers_written() const; // trigger events received
    uint64_t bytes_written() const;
    const std::filesystem::path &path() const { return path_; }

    static std::string build_header(EvtFormat format, int width, int height,
                                    const std::map<std::string, std::string> &metadata);

private:
    void encode_until_locked(timestamp safe_t);

    mutable std::mutex mutex_;
    std::vector<EventCD> cd_queue_;
    std::vector<EventExtTrigger> trigger_queue_;
    size_t cd_head_ = 0, trigger_head_ = 0;
    uint64_t triggers_ = 0;
    std::filesystem::path path_;
    std::ofstream file_;
    std::unique_ptr<EventEncoder> encoder_;
    std::vector<uint8_t> buffer_;
    std::unique_ptr<char[]> stream_buffer_;
    uint64_t events_ = 0, bytes_ = 0;
};

} // namespace evs
