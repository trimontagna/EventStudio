#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/polygon.h"

namespace evs {

// Software mask: user-drawn polygon (any shape, any size). Software only.
struct MaskShape {
    std::string name;
    bool enabled = true;
    Polygon polygon;
};

// Region of Interest / Region of Non-Interest: one rectangle, programmed in the sensor (I_ROI) when a camera is open.
//  ROI : only the pixels inside the rectangle are active.
//  RONI: the pixels inside the rectangle are inactive.
enum class RoiMode { Roi, Roni };
const char *to_string(RoiMode mode);

struct RoiRect {
    bool enabled = false;
    RoiMode mode = RoiMode::Roi;
    int x = 0, y = 0, width = 0, height = 0; // pixels [x, x + width) x [y, y + height)

    bool valid() const { return width > 0 && height > 0; }
    bool inside(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    // Clips the rectangle to the sensor.
    void clamp(int sensor_width, int sensor_height);
    bool operator==(const RoiRect &o) const;
};

// Individual masked pixels, programmed in the sensor's Digital Event Mask (limited slots) when a camera is open.
struct PixelMasks {
    bool enabled = true;
    std::vector<std::pair<uint32_t, uint32_t>> pixels; // (x, y), insertion order

    bool contains(uint32_t x, uint32_t y) const;
    bool add(uint32_t x, uint32_t y);    // false if already present
    bool remove(uint32_t x, uint32_t y); // false if absent
    bool operator==(const PixelMasks &o) const { return enabled == o.enabled && pixels == o.pixels; }
};

// Mask categories, combinable as flags.
enum MaskCategory : unsigned { kMaskSoftware = 1, kMaskPixels = 2, kMaskRoi = 4, kMaskAll = 7 };

// All the masks of a session: the ROI / RONI rectangle, the pixel masks and the software polygons.
class MaskSet {
public:
    std::vector<MaskShape> &shapes() { return shapes_; }
    const std::vector<MaskShape> &shapes() const { return shapes_; }
    RoiRect &roi() { return roi_; }
    const RoiRect &roi() const { return roi_; }
    PixelMasks &pixels() { return pixels_; }
    const PixelMasks &pixels() const { return pixels_; }
    // Individual pixels masked in software only (no capacity limit), e.g. hot pixels beyond the sensor slots.
    // Part of the software category.
    PixelMasks &software_pixels() { return software_pixels_; }
    const PixelMasks &software_pixels() const { return software_pixels_; }

    MaskShape &add(Polygon polygon, std::string name = std::string());
    bool remove(size_t index);
    std::string next_default_name() const;

    // width*height bitmap, 1 for the pixels removed by the enabled masks of the selected categories
    // (empty vector if nothing is masked).
    std::vector<uint8_t> build_bitmap(int width, int height, unsigned categories = kMaskAll) const;
    // Coordinates of all masked pixels (selected categories), row-major order.
    std::vector<std::pair<uint32_t, uint32_t>> masked_pixels(int width, int height,
                                                             unsigned categories = kMaskAll) const;

    // Text format:
    //   # EventStudio masks v2
    //   roi <0|1 enabled> <roi|roni> <x> <y> <width> <height>
    //   pixels <0|1 enabled>
    //   px <x> <y>
    //   spixels <0|1 enabled>
    //   spx <x> <y>
    //   mask <0|1 enabled> <name...>
    //   pt <x> <y>
    // v1 files (polygons only) are accepted.
    bool save(const std::filesystem::path &path, std::string *error = nullptr) const;
    bool load(const std::filesystem::path &path, std::string *error = nullptr);

    bool operator==(const MaskSet &other) const;

private:
    std::vector<MaskShape> shapes_;
    RoiRect roi_;
    PixelMasks pixels_;
    PixelMasks software_pixels_;
};

} // namespace evs
