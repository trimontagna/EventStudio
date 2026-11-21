#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/app.h"
#include "app/platform.h"

namespace evs::app {

namespace {

void help_marker(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool slider_us(const char *label, timestamp &value, int min_us, int max_us, const char *format = "%d us") {
    int v              = static_cast<int>(std::clamp<timestamp>(value, min_us, max_us));
    const bool changed = ImGui::SliderInt(label, &v, min_us, max_us, format, ImGuiSliderFlags_Logarithmic);
    if (changed)
        value = v;
    return changed;
}

bool slider_rate(const char *label, uint64_t &rate, float min_mev, float max_mev) {
    float mev          = static_cast<float>(rate / 1e6);
    const bool changed = ImGui::SliderFloat(label, &mev, min_mev, max_mev, "%.3f Mev/s", ImGuiSliderFlags_Logarithmic);
    if (changed)
        rate = static_cast<uint64_t>(std::max(0.f, mev) * 1e6);
    return changed;
}

struct BiasLabel {
    const char *name, *label, *help;
};
// The parameters requested for the UI, in this order; other biases of the device follow.
const BiasLabel kBiasLabels[] = {
    {"bias_diff_on", "Diff ON", "Contrast threshold for ON events (positive contrast), relative to bias_diff."},
    {"bias_diff_off", "Diff OFF", "Contrast threshold for OFF events (negative contrast), relative to bias_diff."},
    {"bias_diff", "Diff reference", "Reference level of the contrast detector (bias_diff)."},
    {"bias_refr", "Refractory time", "Refractory period of the pixels after an event (bias_refr)."},
    {"bias_fo", "Low-pass filter", "Low-pass filter of the photoreceptor (bias_fo): cuts fast flickers."},
    {"bias_hpf", "High-pass filter", "High-pass filter (bias_hpf): cuts slow illumination changes."},
};

const char *trail_help =
    "TRAIL: keeps the first event of a burst (same polarity, each event within the threshold of the previous one).\n"
    "STC_CUT_TRAIL: keeps only the second event of a burst (isolated events removed).\n"
    "STC_KEEP_TRAIL: keeps the second and following events of a burst (isolated events removed).";

} // namespace

// ================================================================================================ controls window

void App::draw_controls_window() {
    if (!ImGui::Begin("Controls")) {
        ImGui::End();
        return;
    }
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 9);
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen))
        section_source();
    if (source_ == SourceKind::File && ImGui::CollapsingHeader("Playback"))
        section_playback();
    if (ImGui::CollapsingHeader("Recording", ImGuiTreeNodeFlags_DefaultOpen))
        section_recording();
    if (ImGui::CollapsingHeader("Camera biases"))
        section_biases();
    if (ImGui::CollapsingHeader("Hardware filters (sensor ESP)", ImGuiTreeNodeFlags_DefaultOpen))
        section_hardware_filters();
    if (ImGui::CollapsingHeader("Software filters"))
        section_software_filters();
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
        section_preview();
    if (ImGui::CollapsingHeader("Process & export"))
        section_export();
    ImGui::PopItemWidth();
    ImGui::End();
}

void App::section_source() {
    ImGui::SeparatorText("Live camera");
    const bool camera_open = source_ == SourceKind::Camera;
    ImGui::BeginDisabled(camera_open);
    const char *preview = camera_list_.empty() ? "(first available)" : camera_list_[camera_index_].c_str();
    if (ImGui::BeginCombo("Camera", preview)) {
        for (int i = 0; i < static_cast<int>(camera_list_.size()); ++i)
            if (ImGui::Selectable(camera_list_[i].c_str(), i == camera_index_))
                camera_index_ = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        refresh_camera_list();
    ImGui::EndDisabled();

    int fmt = camera_format_ == EvtFormat::Evt21 ? 1 : 0;
    ImGui::TextUnformatted("Stream format");
    ImGui::SameLine();
    ImGui::BeginDisabled(recording());
    bool fmt_changed = ImGui::RadioButton("EVT 3.0", &fmt, 0);
    ImGui::SameLine();
    fmt_changed |= ImGui::RadioButton("EVT 2.1", &fmt, 1);
    ImGui::EndDisabled();
    help_marker("Encoding of the data sent by the sensor and of raw recordings. EVT 3.0 is more compact on sparse "
                "scenes, EVT 2.1 (vectorized 64-bit words) copes better with very high event rates. Changing it "
                "reopens the camera.");
    if (fmt_changed)
        reopen_camera_with_format(fmt == 1 ? EvtFormat::Evt21 : EvtFormat::Evt3);

    if (!camera_open) {
        if (ImGui::Button("Open camera", ImVec2(-1, 0)))
            open_camera();
    } else {
        if (camera_.is_running()) {
            if (ImGui::Button("Stop streaming"))
                stop_camera();
        } else if (ImGui::Button("Start streaming")) {
            start_camera();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset camera"))
            reset_camera();
        ImGui::SetItemTooltip("Closes and reopens the camera, then restores its settings (biases, ERC, trail filter,\n"
                              "trigger inputs, masks) and restarts streaming. Discards the data buffered in the\n"
                              "camera and USB driver: use it when the stream is far behind after a saturation\n"
                              "(much faster than waiting for the backlog). Stops a running recording.");
        ImGui::SameLine();
        if (ImGui::Button("Close camera"))
            close_source();
        ImGui::TextDisabled("%s  |  %s  |  %dx%d  |  %s", camera_.serial().c_str(), camera_.sensor().c_str(), width_,
                            height_, camera_.format_string().c_str());
    }
    if (camera_open) {
        section_latency();
        section_event_rate();
    }

    ImGui::SeparatorText("Recording file");
    if (ImGui::Button("Open recording...", ImVec2(-1, 0))) {
        const auto path = open_file_dialog("Open recording", "Event files|*.raw;*.hdf5;*.h5;*.dat|All files|*.*");
        if (!path.empty())
            open_file(path);
    }
    if (!last_recording_.empty()) {
        const bool exists = std::filesystem::exists(last_recording_);
        ImGui::BeginDisabled(!exists || recording());
        if (ImGui::Button("Open last recording##source", ImVec2(-1, 0)))
            open_file(last_recording_);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s%s", to_utf8(last_recording_).c_str(), exists ? "" : "\n(file not found)");
    }
    if (source_ == SourceKind::File) {
        ImGui::TextWrapped("%s", to_utf8(player_.path().filename()).c_str());
        ImGui::TextDisabled("%s", file_info_.c_str());
        if (ImGui::SmallButton("Close file"))
            close_source();
    }
}

void App::section_latency() {
    const bool camera = source_ == SourceKind::Camera;
    ImGui::SeparatorText(camera ? "Stream delay (camera -> PC)" : "Playback delay");
    if (!camera)
        help_marker("How far the playback slips behind the requested pace: every delivered slice pairs the "
                    "recording time / speed with the PC clock, like the camera delay. It grows when decoding and "
                    "the software filters cannot keep up with the playback speed (the player then plays slower "
                    "than asked). Pause, seek and speed changes take a new reference.");
    else
        help_marker("How late the camera data reaches EventStudio. The camera only sends relative timestamps (EVT 3.0: "
                "24-bit microsecond counter since the stream started, no date), so every received buffer is paired "
                "with the PC clock: offset = PC time - camera time. The smallest offset is the reference and the "
                "delay is the offset above it, i.e. the delay in excess of the fastest buffer (USB / driver "
                "buffering, the PC falling behind). The fixed part (exposure to first USB packet) cannot be seen "
                "this way. Camera and PC clocks drift apart by ~10-50 ppm: reset the reference in long sessions.\n\n"
                "While recording a camera, <recording>_latency.csv logs every buffer: PC UTC time, PC time since "
                "the recording started, camera time and delay (us).");
    LatencyMonitor *monitor = active_latency();
    if (!monitor) {
        ImGui::TextDisabled(camera                ? "Start streaming to measure" :
                            player_.speed() <= 0 ? "Not measured at max speed (no pacing)" :
                                                   "Play to measure");
    } else {
        const timestamp now = LiveCamera::wall_us();
        const auto st       = monitor->stats(now);
        if (!st.valid) {
            ImGui::TextDisabled("Measuring the reference...");
        } else {
            const bool late = latency_warn_enabled_ && st.current_ms > latency_threshold_ms();
            if (late)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.35f, 0.35f, 1));
            ImGui::Text("Current %.1f ms", st.current_ms);
            if (late)
                ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("|  max 10 s %.1f ms  |  max %.1f ms", st.recent_max_ms, st.max_ms);

            const auto history = monitor->history_ms(now);
            float top          = latency_warn_enabled_ ? static_cast<float>(latency_threshold_ms()) * 1.2f : 1.f;
            for (float v : history)
                top = std::max(top, v * 1.1f);
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "last %.0f s, 0 - %.0f ms",
                          monitor->history_window_us() * 1e-6, top);
            ImGui::PlotLines("##delay_history", history.data(), static_cast<int>(history.size()), 0, overlay, 0.f,
                             top, ImVec2(-1, 60));
            ImGui::TextDisabled(camera ? "Reference set %.0f s ago  |  %llu buffers" :
                                         "Reference set %.0f s ago  |  %llu slices",
                                st.reference_age_s,
                                static_cast<unsigned long long>(st.samples));
        }
    }
    char warn_label[96];
    std::snprintf(warn_label, sizeof(warn_label), "Warn when longer than the accumulation time (%.3g ms)",
                  latency_threshold_ms());
    ImGui::Checkbox(warn_label, &latency_warn_enabled_);
    ImGui::SetItemTooltip("The preview then shows data older than one accumulation window: the delay turns red\n"
                          "and a warning is logged. Change the accumulation time in the Preview panel.");
    if (monitor && ImGui::SmallButton("Reset reference"))
        monitor->reset();
    if (latency_log_.is_open())
        ImGui::TextDisabled("Logging to %s", to_utf8(latency_log_path_.filename()).c_str());
}

void App::section_event_rate() {
    const bool camera = source_ == SourceKind::Camera;
    ImGui::SeparatorText("Event rate");
    help_marker(camera ? "Events received from the camera (before the software filters). Current: last second. The "
                         "graph shows every 100 ms (PC clock) over the last 60 s; the dashed line is the mean over "
                         "the last 10 s, as for the max delay, to read a steady level through the noise." :
                         "Events read from the recording (before the software filters), in recording time. Current: "
                         "last second. The graph shows every 100 ms over the last 60 s of recording; the dashed line "
                         "is the mean over the last 10 s.");
    if (camera && !camera_.is_running()) {
        ImGui::TextDisabled("Start streaming to measure");
        return;
    }
    if (event_history_.empty()) {
        ImGui::TextDisabled(camera ? "No events received yet" : "Play to measure");
        return;
    }
    const timestamp now = camera ? LiveCamera::wall_us() : last_event_t_.load();
    const double live   = camera ? camera_.event_rate() : in_meter_.rate_per_second(now);
    const double mean   = event_history_.mean_per_second(now, 10'000'000);
    ImGui::Text("Current %s", human_rate(live, "ev").c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|  mean 10 s %s", human_rate(mean, "ev").c_str());

    const auto history = event_history_.per_second(now);
    float top          = std::max(1.f, static_cast<float>(mean) * 1.2f);
    for (float v : history)
        top = std::max(top, v * 1.1f);
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "last %.0f s, 0 - %s", event_history_.history_window_us() * 1e-6,
                  human_rate(top, "ev").c_str());
    ImGui::PlotLines("##event_rate_history", history.data(), static_cast<int>(history.size()), 0, overlay, 0.f, top,
                     ImVec2(-1, 60));
    // Dashed line at the 10 s mean, inside the plot area (the frame minus its padding).
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 lo(ImGui::GetItemRectMin().x + pad.x, ImGui::GetItemRectMin().y + pad.y);
    const ImVec2 hi(ImGui::GetItemRectMax().x - pad.x, ImGui::GetItemRectMax().y - pad.y);
    const float y  = hi.y - static_cast<float>(mean / top) * (hi.y - lo.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (float x = lo.x; x < hi.x; x += 9.f)
        dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5.f, hi.x), y), IM_COL32(255, 200, 80, 220), 1.5f);
}

void App::section_playback() {
    static const float speeds[]      = {0.01f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 0.f};
    static const char *speed_names[] = {"0.01x", "0.1x", "0.25x", "0.5x", "1x (real time)", "2x", "5x", "10x",
                                        "Max (no pacing)"};
    speed_index_ = std::clamp(speed_index_, 0, 8);
    if (ImGui::Combo("Speed", &speed_index_, speed_names, IM_ARRAYSIZE(speed_names))) {
        playback_speed_ = speeds[speed_index_];
        player_.set_speed(playback_speed_);
    }
    bool loop = player_.loop();
    if (ImGui::Checkbox("Loop", &loop))
        player_.set_loop(loop);
    section_latency();
    section_event_rate();
    timestamp min_t = 0, max_t = 0;
    if (player_.seek_range(min_t, max_t))
        ImGui::TextDisabled("Duration %s", format_time_us(max_t).c_str());
    else
        ImGui::TextDisabled("Building seek index...");
    const auto st = player_.status();
    if (!st.empty())
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", st.c_str());
}

void App::section_recording() {
    int mode = static_cast<int>(record_mode_);
    ImGui::BeginDisabled(recording());
    ImGui::RadioButton("Raw sensor stream", &mode, 0);
    help_marker("Exact data sent by the camera, in the stream format chosen above. Hardware settings (biases, ESP "
                "filters, sensor mask) apply; software filters do not.");
    ImGui::RadioButton("Processed stream", &mode, 1);
    help_marker("What the preview shows: events after the software filters (polarity, masks, trail, refractory, "
                "rate control), re-encoded in the chosen format. Also works while playing a recording.");
    record_mode_ = static_cast<RecordMode>(mode);
    if (record_mode_ == RecordMode::Processed) {
        int f = processed_format_ == EvtFormat::Evt21 ? 1 : processed_format_ == EvtFormat::Evt2 ? 2 : 0;
        const char *names[] = {"EVT 3.0", "EVT 2.1", "EVT 2.0"};
        if (ImGui::Combo("Format", &f, names, 3))
            processed_format_ = f == 1 ? EvtFormat::Evt21 : f == 2 ? EvtFormat::Evt2 : EvtFormat::Evt3;
    } else {
        ImGui::TextDisabled("Format: %s (camera stream format)", display_name(camera_format_));
    }
    ImGui::InputText("Folder##rec", &record_dir_);
    if (ImGui::IsItemDeactivatedAfterEdit())
        save_app_settings();
    ImGui::SameLine();
    if (ImGui::SmallButton("...##recdir")) {
        const auto p = pick_folder_dialog("Folder of the recordings", from_utf8(record_dir_));
        if (!p.empty()) {
            record_dir_ = to_utf8(p);
            save_app_settings();
        }
    }
    ImGui::SetItemTooltip("Choose the folder (kept between sessions).");
    ImGui::InputText("File name##rec", &record_name_);
    ImGui::SameLine();
    ImGui::TextDisabled(".raw");
    ImGui::SameLine();
    if (ImGui::SmallButton("New name"))
        record_name_ = to_utf8(default_output_path("recording", ".raw").stem());
    ImGui::SetItemTooltip("recording_<date>_<time>");
    ImGui::EndDisabled();
    if (!recording()) {
        const auto target = record_target();
        std::error_code ec;
        if (std::filesystem::exists(target, ec))
            ImGui::TextDisabled("Exists: will record to %s", to_utf8(unique_path(target).filename()).c_str());
    }

    if (ImGui::Checkbox("Stop after##rectimed", &record_timed_))
        save_app_settings();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::BeginDisabled(!record_timed_);
    ImGui::DragFloat("##recduration", &record_duration_s_, 0.1f, 0.1f, 86400.f, "%.1f s",
                     ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit())
        save_app_settings();
    ImGui::EndDisabled();
    help_marker("Timed recording: stops on its own after this duration (PC clock, checked every frame, also while "
                "the window is minimized). Drag, or double click to type. Can be changed during a recording.");

    const bool can_record =
        source_ == SourceKind::Camera ? camera_.is_running() : (source_ == SourceKind::File && mode == 1);
    if (!recording()) {
        ImGui::BeginDisabled(!can_record);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.2f, 0.2f, 1));
        if (ImGui::Button("  Record  ", ImVec2(-1, 0)))
            start_recording();
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        if (!can_record)
            ImGui::TextDisabled(source_ == SourceKind::File ? "Recording a file: use the Processed mode" :
                                                              "Open and start a camera to record");
        if (!last_recording_.empty()) {
            if (ImGui::SmallButton("Show last recording"))
                reveal_in_explorer(last_recording_);
            ImGui::SameLine();
            ImGui::BeginDisabled(!std::filesystem::exists(last_recording_));
            if (ImGui::SmallButton("Open last recording##rec"))
                open_file(last_recording_);
            ImGui::EndDisabled();
        }
    } else {
        if (ImGui::Button("  Stop recording  ", ImVec2(-1, 0)))
            stop_recording();
        const double elapsed = recording_elapsed_s();
        std::error_code ec;
        const double size = raw_recording_ ? static_cast<double>(std::filesystem::file_size(last_recording_, ec)) :
                                             static_cast<double>(processed_writer_.bytes_written());
        const float blink = std::fmod(static_cast<float>(ImGui::GetTime()), 1.f) < 0.5f ? 1.f : 0.4f;
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, blink), "REC");
        ImGui::SameLine();
        ImGui::Text("%s  |  %s", format_time_us(static_cast<long long>(elapsed * 1e6)).c_str(),
                    human_bytes(size).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", to_utf8(last_recording_.filename()).c_str());
        if (record_timed_) {
            const double f = std::clamp(elapsed / std::max(0.1, static_cast<double>(record_duration_s_)), 0.0, 1.0);
            char overlay[96];
            std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f s", elapsed, record_duration_s_);
            ImGui::ProgressBar(static_cast<float>(f), ImVec2(-1, 0), overlay);
        }
    }
}

void App::section_biases() {
    if (source_ != SourceKind::Camera) {
        ImGui::TextDisabled("Open a camera to adjust its biases.");
        return;
    }
    if (biases_.empty()) {
        ImGui::TextDisabled("This camera exposes no biases.");
        return;
    }
    std::vector<size_t> order;
    for (const auto &l : kBiasLabels)
        for (size_t i = 0; i < biases_.size(); ++i)
            if (biases_[i].name == l.name)
                order.push_back(i);
    for (size_t i = 0; i < biases_.size(); ++i)
        if (std::find(order.begin(), order.end(), i) == order.end())
            order.push_back(i);

    for (size_t i : order) {
        auto &b             = biases_[i];
        const BiasLabel *bl = nullptr;
        for (const auto &l : kBiasLabels)
            if (b.name == l.name)
                bl = &l;
        const std::string label = (bl ? std::string(bl->label) : b.name) + "##" + b.name;
        const bool outside      = b.value < b.rec_min || b.value > b.rec_max;
        if (outside)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.2f, 0.1f, 1));
        ImGui::BeginDisabled(!b.modifiable);
        // Detent at the edges of the recommended range (when inside the allowed one): a drag stops there and
        // must push on by ~1.25 font sizes to go through. Typed values (Ctrl+click) are not affected.
        std::vector<int> detents;
        for (int p : {b.rec_min, b.rec_max})
            if (p > b.min && p < b.max && b.rec_min < b.rec_max)
                detents.push_back(p);
        const float frame_w = ImGui::CalcItemWidth();
        int v               = b.value;
        bool changed        = ImGui::SliderInt(label.c_str(), &v, b.min, b.max);
        const bool active   = ImGui::IsItemActive();
        if (active && (ImGui::IsItemActivated() || bias_detent_name_ != b.name)) { // a new drag
            bias_detent_.release();
            bias_detent_name_ = b.name;
        } else if (!active && bias_detent_name_ == b.name) { // drag over
            bias_detent_.release();
            bias_detent_name_.clear();
        }
        if (changed && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !detents.empty()) {
            const float units_per_px = static_cast<float>(b.max - b.min) / std::max(frame_w, 1.f);
            const int width = std::max(1, static_cast<int>(std::lround(ImGui::GetFontSize() * 1.25f * units_per_px)));
            v               = bias_detent_.apply(b.value, v, detents, width);
            changed         = v != b.value;
        }
        if (changed) {
            std::string error;
            if (camera_.set_bias(b.name, v, &error)) {
                b.value = v;
                camera_settings_changed();
            } else {
                log(error, true);
            }
        }
        {
            // Tick marks at the recommended limits, where the grab centre sits for that value.
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            const float pad = 2.f; // ImGui's slider grab padding
            const float range = static_cast<float>(std::max(1, b.max - b.min));
            const float grab  = std::max((frame_w - 2 * pad) / (range + 1), ImGui::GetStyle().GrabMinSize);
            const float usable = frame_w - 2 * pad - grab;
            const bool held    = bias_detent_name_ == b.name && bias_detent_.holding();
            for (int p : detents) {
                const float x   = r0.x + pad + grab * 0.5f + usable * static_cast<float>(p - b.min) / range;
                const ImU32 col = held && bias_detent_.point() == p ? IM_COL32(255, 220, 90, 255) :
                                                                      IM_COL32(255, 255, 255, 110);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(x, r0.y + 1), ImVec2(x, r1.y - 1), col, 2.f);
            }
        }
        ImGui::EndDisabled();
        if (outside)
            ImGui::PopStyleColor();
        if (ImGui::BeginItemTooltip()) {
            ImGui::Text("%s", b.name.c_str());
            if (bl)
                ImGui::TextUnformatted(bl->help);
            if (!b.description.empty())
                ImGui::TextWrapped("%s", b.description.c_str());
            ImGui::Text("Allowed [%d, %d], recommended [%d, %d]", b.min, b.max, b.rec_min, b.rec_max);
            ImGui::TextDisabled("Dragging stops at the recommended limits (ticks): push on to go through.");
            ImGui::EndTooltip();
        }
    }
    if (ImGui::Button("Reset to camera defaults")) {
        for (const auto &b : biases_at_open_) {
            std::string error;
            if (b.modifiable && !camera_.set_bias(b.name, b.value, &error))
                log(error, true);
        }
        biases_ = camera_.biases();
        camera_settings_changed();
    }
    ImGui::SetItemTooltip("Values the camera reported when it was opened, before EventStudio restored the\n"
                          "settings it last gave this camera (normally the sensor defaults).");
    ImGui::SameLine();
    if (ImGui::Button("Load .bias")) {
        const auto p = open_file_dialog("Load biases", "Bias files|*.bias|All files|*.*");
        std::string error;
        if (!p.empty()) {
            camera_.load_biases(p, &error) ? log("Biases loaded from " + to_utf8(p)) : log("Load failed: " + error, true);
            biases_ = camera_.biases();
            camera_settings_changed();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save .bias")) {
        const auto p = save_file_dialog("Save biases", "Bias files|*.bias", "camera.bias", ".bias");
        std::string error;
        if (!p.empty())
            camera_.save_biases(p, &error) ? log("Biases saved to " + to_utf8(p)) : log("Save failed: " + error, true);
    }
    ImGui::TextDisabled("Sliders span the range the sensor allows; ticks: recommended limits, where a drag\n"
                        "stops (push on to go through); orange: outside the recommended range.\n"
                        "Settings are restored when this camera is opened again.");
}

void App::section_hardware_filters() {
    if (source_ != SourceKind::Camera) {
        ImGui::TextDisabled("Open a camera to use the sensor filters.\nOffline, use the software filters below.");
        return;
    }
    std::string error;

    // ---- Event Rate Controller
    ImGui::SeparatorText("Event Rate Controller");
    if (!erc_.available) {
        ImGui::TextDisabled("Not available on this camera");
    } else {
        bool enabled = erc_.enabled;
        uint64_t rate = erc_.rate;
        bool changed  = ImGui::Checkbox("Enable##erc", &enabled);
        changed |= slider_rate("Max event rate##erc", rate, std::max(0.001f, erc_.min_rate / 1e6f),
                               std::max(erc_.max_rate / 1e6f, 1.f));
        help_marker("The sensor drops events (spread in space and time) so that its output stays under this rate. "
                    "The overlay on the preview shows the data rate, event rate and drop rate.");
        if (changed) {
            if (!camera_.set_erc(enabled, static_cast<uint32_t>(rate), &error))
                log("ERC: " + error, true);
            erc_ = camera_.erc();
            camera_settings_changed();
        }
    }

    // ---- Event Trail Filter
    ImGui::SeparatorText("Event Trail Filter");
    if (!trail_.available) {
        ImGui::TextDisabled("Not available on this camera");
    } else {
        bool enabled   = trail_.enabled;
        TrailMode mode = trail_.mode;
        int ms         = static_cast<int>(std::lround(trail_.threshold / 1000.0));
        bool changed   = ImGui::Checkbox("Enable##trail", &enabled);
        if (ImGui::BeginCombo("Type##trail", to_string(mode))) {
            for (TrailMode m : trail_.modes)
                if (ImGui::Selectable(to_string(m), m == mode)) {
                    mode    = m;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        help_marker(trail_help);
        changed |= ImGui::SliderInt("Threshold##trail", &ms, static_cast<int>(trail_.min_threshold / 1000),
                                    static_cast<int>(trail_.max_threshold / 1000), "%d ms");
        ImGui::TextDisabled("Sensor range: %u - %u ms (1 ms steps). For windows down to 100 us,\nuse the software "
                            "trail filter.",
                            trail_.min_threshold / 1000, trail_.max_threshold / 1000);
        if (changed) {
            if (!camera_.set_trail(enabled, mode, static_cast<uint32_t>(ms) * 1000, &error))
                log("Trail filter: " + error, true);
            trail_ = camera_.trail();
            camera_settings_changed();
        }
    }

    // ---- External trigger inputs
    ImGui::SeparatorText("External trigger inputs");
    if (!trigger_in_.available || trigger_in_.channels.empty()) {
        ImGui::TextDisabled("Not available on this camera");
    } else {
        bool trigger_changed = false;
        for (auto &[name, id, enabled] : trigger_in_.channels) {
            const std::string label = name + " (id " + std::to_string(id) + ")##tin";
            if (ImGui::Checkbox(label.c_str(), &enabled)) {
                if (!camera_.set_trigger_in(name, enabled, &error))
                    log("Trigger input " + name + ": " + error, true);
                trigger_changed = true;
            }
        }
        if (trigger_changed) { // read back after the loop: reassigning the vector inside it would invalidate it
            trigger_in_ = camera_.trigger_in();
            camera_settings_changed();
        }
        help_marker("Trigger events are kept in raw and processed recordings and in exports.");
        ImGui::TextDisabled("%llu trigger events received", static_cast<unsigned long long>(total_triggers_.load()));
    }
    ImGui::TextDisabled("ROI / RONI and pixel masks: see the Masks window.");
    if (ImGui::SmallButton("Read back from camera"))
        read_camera_state();
}

void App::section_software_filters() {
    ImGui::TextDisabled("Applied to the preview, processed recordings and exports.");
    FilterConfig &f = filters_;
    bool changed    = false;

    ImGui::SeparatorText("Activity (neighbourhood) filter");
    changed |= ImGui::Checkbox("Enable##act", &f.activity_enabled);
    help_marker("Keeps an event only if enough pixels of its neighbourhood fired within the time window: removes "
                "isolated background-noise events, keeps the spatially correlated events of moving particles. "
                "Choose the window from the time a particle image takes to cross a pixel plus the pixel response "
                "time (see View > PTV planner).");
    changed |= slider_us("Time window##act", f.activity_window_us, 1, 100000);
    changed |= ImGui::SliderInt("Radius##act", &f.activity_radius, 1, 3, "%d px");
    const int max_neighbors = (2 * f.activity_radius + 1) * (2 * f.activity_radius + 1) - 1;
    f.activity_min_neighbors = std::clamp(f.activity_min_neighbors, 1, max_neighbors);
    changed |= ImGui::SliderInt("Min. neighbours##act", &f.activity_min_neighbors, 1, max_neighbors);
    changed |= ImGui::Checkbox("Same polarity only##act", &f.activity_same_polarity);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Look ahead##act", &f.activity_look_ahead);
    help_marker("Counts neighbours firing up to the time window after the event too (+/- window), at the cost of "
                "a delay of one window. Without it, the first event of every particle neighbourhood is dropped, "
                "which biases arrival times late.");

    ImGui::SeparatorText("Polarity");
    bool drop_on = !f.keep_on, drop_off = !f.keep_off;
    changed |= ImGui::Checkbox("Filter ON events", &drop_on);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Filter OFF events", &drop_off);
    f.keep_on          = !drop_on;
    f.keep_off         = !drop_off;
    f.polarity_enabled = drop_on || drop_off;

    ImGui::SeparatorText("Event trail filter");
    changed |= ImGui::Checkbox("Enable##swtrail", &f.trail_enabled);
    if (ImGui::BeginCombo("Type##swtrail", to_string(f.trail_mode))) {
        for (TrailMode m : {TrailMode::Trail, TrailMode::StcCutTrail, TrailMode::StcKeepTrail})
            if (ImGui::Selectable(to_string(m), m == f.trail_mode)) {
                f.trail_mode = m;
                changed      = true;
            }
        ImGui::EndCombo();
    }
    help_marker(trail_help);
    changed |= slider_us("Threshold##swtrail", f.trail_threshold_us, 100, 100000);
    ImGui::SameLine();
    ImGui::TextDisabled("(Ctrl+click to type)");

    ImGui::SeparatorText("Refractory period");
    changed |= ImGui::Checkbox("Enable##refr", &f.refractory_enabled);
    help_marker("Drops any event on a pixel during this period after its previous kept event. Software "
                "approximation of bias_refr: it can only remove events, not restore suppressed ones.");
    changed |= slider_us("Period##refr", f.refractory_us, 10, 100000);

    ImGui::SeparatorText("Event rate control (emulation)");
    changed |= ImGui::Checkbox("Enable##swerc", &f.erc_enabled);
    help_marker("Caps the event count per time window (rate x window), dropping events evenly within each window. "
                "Approximates the sensor ERC for offline testing.");
    changed |= slider_rate("Max event rate##swerc", f.erc_rate, 0.01f, 200.f);
    changed |= slider_us("Window##swerc", f.erc_period_us, 50, 10000);

    ImGui::SeparatorText("Masks");
    changed |= ImGui::Checkbox("Apply masks in software", &f.mask_enabled);
    help_marker("Removes the events of every enabled mask (software polygons, and the ROI / RONI and pixel masks, "
                "which a camera already applies in the sensor and which are emulated on recordings).");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu masked pixels", masked_pixel_count_);

    if (changed)
        push_filter_config();

    const auto stats = pipeline_->stats();
    if (stats.in > 0)
        ImGui::TextDisabled("Kept %.1f %% of %llu events since opening", 100.0 * stats.out / stats.in,
                            static_cast<unsigned long long>(stats.in));
    if (ImGui::SmallButton("Disable all software filters")) {
        const bool masks = f.mask_enabled;
        f                = FilterConfig();
        f.mask_enabled   = masks;
        push_filter_config();
    }
}

void App::section_preview() {
    PreviewSettings &p = preview_;
    bool changed       = false;
    if (ImGui::BeginCombo("Mode", to_string(p.mode))) {
        for (int i = 0; i < static_cast<int>(PreviewMode::Count); ++i) {
            const auto m = static_cast<PreviewMode>(i);
            if (ImGui::Selectable(to_string(m), m == p.mode)) {
                p.mode  = m;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    const bool uses_window = p.mode != PreviewMode::TimeDecay && p.mode != PreviewMode::Integration;
    if (uses_window) {
        float ms = static_cast<float>(p.accumulation_us / 1000.0);
        if (ImGui::SliderFloat("Accumulation", &ms, 0.1f, 1000.f, "%.1f ms", ImGuiSliderFlags_Logarithmic)) {
            p.accumulation_us = static_cast<timestamp>(ms * 1000);
            changed           = true;
        }
        help_marker("Time window of events shown in each frame.");
    } else {
        float ms = static_cast<float>(p.decay_us / 1000.0);
        if (ImGui::SliderFloat("Decay time", &ms, 0.1f, 2000.f, "%.1f ms", ImGuiSliderFlags_Logarithmic)) {
            p.decay_us = static_cast<timestamp>(ms * 1000);
            changed    = true;
        }
    }
    if (p.mode == PreviewMode::Histogram || p.mode == PreviewMode::Difference)
        changed |= ImGui::SliderInt("Saturation", &p.histo_saturation, 1, 64, "%d events");
    if (p.mode == PreviewMode::Integration)
        changed |= ImGui::SliderFloat("Contrast step", &p.integration_step, 0.01f, 1.f, "%.2f");
    if (p.mode != PreviewMode::TimeSurface && p.mode != PreviewMode::Integration) {
        int pal = static_cast<int>(p.palette);
        if (ImGui::Combo("Palette", &pal, "Dark\0Light\0Gray\0")) {
            p.palette = static_cast<Palette>(pal);
            changed   = true;
        }
    }
    if (changed)
        push_preview_settings();
    ImGui::Checkbox("Freeze", &freeze_);
    ImGui::SameLine();
    ImGui::Checkbox("Rate overlay", &show_overlay_);
    ImGui::SameLine();
    ImGui::Checkbox("Mask fill", &show_mask_fill_);
    ImGui::SameLine();
    ImGui::Checkbox("Measure", &measure_);
    ImGui::SetItemTooltip("Measure on the preview (M): click a first pixel, then a second one (Shift: horizontal /\n"
                          "vertical). Step the time in between (arrow keys) for the pixel speed, from the times the\n"
                          "points were set at. Right click or Esc clears. Mask editing is off meanwhile.");
    ImGui::TextDisabled("Wheel: zoom, middle drag: pan, middle double click: fit.");
}

void App::section_export() {
    if (source_ != SourceKind::File) {
        ImGui::TextDisabled("Open a recording to process and export it.");
        return;
    }
    ImGui::BeginDisabled(export_running_);
    int fmt = static_cast<int>(export_format_);
    if (ImGui::BeginCombo("Format##exp", to_string(export_format_))) {
        for (int i = 0; i < static_cast<int>(ExportFormat::Count); ++i)
            if (ImGui::Selectable(to_string(static_cast<ExportFormat>(i)), i == fmt)) {
                export_format_ = static_cast<ExportFormat>(i);
                if (export_format_ != ExportFormat::Images) {
                    auto path = from_utf8(export_path_);
                    path.replace_extension(default_extension(export_format_));
                    export_path_ = to_utf8(path);
                }
            }
        ImGui::EndCombo();
    }
    const bool images = export_format_ == ExportFormat::Images;
    if (images) {
        ImGui::InputText("Location##expimg", &export_images_location_);
        ImGui::SameLine();
        if (ImGui::SmallButton("...##expimg")) {
            const auto p = pick_folder_dialog("Folder receiving the image folder", from_utf8(export_images_location_));
            if (!p.empty())
                export_images_location_ = to_utf8(p);
        }
        if (export_images_name_auto_)
            export_images_name_ = default_images_name();
        if (ImGui::InputText("Folder name##expimg", &export_images_name_))
            export_images_name_auto_ = false;
        if (!export_images_name_auto_) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Default##expimg"))
                export_images_name_auto_ = true;
        }
        help_marker("By default <recording>_images_<options>, updated with the settings: frame period (dt), preview "
                    "mode if not the CD frame, time range, software filters (s prefix: sAct activity, sRefr "
                    "refractory, sTrail / sSTCcut / sSTCkeep trail, sERC rate control) and \"masked\". "
                    "Editing the name keeps it fixed.");
    } else {
        ImGui::InputText("Output##exp", &export_path_);
        ImGui::SameLine();
        if (ImGui::SmallButton("...##exp")) {
            const auto ext = default_extension(export_format_);
            const std::string filter = std::string("Output|*") + ext + "|All files|*.*";
            const auto p   = save_file_dialog("Export to", filter.c_str(), from_utf8(export_path_), ext);
            if (!p.empty())
                export_path_ = to_utf8(p);
        }
    }
    ImGui::Checkbox("Apply software filters & masks", &export_apply_filters_);
    help_marker("Uses the current software filter settings (Software filters panel) and the selected mask categories. "
                "Biases and other analog sensor settings cannot be applied after recording. External triggers are "
                "always kept (RAW, HDF5; CSV: <name>_triggers.csv).");
    if (export_apply_filters_) {
        ImGui::TextUnformatted("Masks:");
        const struct {
            const char *label;
            unsigned flag;
        } cats[] = {{"Software##exm", kMaskSoftware}, {"Pixel##exm", kMaskPixels}, {"ROI / RONI##exm", kMaskRoi}};
        for (const auto &c : cats) {
            ImGui::SameLine();
            bool on = (export_mask_categories_ & c.flag) != 0;
            if (ImGui::Checkbox(c.label, &on))
                export_mask_categories_ = on ? (export_mask_categories_ | c.flag) : (export_mask_categories_ & ~c.flag);
        }
    }
    ImGui::Checkbox("Whole recording", &export_whole_file_);
    if (!export_whole_file_) {
        ImGui::DragFloat("Start (s)", &export_start_s_, 0.01f, 0.f, 1e6f, "%.3f");
        ImGui::DragFloat("End (s)", &export_end_s_, 0.01f, 0.f, 1e6f, "%.3f");
        if (ImGui::SmallButton("Start = current position"))
            export_start_s_ = static_cast<float>(player_.current_time() / 1e6);
        ImGui::SameLine();
        if (ImGui::SmallButton("End = current position"))
            export_end_s_ = static_cast<float>(player_.current_time() / 1e6);
    }
    if (export_format_ == ExportFormat::Avi) {
        ImGui::SliderFloat("Video FPS", &export_fps_, 1.f, 240.f, "%.0f");
        ImGui::TextDisabled("Frames use the current preview settings.");
    }
    if (images) {
        PreviewSettings &p = preview_;
        if (p.mode == PreviewMode::TimeDecay || p.mode == PreviewMode::Integration) {
            // these modes have no accumulation window in the Preview panel: the frame period is set here
            float ms = static_cast<float>(p.accumulation_us / 1000.0);
            if (ImGui::SliderFloat("Frame period##expimg", &ms, 0.1f, 1000.f, "%.1f ms", ImGuiSliderFlags_Logarithmic)) {
                p.accumulation_us = std::max<timestamp>(1, static_cast<timestamp>(ms * 1000));
                push_preview_settings();
            }
        }
        const timestamp dt = std::max<timestamp>(1, p.accumulation_us);
        timestamp t_min = 0, t_max = 0;
        const bool known = player_.seek_range(t_min, t_max);
        const double span_us = export_whole_file_ ? static_cast<double>(t_max) :
                                                    (export_end_s_ - export_start_s_) * 1e6;
        if (known && span_us > 0)
            ImGui::TextDisabled("One %s image per %.6g ms of recording (%.4g images/s of recording), ~%.0f images.",
                                to_string(p.mode), dt / 1000.0, 1e6 / dt, std::ceil(span_us / dt));
        else
            ImGui::TextDisabled("One %s image per %.6g ms of recording.", to_string(p.mode), dt / 1000.0);
        ImGui::TextDisabled("Frame period = Preview accumulation time; images use the preview mode and palette.");
        std::error_code ec;
        const auto out = images_output();
        if (std::filesystem::is_directory(out, ec) && !std::filesystem::is_empty(out, ec))
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "The folder exists: its frame_*.png files will be overwritten.");
    }
    ImGui::EndDisabled();

    if (!export_running_) {
        if (ImGui::Button("Export", ImVec2(-1, 0)))
            start_export();
        if (!export_output_.empty() && ImGui::SmallButton(images ? "Show image folder" : "Show exported file"))
            reveal_in_explorer(export_output_);
    } else if (export_progress_) {
        const float f = static_cast<float>(export_progress_->fraction.load());
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%.0f %%  (%llu events)", f * 100,
                      static_cast<unsigned long long>(export_progress_->events_in.load()));
        ImGui::ProgressBar(f, ImVec2(-ImGui::GetFontSize() * 5, 0), overlay);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            export_progress_->cancel = true;
    }
}

// ================================================================================================ masks window

void App::draw_masks_window() {
    if (!ImGui::Begin("Masks")) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!editor_.can_undo());
    if (ImGui::Button("Undo") && editor_.undo())
        masks_changed(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor_.can_redo());
    if (ImGui::Button("Redo") && editor_.redo())
        masks_changed(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load...")) {
        const auto path = open_file_dialog("Load masks", "Mask files|*.masks;*.txt|All files|*.*");
        std::string error;
        if (!path.empty()) {
            editor_.snapshot();
            if (masks_.load(path, &error)) {
                masks_.roi().clamp(width_, height_);
                editor_.select(-1);
                masks_changed(true);
                log("Masks loaded from " + to_utf8(path));
            } else {
                log(error, true);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save...")) {
        const auto path = save_file_dialog("Save masks", "Mask files|*.masks|All files|*.*", "masks.masks", ".masks");
        std::string error;
        if (!path.empty())
            masks_.save(path, &error) ? log("Masks saved to " + to_utf8(path)) : log(error, true);
    }
    ImGui::TextDisabled("%zu pixels masked in total", masked_pixel_count_);

    if (ImGui::CollapsingHeader("ROI / RONI rectangle (sensor)", ImGuiTreeNodeFlags_DefaultOpen))
        masks_section_roi();
    if (ImGui::CollapsingHeader("Pixel masks (sensor)", ImGuiTreeNodeFlags_DefaultOpen))
        masks_section_pixels();
    if (ImGui::CollapsingHeader("Software masks (polygons)", ImGuiTreeNodeFlags_DefaultOpen))
        masks_section_polygons();

    if (ImGui::TreeNode("Mouse & keyboard")) {
        ImGui::TextWrapped(
            "ROI tool: drag on the preview to draw the rectangle, drag inside to move it, drag its edges or corners "
            "to resize it.\n"
            "Pixel tool: click a pixel to mask / unmask it (right click unmasks); the magnifier shows the pixels "
            "around the cursor.\n"
            "Polygon drawing: click to add points; click the first point, double click, right click or Enter to "
            "close; Backspace removes the last point; Esc cancels.\n"
            "Polygon editing: click a polygon to select it; drag a vertex to move it; drag inside to move the "
            "polygon; blue square: scale (Shift: non-uniform); blue circle: rotate (Shift: 15 deg steps); double "
            "click an edge to add a point; right click a vertex to remove it; Ctrl+wheel: scale, Shift+wheel: rotate; "
            "Delete: remove the polygon.\n"
            "Ctrl+Z / Ctrl+Y: undo / redo.");
        ImGui::TreePop();
    }
    ImGui::End();
}

namespace {
// Button selecting a mask editor tool, highlighted while the tool is active.
bool tool_button(const char *label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.3f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.35f, 1));
    }
    const bool clicked = ImGui::Button(label);
    if (active)
        ImGui::PopStyleColor(2);
    return clicked;
}
} // namespace

void App::masks_section_roi() {
    RoiRect &roi    = masks_.roi();
    const bool tool = editor_.tool() == MaskEditor::Tool::Roi;
    if (tool_button(tool ? "Done##roi" : "Draw / edit on preview##roi", tool))
        editor_.set_tool(tool ? MaskEditor::Tool::Polygons : MaskEditor::Tool::Roi);
    help_marker("A single rectangle, set in the sensor (I_ROI) when a camera is open. ROI: only the pixels inside "
                "produce events. RONI: the pixels inside produce no events. Deactivated pixels do no detection, which "
                "also saves bandwidth. On recordings it is emulated in software.");

    const MaskSet before = masks_;
    bool commit          = ImGui::Checkbox("Enabled##roi", &roi.enabled);
    ImGui::SameLine();
    int mode = roi.mode == RoiMode::Roi ? 0 : 1;
    commit |= ImGui::RadioButton("ROI (keep inside)", &mode, 0);
    ImGui::SameLine();
    commit |= ImGui::RadioButton("RONI (mask inside)", &mode, 1);
    roi.mode = mode == 0 ? RoiMode::Roi : RoiMode::Roni;

    int xywh[4] = {roi.x, roi.y, roi.width, roi.height};
    if (ImGui::DragInt4("x y w h##roi", xywh, 1.f, 0, std::max(width_, height_))) {
        roi.x      = xywh[0];
        roi.y      = xywh[1];
        roi.width  = std::max(1, xywh[2]);
        roi.height = std::max(1, xywh[3]);
        roi.clamp(width_, height_);
    }
    if (ImGui::IsItemActivated())
        edit_start_ = before;
    const bool numeric_done = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::SmallButton("Full frame")) {
        roi = RoiRect{roi.enabled, roi.mode, 0, 0, width_, height_};
        commit = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Centered half")) {
        roi    = RoiRect{roi.enabled, roi.mode, width_ / 4, height_ / 4, width_ / 2, height_ / 2};
        commit = true;
    }

    if (commit && !(before == masks_)) {
        editor_.push_undo(before);
        masks_changed(true);
    } else if (numeric_done) {
        editor_.push_undo(edit_start_);
        masks_changed(true);
    } else if (!(before == masks_)) {
        masks_changed(false); // live update while dragging the numbers; sensor updated at the end
    }

    if (!roi.valid())
        ImGui::TextDisabled("No rectangle yet: use the tool and drag on the preview.");
    if (!hw_roi_status_.empty())
        ImGui::TextWrapped("%s", hw_roi_status_.c_str());
}

void App::masks_section_pixels() {
    auto &px        = masks_.pixels();
    const bool tool = editor_.tool() == MaskEditor::Tool::Pixels;
    if (tool_button(tool ? "Done##px" : "Pick pixels on preview##px", tool))
        editor_.set_tool(tool ? MaskEditor::Tool::Polygons : MaskEditor::Tool::Pixels);
    help_marker("Individual pixels masked by the sensor's Digital Event Mask (limited number of slots) when a camera "
                "is open; emulated in software on recordings. Typical use: hot / flickering pixels. Zoom in on the "
                "preview (mouse wheel) for precise picking.");

    const MaskSet before = masks_;
    bool commit          = ImGui::Checkbox("Enabled##px", &px.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu pixels", px.pixels.size(), editor_.pixel_capacity());

    int remove = -1;
    if (!px.pixels.empty() &&
        ImGui::BeginTable("pixels", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>(px.pixels.size() + 1, 7)))) {
        ImGui::TableSetupColumn("x");
        ImGui::TableSetupColumn("y");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(px.pixels.size()); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", px.pixels[i].first);
            ImGui::TableNextColumn();
            ImGui::Text("%u", px.pixels[i].second);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X"))
                remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        px.pixels.erase(px.pixels.begin() + remove);
        commit = true;
    }
    if (!px.pixels.empty() && ImGui::SmallButton("Clear pixels")) {
        px.pixels.clear();
        commit = true;
    }
    if (commit) {
        editor_.push_undo(before);
        masks_changed(true);
    }
    if (px.pixels.empty())
        ImGui::TextDisabled("No masked pixels.");
    if (!hw_pixel_status_.empty())
        ImGui::TextWrapped("%s", hw_pixel_status_.c_str());
    draw_hot_pixel_tool();
}

void App::draw_hot_pixel_tool() {
    if (!ImGui::TreeNodeEx("Hot pixel detection", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::TextWrapped("Measures the event rate of every pixel (before the software filters) and flags pixels much "
                       "more active than their neighbours. Best with the illumination on and no seeding.");
    const auto state = hp_state_.load();
    const bool busy  = state == HotPixelState::Collecting || state == HotPixelState::Scanning;
    ImGui::BeginDisabled(busy);
    ImGui::DragFloat("Duration##hp", &hp_seconds_, 0.1f, 0.f, 600.f, hp_seconds_ <= 0 ? "whole file" : "%.1f s");
    help_marker("Live camera: measurement time. Recording: scanned span from the start (0 = whole file).");
    float min_rate = static_cast<float>(hp_params_.min_rate_hz), ratio = static_cast<float>(hp_params_.local_ratio);
    if (ImGui::SliderFloat("Min. rate##hp", &min_rate, 0.1f, 10000.f, "%.1f Hz", ImGuiSliderFlags_Logarithmic))
        hp_params_.min_rate_hz = min_rate;
    if (ImGui::SliderFloat("Ratio to neighbours##hp", &ratio, 2.f, 1000.f, "x %.1f", ImGuiSliderFlags_Logarithmic))
        hp_params_.local_ratio = ratio;
    ImGui::EndDisabled();

    if (busy) {
        ImGui::ProgressBar(hp_progress_.load(), ImVec2(-ImGui::GetFontSize() * 5, 0),
                           state == HotPixelState::Collecting ? "measuring" : "scanning");
        ImGui::SameLine();
        if (ImGui::Button("Cancel##hp"))
            cancel_hot_pixel_detection();
    } else {
        ImGui::BeginDisabled(source_ == SourceKind::None || (source_ == SourceKind::Camera && !camera_.is_running()));
        if (ImGui::Button(source_ == SourceKind::File ? "Scan recording##hp" : "Measure##hp"))
            start_hot_pixel_detection();
        ImGui::EndDisabled();
    }

    if (state == HotPixelState::Results) {
        ImGui::Text("%zu hot pixels (%.2f s measured)", hp_results_.size(), hp_measured_us_ / 1e6);
        // re-evaluating with new thresholds needs a new measurement: the counts are not kept
        if (!hp_results_.empty()) {
            if (ImGui::BeginTable("hot", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                                  ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7))) {
                ImGui::TableSetupColumn("x");
                ImGui::TableSetupColumn("y");
                ImGui::TableSetupColumn("rate");
                ImGui::TableSetupColumn("x neighbours");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const auto &hp : hp_results_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", hp.x);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", hp.y);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f Hz", hp.rate_hz);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", hp.ratio);
                }
                ImGui::EndTable();
            }
            ImGui::Checkbox("Replace current pixel masks##hp", &hp_replace_);
            const size_t cap = editor_.pixel_capacity();
            const size_t to_sensor = std::min(cap, hp_results_.size());
            char label[96];
            std::snprintf(label, sizeof(label), "Mask them: %zu in the sensor, %zu in software", to_sensor,
                          hp_results_.size() - to_sensor);
            if (ImGui::Button(label))
                apply_hot_pixels();
            help_marker("The most active pixels take the sensor's Digital Event Mask slots (no bandwidth used); the "
                        "rest are masked in software (preview, processed recordings, exports).");
        }
    }
    ImGui::TreePop();
}

void App::masks_section_polygons() {
    if (!editor_.drawing()) {
        if (ImGui::Button("New polygon"))
            editor_.start_drawing();
    } else {
        if (ImGui::Button("Finish") && editor_.draft().is_closed_shape()) {
            editor_.snapshot();
            masks_.add(editor_.draft());
            editor_.cancel_drawing();
            editor_.select(static_cast<int>(masks_.shapes().size()) - 1);
            masks_changed(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            editor_.cancel_drawing();
    }
    if (editor_.tool() != MaskEditor::Tool::Polygons && !editor_.drawing()) {
        ImGui::SameLine();
        if (ImGui::Button("Edit polygons"))
            editor_.set_tool(MaskEditor::Tool::Polygons);
    }
    help_marker("Any shape, any number of pixels. Software only: applied to the preview, processed recordings and "
                "exports (also to recordings after the fact), never to the sensor.");
    if (editor_.drawing())
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Click on the preview to add points (%zu).",
                           editor_.draft().size());

    auto &shapes  = masks_.shapes();
    int to_delete = -1;
    if (!shapes.empty() &&
        ImGui::BeginTable("masks", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>(shapes.size() + 1, 8)))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < static_cast<int>(shapes.size()); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool enabled = shapes[i].enabled;
            if (ImGui::Checkbox("##on", &enabled)) {
                editor_.snapshot();
                shapes[i].enabled = enabled;
                masks_changed(true);
            }
            ImGui::TableNextColumn();
            if (ImGui::Selectable(shapes[i].name.c_str(), editor_.selected() == i)) {
                editor_.select(i);
                if (!editor_.drawing())
                    editor_.set_tool(MaskEditor::Tool::Polygons);
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X"))
                to_delete = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (to_delete >= 0) {
        editor_.snapshot();
        masks_.remove(static_cast<size_t>(to_delete));
        editor_.select(-1);
        masks_changed(true);
    }
    if (shapes.empty())
        ImGui::TextDisabled("No polygons. Click 'New polygon', then click on the preview.");

    const int sel = editor_.selected();
    if (sel >= 0) {
        auto &s = shapes[sel];
        ImGui::SeparatorText("Selected polygon");
        ImGui::InputText("Name", &s.name);
        ImGui::Text("%zu points, %zu pixels", s.polygon.size(), s.polygon.pixel_count(width_, height_));
        const Vec2 c   = s.polygon.centroid();
        auto transform = [&](auto &&fn) {
            editor_.snapshot();
            fn(s.polygon);
            masks_changed(true);
        };
        ImGui::TextUnformatted("Scale");
        for (float f : {0.5f, 0.9f, 0.99f, 1.01f, 1.1f, 2.f}) {
            ImGui::SameLine();
            char label[16];
            std::snprintf(label, sizeof(label), "x%.2g", f);
            if (ImGui::SmallButton(label))
                transform([&](Polygon &p) { p.scale(f, f, c); });
        }
        ImGui::TextUnformatted("Rotate");
        for (float d : {-15.f, -1.f, 1.f, 15.f}) {
            ImGui::SameLine();
            char label[16];
            std::snprintf(label, sizeof(label), "%+.0f deg", d);
            if (ImGui::SmallButton(label))
                transform([&](Polygon &p) { p.rotate(d * 3.14159265f / 180.f, c); });
        }
        ImGui::TextUnformatted("Move");
        const struct {
            const char *id;
            ImGuiDir dir;
            Vec2 d;
        } moves[] = {{"<", ImGuiDir_Left, {-1, 0}}, {">", ImGuiDir_Right, {1, 0}}, {"^", ImGuiDir_Up, {0, -1}},
                     {"v", ImGuiDir_Down, {0, 1}}};
        for (const auto &m : moves) {
            ImGui::SameLine();
            if (ImGui::ArrowButton(m.id, m.dir)) {
                const float step = ImGui::GetIO().KeyShift ? 10.f : 1.f;
                transform([&](Polygon &p) { p.translate(m.d * step); });
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(Shift: 10 px)");
        if (ImGui::Button("Duplicate")) {
            editor_.snapshot();
            Polygon copy = s.polygon;
            copy.translate({10, 10});
            const std::string name = s.name + " copy";
            masks_.add(copy, name);
            editor_.select(static_cast<int>(shapes.size()) - 1);
            masks_changed(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            editor_.snapshot();
            masks_.remove(static_cast<size_t>(sel));
            editor_.select(-1);
            masks_changed(true);
        } else if (ImGui::TreeNode("Points")) {
            auto &pts = s.polygon.points();
            for (int k = 0; k < static_cast<int>(pts.size()); ++k) {
                ImGui::PushID(k);
                const MaskSet before = masks_;
                float xy[2]          = {pts[k].x, pts[k].y};
                if (ImGui::DragFloat2("##pt", xy, 0.25f, -10000.f, 10000.f, "%.1f")) {
                    pts[k] = {xy[0], xy[1]};
                    masks_changed(false);
                }
                if (ImGui::IsItemActivated())
                    edit_start_ = before;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    editor_.push_undo(edit_start_);
                    masks_changed(true);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(pts.size() <= 3);
                const bool remove = ImGui::SmallButton("-");
                ImGui::EndDisabled();
                ImGui::PopID();
                if (remove) {
                    editor_.snapshot();
                    s.polygon.remove_point(static_cast<size_t>(k));
                    masks_changed(true);
                    break;
                }
            }
            ImGui::TreePop();
        }
    }
    auto &soft_px = masks_.software_pixels();
    if (!soft_px.pixels.empty()) {
        ImGui::SeparatorText("Individual pixels (software)");
        const MaskSet before = masks_;
        bool commit          = ImGui::Checkbox("Enabled##spx", &soft_px.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("%zu pixels (e.g. hot pixels beyond the sensor capacity)", soft_px.pixels.size());
        if (ImGui::SmallButton("Clear##spx")) {
            soft_px.pixels.clear();
            commit = true;
        }
        if (commit) {
            editor_.push_undo(before);
            masks_changed(true);
        }
    }
    if (!shapes.empty() && ImGui::SmallButton("Clear all polygons")) {
        editor_.snapshot();
        shapes.clear();
        editor_.select(-1);
        masks_changed(true);
    }
}

// ================================================================================================ PTV planner

void App::draw_ptv_planner() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("PTV planner", &show_ptv_planner_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Order-of-magnitude estimate for event-based particle tracking. Verify against a test "
                       "recording: the event-rate overlay gives the real numbers.");
    PtvSetup &p = ptv_;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 9);
    ImGui::SeparatorText("Setup");
    ImGui::InputDouble("Field of view width (mm)", &p.fov_width_mm, 1, 10, "%.1f");
    ImGui::InputInt("Sensor width (px)", &p.sensor_width_px);
    ImGui::InputInt("ROI width (px)", &p.roi_width_px);
    ImGui::InputInt("ROI height (px)", &p.roi_height_px);
    if (ImGui::SmallButton("Use the ROI rectangle / sensor size")) {
        const auto &r = masks_.roi();
        const bool roi = r.enabled && r.valid() && r.mode == RoiMode::Roi;
        p.sensor_width_px = width_;
        p.roi_width_px    = roi ? r.width : width_;
        p.roi_height_px   = roi ? r.height : height_;
    }
    ImGui::InputDouble("Pixel pitch (um)", &p.pixel_pitch_um, 0.1, 1, "%.2f");
    ImGui::InputDouble("Velocity (m/s)", &p.velocity_m_s, 0.5, 5, "%.2f");
    ImGui::InputDouble("Particle diameter (um)", &p.particle_diam_um, 0.5, 5, "%.1f");
    ImGui::InputDouble("Lens f-number", &p.f_number, 0.5, 1, "%.1f");
    ImGui::InputDouble("Wavelength (nm)", &p.wavelength_nm, 10, 50, "%.0f");
    ImGui::InputDouble("Seeding (particles / pixel)", &p.ppp, 0.0005, 0.005, "%.4f");
    ImGui::InputDouble("Events per pixel crossed", &p.events_per_pixel, 0.5, 1, "%.1f");
    help_marker("Bright particle on a dark background: at least one ON event when the image arrives and one OFF "
                "event when it leaves; more for high-contrast particles.");
    ImGui::InputDouble("Pixel response time (us)", &p.pixel_latency_us, 10, 50, "%.0f");
    help_marker("IMX636 datasheet: < 100 us at 1000 lux; shorter with more light, longer in dim conditions.");
    ImGui::InputDouble("Max event rate (ev/s)", &p.max_event_rate, 1e8, 1e9, "%.3g");
    help_marker("Readout / link limit of the camera (check the datasheet; USB links are lower than the sensor).");
    ImGui::PopItemWidth();
    p.sensor_width_px = std::max(1, p.sensor_width_px);

    const PtvEstimate e = estimate_ptv(p);
    ImGui::SeparatorText("Estimate");
    if (ImGui::BeginTable("ptv", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char *name, const char *fmt, double v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, v);
        };
        row("Scale", "%.1f um/px", e.scale_um_per_px);
        row("Magnification", "%.3f", e.magnification);
        row("Velocity", "%.3f px/us", e.velocity_px_per_us);
        row("Particle image (geometric)", "%.2f um", e.image_diam_geom_um);
        row("Particle image (diffraction)", "%.2f um", e.image_diam_diff_um);
        row("Particle image", "%.2f px", e.image_diam_px);
        row("Pixel dwell time", "%.2f us", e.pixel_dwell_us);
        row("Particles in the ROI", "%.0f", e.particles_in_roi);
        row("Events / s per particle", "%.3g", e.events_per_particle_s);
        row("Total event rate", "%.3g ev/s", e.total_event_rate);
        row("Load vs max rate", "%.2f", e.rate_load);
        row("Max seeding for the bandwidth", "%.5f ppp", e.max_ppp_for_bandwidth);
        row("Displacement during response", "%.1f px", e.latency_blur_px);
        row("Suggested activity window", "%.0f us", e.suggested_activity_window_us);
        ImGui::EndTable();
    }
    if (e.exceeds_bandwidth)
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1),
                           "Event rate above the camera limit (x%.1f): the stream will saturate and drop events. "
                           "Reduce seeding, the ROI height or the particle image size (lower f-number).",
                           e.rate_load);
    if (e.faster_than_pixel)
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "Particles cross a pixel faster than it responds: reduced contrast (faint particles may "
                           "produce no event) and timestamp jitter of about %.0f px along the flow. More light "
                           "shortens the response time.",
                           e.latency_blur_px);
    if (ImGui::Button("Apply the suggested activity filter")) {
        filters_.activity_enabled   = true;
        filters_.activity_window_us = static_cast<timestamp>(e.suggested_activity_window_us);
        push_filter_config();
        log("Activity filter enabled with a " + std::to_string(filters_.activity_window_us) + " us window");
    }
    ImGui::End();
}

// ================================================================================================ log / about

void App::draw_log_window() {
    if (!ImGui::Begin("Log")) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Clear"))
        log_.clear();
    ImGui::Separator();
    ImGui::BeginChild("log_scroll");
    for (const auto &e : log_) {
        ImGui::TextDisabled("%s", e.time.c_str());
        ImGui::SameLine();
        if (e.error)
            ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", e.text.c_str());
        else
            ImGui::TextUnformatted(e.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5)
        ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::End();
}

void App::draw_about() {
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("About EventStudio", &show_about_)) {
        ImGui::TextUnformatted("EventStudio 0.1 - OpenEB 5.2 camera & recording tool");
        ImGui::Separator();
        ImGui::BulletText("Space: play / pause the recording");
        ImGui::BulletText("Ctrl+O: open a recording (or drop a file on the window)");
        ImGui::BulletText("Preview: mouse wheel zoom, middle button pan, middle double click to fit");
        ImGui::BulletText("Masks: see Masks > Mouse & keyboard");
        ImGui::Separator();
        ImGui::TextWrapped("Hardware settings (biases, ERC, trail filter, sensor mask) act on the live sensor. "
                           "Software filters reproduce the digital ones on any stream and can be applied to "
                           "recordings after the fact; biases cannot.");
    }
    ImGui::End();
}

} // namespace evs::app
