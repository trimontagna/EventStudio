#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "core/mask_set.h"

namespace evs::app {

// Mapping between sensor pixel coordinates and screen coordinates of the preview image.
struct ViewTransform {
    ImVec2 origin; // screen position of sensor point (0, 0)
    float scale = 1.f;

    Vec2 to_sensor(ImVec2 s) const { return {(s.x - origin.x) / scale, (s.y - origin.y) / scale}; }
    ImVec2 to_screen(Vec2 p) const { return {origin.x + p.x * scale, origin.y + p.y * scale}; }
};

// Interactive mask editing on top of the preview image. One tool is active at a time:
//
// Polygons     : click a polygon to select it; drag a vertex to move it; drag inside to move the polygon; drag the
//                corner handle to scale (Shift: non-uniform) or the round handle to rotate (Shift: 15 deg steps);
//                double click an edge to insert a point; right click a vertex to delete it; Ctrl+wheel scales,
//                Shift+wheel rotates; Delete removes the selected polygon.
// DrawPolygon  : left click adds a point; click the first point / double click / right click / Enter closes the
//                polygon (>= 3 points); Backspace removes the last point; Esc cancels.
// Roi          : drag on an empty area to draw the rectangle; drag inside to move it; drag an edge / corner handle to
//                resize it.
// Pixels       : left click toggles the pixel under the cursor; right click removes it.
// Ctrl+Z / Ctrl+Y undo / redo in every tool.
class MaskEditor {
public:
    enum class Tool { Polygons, DrawPolygon, Roi, Pixels };

    explicit MaskEditor(MaskSet &masks) : masks_(masks) {}

    struct Result {
        bool changed   = false; // masks were modified this frame
        bool committed = false; // an interaction ended (good time to push masks to the hardware)
    };

    // Handles the mouse / keyboard when `hovered` and draws outlines and handles. Call once per frame.
    Result update(const ViewTransform &view, bool hovered, ImDrawList *draw_list);

    void set_tool(Tool tool);
    Tool tool() const { return tool_; }
    void start_drawing() { set_tool(Tool::DrawPolygon); }
    void cancel_drawing() { set_tool(Tool::Polygons); }
    bool drawing() const { return tool_ == Tool::DrawPolygon; }
    const Polygon &draft() const { return draft_; }

    void set_sensor_size(int width, int height) {
        sensor_w_ = width;
        sensor_h_ = height;
    }
    void set_pixel_capacity(size_t capacity) { pixel_capacity_ = capacity; }
    size_t pixel_capacity() const { return pixel_capacity_; }
    // Pixel under the cursor (Pixels tool), -1 when outside the sensor.
    int hovered_pixel_x() const { return hover_px_; }
    int hovered_pixel_y() const { return hover_py_; }
    // Message for the user (e.g. pixel mask full), cleared when read.
    std::string take_message();

    int selected() const { return selected_; }
    void select(int index);

    // Records the current state for undo; call before modifying the masks from outside the editor.
    void snapshot();
    // Records a given earlier state for undo (edits whose start state was captured by the caller).
    void push_undo(const MaskSet &state);
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    bool show_outlines = true;

private:
    enum class Drag { None, Vertex, Body, Scale, Rotate, RoiNew, RoiMove, RoiResize };

    Result update_draw(const ViewTransform &view, bool hovered);
    Result update_polygons(const ViewTransform &view, bool hovered);
    Result update_roi(const ViewTransform &view, bool hovered);
    Result update_pixels(bool hovered);
    Result end_drag();
    Result finish_draft();
    void draw(const ViewTransform &view, ImDrawList *dl) const;
    bool handle_positions(const ViewTransform &view, ImVec2 &scale_handle, ImVec2 &rotate_handle,
                          ImVec2 &top_center) const;
    int roi_handle_at(const ViewTransform &view, ImVec2 mouse) const; // bit flags: 1 left, 2 top, 4 right, 8 bottom
    int mask_at(Vec2 p) const;

    MaskSet &masks_;
    Tool tool_    = Tool::Polygons;
    int selected_ = -1;
    Polygon draft_;
    Vec2 mouse_sensor_;
    bool mouse_valid_ = false;
    int sensor_w_ = 1280, sensor_h_ = 720;
    size_t pixel_capacity_ = 64;
    int hover_px_ = -1, hover_py_ = -1;
    std::string message_;

    Drag drag_       = Drag::None;
    int drag_vertex_ = -1;
    int roi_edges_   = 0;
    Vec2 drag_start_;
    Vec2 pivot_;
    Polygon drag_orig_;
    RoiRect roi_orig_;
    MaskSet before_drag_;
    int hover_vertex_ = -1;

    std::vector<MaskSet> undo_, redo_;
};

} // namespace evs::app
