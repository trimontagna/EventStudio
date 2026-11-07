#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/evt_encoder.h"
#include "core/filters.h"
#include "core/mask_set.h"
#include "core/rate_meter.h"
#include "core/types.h"

namespace Metavision {
class Camera;
}

namespace evs {

struct BiasInfo {
    std::string name;
    int value = 0;
    int min = 0, max = 0;         // allowed range
    int rec_min = 0, rec_max = 0; // recommended range
    std::string description;
    bool modifiable = true;
};

struct ErcInfo {
    bool available = false, enabled = false;
    uint32_t rate = 0, min_rate = 0, max_rate = 0; // events / s
    uint32_t count_period_us = 0;
};

struct TrailInfo {
    bool available = false, enabled = false;
    std::set<TrailMode> modes;
    TrailMode mode     = TrailMode::StcCutTrail;
    uint32_t threshold = 0, min_threshold = 0, max_threshold = 0; // us
};

struct RoiInfo {
    bool available     = false;
    size_t max_windows = 0;
};

struct TriggerInInfo {
    bool available = false;
    // (channel name "Main" / "Aux" / "Loopback", channel id found in the trigger events, enabled)
    std::vector<std::tuple<std::string, short, bool>> channels;
};

struct DigitalMaskInfo {
    bool available  = false;
    size_t capacity = 0; // number of pixel mask slots (64 on IMX636)
};

// A live Prophesee camera driven through the OpenEB Camera API.
class LiveCamera {
public:
    using EventsCallback   = std::function<void(const EventCD *, const EventCD *)>;
    using TriggersCallback = std::function<void(const EventExtTrigger *, const EventExtTrigger *)>;

    LiveCamera();
    ~LiveCamera();

    static std::vector<std::string> list_cameras();

    // Empty serial: first available camera. The stream format is selected at opening (EVT3 / EVT21 on IMX636).
    bool open(const std::string &serial, EvtFormat format, std::string *error);
    void close();
    bool is_open() const;

    // Must be set before start(); runs on the camera decoding thread.
    void set_events_callback(EventsCallback cb) { events_cb_ = std::move(cb); }
    void set_triggers_callback(TriggersCallback cb) { triggers_cb_ = std::move(cb); }
    bool start(std::string *error);
    void stop();
    bool is_running() const;

    int width() const { return width_; }
    int height() const { return height_; }
    const std::string &serial() const { return serial_; }
    const std::string &sensor() const { return sensor_; }
    EvtFormat format() const { return format_; }
    const std::string &format_string() const { return format_string_; }
    // Camera description (RAW header fields), used for processed recordings.
    const std::map<std::string, std::string> &metadata() const { return metadata_; }
    std::vector<EvtFormat> supported_formats() const { return supported_formats_; }

    // Biases (I_LL_Biases)
    std::vector<BiasInfo> biases() const;
    bool set_bias(const std::string &name, int value, std::string *error);
    bool save_biases(const std::filesystem::path &path, std::string *error) const;
    bool load_biases(const std::filesystem::path &path, std::string *error);

    // Event Rate Controller (I_ErcModule)
    ErcInfo erc() const;
    bool set_erc(bool enabled, uint32_t rate, std::string *error);

    // Event Trail Filter (I_EventTrailFilterModule)
    TrailInfo trail() const;
    bool set_trail(bool enabled, TrailMode mode, uint32_t threshold_us, std::string *error);

    // Digital Event Mask (I_DigitalEventMask): masks the given pixels, unmasks the remaining slots.
    DigitalMaskInfo digital_mask() const;
    bool apply_digital_mask(const std::vector<std::pair<uint32_t, uint32_t>> &pixels, std::string *error);

    // Region of interest (I_ROI): a single rectangle, ROI or RONI. A disabled / invalid rect disables the ROI.
    RoiInfo roi_info() const;
    bool apply_roi(const RoiRect &roi, std::string *error);

    // External trigger inputs (I_TriggerIn)
    TriggerInInfo trigger_in() const;
    bool set_trigger_in(const std::string &channel, bool enabled, std::string *error);

    // Exact sensor stream (RAW file in the format chosen at open()).
    bool start_raw_recording(const std::filesystem::path &path, std::string *error);
    void stop_raw_recording();
    bool is_raw_recording() const { return raw_recording_; }

    // Statistics, wall clock based.
    double data_rate_bytes() const;     // bytes / s received from the camera
    double event_rate() const;          // CD events / s decoded
    // ERC counters (if the sensor sends them): events / s entering and leaving the ERC. -1 when unknown.
    double erc_input_rate() const;
    double erc_output_rate() const;
    std::string take_runtime_error();

    static timestamp wall_us();

private:
    std::unique_ptr<Metavision::Camera> camera_;
    EventsCallback events_cb_;
    TriggersCallback triggers_cb_;
    int width_ = kDefaultWidth, height_ = kDefaultHeight;
    std::string serial_, sensor_, format_string_;
    EvtFormat format_ = EvtFormat::Evt3;
    std::vector<EvtFormat> supported_formats_;
    std::map<std::string, std::string> metadata_;
    std::atomic<bool> raw_recording_{false};
    std::filesystem::path raw_recording_path_;

    RateMeter bytes_meter_, events_meter_, erc_in_meter_, erc_out_meter_;
    std::atomic<bool> erc_counters_seen_{false};
    mutable std::mutex error_mutex_;
    std::string runtime_error_;
};

} // namespace evs
