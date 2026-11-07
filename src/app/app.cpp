#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <ctime>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/platform.h"
#include "io/event_file_source.h"

#include <metavision/sdk/stream/event_file_reader.h>
#include "io/sample_launcher.h"

namespace evs::app {

namespace {
std::filesystem::path unique_path(std::filesystem::path p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return p;
    const auto stem = p.stem().string(), ext = p.extension().string();
    for (int i = 1;; ++i) {
        auto candidate = p.parent_path() / (stem + "_" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }
}
} // namespace

App::App() {
    refresh_camera_list();
    samples_dir_ = find_samples_dir();
    record_path_ = to_utf8(default_output_path("recording", ".raw"));
    export_path_ = to_utf8(default_output_path("export", ".raw"));
    prepare_engine(kDefaultWidth, kDefaultHeight);
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
    pipeline_->reset();
    renderer_->reset();
    in_meter_.reset();
    out_meter_.reset();
    file_bytes_meter_.reset();
    last_event_t_ = t;
}

timestamp App::preview_time() const {
    const timestamp last = renderer_->last_timestamp();
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
    const std::string serial = camera_list_.empty() ? std::string() : camera_list_[camera_index_];
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
    biases_at_open_ = biases_;
    masks_changed(false);
    apply_hardware_masks();
    log("Opened camera " + camera_.serial() + " (" + camera_.sensor() + ", " + std::to_string(width_) + "x" +
        std::to_string(height_) + ", " + camera_.format_string() + ")");
    start_camera();
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
    if (source_ == SourceKind::Camera)
        camera_.close();
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
    std::filesystem::path path = record_path_.empty() ? default_output_path("recording", ".raw") : from_utf8(record_path_);
    if (path.extension() != ".raw")
        path += ".raw";
    path = unique_path(path);
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
    last_recording_ = path;
    record_path_    = to_utf8(path);
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
    std::error_code ec;
    const auto size = std::filesystem::file_size(last_recording_, ec);
    log("Recording saved: " + to_utf8(last_recording_) + " (" + human_bytes(static_cast<double>(size)) + ")");
    // Next recording gets a fresh name.
    record_path_ = to_utf8(unique_path(last_recording_));
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

    const ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && source_ == SourceKind::File && !editor_.drawing())
            player_.is_playing() ? player_.pause() : player_.play();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            const auto path = open_file_dialog("Open recording", "Event files|*.raw;*.hdf5;*.h5;*.dat|All files|*.*");
            if (!path.empty())
                open_file(path);
        }
    }

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
