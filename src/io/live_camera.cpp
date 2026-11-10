#include "io/live_camera.h"

#include <chrono>

#include <metavision/hal/facilities/i_digital_event_mask.h>
#include <metavision/hal/facilities/i_erc_module.h>
#include <metavision/hal/facilities/i_event_trail_filter_module.h>
#include <metavision/hal/facilities/i_hw_identification.h>
#include <metavision/hal/facilities/i_ll_biases.h>
#include <metavision/hal/facilities/i_roi.h>
#include <metavision/hal/facilities/i_trigger_in.h>
#include <metavision/hal/utils/device_config.h>
#include <metavision/sdk/stream/camera.h>

namespace evs {

namespace {

Metavision::I_EventTrailFilterModule::Type to_hal(TrailMode mode) {
    switch (mode) {
    case TrailMode::Trail:
        return Metavision::I_EventTrailFilterModule::Type::TRAIL;
    case TrailMode::StcKeepTrail:
        return Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL;
    default:
        return Metavision::I_EventTrailFilterModule::Type::STC_CUT_TRAIL;
    }
}

TrailMode from_hal(Metavision::I_EventTrailFilterModule::Type type) {
    switch (type) {
    case Metavision::I_EventTrailFilterModule::Type::TRAIL:
        return TrailMode::Trail;
    case Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL:
        return TrailMode::StcKeepTrail;
    default:
        return TrailMode::StcCutTrail;
    }
}

template<typename F>
bool guarded(std::string *error, F &&f) {
    try {
        return f();
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace

LiveCamera::LiveCamera() = default;

LiveCamera::~LiveCamera() {
    close();
}

timestamp LiveCamera::wall_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t LiveCamera::unix_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<std::string> LiveCamera::list_cameras() {
    std::vector<std::string> serials;
    try {
        for (const auto &[type, list] : Metavision::Camera::list_online_sources())
            serials.insert(serials.end(), list.begin(), list.end());
    } catch (...) {}
    return serials;
}

bool LiveCamera::open(const std::string &serial, EvtFormat format, std::string *error) {
    close();
    return guarded(error, [&] {
        Metavision::DeviceConfig config;
        config.set_format(to_string(format));
        // Without this OpenEB rejects bias values outside the *recommended* range; with it the whole range the
        // manufacturer allows for this sensor (LL_Bias_Info::get_bias_allowed_range) can be set.
        config.enable_biases_range_check_bypass(true);
        camera_ = std::make_unique<Metavision::Camera>(serial.empty() ? Metavision::Camera::from_first_available(config) :
                                                                        Metavision::Camera::from_serial(serial, config));
        width_  = camera_->geometry().get_width();
        height_ = camera_->geometry().get_height();

        auto &device = camera_->get_device();
        supported_formats_.clear();
        if (auto *hw_id = device.get_facility<Metavision::I_HW_Identification>()) {
            serial_        = hw_id->get_serial();
            sensor_        = hw_id->get_sensor_info().name_;
            format_string_ = hw_id->get_current_data_encoding_format();
            for (const auto &f : hw_id->get_available_data_encoding_formats()) {
                const std::string name = f.substr(0, f.find(';'));
                if (name == "EVT2")
                    supported_formats_.push_back(EvtFormat::Evt2);
                else if (name == "EVT21")
                    supported_formats_.push_back(EvtFormat::Evt21);
                else if (name == "EVT3")
                    supported_formats_.push_back(EvtFormat::Evt3);
            }
            metadata_.clear();
            for (const auto &[k, v] : hw_id->get_header().get_header_map())
                metadata_[k] = v;
        }
        const std::string current = format_string_.substr(0, format_string_.find(';'));
        format_ = current == "EVT21" ? EvtFormat::Evt21 : current == "EVT2" ? EvtFormat::Evt2 : EvtFormat::Evt3;
        if (current.empty())
            format_ = format;

        camera_->add_runtime_error_callback([this](const Metavision::CameraException &e) {
            std::lock_guard<std::mutex> lock(error_mutex_);
            runtime_error_ = e.what();
        });
        camera_->cd().add_callback([this](const EventCD *b, const EventCD *e) {
            events_meter_.add(static_cast<double>(e - b), wall_us());
            if (events_cb_)
                events_cb_(b, e);
        });
        camera_->raw_data().add_callback(
            [this](const std::uint8_t *, size_t size) {
                // Called right after the buffer was decoded: the last decoded timestamp is the camera time at the
                // end of the buffer (it advances with the time-high words even when no event is sent).
                const timestamp now = wall_us();
                bytes_meter_.add(static_cast<double>(size), now);
                latency_.add(camera_->get_last_timestamp(), now, unix_us());
            });
        try {
            camera_->ext_trigger().add_callback([this](const EventExtTrigger *b, const EventExtTrigger *e) {
                if (triggers_cb_ && b != e)
                    triggers_cb_(b, e);
            });
        } catch (...) {} // no trigger events for this device
        try {
            camera_->erc_counter().add_callback(
                [this](const Metavision::EventERCCounter *b, const Metavision::EventERCCounter *e) {
                    const timestamp now = wall_us();
                    for (auto it = b; it != e; ++it)
                        (it->is_output ? erc_out_meter_ : erc_in_meter_).add(static_cast<double>(it->event_count), now);
                    erc_counters_seen_ = true;
                });
        } catch (...) {} // no ERC counter events for this device
        bytes_meter_.reset();
        events_meter_.reset();
        erc_in_meter_.reset();
        erc_out_meter_.reset();
        erc_counters_seen_ = false;
        latency_.reset();
        return true;
    });
}

void LiveCamera::close() {
    if (!camera_)
        return;
    try {
        stop_raw_recording();
        if (camera_->is_running())
            camera_->stop();
    } catch (...) {}
    camera_.reset();
}

bool LiveCamera::is_open() const {
    return camera_ != nullptr;
}

bool LiveCamera::start(std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        if (camera_->is_running())
            return true;
        latency_.reset(); // the time base restarts with the stream
        return camera_->start();
    });
}

void LiveCamera::stop() {
    if (!camera_)
        return;
    try {
        stop_raw_recording();
        if (camera_->is_running())
            camera_->stop();
    } catch (...) {}
}

bool LiveCamera::is_running() const {
    return camera_ && const_cast<Metavision::Camera &>(*camera_).is_running();
}

// ------------------------------------------------------------------------------------------------ biases

std::vector<BiasInfo> LiveCamera::biases() const {
    std::vector<BiasInfo> result;
    if (!camera_)
        return result;
    try {
        auto *biases = camera_->get_device().get_facility<Metavision::I_LL_Biases>();
        if (!biases)
            return result;
        for (const auto &[name, value] : biases->get_all_biases()) {
            BiasInfo info;
            info.name  = name;
            info.value = value;
            Metavision::LL_Bias_Info hal_info;
            if (biases->get_bias_info(name, hal_info)) {
                std::tie(info.min, info.max)         = hal_info.get_bias_allowed_range();
                std::tie(info.rec_min, info.rec_max) = hal_info.get_bias_recommended_range();
                info.description                     = hal_info.get_description();
                info.modifiable                      = hal_info.is_modifiable();
            }
            result.push_back(info);
        }
    } catch (...) {}
    return result;
}

bool LiveCamera::set_bias(const std::string &name, int value, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *biases = camera_->get_device().get_facility<Metavision::I_LL_Biases>();
        if (!biases) {
            if (error)
                *error = "This camera has no bias facility";
            return false;
        }
        if (!biases->set(name, value)) {
            if (error)
                *error = "Bias " + name + " rejected value " + std::to_string(value);
            return false;
        }
        return true;
    });
}

bool LiveCamera::save_biases(const std::filesystem::path &path, std::string *error) const {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *biases = camera_->get_device().get_facility<Metavision::I_LL_Biases>();
        if (!biases)
            return false;
        biases->save_to_file(path);
        return true;
    });
}

bool LiveCamera::load_biases(const std::filesystem::path &path, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *biases = camera_->get_device().get_facility<Metavision::I_LL_Biases>();
        if (!biases)
            return false;
        biases->load_from_file(path);
        return true;
    });
}

// ------------------------------------------------------------------------------------------------ ERC

ErcInfo LiveCamera::erc() const {
    ErcInfo info;
    if (!camera_)
        return info;
    try {
        auto *erc = camera_->get_device().get_facility<Metavision::I_ErcModule>();
        if (!erc)
            return info;
        info.available       = true;
        info.enabled         = erc->is_enabled();
        info.rate            = erc->get_cd_event_rate();
        info.min_rate        = erc->get_min_supported_cd_event_rate();
        info.max_rate        = erc->get_max_supported_cd_event_rate();
        info.count_period_us = erc->get_count_period();
    } catch (...) {}
    return info;
}

bool LiveCamera::set_erc(bool enabled, uint32_t rate, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *erc = camera_->get_device().get_facility<Metavision::I_ErcModule>();
        if (!erc) {
            if (error)
                *error = "This camera has no Event Rate Controller";
            return false;
        }
        bool ok = true;
        if (enabled)
            ok = erc->set_cd_event_rate(rate);
        return erc->enable(enabled) && ok;
    });
}

// ------------------------------------------------------------------------------------------------ trail filter

TrailInfo LiveCamera::trail() const {
    TrailInfo info;
    if (!camera_)
        return info;
    try {
        auto *trail = camera_->get_device().get_facility<Metavision::I_EventTrailFilterModule>();
        if (!trail)
            return info;
        info.available = true;
        info.enabled   = trail->is_enabled();
        for (auto t : trail->get_available_types())
            info.modes.insert(from_hal(t));
        info.mode          = from_hal(trail->get_type());
        info.threshold     = trail->get_threshold();
        info.min_threshold = trail->get_min_supported_threshold();
        info.max_threshold = trail->get_max_supported_threshold();
    } catch (...) {}
    return info;
}

bool LiveCamera::set_trail(bool enabled, TrailMode mode, uint32_t threshold_us, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *trail = camera_->get_device().get_facility<Metavision::I_EventTrailFilterModule>();
        if (!trail) {
            if (error)
                *error = "This camera has no Event Trail Filter";
            return false;
        }
        const uint32_t clamped =
            std::clamp(threshold_us, trail->get_min_supported_threshold(), trail->get_max_supported_threshold());
        bool ok = trail->set_type(to_hal(mode));
        ok      = trail->set_threshold(clamped) && ok;
        ok      = trail->enable(enabled) && ok;
        return ok;
    });
}

// ------------------------------------------------------------------------------------------------ digital mask

DigitalMaskInfo LiveCamera::digital_mask() const {
    DigitalMaskInfo info;
    if (!camera_)
        return info;
    try {
        auto *dem = camera_->get_device().get_facility<Metavision::I_DigitalEventMask>();
        if (!dem)
            return info;
        info.available = true;
        info.capacity  = dem->get_pixel_masks().size();
    } catch (...) {}
    return info;
}

bool LiveCamera::apply_digital_mask(const std::vector<std::pair<uint32_t, uint32_t>> &pixels, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *dem = camera_->get_device().get_facility<Metavision::I_DigitalEventMask>();
        if (!dem) {
            if (error)
                *error = "This camera has no Digital Event Mask";
            return false;
        }
        const auto &masks = dem->get_pixel_masks();
        if (pixels.size() > masks.size()) {
            if (error)
                *error = std::to_string(pixels.size()) + " pixels to mask, the sensor supports " +
                         std::to_string(masks.size());
            return false;
        }
        bool ok = true;
        for (size_t i = 0; i < masks.size(); ++i) {
            if (i < pixels.size())
                ok = masks[i]->set_mask(pixels[i].first, pixels[i].second, true) && ok;
            else
                ok = masks[i]->set_mask(0, 0, false) && ok;
        }
        return ok;
    });
}

// ------------------------------------------------------------------------------------------------ ROI

RoiInfo LiveCamera::roi_info() const {
    RoiInfo info;
    if (!camera_)
        return info;
    try {
        if (auto *roi = camera_->get_device().get_facility<Metavision::I_ROI>()) {
            info.available   = true;
            info.max_windows = roi->get_max_supported_windows_count();
        }
    } catch (...) {}
    return info;
}

bool LiveCamera::apply_roi(const RoiRect &rect, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *roi = camera_->get_device().get_facility<Metavision::I_ROI>();
        if (!roi) {
            if (error)
                *error = "This camera has no ROI facility";
            return false;
        }
        if (!rect.enabled || !rect.valid())
            return roi->enable(false);
        // Same order as metavision_viewer: mode, window, enable (enable() also latches the ROI / RONI selection).
        bool ok = roi->set_mode(rect.mode == RoiMode::Roi ? Metavision::I_ROI::Mode::ROI : Metavision::I_ROI::Mode::RONI);
        ok      = roi->set_window(Metavision::I_ROI::Window(rect.x, rect.y, rect.width, rect.height)) && ok;
        ok      = roi->enable(true) && ok;
        return ok;
    });
}

// ------------------------------------------------------------------------------------------------ trigger in

namespace {
const char *channel_name(Metavision::I_TriggerIn::Channel c) {
    switch (c) {
    case Metavision::I_TriggerIn::Channel::Main:
        return "Main";
    case Metavision::I_TriggerIn::Channel::Aux:
        return "Aux";
    default:
        return "Loopback";
    }
}
} // namespace

TriggerInInfo LiveCamera::trigger_in() const {
    TriggerInInfo info;
    if (!camera_)
        return info;
    try {
        if (auto *tin = camera_->get_device().get_facility<Metavision::I_TriggerIn>()) {
            info.available = true;
            for (const auto &[channel, id] : tin->get_available_channels())
                info.channels.emplace_back(channel_name(channel), id, tin->is_enabled(channel));
        }
    } catch (...) {}
    return info;
}

bool LiveCamera::set_trigger_in(const std::string &channel, bool enabled, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        auto *tin = camera_->get_device().get_facility<Metavision::I_TriggerIn>();
        if (!tin)
            return false;
        for (const auto &[c, id] : tin->get_available_channels())
            if (channel == channel_name(c))
                return enabled ? tin->enable(c) : tin->disable(c);
        return false;
    });
}

// ------------------------------------------------------------------------------------------------ recording

bool LiveCamera::start_raw_recording(const std::filesystem::path &path, std::string *error) {
    if (!camera_)
        return false;
    return guarded(error, [&] {
        if (!camera_->start_recording(path)) {
            if (error)
                *error = "Cannot record to " + path.string();
            return false;
        }
        raw_recording_path_ = path;
        raw_recording_      = true;
        return true;
    });
}

void LiveCamera::stop_raw_recording() {
    if (!camera_ || !raw_recording_)
        return;
    try {
        camera_->stop_recording(raw_recording_path_);
    } catch (...) {}
    raw_recording_ = false;
}

// ------------------------------------------------------------------------------------------------ statistics

double LiveCamera::data_rate_bytes() const {
    return bytes_meter_.rate_per_second(wall_us());
}

double LiveCamera::event_rate() const {
    return events_meter_.rate_per_second(wall_us());
}

double LiveCamera::erc_input_rate() const {
    return erc_counters_seen_ ? erc_in_meter_.rate_per_second(wall_us()) : -1.0;
}

double LiveCamera::erc_output_rate() const {
    return erc_counters_seen_ ? erc_out_meter_.rate_per_second(wall_us()) : -1.0;
}

std::string LiveCamera::take_runtime_error() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    std::string e;
    std::swap(e, runtime_error_);
    return e;
}

} // namespace evs
