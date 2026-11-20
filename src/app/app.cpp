#include "app/app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/platform.h"
#include "io/event_file_source.h"

#include <metavision/sdk/stream/event_file_reader.h>
#include "io/sample_launcher.h"

namespace evs::app {

App::App() {
    refresh_camera_list();
    samples_dir_ = find_samples_dir();
    {
        const auto rec = default_output_path("recording", ".raw");
        record_dir_    = to_utf8(rec.parent_path());
        record_name_   = to_utf8(rec.stem());
    }
    export_path_ = to_utf8(default_output_path("export", ".raw"));
    prepare_engine(kDefaultWidth, kDefaultHeight);
    load_app_settings();
    log("EventStudio ready. " + std::to_string(camera_list_.size()) + " camera(s) detected." +
        (samples_dir_.empty() ? " OpenEB samples not found." : " OpenEB samples: " + to_utf8(samples_dir_)));
}

App::~App() {
    shutdown();
}

void App::shutdown() {
    if (export_progress_)
        export_progress_->cancel = true;
    if (export_thread_.joinable())
        export_thread_.join();
    cancel_hot_pixel_detection();
    close_source();
}

void App::log(const std::string &message, bool error) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char t[16];
    std::strftime(t, sizeof(t), "%H:%M:%S", &tm);
    log_.push_back({t, message, error});
    if (log_.size() > 2000)
        log_.erase(log_.begin(), log_.begin() + 500);
    status_       = message;
    status_error_ = error;
}

// ================================================================================================ sources

void App::refresh_camera_list() {
    camera_list_  = LiveCamera::list_cameras();
    camera_index_ = std::clamp(camera_index_, 0, std::max(0, static_cast<int>(camera_list_.size()) - 1));
}

void App::prepare_engine(int width, int height) {
    width_    = width;
    height_   = height;
    pipeline_ = std::make_unique<FilterPipeline>(width, height);
    renderer_ = std::make_unique<FrameRenderer>(width, height);
    pipeline_->set_config(filters_);
    renderer_->set_settings(preview_);
    in_meter_.reset();
    out_meter_.reset();
    file_bytes_meter_.reset();
    event_history_.reset();
    last_event_t_    = 0;
    last_event_wall_ = 0;
    total_events_    = 0;
    total_triggers_  = 0;
    editor_.set_sensor_size(width, height);
    masks_.roi().clamp(width, height);
    masks_changed(false);
}

void App::on_events(const EventCD *begin, const EventCD *end) {
    if (begin == end)
        return;
    const timestamp t = (end - 1)->t;
    in_meter_.add(static_cast<double>(end - begin), t);
    event_history_.add(static_cast<double>(end - begin), source_ == SourceKind::Camera ? LiveCamera::wall_us() : t);
    if (hp_state_ == HotPixelState::Collecting) {
        // live hot pixel measurement: raw input, before the software filters
        std::lock_guard<std::mutex> lock(hp_mutex_);
        if (hp_activity_) {
            if (hp_start_t_ < 0)
                hp_start_t_ = begin->t;
            hp_activity_->add(begin, end);
            hp_progress_ = static_cast<float>(std::min(1.0, double(t - hp_start_t_) / std::max<timestamp>(1, hp_duration_us_)));
            if (t - hp_start_t_ >= hp_duration_us_)
                hp_state_ = HotPixelState::Finished;
        }
    }
    const auto &out = pipeline_->process(begin, end);
    out_meter_.add(static_cast<double>(out.size()), t);
    if (!out.empty()) {
        if (!freeze_)
            renderer_->ingest(out.data(), out.data() + out.size());
        if (processed_recording_)
            processed_writer_.write(out.data(), out.data() + out.size());
    }
    if (processed_recording_) {
        // Triggers of the same decoded buffer may arrive after these events: keep a margin before encoding.
        processed_writer_.advance(t - 20'000 - pipeline_->holding_time());
    }
    if (source_ == SourceKind::File) {
        const double speed = player_.speed();
        if (speed > 0 && player_.is_playing())
            playback_latency_.add(static_cast<timestamp>(std::llround(t / speed)), LiveCamera::wall_us(),
                                  LiveCamera::unix_us());
    }
    total_events_ += static_cast<uint64_t>(end - begin);
    last_event_t_    = t;
    last_event_wall_ = LiveCamera::wall_us();
}

void App::on_triggers(const EventExtTrigger *begin, const EventExtTrigger *end) {
    total_triggers_ += static_cast<uint64_t>(end - begin);
    if (processed_recording_)
        processed_writer_.write_triggers(begin, end);
}

void App::on_seek(timestamp t) {
    playback_latency_.reset();
    pipeline_->reset();
    renderer_->reset();
    in_meter_.reset();
    out_meter_.reset();
    file_bytes_meter_.reset();
    event_history_.reset();
    last_event_t_ = t;
}

timestamp App::preview_time() const {
    const timestamp last = renderer_->last_timestamp();
    if (source_ == SourceKind::File && player_.target_reached())
        return std::max(last, player_.position()); // paused on a seek / step: the view at the target exactly
    if (source_ == SourceKind::Camera && last_event_wall_ > 0 && !freeze_) {
        // Keep the live view moving (and fading) when the sensor is quiet.
        const timestamp idle = LiveCamera::wall_us() - last_event_wall_;
        return last + std::clamp<timestamp>(idle, 0, 1'000'000);
    }
    return last;
}

void App::open_camera() {
    close_source();
    refresh_camera_list();
    open_camera_serial(camera_list_.empty() ? std::string() : camera_list_[camera_index_]);
}

void App::open_camera_serial(const std::string &serial) {
    close_source();
    camera_.set_events_callback([this](const EventCD *b, const EventCD *e) { on_events(b, e); });
    camera_.set_triggers_callback([this](const EventExtTrigger *b, const EventExtTrigger *e) { on_triggers(b, e); });
    std::string error;
    if (!camera_.open(serial, camera_format_, &error)) {
        log("Cannot open camera: " + error, true);
        return;
    }
    prepare_engine(camera_.width(), camera_.height());
    source_        = SourceKind::Camera;
    camera_format_ = camera_.format();
    if (camera_format_ != EvtFormat::Evt2)
        processed_format_ = camera_format_;
    read_camera_state();
    biases_at_open_ = biases_; // what the camera came up with, before the saved settings are restored
    restore_camera_settings();
    masks_changed(false);
    apply_hardware_masks();
    log("Opened camera " + camera_.serial() + " (" + camera_.sensor() + ", " + std::to_string(width_) + "x" +
        std::to_string(height_) + ", " + camera_.format_string() + ")");
    start_camera();
}

void App::reset_camera() {
    if (source_ != SourceKind::Camera)
        return;
    const std::string serial = camera_.serial();
    if (recording())
        log("Recording stopped by the camera reset", true);
    save_camera_settings(); // current settings, restored after reopening
    log("Resetting camera " + serial + " (discarding the buffered data)...");
    const auto t0 = std::chrono::steady_clock::now();
    open_camera_serial(serial);
    if (source_ == SourceKind::Camera) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Camera reset in %.1f s",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        log(buf);
    }
}

// ------------------------------------------------------------------------------------------------ saved settings

std::filesystem::path App::camera_settings_path() const {
    return app_data_dir() / "cameras" / camera_settings_file_name(camera_.serial());
}

CameraSettings App::current_camera_settings() const {
    CameraSettings s;
    for (const auto &b : biases_)
        if (b.modifiable)
            s.biases[b.name] = b.value;
    if (erc_.available) {
        s.has_erc     = true;
        s.erc_enabled = erc_.enabled;
        s.erc_rate    = erc_.rate;
    }
    if (trail_.available) {
        s.has_trail          = true;
        s.trail_enabled      = trail_.enabled;
        s.trail_mode         = trail_.mode;
        s.trail_threshold_us = trail_.threshold;
    }
    if (trigger_in_.available)
        for (const auto &[name, id, enabled] : trigger_in_.channels)
            s.trigger_in[name] = enabled;
    return s;
}

void App::apply_camera_settings(const CameraSettings &s) {
    std::string error;
    for (const auto &b : biases_) {
        const auto it = s.biases.find(b.name);
        if (it == s.biases.end() || !b.modifiable || it->second == b.value)
            continue;
        if (!camera_.set_bias(b.name, it->second, &error))
            log("Restoring " + b.name + ": " + error, true);
    }
    if (s.has_erc && erc_.available) {
        const uint32_t rate = std::clamp(s.erc_rate, erc_.min_rate, std::max(erc_.min_rate, erc_.max_rate));
        if (!camera_.set_erc(s.erc_enabled, rate, &error))
            log("Restoring the ERC: " + error, true);
    }
    if (s.has_trail && trail_.available) {
        const TrailMode mode = trail_.modes.count(s.trail_mode) ? s.trail_mode : trail_.mode;
        const uint32_t threshold =
            std::clamp(s.trail_threshold_us, trail_.min_threshold, std::max(trail_.min_threshold, trail_.max_threshold));
        if (!camera_.set_trail(s.trail_enabled, mode, threshold, &error))
            log("Restoring the trail filter: " + error, true);
    }
    if (trigger_in_.available)
        for (const auto &[name, id, enabled] : trigger_in_.channels) {
            const auto it = s.trigger_in.find(name);
            if (it != s.trigger_in.end() && it->second != enabled && !camera_.set_trigger_in(name, it->second, &error))
                log("Restoring trigger input " + name + ": " + error, true);
        }
    read_camera_state();
}

void App::restore_camera_settings() {
    const auto path = camera_settings_path();
    KeyValueFile file;
    if (file.load(path)) {
        apply_camera_settings(CameraSettings::from_file(file));
        log("Restored the settings last given to camera " + camera_.serial() + " (" + to_utf8(path) + ")");
        camera_settings_dirty_ = false;
        return;
    }
    // First time this camera is seen on this PC: defaults.
    if (erc_.available) {
        constexpr uint32_t kDefaultErcRate = 40'000'000;
        const uint32_t rate = std::clamp(kDefaultErcRate, erc_.min_rate, std::max(erc_.min_rate, erc_.max_rate));
        std::string error;
        if (camera_.set_erc(true, rate, &error))
            log("Event Rate Controller enabled at " + human_rate(rate, "ev") + " (default)");
        else
            log("ERC: " + error, true);
        read_camera_state();
    }
    save_camera_settings();
}

void App::camera_settings_changed() {
    camera_settings_dirty_      = true;
    camera_settings_changed_at_ = ImGui::GetTime();
}

void App::save_camera_settings() {
    camera_settings_dirty_ = false;
    if (source_ != SourceKind::Camera && !camera_.is_open())
        return;
    std::string error;
    if (!current_camera_settings().to_file(camera_.serial()).save(camera_settings_path(), &error))
        log("Cannot save the camera settings: " + error, true);
}

void App::load_app_settings() {
    KeyValueFile file;
    if (!file.load(app_data_dir() / "settings.ini"))
        return;
    if (const auto last = file.get("last_recording"); last && !last->empty())
        last_recording_ = from_utf8(*last);
    if (const auto dir = file.get("record_folder"); dir && !dir->empty())
        record_dir_ = *dir;
    record_timed_      = file.get_bool("record_timed", record_timed_);
    record_duration_s_ = std::max(0.1f, file.get_int("record_duration_ms", 10'000) / 1000.f);
    step_us_           = std::max<timestamp>(1, file.get_int("step_us", step_us_));
}

void App::save_app_settings() {
    KeyValueFile file;
    file.header = "EventStudio settings (local to this PC)";
    file.set("last_recording", to_utf8(last_recording_));
    file.set("record_folder", record_dir_);
    file.set_bool("record_timed", record_timed_);
    file.set_int("record_duration_ms", std::llround(record_duration_s_ * 1000.0));
    file.set_int("step_us", step_us_);
    std::string error;
    if (!file.save(app_data_dir() / "settings.ini", &error))
        log("Cannot save the settings: " + error, true);
}

void App::reopen_camera_with_format(EvtFormat format) {
    if (recording()) {
        log("Stop the recording before changing the stream format", true);
        return;
    }
    camera_format_ = format;
    if (source_ == SourceKind::Camera)
        open_camera();
}

void App::read_camera_state() {
    biases_ = camera_.biases();
    erc_    = camera_.erc();
    trail_  = camera_.trail();
    dem_        = camera_.digital_mask();
    roi_info_   = camera_.roi_info();
    trigger_in_ = camera_.trigger_in();
}

void App::start_camera() {
    std::string error;
    if (!camera_.start(&error))
        log("Cannot start streaming: " + error, true);
}

void App::stop_camera() {
    if (recording())
        stop_recording();
    camera_.stop();
}

void App::close_source() {
    if (recording())
        stop_recording();
    if (source_ == SourceKind::Camera) {
        if (camera_settings_dirty_)
            save_camera_settings();
        camera_.close();
    }
    else if (source_ == SourceKind::File)
        player_.close();
    source_ = SourceKind::None;
    file_info_.clear();
    dem_      = DigitalMaskInfo();
    roi_info_ = RoiInfo();
    hw_roi_status_.clear();
    hw_pixel_status_.clear();
}

void App::open_file(const std::filesystem::path &path) {
    close_source();
    player_.set_events_callback([this](const EventCD *b, const EventCD *e) { on_events(b, e); });
    player_.set_triggers_callback([this](const EventExtTrigger *b, const EventExtTrigger *e) { on_triggers(b, e); });
    player_.set_seek_callback([this](timestamp t) { on_seek(t); });
    playback_latency_.reset();
    player_.set_raw_bytes_callback(
        [this](size_t n) { file_bytes_meter_.add(static_cast<double>(n), last_event_t_.load()); });
    std::string error;
    if (!player_.open(path, &error)) {
        log("Cannot open " + to_utf8(path) + ": " + error, true);
        return;
    }
    prepare_engine(player_.width(), player_.height());
    source_ = SourceKind::File;
    player_.set_speed(playback_speed_);
    file_info_ = player_.encoding() + ", " + std::to_string(width_) + "x" + std::to_string(height_);
    const auto md = player_.metadata();
    for (const char *key : {"sensor_name", "sensor_generation", "serial_number", "date"}) {
        auto it = md.find(key);
        if (it != md.end() && !it->second.empty())
            file_info_ += std::string("\n") + key + ": " + it->second;
    }
    auto stem    = path.stem().string();
    export_path_ = to_utf8(path.parent_path() /
                           (stem + "_processed" +
                            (export_format_ == ExportFormat::Images ? ".raw" : default_extension(export_format_))));
    export_images_location_  = to_utf8(path.parent_path());
    export_images_name_auto_ = true;
    log("Opened " + to_utf8(path) + " (" + player_.encoding() + ")");
    player_.play();
}

bool App::load_masks(const std::filesystem::path &path) {
    std::string error;
    editor_.snapshot();
    if (!masks_.load(path, &error)) {
        log(error, true);
        return false;
    }
    masks_.roi().clamp(width_, height_);
    editor_.select(-1);
    masks_changed(true);
    log("Masks loaded from " + to_utf8(path));
    return true;
}

// ================================================================================================ recording

bool App::recording() const {
    return raw_recording_ || processed_recording_;
}

void App::start_recording() {
    if (recording() || source_ == SourceKind::None)
        return;
    // The name box keeps the name: the next recordings of a series get _1, _2... (see record_target()).
    std::filesystem::path path = unique_path(record_target());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::string error;

    if (record_mode_ == RecordMode::Raw) {
        if (source_ != SourceKind::Camera) {
            log("Raw recording needs a live camera (use Processed mode or Export for files)", true);
            return;
        }
        if (!camera_.start_raw_recording(path, &error)) {
            log("Cannot start recording: " + error, true);
            return;
        }
        raw_recording_ = true;
    } else {
        const auto metadata = source_ == SourceKind::Camera ? camera_.metadata() : player_.metadata();
        if (!processed_writer_.open(path, processed_format_, width_, height_, metadata, &error)) {
            log("Cannot start recording: " + error, true);
            return;
        }
        processed_recording_ = true;
    }
    record_start_   = std::chrono::steady_clock::now();
    if (source_ == SourceKind::Camera)
        open_latency_log(path);
    last_recording_ = path;
    save_app_settings(); // the folder, the timed recording settings
    log(std::string("Recording ") + (record_mode_ == RecordMode::Raw ? "sensor stream" : "processed stream") + " to " +
        to_utf8(path));
}

void App::stop_recording() {
    if (raw_recording_) {
        camera_.stop_raw_recording();
        raw_recording_ = false;
    }
    if (processed_recording_) {
        processed_recording_ = false;
        processed_writer_.close();
    }
    close_latency_log();
    std::error_code ec;
    const auto size = std::filesystem::file_size(last_recording_, ec);
    log("Recording saved: " + to_utf8(last_recording_) + " (" + human_bytes(static_cast<double>(size)) + ")");
    save_app_settings();
}

double App::recording_elapsed_s() const {
    return recording() ? std::chrono::duration<double>(std::chrono::steady_clock::now() - record_start_).count() : 0;
}

std::filesystem::path App::record_target() const {
    std::string name = record_name_;
    const char *blanks = " \t";
    const auto first   = name.find_first_not_of(blanks);
    const auto last    = name.find_last_not_of(blanks);
    name             = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (name.size() >= 4) {
        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".raw")
            name.resize(name.size() - 4);
    }
    if (name.empty() || record_dir_.empty()) {
        const auto fallback = default_output_path("recording", ".raw");
        return (record_dir_.empty() ? fallback.parent_path() : from_utf8(record_dir_)) /
               (name.empty() ? fallback.filename() : from_utf8(name + ".raw"));
    }
    return from_utf8(record_dir_) / from_utf8(name + ".raw");
}

void App::background_tick() {
    if (record_timed_ && recording() && recording_elapsed_s() >= record_duration_s_) {
        log("Timed recording: stopped after " + format_time_us(static_cast<long long>(recording_elapsed_s() * 1e6)));
        stop_recording();
    }
}

// ================================================================================================ camera -> PC delay

void App::open_latency_log(const std::filesystem::path &recording) {
    close_latency_log();
    auto path = recording;
    path.replace_filename(recording.stem().wstring() + L"_latency.csv");
    latency_log_.open(path, std::ios::out | std::ios::trunc);
    if (!latency_log_) {
        log("Cannot write the delay log " + to_utf8(path), true);
        return;
    }
    latency_log_ << "pc_unix_time_us,pc_elapsed_us,camera_time_us,delay_us\n";
    latency_log_path_     = path;
    latency_log_start_us_ = LiveCamera::wall_us();
    latency_log_rows_     = 0;
    camera_.latency().set_logging(true);
}

void App::close_latency_log() {
    if (!latency_log_.is_open())
        return;
    poll_latency(); // write the last samples
    camera_.latency().set_logging(false);
    latency_log_.close();
    log("Delay log saved: " + to_utf8(latency_log_path_) + " (" + std::to_string(latency_log_rows_) + " buffers)");
}

LatencyMonitor *App::active_latency() {
    if (source_ == SourceKind::Camera && camera_.is_running())
        return &camera_.latency();
    if (source_ == SourceKind::File && player_.is_playing() && !player_.at_end() && player_.speed() > 0)
        return &playback_latency_;
    return nullptr;
}

void App::poll_latency() {
    if (latency_log_.is_open()) {
        for (const auto &smp : camera_.latency().take_log()) {
            latency_log_ << smp.unix_us << ',' << (smp.pc_us - latency_log_start_us_) << ',' << smp.camera_us << ','
                         << smp.delay_us << '\n';
            ++latency_log_rows_;
        }
    }
    if (source_ == SourceKind::File) {
        // Pause / play and speed changes restart the pacing: new reference.
        const bool playing = player_.is_playing();
        const double speed = player_.speed();
        if (playing != playback_was_playing_ || speed != playback_speed_seen_)
            playback_latency_.reset();
        playback_was_playing_ = playing;
        playback_speed_seen_  = speed;
    }
    LatencyMonitor *monitor = active_latency();
    if (!monitor) {
        latency_warning_active_ = false;
        return;
    }
    const auto st = monitor->stats(LiveCamera::wall_us());
    if (!st.valid)
        return;
    char buf[200];
    if (latency_warn_enabled_ && !latency_warning_active_ && st.current_ms > latency_threshold_ms()) {
        latency_warning_active_ = true;
        if (source_ == SourceKind::Camera)
            std::snprintf(buf, sizeof(buf),
                          "Stream delay %.0f ms, longer than the %.0f ms accumulation time: the PC is falling behind "
                          "the camera (Source > Reset camera discards the backlog)",
                          st.current_ms, latency_threshold_ms());
        else
            std::snprintf(buf, sizeof(buf),
                          "Playback delay %.0f ms, longer than the %.0f ms accumulation time: decoding / filtering "
                          "cannot keep up with the playback speed",
                          st.current_ms, latency_threshold_ms());
        log(buf, true);
    } else if (latency_warning_active_ && (!latency_warn_enabled_ || st.current_ms < 0.8 * latency_threshold_ms())) {
        latency_warning_active_ = false;
        if (latency_warn_enabled_) {
            std::snprintf(buf, sizeof(buf), "%s delay back to %.0f ms",
                          source_ == SourceKind::Camera ? "Stream" : "Playback", st.current_ms);
            log(buf);
        }
    }
}

// ================================================================================================ masks / filters

void App::masks_changed(bool committed) {
    // Software bitmap: every enabled category. With a camera the sensor already removes the ROI / pixel-masked
    // events (applying them again is harmless); on recordings this emulates them.
    auto bitmap         = std::make_shared<std::vector<uint8_t>>(masks_.build_bitmap(width_, height_, kMaskAll));
    masked_pixel_count_ = static_cast<size_t>(std::count(bitmap->begin(), bitmap->end(), uint8_t(1)));
    mask_bitmap_        = bitmap;
    if (pipeline_)
        pipeline_->set_mask(mask_bitmap_);
    editor_.set_pixel_capacity(dem_.available ? dem_.capacity : 64);
    mask_overlay_dirty_ = true;
    if (committed)
        apply_hardware_masks();
}

void App::apply_hardware_masks() {
    if (source_ != SourceKind::Camera) {
        hw_roi_status_ = hw_pixel_status_ = source_ == SourceKind::File ? "Emulated in software (recording)" : "";
        return;
    }
    std::string error;
    // ---- ROI / RONI rectangle
    if (!roi_info_.available) {
        hw_roi_status_ = "Camera has no ROI: emulated in software";
    } else if (!camera_.apply_roi(masks_.roi(), &error)) {
        hw_roi_status_ = "ROI error: " + error;
        log(hw_roi_status_, true);
    } else {
        const auto &r  = masks_.roi();
        hw_roi_status_ = r.enabled && r.valid() ? std::string("Sensor ") + to_string(r.mode) + " active" : "Sensor ROI disabled";
    }
    // ---- pixel masks
    if (!dem_.available) {
        hw_pixel_status_ = "Camera has no Digital Event Mask: emulated in software";
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> pixels;
    if (masks_.pixels().enabled)
        pixels = masks_.pixels().pixels;
    if (pixels.size() > dem_.capacity)
        pixels.resize(dem_.capacity);
    if (!camera_.apply_digital_mask(pixels, &error)) {
        hw_pixel_status_ = "Pixel mask error: " + error;
        log(hw_pixel_status_, true);
        return;
    }
    hw_pixel_status_ = std::to_string(pixels.size()) + " / " + std::to_string(dem_.capacity) + " pixels masked in the sensor";
}

void App::push_filter_config() {
    pipeline_->set_config(filters_);
}

void App::push_preview_settings() {
    renderer_->set_settings(preview_);
}

// ================================================================================================ export

void App::start_export() {
    if (export_running_ || source_ != SourceKind::File)
        return;
    if (export_thread_.joinable())
        export_thread_.join();
    ExportJob job = make_export_job(true);
    if (job.format == ExportFormat::Images) {
        if (export_images_name_auto_) // exact mask state: the bitmap may turn out empty
            export_images_name_ = to_utf8(
                default_image_folder(job.input, image_export_tag(job, job.mask && !job.mask->empty())).filename());
        job.output = images_output();
        if (export_images_name_.empty()) {
            log("Enter a folder name for the images", true);
            return;
        }
    } else if (job.output.extension().empty()) {
        job.output += default_extension(export_format_);
    }
    std::error_code ec;
    if (std::filesystem::equivalent(job.input, job.output, ec)) {
        log("The export would overwrite the source file", true);
        return;
    }
    export_output_   = job.output;
    export_progress_ = std::make_unique<ExportProgress>();
    {
        std::lock_guard<std::mutex> lock(export_mutex_);
        export_result_.reset();
    }
    export_running_ = true;
    log("Exporting " + to_utf8(job.input.filename()) + " to " + to_utf8(job.output) + " (" + to_string(job.format) +
        (job.apply_filters ? ", software filters applied)" : ", unfiltered)"));
    export_thread_ = std::thread([this, job, progress = export_progress_.get()] {
        auto result = std::make_unique<ExportResult>(run_export(job, progress));
        std::lock_guard<std::mutex> lock(export_mutex_);
        export_result_  = std::move(result);
        export_running_ = false;
    });
}

ExportJob App::make_export_job(bool with_mask) const {
    ExportJob job;
    job.input         = player_.path();
    job.output        = from_utf8(export_path_);
    job.format        = export_format_;
    job.apply_filters = export_apply_filters_;
    job.filters       = filters_;
    if (with_mask)
        job.mask = std::make_shared<std::vector<uint8_t>>(masks_.build_bitmap(width_, height_, export_mask_categories_));
    job.preview   = preview_;
    job.video_fps = export_fps_;
    if (!export_whole_file_) {
        job.start_us = static_cast<timestamp>(export_start_s_ * 1e6);
        job.end_us   = static_cast<timestamp>(export_end_s_ * 1e6);
    }
    return job;
}

bool App::export_masks_anything() const {
    const unsigned c = export_mask_categories_;
    if (c & kMaskSoftware) {
        for (const auto &s : masks_.shapes())
            if (s.enabled)
                return true;
        if (masks_.software_pixels().enabled && !masks_.software_pixels().pixels.empty())
            return true;
    }
    if ((c & kMaskPixels) && masks_.pixels().enabled && !masks_.pixels().pixels.empty())
        return true;
    return (c & kMaskRoi) && masks_.roi().enabled && masks_.roi().valid();
}

std::string App::default_images_name() const {
    const ExportJob job = make_export_job(false);
    return to_utf8(default_image_folder(job.input, image_export_tag(job, export_masks_anything())).filename());
}

std::filesystem::path App::images_output() const {
    return from_utf8(export_images_location_) / from_utf8(export_images_name_);
}

void App::poll_export() {
    std::unique_ptr<ExportResult> result;
    {
        std::lock_guard<std::mutex> lock(export_mutex_);
        if (!export_result_ || export_running_)
            return;
        result = std::move(export_result_);
    }
    if (export_thread_.joinable())
        export_thread_.join();
    if (result->ok) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Export done in %.1f s: %llu -> %llu events (%.1f %% kept)", result->seconds,
                      static_cast<unsigned long long>(result->events_in),
                      static_cast<unsigned long long>(result->events_out),
                      result->events_in ? 100.0 * result->events_out / result->events_in : 0.0);
        log(buf);
        if (result->triggers)
            log(std::to_string(result->triggers) + " external trigger events written");
        if (result->frames)
            log(std::to_string(result->frames) +
                (export_format_ == ExportFormat::Images ? " images written to " + to_utf8(export_output_) :
                                                          std::string(" video frames written")));
    } else if (result->cancelled) {
        log("Export cancelled");
    } else {
        log("Export failed: " + result->error, true);
    }
}

// ================================================================================================ hot pixels

void App::start_hot_pixel_detection() {
    cancel_hot_pixel_detection();
    hp_results_.clear();
    hp_progress_ = 0.f;
    hp_cancel_   = false;
    {
        std::lock_guard<std::mutex> lock(hp_mutex_);
        hp_activity_    = std::make_unique<PixelActivity>(width_, height_);
        hp_start_t_     = -1;
        hp_duration_us_ = static_cast<timestamp>(std::max(0.f, hp_seconds_) * 1e6);
    }
    if (source_ == SourceKind::Camera) {
        if (hp_duration_us_ <= 0)
            hp_duration_us_ = 5'000'000;
        hp_state_ = HotPixelState::Collecting;
        log("Measuring pixel activity for " + std::to_string(hp_duration_us_ / 1'000'000.0).substr(0, 4) + " s...");
        return;
    }
    if (source_ != SourceKind::File)
        return;
    // Recording: fast scan of the file (independent of the playback), first hp_seconds_ (0 = whole file).
    hp_state_  = HotPixelState::Scanning;
    const auto path = player_.path();
    hp_thread_ = std::thread([this, path] {
        std::string error;
        auto src = EventFileSource::open(path, true, &error);
        if (!src) {
            hp_state_ = HotPixelState::Idle;
            return;
        }
        timestamp duration = 0, unused = 0;
        bool done          = false;
        src->reader().add_read_callback([&](const EventCD *b, const EventCD *e) {
            if (done || b == e)
                return;
            std::lock_guard<std::mutex> lock(hp_mutex_);
            if (hp_start_t_ < 0)
                hp_start_t_ = b->t;
            if (hp_duration_us_ > 0) {
                const timestamp limit = hp_start_t_ + hp_duration_us_;
                const EventCD *stop =
                    std::lower_bound(b, e, limit, [](const EventCD &ev, timestamp t) { return ev.t < t; });
                hp_activity_->add(b, stop);
                if (stop != e)
                    done = true;
                hp_progress_ = static_cast<float>(std::min(1.0, double((e - 1)->t - hp_start_t_) / hp_duration_us_));
            } else {
                hp_activity_->add(b, e);
                if (duration > 0)
                    hp_progress_ = static_cast<float>(std::min(1.0, double((e - 1)->t) / duration));
            }
        });
        src->seek_range(unused, duration);
        try {
            while (!done && !hp_cancel_ && src->reader().read()) {}
        } catch (...) {}
        hp_state_ = hp_cancel_ ? HotPixelState::Idle : HotPixelState::Finished;
    });
}

void App::cancel_hot_pixel_detection() {
    hp_cancel_ = true;
    if (hp_thread_.joinable())
        hp_thread_.join();
    if (hp_state_ == HotPixelState::Collecting || hp_state_ == HotPixelState::Scanning)
        hp_state_ = HotPixelState::Idle;
}

void App::poll_hot_pixel_detection() {
    if (hp_state_ != HotPixelState::Finished)
        return;
    if (hp_thread_.joinable())
        hp_thread_.join();
    std::lock_guard<std::mutex> lock(hp_mutex_);
    if (!hp_activity_) {
        hp_state_ = HotPixelState::Idle;
        return;
    }
    hp_results_     = detect_hot_pixels(*hp_activity_, hp_params_);
    hp_measured_us_ = hp_activity_->duration_us();
    hp_activity_.reset();
    hp_state_ = HotPixelState::Results;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Hot pixel detection: %zu hot pixels over %.2f s", hp_results_.size(),
                  hp_measured_us_ / 1e6);
    log(buf);
}

void App::apply_hot_pixels() {
    editor_.snapshot();
    auto &sensor = masks_.pixels();
    auto &soft   = masks_.software_pixels();
    if (hp_replace_) {
        sensor.pixels.clear();
        soft.pixels.clear();
    }
    const size_t capacity = editor_.pixel_capacity();
    size_t to_sensor = 0, to_software = 0;
    for (const auto &hp : hp_results_) { // hottest first: they get the sensor slots
        if (sensor.contains(hp.x, hp.y) || soft.contains(hp.x, hp.y))
            continue;
        if (sensor.pixels.size() < capacity) {
            sensor.add(hp.x, hp.y);
            ++to_sensor;
        } else {
            soft.add(hp.x, hp.y);
            ++to_software;
        }
    }
    sensor.enabled = soft.enabled = true;
    masks_changed(true);
    log("Hot pixels masked: " + std::to_string(to_sensor) + " in the sensor pixel mask, " +
        std::to_string(to_software) + " in software");
}

// ================================================================================================ samples

void App::launch_sample(size_t index) {
    const auto &sample = known_samples()[index];
    std::filesystem::path input;
    if (source_ == SourceKind::File) {
        input = player_.path();
        player_.pause();
    } else if (source_ == SourceKind::Camera) {
        log("Closing the camera so that " + sample.name + " can use it");
        close_source();
    }
    std::string error;
    if (evs::launch_sample(sample, samples_dir_, input, &error))
        log("Started " + sample.executable + (input.empty() ? " (camera)" : " on " + to_utf8(input.filename())));
    else
        log(error, true);
}

// ================================================================================================ frame

void App::frame() {
    if (source_ == SourceKind::Camera) {
        const auto error = camera_.take_runtime_error();
        if (!error.empty())
            log("Camera error: " + error, true);
    }
    poll_export();
    poll_hot_pixel_detection();
    poll_latency();
    if (camera_settings_dirty_ && ImGui::GetTime() - camera_settings_changed_at_ > 0.5)
        save_camera_settings();

    const ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && source_ == SourceKind::File && !editor_.drawing())
            player_.is_playing() ? player_.pause() : player_.play();
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false) && !editor_.drawing())
            measure_ = !measure_;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            const auto path = open_file_dialog("Open recording", "Event files|*.raw;*.hdf5;*.h5;*.dat|All files|*.*");
            if (!path.empty())
                open_file(path);
        }
    }
    handle_step_keys();

    draw_menu_bar();
    draw_dockspace();
    draw_controls_window();
    draw_masks_window();
    draw_preview_window();
    draw_log_window();
    if (show_about_)
        draw_about();
    if (show_imgui_demo_)
        ImGui::ShowDemoWindow(&show_imgui_demo_);
    if (show_ptv_planner_)
        draw_ptv_planner();
}

void App::draw_dockspace() {
    const ImGuiID dock_id = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (layout_initialized_)
        return;
    layout_initialized_ = true;
    ImGuiDockNode *node = ImGui::DockBuilderGetNode(dock_id);
    if (node && (node->IsSplitNode() || node->Windows.Size > 0))
        return; // layout restored from imgui.ini

    ImGui::DockBuilderRemoveNode(dock_id);
    ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock_id, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dock_id;
    const ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.27f, nullptr, &center);
    const ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.16f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Controls", left);
    ImGui::DockBuilderDockWindow("Masks", right);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Preview", center);
    ImGui::DockBuilderFinish(dock_id);
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open camera"))
            open_camera();
        if (ImGui::MenuItem("Open recording...", "Ctrl+O")) {
            const auto path = open_file_dialog("Open recording", "Event files|*.raw;*.hdf5;*.h5;*.dat|All files|*.*");
            if (!path.empty())
                open_file(path);
        }
        if (ImGui::MenuItem("Open last recording", nullptr, false,
                            !last_recording_.empty() && !recording() && std::filesystem::exists(last_recording_)))
            open_file(last_recording_);
        if (ImGui::MenuItem("Reset camera", nullptr, false, source_ == SourceKind::Camera))
            reset_camera();
        if (ImGui::MenuItem("Close source", nullptr, false, source_ != SourceKind::None))
            close_source();
        ImGui::Separator();
        if (ImGui::MenuItem("Load masks...")) {
            const auto path = open_file_dialog("Load masks", "Mask files|*.masks;*.txt|All files|*.*");
            std::string error;
            if (!path.empty()) {
                editor_.snapshot();
                if (masks_.load(path, &error)) {
                    editor_.select(-1);
                    masks_changed(true);
                    log("Loaded " + std::to_string(masks_.shapes().size()) + " masks from " + to_utf8(path));
                } else {
                    log(error, true);
                }
            }
        }
        if (ImGui::MenuItem("Save masks...", nullptr, false, !masks_.shapes().empty())) {
            const auto path = save_file_dialog("Save masks", "Mask files|*.masks|All files|*.*", "masks.masks", ".masks");
            std::string error;
            if (!path.empty())
                masks_.save(path, &error) ? log("Masks saved to " + to_utf8(path)) : log(error, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
            quit_requested_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Samples")) {
        if (samples_dir_.empty())
            ImGui::TextDisabled("OpenEB samples not found (set EVS_OPENEB_BIN_DIR)");
        const auto &samples = known_samples();
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &s = samples[i];
            std::error_code ec;
            const bool present = !samples_dir_.empty() && std::filesystem::exists(samples_dir_ / s.executable, ec);
            const bool usable  = present && (source_ == SourceKind::File || s.supports_camera);
            if (ImGui::MenuItem(s.name.c_str(), nullptr, false, usable))
                launch_sample(i);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s\n%s\n%s", s.executable.c_str(), s.description.c_str(),
                                  source_ == SourceKind::File   ? "Runs on the current recording." :
                                  source_ == SourceKind::Camera ? "The camera will be closed in EventStudio first." :
                                                                  "Runs on the first available camera.");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Rate overlay", nullptr, &show_overlay_);
        ImGui::MenuItem("Fill masked pixels", nullptr, &show_mask_fill_);
        ImGui::MenuItem("Mask outlines", nullptr, &editor_.show_outlines);
        ImGui::MenuItem("Measure distances", "M", &measure_);
        if (ImGui::MenuItem("Reset zoom")) {
            zoom_ = 1.f;
            pan_  = ImVec2(0, 0);
        }
        ImGui::Separator();
        ImGui::MenuItem("PTV planner", nullptr, &show_ptv_planner_);
        ImGui::MenuItem("ImGui demo", nullptr, &show_imgui_demo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About / shortcuts"))
            show_about_ = true;
        ImGui::EndMenu();
    }

    // status on the right of the menu bar
    const std::string status = status_.size() > 120 ? status_.substr(0, 117) + "..." : status_;
    const float w            = ImGui::CalcTextSize(status.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 16));
    ImGui::TextColored(status_error_ ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.6f, 0.75f, 0.6f, 1), "%s", status.c_str());
    ImGui::EndMainMenuBar();
}

} // namespace evs::app
