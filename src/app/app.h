#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/mask_editor.h"
#include "core/camera_settings.h"
#include "core/filters.h"
#include "core/latency_monitor.h"
#include "core/frame_renderer.h"
#include "core/hot_pixels.h"
#include "core/mask_set.h"
#include "core/ptv_planner.h"
#include "core/rate_history.h"
#include "core/rate_meter.h"
#include "core/raw_file_writer.h"
#include "core/slider_detent.h"
#include "io/file_player.h"
#include "io/live_camera.h"
#include "io/offline_processor.h"

namespace evs::app {

enum class SourceKind { None, Camera, File };

// The whole application state; the UI code lives in app.cpp (frame / menus), panels.cpp and viewport.cpp.
class App {
public:
    App();
    ~App();

    void frame(); // builds the UI of one frame
    void open_file(const std::filesystem::path &path);
    bool load_masks(const std::filesystem::path &path);
    void shutdown();
    bool quit_requested() const { return quit_requested_; }
    // Work that must go on while the window is minimized (no frame() then): the timed recording stop.
    void background_tick();

private:
    // ---- sources (app.cpp)
    void refresh_camera_list();
    void open_camera();
    void open_camera_serial(const std::string &serial); // empty: first available
    void reset_camera();                                // close + reopen + restore: flushes the stream backlog
    void close_source();
    void start_camera();
    void stop_camera();
    void reopen_camera_with_format(EvtFormat format);
    void read_camera_state();

    // ---- settings kept between sessions (app.cpp), in app_data_dir()
    std::filesystem::path camera_settings_path() const; // cameras\<serial>.ini
    CameraSettings current_camera_settings() const;
    void apply_camera_settings(const CameraSettings &s);
    void restore_camera_settings();  // on open: saved settings of this serial, or the defaults (ERC 40 Mev/s)
    void camera_settings_changed();  // saved shortly after (sliders change many times per second)
    void save_camera_settings();
    void load_app_settings();
    void save_app_settings();
    void prepare_engine(int width, int height);
    void on_events(const EventCD *begin, const EventCD *end); // source thread
    void on_triggers(const EventExtTrigger *begin, const EventExtTrigger *end); // source thread
    void on_seek(timestamp t);                                // source thread
    timestamp preview_time() const;

    // ---- recording (app.cpp)
    void start_recording();
    void stop_recording();
    bool recording() const;
    double recording_elapsed_s() const;
    std::filesystem::path record_target() const; // folder / name.raw, before making it unique

    // ---- camera -> PC delay (app.cpp)
    void poll_latency();                 // every frame: delay warning, CSV log while recording
    void open_latency_log(const std::filesystem::path &recording);
    void close_latency_log();
    // Delay above which the preview is behind by more than one frame: the preview accumulation time.
    double latency_threshold_ms() const { return preview_.accumulation_us * 1e-3; }
    // Monitor of the current source: the camera stream while streaming, the playback while a recording plays
    // paced (speed > 0); nullptr otherwise.
    LatencyMonitor *active_latency();

    // ---- masks / filters (app.cpp)
    void masks_changed(bool committed);
    void apply_hardware_masks();
    void push_filter_config();
    void push_preview_settings();

    // ---- export (app.cpp)
    void start_export();
    void poll_export();
    ExportJob make_export_job(bool with_mask) const; // the mask bitmap is costly: only when starting
    bool export_masks_anything() const;              // an enabled mask in the selected export categories
    std::string default_images_name() const;         // <recording>_images_<options>
    std::filesystem::path images_output() const;

    // ---- hot pixel detection (app.cpp)
    void start_hot_pixel_detection();
    void cancel_hot_pixel_detection();
    void poll_hot_pixel_detection();
    void apply_hot_pixels();

    // ---- samples (app.cpp)
    void launch_sample(size_t index);

    // ---- UI (panels.cpp / viewport.cpp)
    void draw_dockspace();
    void draw_menu_bar();
    void draw_controls_window();
    void section_source();
    void section_latency();
    void section_event_rate();
    void section_playback();
    void section_recording();
    void section_biases();
    void section_hardware_filters();
    void section_software_filters();
    void section_preview();
    void section_export();
    void draw_masks_window();
    void masks_section_roi();
    void masks_section_pixels();
    void masks_section_polygons();
    void draw_pixel_loupe(const ImVec2 &image_origin, float scale);
    void draw_preview_window();
    void draw_playback_bar();
    void draw_overlay(const ImVec2 &image_min, const ImVec2 &image_max);
    void draw_log_window();
    void draw_hot_pixel_tool();
    void draw_ptv_planner();
    void draw_about();
    void upload_frame();
    void upload_mask_overlay();

    void log(const std::string &message, bool error = false);

    // ---- sources
    SourceKind source_ = SourceKind::None;
    LiveCamera camera_;
    FilePlayer player_;
    std::vector<std::string> camera_list_;
    int camera_index_       = 0;
    EvtFormat camera_format_ = EvtFormat::Evt3;
    int width_ = kDefaultWidth, height_ = kDefaultHeight;
    std::string file_info_;

    // cached camera state (hardware access is slow: read on open / after changes only)
    std::vector<BiasInfo> biases_, biases_at_open_;
    ErcInfo erc_;
    TrailInfo trail_;
    DigitalMaskInfo dem_;
    RoiInfo roi_info_;
    TriggerInInfo trigger_in_;
    std::string hw_roi_status_, hw_pixel_status_;
    size_t masked_pixel_count_ = 0;

    // ---- processing (shared with the source thread)
    std::unique_ptr<FilterPipeline> pipeline_;
    std::unique_ptr<FrameRenderer> renderer_;
    FilterConfig filters_;
    PreviewSettings preview_;
    RateMeter in_meter_, out_meter_, file_bytes_meter_;
    // Input events (before the software filters) per 100 ms bin over 60 s: graph and 10 s mean. Camera: PC clock;
    // file: recording time.
    RateHistory event_history_;
    std::atomic<timestamp> last_event_t_{0};
    std::atomic<timestamp> last_event_wall_{0};
    std::atomic<uint64_t> total_events_{0};
    std::atomic<uint64_t> total_triggers_{0};

    // ---- recording
    enum class RecordMode { Raw = 0, Processed = 1 };
    RecordMode record_mode_ = RecordMode::Raw;
    EvtFormat processed_format_ = EvtFormat::Evt3;
    std::string record_dir_;  // folder of the recordings (kept between sessions)
    std::string record_name_; // file name, ".raw" added when missing
    bool record_timed_        = false;
    float record_duration_s_  = 10.f;
    RawFileWriter processed_writer_;
    std::atomic<bool> processed_recording_{false};
    bool raw_recording_ = false;
    std::chrono::steady_clock::time_point record_start_;
    std::filesystem::path last_recording_;

    // ---- settings kept between sessions
    bool camera_settings_dirty_       = false;
    SliderDetent bias_detent_;
    std::string bias_detent_name_; // bias whose slider the detent belongs to
    double camera_settings_changed_at_ = 0;

    // ---- camera -> PC delay
    bool latency_warn_enabled_   = true;
    bool latency_warning_active_ = false;
    // Playback: recording time / speed vs PC clock, i.e. how far the playback slips behind the requested pace.
    LatencyMonitor playback_latency_{500'000};
    bool playback_was_playing_ = false;
    double playback_speed_seen_ = -1;
    std::ofstream latency_log_;          // <recording>_latency.csv while recording a camera
    std::filesystem::path latency_log_path_;
    timestamp latency_log_start_us_ = 0;
    uint64_t latency_log_rows_      = 0;

    // ---- preview
    unsigned int frame_tex_ = 0, mask_tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0, mask_tex_w_ = 0, mask_tex_h_ = 0;
    std::vector<uint8_t> rgba_;
    bool mask_overlay_dirty_ = true;
    bool show_overlay_       = true;
    bool show_mask_fill_     = true;
    bool freeze_             = false;
    float zoom_              = 1.f;
    ImVec2 pan_{0, 0};
    float playback_speed_    = 1.f;
    int speed_index_         = 4; // 1x

    // ---- masks
    MaskSet masks_;
    MaskEditor editor_{masks_};
    std::shared_ptr<const std::vector<uint8_t>> mask_bitmap_;
    MaskSet edit_start_; // masks when a numeric mask field started being edited (undo)

    // ---- export
    ExportFormat export_format_ = ExportFormat::RawEvt3;
    std::string export_path_;
    bool export_apply_filters_ = true;
    unsigned export_mask_categories_ = kMaskAll;
    bool export_whole_file_    = true;
    float export_start_s_ = 0.f, export_end_s_ = 10.f;
    float export_fps_           = 30.f;
    std::string export_images_location_;  // Images: folder receiving the image folder (default: the recording's)
    std::string export_images_name_;      // Images: name of the image folder
    bool export_images_name_auto_ = true; // the name follows the settings (default_images_name)
    std::thread export_thread_;
    std::unique_ptr<ExportProgress> export_progress_;
    std::atomic<bool> export_running_{false};
    std::mutex export_mutex_;
    std::unique_ptr<ExportResult> export_result_;
    std::filesystem::path export_output_;

    // ---- hot pixel detection
    enum class HotPixelState { Idle, Collecting, Scanning, Finished, Results };
    std::atomic<HotPixelState> hp_state_{HotPixelState::Idle};
    HotPixelParams hp_params_;
    float hp_seconds_ = 5.f;
    bool hp_replace_  = true;
    std::mutex hp_mutex_;
    std::unique_ptr<PixelActivity> hp_activity_; // guarded by hp_mutex_
    timestamp hp_start_t_     = -1;              // guarded by hp_mutex_
    timestamp hp_duration_us_ = 0;
    std::atomic<float> hp_progress_{0.f};
    std::atomic<bool> hp_cancel_{false};
    std::thread hp_thread_;
    std::vector<HotPixel> hp_results_;
    timestamp hp_measured_us_ = 0;

    // ---- PTV planner
    bool show_ptv_planner_ = false;
    PtvSetup ptv_;

    // ---- misc
    struct LogEntry {
        std::string time, text;
        bool error;
    };
    std::vector<LogEntry> log_;
    std::string status_;
    bool status_error_      = false;
    bool show_about_        = false;
    bool quit_requested_    = false;
    bool show_imgui_demo_   = false;
    bool layout_initialized_ = false;
    std::filesystem::path samples_dir_;
};

} // namespace evs::app
