#include "app/mask_editor.h"

#include <algorithm>
#include <cmath>

namespace evs::app {

namespace {
constexpr float kHitRadius   = 7.f;  // screen pixels
constexpr float kHandleSize  = 6.f;
constexpr float kRotateReach = 28.f; // distance of the rotation handle above the bounding box
constexpr size_t kMaxUndo    = 200;

const ImU32 kOutline         = IM_COL32(255, 90, 90, 230);
const ImU32 kOutlineDisabled = IM_COL32(160, 160, 160, 160);
const ImU32 kSelected        = IM_COL32(255, 210, 60, 255);
const ImU32 kDraft           = IM_COL32(80, 220, 120, 255);
const ImU32 kRoiColor        = IM_COL32(60, 220, 255, 255);
const ImU32 kRoniColor       = IM_COL32(255, 80, 230, 255);
const ImU32 kPixelColor      = IM_COL32(255, 140, 40, 255);

float dist(ImVec2 a, ImVec2 b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
} // namespace

void MaskEditor::set_tool(Tool tool) {
    tool_  = tool;
    draft_ = Polygon();
    drag_  = Drag::None;
}

std::string MaskEditor::take_message() {
    std::string m;
    std::swap(m, message_);
    return m;
}

void MaskEditor::select(int index) {
    selected_ = (index >= 0 && index < static_cast<int>(masks_.shapes().size())) ? index : -1;
}

void MaskEditor::snapshot() {
    push_undo(masks_);
}

void MaskEditor::push_undo(const MaskSet &state) {
    undo_.push_back(state);
    if (undo_.size() > kMaxUndo)
        undo_.erase(undo_.begin());
    redo_.clear();
}

bool MaskEditor::undo() {
    if (undo_.empty())
        return false;
    redo_.push_back(masks_);
    masks_ = undo_.back();
    undo_.pop_back();
    select(selected_);
    return true;
}

bool MaskEditor::redo() {
    if (redo_.empty())
        return false;
    undo_.push_back(masks_);
    masks_ = redo_.back();
    redo_.pop_back();
    select(selected_);
    return true;
}

int MaskEditor::mask_at(Vec2 p) const {
    const auto &shapes = masks_.shapes();
    if (selected_ >= 0 && shapes[selected_].polygon.contains(p))
        return selected_;
    for (int i = static_cast<int>(shapes.size()) - 1; i >= 0; --i)
        if (shapes[i].polygon.contains(p))
            return i;
    return -1;
}

bool MaskEditor::handle_positions(const ViewTransform &view, ImVec2 &scale_handle, ImVec2 &rotate_handle,
                                  ImVec2 &top_center) const {
    if (selected_ < 0)
        return false;
    const auto &poly = masks_.shapes()[selected_].polygon;
    if (!poly.is_closed_shape())
        return false;
    Vec2 mn, mx;
    poly.bounds(mn, mx);
    const ImVec2 a = view.to_screen(mn), b = view.to_screen(mx);
    scale_handle   = b;
    top_center     = ImVec2((a.x + b.x) / 2, a.y);
    rotate_handle  = ImVec2(top_center.x, a.y - kRotateReach);
    return true;
}

int MaskEditor::roi_handle_at(const ViewTransform &view, ImVec2 m) const {
    const RoiRect &r = masks_.roi();
    if (!r.valid())
        return 0;
    const ImVec2 a = view.to_screen({static_cast<float>(r.x), static_cast<float>(r.y)});
    const ImVec2 b = view.to_screen({static_cast<float>(r.x + r.width), static_cast<float>(r.y + r.height)});
    const float h  = kHitRadius;
    const bool in_y = m.y >= a.y - h && m.y <= b.y + h, in_x = m.x >= a.x - h && m.x <= b.x + h;
    int edges = 0;
    if (in_y && std::abs(m.x - a.x) <= h)
        edges |= 1;
    if (in_x && std::abs(m.y - a.y) <= h)
        edges |= 2;
    if (in_y && std::abs(m.x - b.x) <= h)
        edges |= 4;
    if (in_x && std::abs(m.y - b.y) <= h)
        edges |= 8;
    return edges;
}

MaskEditor::Result MaskEditor::update(const ViewTransform &view, bool hovered, ImDrawList *draw_list) {
    const ImGuiIO &io = ImGui::GetIO();
    mouse_sensor_     = view.to_sensor(io.MousePos);
    mouse_valid_      = hovered;
    select(selected_);
    hover_px_ = hover_py_ = -1;
    if (hovered && mouse_sensor_.x >= 0 && mouse_sensor_.y >= 0 && mouse_sensor_.x < sensor_w_ &&
        mouse_sensor_.y < sensor_h_) {
        hover_px_ = static_cast<int>(mouse_sensor_.x);
        hover_py_ = static_cast<int>(mouse_sensor_.y);
    }

    Result r;
    if ((hovered || drag_ != Drag::None) && tool_ != Tool::DrawPolygon) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && undo())
            r.changed = r.committed = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && redo())
            r.changed = r.committed = true;
    }
    Result step;
    switch (tool_) {
    case Tool::DrawPolygon:
        step = update_draw(view, hovered);
        break;
    case Tool::Polygons:
        step = update_polygons(view, hovered);
        break;
    case Tool::Roi:
        step = update_roi(view, hovered);
        break;
    case Tool::Pixels:
        step = update_pixels(hovered);
        break;
    }
    r.changed |= step.changed;
    r.committed |= step.committed;
    if (draw_list)
        draw(view, draw_list);
    return r;
}

MaskEditor::Result MaskEditor::end_drag() {
    Result r;
    if (!(before_drag_ == masks_)) {
        undo_.push_back(before_drag_);
        if (undo_.size() > kMaxUndo)
            undo_.erase(undo_.begin());
        redo_.clear();
        r.committed = true;
    }
    drag_ = Drag::None;
    return r;
}

// ------------------------------------------------------------------------------------------------ polygon drawing

MaskEditor::Result MaskEditor::finish_draft() {
    Result r;
    if (draft_.is_closed_shape()) {
        snapshot();
        masks_.add(draft_);
        selected_ = static_cast<int>(masks_.shapes().size()) - 1;
        r.changed = r.committed = true;
    }
    set_tool(Tool::Polygons);
    return r;
}

MaskEditor::Result MaskEditor::update_draw(const ViewTransform &view, bool hovered) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        set_tool(Tool::Polygons);
        return {};
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        return finish_draft();
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && draft_.size() > 0)
        draft_.points().pop_back();
    if (!hovered)
        return {};

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        return finish_draft();
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && draft_.size() >= 3)
        return finish_draft(); // the first click of the double click already added the last point
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (draft_.size() >= 3 &&
            dist(view.to_screen(draft_.points().front()), ImGui::GetIO().MousePos) <= kHitRadius)
            return finish_draft();
        draft_.add_point(mouse_sensor_);
    }
    return {};
}

// ------------------------------------------------------------------------------------------------ polygon editing

MaskEditor::Result MaskEditor::update_polygons(const ViewTransform &view, bool hovered) {
    Result r;
    const ImGuiIO &io = ImGui::GetIO();
    const float hit   = kHitRadius / view.scale;
    auto *poly        = selected_ >= 0 ? &masks_.shapes()[selected_].polygon : nullptr;

    if (drag_ != Drag::None) {
        if (!poly) {
            drag_ = Drag::None;
            return r;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            return end_drag();
        const Vec2 m = mouse_sensor_;
        switch (drag_) {
        case Drag::Vertex:
            poly->move_point(static_cast<size_t>(drag_vertex_), m);
            break;
        case Drag::Body:
            *poly = drag_orig_;
            poly->translate(m - drag_start_);
            break;
        case Drag::Scale: {
            const Vec2 a = drag_start_ - pivot_, b = m - pivot_;
            const float la = std::hypot(a.x, a.y), lb = std::hypot(b.x, b.y);
            if (la > 1e-3f) {
                *poly = drag_orig_;
                const float s = std::max(lb / la, 0.01f);
                if (io.KeyShift) // non-uniform when Shift is held
                    poly->scale(std::abs(a.x) > 1e-3f ? std::max(b.x / a.x, 0.01f) : 1.f,
                                std::abs(a.y) > 1e-3f ? std::max(b.y / a.y, 0.01f) : 1.f, pivot_);
                else
                    poly->scale(s, s, pivot_);
            }
            break;
        }
        case Drag::Rotate: {
            const float a0 = std::atan2(drag_start_.y - pivot_.y, drag_start_.x - pivot_.x);
            const float a1 = std::atan2(m.y - pivot_.y, m.x - pivot_.x);
            float angle    = a1 - a0;
            if (io.KeyShift) // snap to 15 degrees
                angle = std::round(angle / (3.14159265f / 12)) * (3.14159265f / 12);
            *poly = drag_orig_;
            poly->rotate(angle, pivot_);
            break;
        }
        default:
            break;
        }
        r.changed = true;
        return r;
    }

    hover_vertex_ = -1;
    if (!hovered)
        return r;

    ImVec2 scale_handle, rotate_handle, top_center;
    const bool has_handles = poly && handle_positions(view, scale_handle, rotate_handle, top_center);
    const bool on_scale    = has_handles && dist(io.MousePos, scale_handle) <= kHitRadius + 2;
    const bool on_rotate   = has_handles && dist(io.MousePos, rotate_handle) <= kHitRadius + 2;
    Vec2 projection;
    const int vertex = poly ? poly->nearest_vertex(mouse_sensor_, hit) : -1;
    const int edge   = poly ? poly->nearest_edge(mouse_sensor_, hit, &projection) : -1;
    hover_vertex_    = vertex;

    if (on_scale || on_rotate || vertex >= 0)
        ImGui::SetMouseCursor(on_rotate ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);

    auto begin_drag = [&](Drag d) {
        drag_        = d;
        drag_start_  = mouse_sensor_;
        drag_orig_   = *poly;
        pivot_       = poly->centroid();
        before_drag_ = masks_;
    };

    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && poly && vertex < 0 && edge >= 0) {
        snapshot();
        poly->insert_point(static_cast<size_t>(edge) + 1, projection);
        r.changed = r.committed = true;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (on_rotate) {
            begin_drag(Drag::Rotate);
        } else if (on_scale) {
            begin_drag(Drag::Scale);
        } else if (vertex >= 0) {
            begin_drag(Drag::Vertex);
            drag_vertex_ = vertex;
        } else {
            select(mask_at(mouse_sensor_));
            poly = selected_ >= 0 ? &masks_.shapes()[selected_].polygon : nullptr;
            if (poly)
                begin_drag(Drag::Body);
        }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && poly && vertex >= 0) {
        if (poly->size() > 3) {
            snapshot();
            poly->remove_point(static_cast<size_t>(vertex));
            r.changed = r.committed = true;
        }
    } else if (poly && io.MouseWheel != 0 && (io.KeyCtrl || io.KeyShift)) {
        snapshot();
        const Vec2 c = poly->centroid();
        if (io.KeyCtrl) {
            const float s = std::pow(1.05f, io.MouseWheel);
            poly->scale(s, s, c);
        } else {
            poly->rotate(io.MouseWheel * 5.f * 3.14159265f / 180.f, c);
        }
        r.changed = r.committed = true;
    } else if (poly && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        snapshot();
        masks_.remove(static_cast<size_t>(selected_));
        selected_ = -1;
        r.changed = r.committed = true;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ ROI rectangle

MaskEditor::Result MaskEditor::update_roi(const ViewTransform &view, bool hovered) {
    Result r;
    RoiRect &roi       = masks_.roi();
    const ImGuiIO &io  = ImGui::GetIO();
    const Vec2 m       = mouse_sensor_;
    auto round_coord   = [](float v) { return static_cast<int>(std::lround(v)); };

    if (drag_ != Drag::None) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (drag_ == Drag::RoiNew && (roi.width < 2 || roi.height < 2)) {
                masks_ = before_drag_; // a click, not a drag: keep the previous rectangle
                drag_  = Drag::None;
                r.changed = true;
                return r;
            }
            return end_drag();
        }
        if (drag_ == Drag::RoiNew) {
            const int x0 = static_cast<int>(std::floor(std::min(drag_start_.x, m.x)));
            const int y0 = static_cast<int>(std::floor(std::min(drag_start_.y, m.y)));
            const int x1 = static_cast<int>(std::ceil(std::max(drag_start_.x, m.x)));
            const int y1 = static_cast<int>(std::ceil(std::max(drag_start_.y, m.y)));
            roi.x = x0;
            roi.y = y0;
            roi.width  = std::max(1, x1 - x0);
            roi.height = std::max(1, y1 - y0);
        } else if (drag_ == Drag::RoiMove) {
            roi   = roi_orig_;
            roi.x = std::clamp(roi_orig_.x + round_coord(m.x - drag_start_.x), 0, std::max(0, sensor_w_ - roi.width));
            roi.y = std::clamp(roi_orig_.y + round_coord(m.y - drag_start_.y), 0, std::max(0, sensor_h_ - roi.height));
        } else if (drag_ == Drag::RoiResize) {
            int x0 = roi_orig_.x, y0 = roi_orig_.y, x1 = roi_orig_.x + roi_orig_.width,
                y1 = roi_orig_.y + roi_orig_.height;
            const int dx = round_coord(m.x - drag_start_.x), dy = round_coord(m.y - drag_start_.y);
            if (roi_edges_ & 1)
                x0 += dx;
            if (roi_edges_ & 4)
                x1 += dx;
            if (roi_edges_ & 2)
                y0 += dy;
            if (roi_edges_ & 8)
                y1 += dy;
            roi.x      = std::min(x0, x1);
            roi.y      = std::min(y0, y1);
            roi.width  = std::max(1, std::abs(x1 - x0));
            roi.height = std::max(1, std::abs(y1 - y0));
        }
        roi.clamp(sensor_w_, sensor_h_);
        r.changed = true;
        return r;
    }
    if (!hovered)
        return r;

    const int edges   = roi_handle_at(view, io.MousePos);
    const bool inside = roi.valid() && roi.inside(static_cast<int>(std::floor(m.x)), static_cast<int>(std::floor(m.y)));
    if (edges == 5 || edges == 10 || edges == 0) {
        if (inside)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (edges == 1 || edges == 4) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if (edges == 2 || edges == 8) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else {
        ImGui::SetMouseCursor((edges == 3 || edges == 12) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        before_drag_ = masks_;
        roi_orig_    = roi;
        drag_start_  = m;
        if (edges != 0 && edges != 5 && edges != 10) {
            drag_      = Drag::RoiResize;
            roi_edges_ = edges;
        } else if (inside) {
            drag_ = Drag::RoiMove;
        } else {
            drag_       = Drag::RoiNew;
            roi.enabled = true;
        }
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ pixel masks

MaskEditor::Result MaskEditor::update_pixels(bool hovered) {
    Result r;
    if (!hovered || hover_px_ < 0)
        return r;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    auto &px          = masks_.pixels();
    const uint32_t x  = static_cast<uint32_t>(hover_px_), y = static_cast<uint32_t>(hover_py_);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (px.contains(x, y)) {
            snapshot();
            px.remove(x, y);
            r.changed = r.committed = true;
        } else if (px.pixels.size() >= pixel_capacity_) {
            message_ = "Pixel mask full: the sensor holds " + std::to_string(pixel_capacity_) + " pixels";
        } else {
            snapshot();
            px.add(x, y);
            px.enabled = true;
            r.changed = r.committed = true;
        }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && px.contains(x, y)) {
        snapshot();
        px.remove(x, y);
        r.changed = r.committed = true;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ drawing

void MaskEditor::draw(const ViewTransform &view, ImDrawList *dl) const {
    const auto &shapes = masks_.shapes();

    // ---- ROI / RONI rectangle
    const RoiRect &roi = masks_.roi();
    if (roi.valid() && (show_outlines || tool_ == Tool::Roi)) {
        const ImVec2 a = view.to_screen({static_cast<float>(roi.x), static_cast<float>(roi.y)});
        const ImVec2 b = view.to_screen({static_cast<float>(roi.x + roi.width), static_cast<float>(roi.y + roi.height)});
        ImU32 col      = roi.mode == RoiMode::Roi ? kRoiColor : kRoniColor;
        if (!roi.enabled)
            col = kOutlineDisabled;
        dl->AddRect(a, b, col, 0.f, 0, tool_ == Tool::Roi ? 2.5f : 1.5f);
        dl->AddText(ImVec2(a.x + 4, a.y + 2), col, to_string(roi.mode));
        if (tool_ == Tool::Roi) {
            const ImVec2 hs[] = {a, b, ImVec2(a.x, b.y), ImVec2(b.x, a.y), ImVec2((a.x + b.x) / 2, a.y),
                                 ImVec2((a.x + b.x) / 2, b.y), ImVec2(a.x, (a.y + b.y) / 2), ImVec2(b.x, (a.y + b.y) / 2)};
            for (const auto &h : hs)
                dl->AddRectFilled(ImVec2(h.x - kHandleSize + 1, h.y - kHandleSize + 1),
                                  ImVec2(h.x + kHandleSize - 1, h.y + kHandleSize - 1), col);
        }
    }

    // ---- pixel masks
    const auto &px = masks_.pixels();
    if (show_outlines || tool_ == Tool::Pixels) {
        const float s = std::max(view.scale, 3.f); // keep single pixels visible when zoomed out
        for (const auto &[x, y] : px.pixels) {
            const ImVec2 c = view.to_screen({x + 0.5f, y + 0.5f});
            dl->AddRect(ImVec2(c.x - s / 2 - 1, c.y - s / 2 - 1), ImVec2(c.x + s / 2 + 1, c.y + s / 2 + 1),
                        px.enabled ? kPixelColor : kOutlineDisabled, 0.f, 0, 1.5f);
        }
    }
    if (tool_ == Tool::Pixels && hover_px_ >= 0) {
        const ImVec2 a = view.to_screen({static_cast<float>(hover_px_), static_cast<float>(hover_py_)});
        const ImVec2 b = view.to_screen({hover_px_ + 1.f, hover_py_ + 1.f});
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(255, 255, 0, 255), 0.f, 0, 1.5f);
    }

    // ---- software polygons
    if (show_outlines) {
        for (int i = 0; i < static_cast<int>(shapes.size()); ++i) {
            const auto &s = shapes[i];
            if (s.polygon.size() < 2 || (i == selected_ && tool_ == Tool::Polygons))
                continue;
            std::vector<ImVec2> pts;
            for (const auto &p : s.polygon.points())
                pts.push_back(view.to_screen(p));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), s.enabled ? kOutline : kOutlineDisabled,
                            ImDrawFlags_Closed, 1.5f);
        }
    }

    if (selected_ >= 0 && tool_ == Tool::Polygons) {
        const auto &s = shapes[selected_];
        std::vector<ImVec2> pts;
        for (const auto &p : s.polygon.points())
            pts.push_back(view.to_screen(p));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kSelected, ImDrawFlags_Closed, 2.5f);
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            const float r = i == hover_vertex_ ? kHandleSize + 2 : kHandleSize - 1;
            dl->AddRectFilled(ImVec2(pts[i].x - r, pts[i].y - r), ImVec2(pts[i].x + r, pts[i].y + r),
                              i == hover_vertex_ ? IM_COL32(255, 255, 255, 255) : kSelected);
        }
        ImVec2 scale_handle, rotate_handle, top_center;
        if (handle_positions(view, scale_handle, rotate_handle, top_center)) {
            Vec2 mn, mx;
            s.polygon.bounds(mn, mx);
            dl->AddRect(view.to_screen(mn), view.to_screen(mx), IM_COL32(255, 210, 60, 90), 0, 0, 1.f);
            dl->AddRectFilled(ImVec2(scale_handle.x - kHandleSize, scale_handle.y - kHandleSize),
                              ImVec2(scale_handle.x + kHandleSize, scale_handle.y + kHandleSize),
                              IM_COL32(80, 170, 255, 255));
            dl->AddLine(top_center, rotate_handle, IM_COL32(80, 170, 255, 200), 1.5f);
            dl->AddCircleFilled(rotate_handle, kHandleSize + 1, IM_COL32(80, 170, 255, 255));
            const ImVec2 c = view.to_screen(s.polygon.centroid());
            dl->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x + 5, c.y), kSelected);
            dl->AddLine(ImVec2(c.x, c.y - 5), ImVec2(c.x, c.y + 5), kSelected);
        }
    }

    if (tool_ == Tool::DrawPolygon) {
        std::vector<ImVec2> pts;
        for (const auto &p : draft_.points())
            pts.push_back(view.to_screen(p));
        if (pts.size() >= 2)
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kDraft, ImDrawFlags_None, 2.f);
        if (!pts.empty() && mouse_valid_) {
            const ImVec2 m = view.to_screen(mouse_sensor_);
            dl->AddLine(pts.back(), m, IM_COL32(80, 220, 120, 150), 1.5f);
            if (pts.size() >= 2)
                dl->AddLine(m, pts.front(), IM_COL32(80, 220, 120, 70), 1.f);
        }
        for (size_t i = 0; i < pts.size(); ++i) {
            const bool closing = i == 0 && pts.size() >= 3 && mouse_valid_ &&
                                 dist(pts[0], ImGui::GetIO().MousePos) <= kHitRadius;
            dl->AddCircleFilled(pts[i], closing ? 7.f : 4.f, kDraft);
        }
    }
}

} // namespace evs::app
