#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app/app.h"
#include "app/platform.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace evs::app {

namespace {

void upload_texture(unsigned int &tex, int &tex_w, int &tex_h, int w, int h, const uint8_t *rgba, bool nearest) {
    if (tex == 0 || tex_w != w || tex_h != h) {
        if (tex == 0)
            glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        tex_w = w;
        tex_h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

ImTextureID tex_id(unsigned int tex) {
    return (ImTextureID)(intptr_t)tex;
}

} // namespace

void App::upload_frame() {
    if (!renderer_ || (freeze_ && frame_tex_ != 0))
        return;
    renderer_->render(preview_time(), rgba_);
    upload_texture(frame_tex_, tex_w_, tex_h_, renderer_->width(), renderer_->height(), rgba_.data(), true);
}

void App::upload_mask_overlay() {
    mask_overlay_dirty_ = false;
    std::vector<uint8_t> rgba(static_cast<size_t>(width_) * height_ * 4, 0);
    if (mask_bitmap_ && !mask_bitmap_->empty()) {
        const auto &bm = *mask_bitmap_;
        for (size_t i = 0; i < bm.size(); ++i)
            if (bm[i]) {
                rgba[i * 4 + 0] = 255;
                rgba[i * 4 + 1] = 60;
                rgba[i * 4 + 2] = 60;
                rgba[i * 4 + 3] = 110;
            }
    }
    upload_texture(mask_tex_, mask_tex_w_, mask_tex_h_, width_, height_, rgba.data(), true);
}

void App::draw_preview_window() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::Begin("Preview", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    upload_frame();
    if (mask_overlay_dirty_)
        upload_mask_overlay();

    const ImGuiIO &io   = ImGui::GetIO();
    const float bar_h   = source_ == SourceKind::File ? ImGui::GetFrameHeightWithSpacing() * 1.4f : 0.f;
    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const ImVec2 size   = ImVec2(std::max(avail.x, 16.f), std::max(avail.y - bar_h, 16.f));
    const ImVec2 pos    = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    // ---- zoom / pan
    const float fit = std::min(size.x / width_, size.y / height_);
    if (hovered) {
        const bool mask_gesture = editor_.selected() >= 0 && (io.KeyCtrl || io.KeyShift);
        if (io.MouseWheel != 0 && !mask_gesture) {
            const float old_zoom = zoom_;
            zoom_                = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 1.f, 64.f);
            // keep the point under the cursor fixed
            const ImVec2 center(pos.x + size.x / 2 + pan_.x, pos.y + size.y / 2 + pan_.y);
            const float k = zoom_ / old_zoom;
            pan_.x += (io.MousePos.x - center.x) * (1 - k);
            pan_.y += (io.MousePos.y - center.y) * (1 - k);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) {
            pan_.x += io.MouseDelta.x;
            pan_.y += io.MouseDelta.y;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
            zoom_ = 1.f;
            pan_  = ImVec2(0, 0);
        }
    }
    if (zoom_ <= 1.f)
        pan_ = ImVec2(0, 0);
    const float scale = fit * zoom_;
    const ImVec2 img_size(width_ * scale, height_ * scale);
    const ImVec2 origin(pos.x + (size.x - img_size.x) / 2 + pan_.x, pos.y + (size.y - img_size.y) / 2 + pan_.y);
    const ImVec2 img_max(origin.x + img_size.x, origin.y + img_size.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(18, 20, 26, 255));
    if (frame_tex_)
        dl->AddImage(tex_id(frame_tex_), origin, img_max);
    if (show_mask_fill_ && mask_tex_ && masked_pixel_count_ > 0)
        dl->AddImage(tex_id(mask_tex_), origin, img_max);
    dl->AddRect(origin, img_max, IM_COL32(90, 90, 100, 255));

    const ViewTransform view{origin, scale};
    const auto edit = editor_.update(view, hovered, dl);
    if (edit.changed || edit.committed)
        masks_changed(edit.committed);
    const auto message = editor_.take_message();
    if (!message.empty())
        log(message, true);
    if (hovered && editor_.tool() == MaskEditor::Tool::Pixels && editor_.hovered_pixel_x() >= 0)
        draw_pixel_loupe(origin, scale);

    if (show_overlay_)
        draw_overlay(pos, ImVec2(pos.x + size.x, pos.y + size.y));

    if (hovered) {
        const Vec2 p = view.to_sensor(io.MousePos);
        if (p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "x %d  y %d  (zoom %.1fx)", static_cast<int>(p.x), static_cast<int>(p.y),
                          zoom_);
            const ImVec2 tp(pos.x + 8, pos.y + size.y - ImGui::GetTextLineHeight() - 6);
            dl->AddRectFilled(ImVec2(tp.x - 4, tp.y - 2),
                              ImVec2(tp.x + ImGui::CalcTextSize(buf).x + 4, tp.y + ImGui::GetTextLineHeight() + 2),
                              IM_COL32(0, 0, 0, 150), 3.f);
            dl->AddText(tp, IM_COL32(230, 230, 230, 255), buf);
        }
    }
    if (source_ == SourceKind::None) {
        const char *msg = "Open a camera or a recording (File menu or Controls panel)";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + (size.y - ts.y) / 2), IM_COL32(200, 200, 200, 255),
                    msg);
    }
    dl->PopClipRect();

    if (source_ == SourceKind::File)
        draw_playback_bar();
    ImGui::End();
}

// Magnified view of the pixels around the cursor, with a pixel grid, for picking pixel masks.
void App::draw_pixel_loupe(const ImVec2 &, float) {
    constexpr int R  = 8; // (2R+1)^2 pixels
    const int n      = 2 * R + 1;
    const float cell = std::max(10.f, ImGui::GetFontSize() * 0.8f);
    const float size = n * cell;
    const int cx = editor_.hovered_pixel_x(), cy = editor_.hovered_pixel_y();

    ImDrawList *dl      = ImGui::GetWindowDrawList();
    const ImGuiIO &io   = ImGui::GetIO();
    const ImVec2 clip_min = dl->GetClipRectMin(), clip_max = dl->GetClipRectMax();
    const float label_h = ImGui::GetTextLineHeightWithSpacing();
    ImVec2 p(io.MousePos.x + 28, io.MousePos.y + 28);
    if (p.x + size > clip_max.x)
        p.x = io.MousePos.x - 28 - size;
    if (p.y + size + label_h > clip_max.y)
        p.y = io.MousePos.y - 28 - size - label_h;
    p.x = std::max(p.x, clip_min.x);
    p.y = std::max(p.y, clip_min.y);

    dl->AddRectFilled(ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + size + 3, p.y + size + label_h + 3),
                      IM_COL32(10, 10, 14, 235), 4.f);
    // visible part of the (2R+1)^2 window, clipped to the sensor
    const int x0 = std::max(cx - R, 0), y0 = std::max(cy - R, 0);
    const int x1 = std::min(cx + R + 1, width_), y1 = std::min(cy + R + 1, height_);
    const ImVec2 a(p.x + (x0 - (cx - R)) * cell, p.y + (y0 - (cy - R)) * cell);
    const ImVec2 b(a.x + (x1 - x0) * cell, a.y + (y1 - y0) * cell);
    const ImVec2 uv0(static_cast<float>(x0) / width_, static_cast<float>(y0) / height_);
    const ImVec2 uv1(static_cast<float>(x1) / width_, static_cast<float>(y1) / height_);
    if (frame_tex_)
        dl->AddImage((ImTextureID)(intptr_t)frame_tex_, a, b, uv0, uv1);
    if (show_mask_fill_ && mask_tex_ && masked_pixel_count_ > 0)
        dl->AddImage((ImTextureID)(intptr_t)mask_tex_, a, b, uv0, uv1);
    for (int i = 0; i <= n; ++i) {
        dl->AddLine(ImVec2(p.x + i * cell, p.y), ImVec2(p.x + i * cell, p.y + size), IM_COL32(255, 255, 255, 40));
        dl->AddLine(ImVec2(p.x, p.y + i * cell), ImVec2(p.x + size, p.y + i * cell), IM_COL32(255, 255, 255, 40));
    }
    for (const auto &[x, y] : masks_.pixels().pixels) {
        const int gx = static_cast<int>(x) - (cx - R), gy = static_cast<int>(y) - (cy - R);
        if (gx >= 0 && gy >= 0 && gx < n && gy < n)
            dl->AddRect(ImVec2(p.x + gx * cell + 1, p.y + gy * cell + 1),
                        ImVec2(p.x + (gx + 1) * cell - 1, p.y + (gy + 1) * cell - 1), IM_COL32(255, 140, 40, 255), 0.f,
                        0, 2.f);
    }
    dl->AddRect(ImVec2(p.x + R * cell, p.y + R * cell), ImVec2(p.x + (R + 1) * cell, p.y + (R + 1) * cell),
                IM_COL32(255, 255, 0, 255), 0.f, 0, 2.f);
    char label[96];
    const bool masked = masks_.pixels().contains(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy));
    std::snprintf(label, sizeof(label), "x %d  y %d  %s  (%zu/%zu)", cx, cy,
                  masked ? "masked: click to unmask" : "click to mask", masks_.pixels().pixels.size(),
                  editor_.pixel_capacity());
    dl->AddText(ImVec2(p.x, p.y + size + 3), IM_COL32(230, 230, 230, 255), label);
}

void App::draw_overlay(const ImVec2 &area_min, const ImVec2 &) {
    std::vector<std::pair<std::string, ImU32>> lines;
    const ImU32 normal = IM_COL32(235, 235, 235, 255), warn = IM_COL32(255, 170, 80, 255),
                red    = IM_COL32(255, 90, 90, 255);
    char buf[256];
    const bool filtering = pipeline_ && pipeline_->is_active();
    const timestamp now  = last_event_t_.load();
    // Camera -> PC delay, or playback delay behind the requested pace.
    const auto add_delay_line = [&] {
        LatencyMonitor *monitor = active_latency();
        if (!monitor)
            return;
        const auto lat = monitor->stats(LiveCamera::wall_us());
        if (lat.valid) {
            std::snprintf(buf, sizeof(buf), "Delay        %.1f ms  (max 10 s: %.1f ms)", lat.current_ms,
                          lat.recent_max_ms);
            lines.push_back({buf, latency_warn_enabled_ && lat.current_ms > latency_threshold_ms() ? red : normal});
        } else {
            lines.push_back({"Delay        measuring...", IM_COL32(170, 170, 170, 255)});
        }
    };

    if (source_ == SourceKind::Camera) {
        const double data = camera_.data_rate_bytes(), events = camera_.event_rate();
        lines.push_back({"Data rate    " + human_rate(data, "B"), normal});
        lines.push_back({"Event rate   " + human_rate(events, "ev") + "  (mean 10 s: " +
                             human_rate(event_history_.mean_per_second(LiveCamera::wall_us(), 10'000'000), "ev") + ")",
                         normal});
        add_delay_line();
        if (erc_.available && erc_.enabled) {
            const double in = camera_.erc_input_rate(), out = camera_.erc_output_rate();
            if (in > 0) {
                std::snprintf(buf, sizeof(buf), "ERC drop     %.1f %%  (input %s)", 100.0 * std::max(0.0, 1 - out / in),
                              human_rate(in, "ev").c_str());
                lines.push_back({buf, in > out * 1.001 ? warn : normal});
            } else {
                const double load = erc_.rate ? events / erc_.rate : 0;
                std::snprintf(buf, sizeof(buf), "ERC limit    %s  (output at %.0f %% of limit%s)",
                              human_rate(erc_.rate, "ev").c_str(), 100 * load,
                              load > 0.95 ? ", dropping" : "");
                lines.push_back({buf, load > 0.95 ? warn : normal});
                lines.push_back({"ERC drop     n/a (sensor sends no ERC counters)", IM_COL32(170, 170, 170, 255)});
            }
        }
        if (filtering) {
            const double in = in_meter_.rate_per_second(now), out = out_meter_.rate_per_second(now);
            std::snprintf(buf, sizeof(buf), "SW filters   %s  (drop %.1f %%)", human_rate(out, "ev").c_str(),
                          in > 0 ? 100.0 * std::max(0.0, 1 - out / in) : 0.0);
            lines.push_back({buf, normal});
        }
    } else if (source_ == SourceKind::File) {
        timestamp min_t = 0, max_t = 0;
        const bool has_range = player_.seek_range(min_t, max_t);
        lines.push_back({"Time         " + format_time_us(player_.current_time()) +
                             (has_range ? " / " + format_time_us(max_t) : std::string()),
                         normal});
        lines.push_back({"Data rate    " + human_rate(file_bytes_meter_.rate_per_second(now), "B"), normal});
        const double in = in_meter_.rate_per_second(now), out = out_meter_.rate_per_second(now);
        lines.push_back({"Event rate   " + human_rate(in, "ev") + "  (mean 10 s: " +
                             human_rate(event_history_.mean_per_second(now, 10'000'000), "ev") + ")",
                         normal});
        if (player_.speed() > 0)
            add_delay_line();
        else
            lines.push_back({"Delay        n/a (max speed, no pacing)", IM_COL32(170, 170, 170, 255)});
        if (filtering) {
            std::snprintf(buf, sizeof(buf), "SW filters   %s  (drop %.1f %%)", human_rate(out, "ev").c_str(),
                          in > 0 ? 100.0 * std::max(0.0, 1 - out / in) : 0.0);
            lines.push_back({buf, normal});
        }
        lines.push_back({"(recording time)", IM_COL32(170, 170, 170, 255)});
    } else {
        return;
    }
    if (total_triggers_ > 0)
        lines.push_back({"Triggers     " + std::to_string(total_triggers_.load()), normal});
    if (recording()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - record_start_).count();
        lines.push_back({"REC " + format_time_us(static_cast<long long>(elapsed * 1e6)), red});
    }

    ImDrawList *dl     = ImGui::GetWindowDrawList();
    const float lh     = ImGui::GetTextLineHeight();
    float w            = 0;
    for (const auto &l : lines)
        w = std::max(w, ImGui::CalcTextSize(l.first.c_str()).x);
    const ImVec2 p0(area_min.x + 8, area_min.y + 8);
    dl->AddRectFilled(p0, ImVec2(p0.x + w + 16, p0.y + lines.size() * lh + 12), IM_COL32(0, 0, 0, 160), 4.f);
    for (size_t i = 0; i < lines.size(); ++i)
        dl->AddText(ImVec2(p0.x + 8, p0.y + 6 + i * lh), lines[i].second, lines[i].first.c_str());
}

void App::draw_playback_bar() {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
    const bool playing = player_.is_playing();
    if (ImGui::Button(playing ? "Pause" : (player_.at_end() ? "Replay" : "Play"), ImVec2(70, 0)))
        playing ? player_.pause() : player_.play();
    ImGui::SameLine();

    timestamp min_t = 0, max_t = 0;
    const bool seekable = player_.seek_range(min_t, max_t) && max_t > min_t;
    static timestamp drag_value = 0;
    static bool dragging        = false;
    timestamp value             = dragging ? drag_value : player_.current_time();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190);
    ImGui::BeginDisabled(!seekable);
    const std::string label = format_time_us(value) + (seekable ? " / " + format_time_us(max_t) : "  (indexing...)");
    if (ImGui::SliderScalar("##timeline", ImGuiDataType_S64, &value, &min_t, &max_t, label.c_str())) {
        drag_value = value;
        // Preroll so that the paused view shows the accumulated state at the target time.
        const timestamp preroll = std::max(preview_.accumulation_us, preview_.decay_us * 3);
        player_.request_seek(value, preroll);
    }
    dragging = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::SameLine();
    static const float speeds[]      = {0.01f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 0.f};
    static const char *speed_names[] = {"0.01x", "0.1x", "0.25x", "0.5x", "1x", "2x", "5x", "10x", "Max"};
    ImGui::SetNextItemWidth(70);
    if (ImGui::Combo("##speed", &speed_index_, speed_names, IM_ARRAYSIZE(speed_names))) {
        playback_speed_ = speeds[speed_index_];
        player_.set_speed(playback_speed_);
    }
    ImGui::SameLine();
    bool loop = player_.loop();
    if (ImGui::Checkbox("Loop", &loop))
        player_.set_loop(loop);
}

} // namespace evs::app
