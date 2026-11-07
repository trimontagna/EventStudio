#include "core/polygon.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace evs {

void Polygon::insert_point(size_t index, Vec2 p) {
    index = std::min(index, points_.size());
    points_.insert(points_.begin() + static_cast<std::ptrdiff_t>(index), p);
}

bool Polygon::remove_point(size_t index) {
    if (index >= points_.size() || points_.size() <= 3)
        return false;
    points_.erase(points_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void Polygon::move_point(size_t index, Vec2 p) {
    if (index < points_.size())
        points_[index] = p;
}

float Polygon::signed_area() const {
    double a       = 0;
    const size_t n = points_.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &p = points_[i], &q = points_[(i + 1) % n];
        a += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    return static_cast<float>(a / 2);
}

Vec2 Polygon::centroid() const {
    const size_t n = points_.size();
    if (n == 0)
        return {};
    double a = 0, cx = 0, cy = 0;
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &p = points_[i], &q = points_[(i + 1) % n];
        const double c = static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
        a += c;
        cx += (static_cast<double>(p.x) + q.x) * c;
        cy += (static_cast<double>(p.y) + q.y) * c;
    }
    if (std::abs(a) < 1e-9) {
        Vec2 m;
        for (const auto &p : points_)
            m = m + p;
        return m * (1.f / static_cast<float>(n));
    }
    a *= 0.5;
    return {static_cast<float>(cx / (6 * a)), static_cast<float>(cy / (6 * a))};
}

void Polygon::bounds(Vec2 &min, Vec2 &max) const {
    min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const auto &p : points_) {
        min.x = std::min(min.x, p.x);
        min.y = std::min(min.y, p.y);
        max.x = std::max(max.x, p.x);
        max.y = std::max(max.y, p.y);
    }
}

void Polygon::translate(Vec2 delta) {
    for (auto &p : points_)
        p = p + delta;
}

void Polygon::scale(float sx, float sy, Vec2 pivot) {
    for (auto &p : points_)
        p = {pivot.x + (p.x - pivot.x) * sx, pivot.y + (p.y - pivot.y) * sy};
}

void Polygon::rotate(float radians, Vec2 pivot) {
    const float c = std::cos(radians), s = std::sin(radians);
    for (auto &p : points_) {
        const float dx = p.x - pivot.x, dy = p.y - pivot.y;
        p              = {pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c};
    }
}

bool Polygon::contains(Vec2 pt) const {
    bool inside    = false;
    const size_t n = points_.size();
    if (n < 3)
        return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2 &a = points_[i], &b = points_[j];
        if ((a.y > pt.y) != (b.y > pt.y)) {
            const float x = a.x + (pt.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (pt.x < x)
                inside = !inside;
        }
    }
    return inside;
}

int Polygon::nearest_vertex(Vec2 p, float max_dist) const {
    int best        = -1;
    float best_dist = max_dist * max_dist;
    for (size_t i = 0; i < points_.size(); ++i) {
        const Vec2 d  = points_[i] - p;
        const float n = d.x * d.x + d.y * d.y;
        if (n <= best_dist) {
            best_dist = n;
            best      = static_cast<int>(i);
        }
    }
    return best;
}

int Polygon::nearest_edge(Vec2 p, float max_dist, Vec2 *projection) const {
    const size_t n = points_.size();
    if (n < 2)
        return -1;
    int best        = -1;
    float best_dist = max_dist * max_dist;
    for (size_t i = 0; i < n; ++i) {
        const Vec2 a = points_[i], b = points_[(i + 1) % n];
        const Vec2 ab = b - a, ap = p - a;
        const float len2 = ab.x * ab.x + ab.y * ab.y;
        const float t    = len2 > 0 ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.f, 1.f) : 0.f;
        const Vec2 proj  = a + ab * t;
        const Vec2 d     = p - proj;
        const float dist = d.x * d.x + d.y * d.y;
        if (dist <= best_dist) {
            best_dist = dist;
            best      = static_cast<int>(i);
            if (projection)
                *projection = proj;
        }
        if (n == 2)
            break; // a 2-point polyline has a single edge
    }
    return best;
}

void Polygon::rasterize(std::vector<uint8_t> &bitmap, int width, int height, uint8_t value) const {
    const size_t n = points_.size();
    if (n < 3 || width <= 0 || height <= 0)
        return;
    if (bitmap.size() != static_cast<size_t>(width) * height)
        bitmap.assign(static_cast<size_t>(width) * height, 0);

    Vec2 mn, mx;
    bounds(mn, mx);
    const int y0 = std::max(0, static_cast<int>(std::floor(mn.y)));
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(mx.y)));
    std::vector<float> xs;
    for (int y = y0; y <= y1; ++y) {
        const float cy = y + 0.5f;
        xs.clear();
        // Same edge-crossing rule as contains(): consistent results between rasterization and hit-testing.
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const Vec2 &a = points_[i], &b = points_[j];
            if ((a.y > cy) != (b.y > cy))
                xs.push_back(a.x + (cy - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            // Pixel centers cx with xs[k] <= cx < xs[k+1]; contains() counts a crossing iff cx < x.
            // inside iff (number of crossings with x > cx) is odd  <=>  xs[k] <= cx < xs[k+1].
            int xa = static_cast<int>(std::ceil(xs[k] - 0.5f));
            int xb = static_cast<int>(std::ceil(xs[k + 1] - 0.5f)) - 1;
            xa     = std::max(xa, 0);
            xb     = std::min(xb, width - 1);
            uint8_t *row = bitmap.data() + static_cast<size_t>(y) * width;
            for (int x = xa; x <= xb; ++x)
                row[x] = value;
        }
    }
}

size_t Polygon::pixel_count(int width, int height) const {
    std::vector<uint8_t> bitmap(static_cast<size_t>(width) * height, 0);
    rasterize(bitmap, width, height);
    return static_cast<size_t>(std::count(bitmap.begin(), bitmap.end(), uint8_t(1)));
}

} // namespace evs
