#include "core/mask_set.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace evs {

const char *to_string(RoiMode mode) {
    return mode == RoiMode::Roi ? "ROI" : "RONI";
}

void RoiRect::clamp(int sensor_width, int sensor_height) {
    const int x0 = std::clamp(x, 0, sensor_width), y0 = std::clamp(y, 0, sensor_height);
    const int x1 = std::clamp(x + width, 0, sensor_width), y1 = std::clamp(y + height, 0, sensor_height);
    x      = x0;
    y      = y0;
    width  = std::max(0, x1 - x0);
    height = std::max(0, y1 - y0);
}

bool RoiRect::operator==(const RoiRect &o) const {
    return enabled == o.enabled && mode == o.mode && x == o.x && y == o.y && width == o.width && height == o.height;
}

bool PixelMasks::contains(uint32_t x, uint32_t y) const {
    return std::find(pixels.begin(), pixels.end(), std::make_pair(x, y)) != pixels.end();
}

bool PixelMasks::add(uint32_t x, uint32_t y) {
    if (contains(x, y))
        return false;
    pixels.emplace_back(x, y);
    return true;
}

bool PixelMasks::remove(uint32_t x, uint32_t y) {
    auto it = std::find(pixels.begin(), pixels.end(), std::make_pair(x, y));
    if (it == pixels.end())
        return false;
    pixels.erase(it);
    return true;
}

MaskShape &MaskSet::add(Polygon polygon, std::string name) {
    if (name.empty())
        name = next_default_name();
    shapes_.push_back(MaskShape{std::move(name), true, std::move(polygon)});
    return shapes_.back();
}

bool MaskSet::remove(size_t index) {
    if (index >= shapes_.size())
        return false;
    shapes_.erase(shapes_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::string MaskSet::next_default_name() const {
    for (int i = static_cast<int>(shapes_.size()) + 1;; ++i) {
        const std::string candidate = "Mask " + std::to_string(i);
        if (std::none_of(shapes_.begin(), shapes_.end(), [&](const MaskShape &s) { return s.name == candidate; }))
            return candidate;
    }
}

std::vector<uint8_t> MaskSet::build_bitmap(int width, int height, unsigned categories) const {
    std::vector<uint8_t> bitmap(static_cast<size_t>(width) * height, 0);
    if (categories & kMaskSoftware)
        for (const auto &s : shapes_)
            if (s.enabled && s.polygon.is_closed_shape())
                s.polygon.rasterize(bitmap, width, height, 1);
    if ((categories & kMaskSoftware) && software_pixels_.enabled)
        for (const auto &[x, y] : software_pixels_.pixels)
            if (x < static_cast<uint32_t>(width) && y < static_cast<uint32_t>(height))
                bitmap[static_cast<size_t>(y) * width + x] = 1;
    if ((categories & kMaskPixels) && pixels_.enabled)
        for (const auto &[x, y] : pixels_.pixels)
            if (x < static_cast<uint32_t>(width) && y < static_cast<uint32_t>(height))
                bitmap[static_cast<size_t>(y) * width + x] = 1;
    if ((categories & kMaskRoi) && roi_.enabled && roi_.valid()) {
        for (int y = 0; y < height; ++y) {
            uint8_t *row = bitmap.data() + static_cast<size_t>(y) * width;
            for (int x = 0; x < width; ++x)
                if (roi_.inside(x, y) == (roi_.mode == RoiMode::Roni))
                    row[x] = 1;
        }
    }
    if (std::none_of(bitmap.begin(), bitmap.end(), [](uint8_t v) { return v != 0; }))
        bitmap.clear();
    return bitmap;
}

std::vector<std::pair<uint32_t, uint32_t>> MaskSet::masked_pixels(int width, int height, unsigned categories) const {
    std::vector<std::pair<uint32_t, uint32_t>> pixels;
    const auto bitmap = build_bitmap(width, height, categories);
    for (size_t i = 0; i < bitmap.size(); ++i)
        if (bitmap[i])
            pixels.emplace_back(static_cast<uint32_t>(i % width), static_cast<uint32_t>(i / width));
    return pixels;
}

bool MaskSet::save(const std::filesystem::path &path, std::string *error) const {
    std::ofstream out(path);
    if (!out) {
        if (error)
            *error = "Cannot write " + path.string();
        return false;
    }
    out << "# EventStudio masks v2\n" << std::setprecision(9);
    out << "roi " << (roi_.enabled ? 1 : 0) << ' ' << (roi_.mode == RoiMode::Roi ? "roi" : "roni") << ' ' << roi_.x
        << ' ' << roi_.y << ' ' << roi_.width << ' ' << roi_.height << '\n';
    out << "pixels " << (pixels_.enabled ? 1 : 0) << '\n';
    for (const auto &[x, y] : pixels_.pixels)
        out << "px " << x << ' ' << y << '\n';
    out << "spixels " << (software_pixels_.enabled ? 1 : 0) << '\n';
    for (const auto &[x, y] : software_pixels_.pixels)
        out << "spx " << x << ' ' << y << '\n';
    for (const auto &s : shapes_) {
        out << "mask " << (s.enabled ? 1 : 0) << ' ' << s.name << '\n';
        for (const auto &p : s.polygon.points())
            out << "pt " << p.x << ' ' << p.y << '\n';
    }
    return static_cast<bool>(out);
}

bool MaskSet::load(const std::filesystem::path &path, std::string *error) {
    std::ifstream in(path);
    if (!in) {
        if (error)
            *error = "Cannot read " + path.string();
        return false;
    }
    MaskSet loaded;
    std::string line;
    int line_no = 0;
    auto fail   = [&](const char *what) {
        if (error)
            *error = std::string(what) + " at line " + std::to_string(line_no) + " of " + path.string();
        return false;
    };
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string keyword;
        ss >> keyword;
        if (keyword == "mask") {
            int enabled = 1;
            ss >> enabled;
            std::string name;
            std::getline(ss >> std::ws, name);
            loaded.shapes_.push_back(MaskShape{name, enabled != 0, Polygon()});
        } else if (keyword == "pt" && !loaded.shapes_.empty()) {
            Vec2 p;
            if (!(ss >> p.x >> p.y))
                return fail("Invalid point");
            loaded.shapes_.back().polygon.add_point(p);
        } else if (keyword == "roi") {
            int enabled = 0;
            std::string mode;
            RoiRect &r = loaded.roi_;
            if (!(ss >> enabled >> mode >> r.x >> r.y >> r.width >> r.height) || (mode != "roi" && mode != "roni"))
                return fail("Invalid roi");
            r.enabled = enabled != 0;
            r.mode    = mode == "roi" ? RoiMode::Roi : RoiMode::Roni;
        } else if (keyword == "pixels") {
            int enabled = 1;
            if (!(ss >> enabled))
                return fail("Invalid pixels");
            loaded.pixels_.enabled = enabled != 0;
        } else if (keyword == "spixels") {
            int enabled = 1;
            if (!(ss >> enabled))
                return fail("Invalid spixels");
            loaded.software_pixels_.enabled = enabled != 0;
        } else if (keyword == "spx") {
            uint32_t x = 0, y = 0;
            if (!(ss >> x >> y))
                return fail("Invalid pixel");
            loaded.software_pixels_.add(x, y);
        } else if (keyword == "px") {
            uint32_t x = 0, y = 0;
            if (!(ss >> x >> y))
                return fail("Invalid pixel");
            loaded.pixels_.add(x, y);
        } else {
            return fail("Unexpected content");
        }
    }
    *this = std::move(loaded);
    return true;
}

bool MaskSet::operator==(const MaskSet &other) const {
    if (!(roi_ == other.roi_) || !(pixels_ == other.pixels_) || !(software_pixels_ == other.software_pixels_) ||
        shapes_.size() != other.shapes_.size())
        return false;
    for (size_t i = 0; i < shapes_.size(); ++i) {
        const auto &a = shapes_[i], &b = other.shapes_[i];
        if (a.name != b.name || a.enabled != b.enabled || a.polygon.size() != b.polygon.size())
            return false;
        for (size_t k = 0; k < a.polygon.size(); ++k)
            if (a.polygon.points()[k].x != b.polygon.points()[k].x || a.polygon.points()[k].y != b.polygon.points()[k].y)
                return false;
    }
    return true;
}

} // namespace evs
