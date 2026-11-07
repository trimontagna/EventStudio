#pragma once

#include <cstdint>
#include <vector>

namespace evs {

struct Vec2 {
    float x = 0, y = 0;
};

inline Vec2 operator+(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}
inline Vec2 operator-(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.y - b.y};
}
inline Vec2 operator*(Vec2 a, float s) {
    return {a.x * s, a.y * s};
}

// Simple polygon in sensor pixel coordinates (pixel (i, j) covers [i, i+1) x [j, j+1)).
// A pixel belongs to the polygon when its center (i + 0.5, j + 0.5) is inside (even-odd rule).
class Polygon {
public:
    Polygon() = default;
    explicit Polygon(std::vector<Vec2> points) : points_(std::move(points)) {}

    const std::vector<Vec2> &points() const { return points_; }
    std::vector<Vec2> &points() { return points_; }
    size_t size() const { return points_.size(); }
    bool is_closed_shape() const { return points_.size() >= 3; }

    void add_point(Vec2 p) { points_.push_back(p); }
    void insert_point(size_t index, Vec2 p);
    // Refuses to go below 3 points; returns false in that case or for an invalid index.
    bool remove_point(size_t index);
    void move_point(size_t index, Vec2 p);

    // Area centroid (vertex mean for degenerate polygons).
    Vec2 centroid() const;
    float signed_area() const;
    void bounds(Vec2 &min, Vec2 &max) const;

    void translate(Vec2 delta);
    void scale(float sx, float sy, Vec2 pivot);
    void rotate(float radians, Vec2 pivot);

    bool contains(Vec2 p) const;

    // Index of the vertex closest to p within max_dist, or -1.
    int nearest_vertex(Vec2 p, float max_dist) const;
    // Index i of the edge (i, i+1) closest to p within max_dist, or -1. `projection` receives the closest point.
    int nearest_edge(Vec2 p, float max_dist, Vec2 *projection = nullptr) const;

    // Sets `value` in the pixels of a width*height bitmap whose center is inside the polygon (scanline fill).
    void rasterize(std::vector<uint8_t> &bitmap, int width, int height, uint8_t value = 1) const;
    size_t pixel_count(int width, int height) const;

private:
    std::vector<Vec2> points_;
};

} // namespace evs
