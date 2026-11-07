#include "core/raw_file_writer.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace evs {

RawFileWriter::~RawFileWriter() {
    close();
}

std::string RawFileWriter::build_header(EvtFormat format, int width, int height,
                                        const std::map<std::string, std::string> &metadata) {
    std::map<std::string, std::string> fields = metadata;
    // Fields that describe the encoding are always rewritten; stale index / size info is dropped.
    fields.erase("end");
    fields["format"]   = format_descriptor(format, width, height);
    fields["evt"]      = evt_version(format);
    fields["geometry"] = std::to_string(width) + "x" + std::to_string(height);
    if (fields.find("date") == fields.end()) {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        localtime_s(&tm, &now);
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        fields["date"] = ss.str();
    }
    if (fields.find("plugin_name") == fields.end())
        fields["plugin_name"] = "hal_plugin_prophesee";
    if (fields.find("camera_integrator_name") == fields.end())
        fields["camera_integrator_name"] = "Prophesee";
    if (fields.find("plugin_integrator_name") == fields.end())
        fields["plugin_integrator_name"] = "Prophesee";
    fields["recorder"] = "EventStudio";

    std::string header;
    for (const auto &[key, value] : fields) {
        if (key.empty() || key.find_first_of(" \r\n") != std::string::npos)
            continue;
        std::string v = value;
        for (auto &c : v)
            if (c == '\n' || c == '\r')
                c = ' ';
        header += "% " + key + " " + v + "\n";
    }
    header += "% end\n";
    return header;
}

bool RawFileWriter::open(const std::filesystem::path &path, EvtFormat format, int width, int height,
                         const std::map<std::string, std::string> &metadata, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open())
        file_.close();
    stream_buffer_ = std::make_unique<char[]>(1 << 22);
    file_.rdbuf()->pubsetbuf(stream_buffer_.get(), 1 << 22);
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_) {
        if (error)
            *error = "Cannot create " + path.string();
        return false;
    }
    path_          = path;
    encoder_       = make_encoder(format, width, height);
    const auto hdr = build_header(format, width, height, metadata);
    file_.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    events_   = 0;
    triggers_ = 0;
    bytes_    = hdr.size();
    cd_queue_.clear();
    trigger_queue_.clear();
    cd_head_ = trigger_head_ = 0;
    return static_cast<bool>(file_);
}

void RawFileWriter::write(const EventCD *begin, const EventCD *end) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open() || begin == end)
        return;
    cd_queue_.insert(cd_queue_.end(), begin, end);
    events_ += static_cast<uint64_t>(end - begin);
}

void RawFileWriter::write_triggers(const EventExtTrigger *begin, const EventExtTrigger *end) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open() || begin == end)
        return;
    trigger_queue_.insert(trigger_queue_.end(), begin, end);
    triggers_ += static_cast<uint64_t>(end - begin);
}

void RawFileWriter::advance(timestamp safe_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open())
        encode_until_locked(safe_t);
}

void RawFileWriter::encode_until_locked(timestamp safe_t) {
    buffer_.clear();
    for (;;) {
        const bool has_cd = cd_head_ < cd_queue_.size() && cd_queue_[cd_head_].t <= safe_t;
        const bool has_tr = trigger_head_ < trigger_queue_.size() && trigger_queue_[trigger_head_].t <= safe_t;
        if (!has_cd && !has_tr)
            break;
        if (has_tr && (!has_cd || trigger_queue_[trigger_head_].t <= cd_queue_[cd_head_].t)) {
            encoder_->encode_trigger(trigger_queue_[trigger_head_++], buffer_);
            continue;
        }
        // run of CD events before the next trigger (or up to the safe time)
        const timestamp limit = has_tr ? trigger_queue_[trigger_head_].t - 1 : safe_t;
        size_t end            = cd_head_;
        while (end < cd_queue_.size() && cd_queue_[end].t <= limit)
            ++end;
        if (end == cd_head_) // out-of-order CD event older than the trigger: write it now
            ++end;
        encoder_->encode(cd_queue_.data() + cd_head_, cd_queue_.data() + end, buffer_);
        cd_head_ = end;
    }
    file_.write(reinterpret_cast<const char *>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
    bytes_ += buffer_.size();
    if (cd_head_ > 4096 && cd_head_ * 2 > cd_queue_.size()) {
        cd_queue_.erase(cd_queue_.begin(), cd_queue_.begin() + static_cast<std::ptrdiff_t>(cd_head_));
        cd_head_ = 0;
    }
    if (trigger_head_ > 256 && trigger_head_ * 2 > trigger_queue_.size()) {
        trigger_queue_.erase(trigger_queue_.begin(), trigger_queue_.begin() + static_cast<std::ptrdiff_t>(trigger_head_));
        trigger_head_ = 0;
    }
}

void RawFileWriter::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_.is_open())
        return;
    encode_until_locked(std::numeric_limits<timestamp>::max());
    buffer_.clear();
    encoder_->flush(buffer_);
    file_.write(reinterpret_cast<const char *>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
    bytes_ += buffer_.size();
    file_.close();
}

bool RawFileWriter::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_.is_open();
}

uint64_t RawFileWriter::events_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

uint64_t RawFileWriter::triggers_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return triggers_;
}

uint64_t RawFileWriter::bytes_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

} // namespace evs
