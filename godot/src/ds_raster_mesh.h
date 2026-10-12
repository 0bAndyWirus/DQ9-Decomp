#pragma once

#include "gpu_scene_frame.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dq9 {

struct RasterMeshVertex {
    dsr::Gpu3DVertex vertex;
    std::uint32_t metadata = 0;
    std::array<float, 3> depth_plane{}; // NDC Z slopes per pixel, Z at this corner
};

// GX attributes interpolate between integer edge-walker spans, not barycentric
// triangle fans. Keep those native sample values, but use the shared 28.4 polygon
// perimeter for continuous coverage. Ceil-rounded row endpoints (especially a
// held terminal row) are not a watertight high-resolution polygon boundary.
// No raster output, texture data or per-pixel colours enter this conversion.
class DsRasterMesh {
    using Values = std::array<float, 7>; // 1/W, Z01, U/W, V/W, RGB6/W
    struct Vertex { std::int32_t x = 0, y = 0; Values value{}; };
    using DepthPlane = std::array<double, 3>; // dZ/dX, dZ/dY, constant; NDC Z
    struct Edge {
        std::int64_t x = 0, error = 0, step_x = 0, numerator = 0, denominator = 1;
        int y = 0, height = 0;
        Values value{}, step{};
    };
    struct Span {
        std::int64_t left_x = 0, right_x = 0;
        Values left{}, right{};
        bool present = false;
    };
    std::array<Span, 192> spans_{};

    static float distance(const dsr::Gpu3DVertex& v, int plane) {
        const auto& p = v.clip;
        switch (plane) {
        case 0: return p[0] + p[3]; case 1: return p[3] - p[0];
        case 2: return p[1] + p[3]; case 3: return p[3] - p[1];
        case 4: return p[2] + p[3]; default: return p[3] - p[2];
        }
    }
    static dsr::Gpu3DVertex mix(const dsr::Gpu3DVertex& a, const dsr::Gpu3DVertex& b, float t) {
        dsr::Gpu3DVertex v;
        for (unsigned i = 0; i < 4; ++i) v.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
        v.u = a.u + (b.u - a.u) * t; v.v = a.v + (b.v - a.v) * t;
        for (unsigned i = 0; i < 3; ++i) v.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
        return v;
    }
    static std::size_t clip(const GpuSceneFrame& frame, const dsr::Gpu3DPolygon& polygon,
                            std::array<dsr::Gpu3DVertex, 16>& vertices) {
        std::size_t count = polygon.count;
        for (std::size_t i = 0; i < count; ++i) vertices[i] = frame.vertices[polygon.index[i]];
        if ((polygon.attr & (1U << 12U)) == 0)
            for (std::size_t i = 0; i < count; ++i) if (distance(vertices[i], 5) < 0) return 0;
        const int planes = (polygon.attr & (1U << 12U)) != 0 ? 6 : 5;
        for (int plane = 0; plane < planes; ++plane) {
            if (count == 0) return 0;
            std::array<dsr::Gpu3DVertex, 16> next{};
            std::size_t n = 0;
            auto previous = vertices[count - 1];
            auto dp = distance(previous, plane);
            for (std::size_t i = 0; i < count; ++i) {
                const auto current = vertices[i];
                const auto dc = distance(current, plane);
                if ((dp >= 0) != (dc >= 0) && std::abs(dp - dc) > 1e-20F && n < next.size())
                    next[n++] = mix(previous, current, dp / (dp - dc));
                if (dc >= 0 && n < next.size()) next[n++] = current;
                previous = current; dp = dc;
            }
            vertices = next; count = n;
        }
        return count;
    }
    static Vertex project(const dsr::Gpu3DVertex& v, std::uint32_t viewport) {
        const float w = v.clip[3] != 0 ? v.clip[3] : 1e-8F;
        const auto x = viewport & 255U, y = (viewport >> 8U) & 255U;
        const float width = static_cast<std::uint8_t>(((viewport >> 16U) & 255U) - x) + 1.0F;
        const float height = static_cast<std::uint8_t>(((viewport >> 24U) & 255U) - y) + 1.0F;
        const float sx = (v.clip[0] + w) / (2 * w) * width + x;
        const float sy = 192 - ((v.clip[1] + w) / (2 * w) * height + (y > 191U ? int(y) - 255 : int(y)));
        return {static_cast<std::int32_t>(std::clamp(sx * 16, -1e6F, 1e6F)),
                static_cast<std::int32_t>(std::clamp(sy * 16, -1e6F, 1e6F)),
                {1 / w, (v.clip[2] + w) / (2 * w), v.u / w, v.v / w,
                 v.color[0] / w, v.color[1] / w, v.color[2] / w}};
    }
    static void divide(std::int64_t n, std::int64_t d, std::int64_t& q, std::int64_t& r) {
        q = n / d; r = n % d;
        if (r < 0) { --q; r += d; }
    }
    static int ceil16(std::int64_t n) {
        std::int64_t q, r; divide(n + 15, 16, q, r); return static_cast<int>(q);
    }
    static bool edge(const Vertex& top, const Vertex& bottom, Edge& e) {
        e = {};
        e.y = ceil16(top.y); e.height = ceil16(bottom.y) - e.y; e.x = ceil16(top.x);
        const auto width = ceil16(bottom.x) - e.x;
        if (e.height != 0 || width != 0) {
            std::int64_t dy = static_cast<std::int64_t>(bottom.y) - top.y;
            const std::int64_t dx = static_cast<std::int64_t>(bottom.x) - top.x;
            if (dy != 0) {
                if (dy < 0) return false;
                divide(dx * 16 * e.y - dx * top.y + dy * top.x - 1 + dy * 16,
                       dy * 16, e.x, e.error);
                divide(dx * 16, dy * 16, e.step_x, e.numerator);
                e.denominator = dy * 16;
            } else { e.step_x = width; dy = 1; }
            const float prestep = static_cast<float>(static_cast<std::int64_t>(e.y) * 16 - top.y) / 16;
            const float per_line = 1 / (static_cast<float>(dy) / 16);
            for (unsigned i = 0; i < 7; ++i) {
                e.step[i] = per_line * (bottom.value[i] - top.value[i]);
                e.value[i] = top.value[i] + prestep * e.step[i];
            }
        } else { e.step_x = 1; e.value = top.value; }
        return true;
    }
    static void step(Edge& e) {
        e.x += e.step_x; ++e.y; --e.height;
        for (unsigned i = 0; i < 7; ++i) e.value[i] += e.step[i];
        e.error += e.numerator;
        if (e.error >= e.denominator) { ++e.x; e.error -= e.denominator; }
    }
    // Both orientations of a shared edge use exactly the same operands. Split
    // at every projected vertex Y as well as native rows, so a terminal triangle
    // reaches its real tip and T-junctions do not join unrelated row endpoints.
    static void boundary(const std::array<Vertex, 10>& vertices, std::size_t count,
                         int y, double& left, double& right) {
        left = 1e30; right = -1e30;
        for (std::size_t i = 0; i < count; ++i) {
            const auto* top = &vertices[i];
            const auto* bottom = &vertices[(i + 1) % count];
            if (top->y > bottom->y) std::swap(top, bottom);
            if (y < top->y || y > bottom->y) continue;
            if (top->y == bottom->y) {
                left = std::min(left, static_cast<double>(std::min(top->x, bottom->x)));
                right = std::max(right, static_cast<double>(std::max(top->x, bottom->x)));
            } else {
                const double x = top->x + static_cast<double>(bottom->x - top->x) *
                    (y - top->y) / (bottom->y - top->y);
                left = std::min(left, x); right = std::max(right, x);
            }
        }
    }
    static Values at_x(const Span& span, double x) {
        const float fraction = static_cast<float>((x / 16 - span.left_x) /
                                                  (span.right_x - span.left_x));
        Values value;
        for (unsigned i = 0; i < value.size(); ++i)
            value[i] = span.left[i] + (span.right[i] - span.left[i]) * fraction;
        return value;
    }
    static Values sample(const std::array<const Span*, 192>& rows, double x, int y) {
        const auto row = std::min(y / 16, 191);
        const auto top = at_x(*rows[row], x);
        if (y % 16 == 0) return top;
        const auto bottom = at_x(*rows[std::min(row + 1, 191)], x);
        const float fraction = static_cast<float>(y % 16) / 16;
        Values value;
        for (unsigned i = 0; i < value.size(); ++i)
            value[i] = top[i] + (bottom[i] - top[i]) * fraction;
        return value;
    }
    static DepthPlane depth_plane(const std::array<Vertex, 10>& vertices, std::size_t count) {
        const auto& a = vertices[0];
        for (std::size_t i = 1; i + 1 < count; ++i) {
            const auto& b = vertices[i];
            const auto& c = vertices[i + 1];
            const double bx = (b.x - a.x) / 16.0, by = (b.y - a.y) / 16.0;
            const double cx = (c.x - a.x) / 16.0, cy = (c.y - a.y) / 16.0;
            const double area = bx * cy - cx * by;
            if (area == 0) continue;
            const double bz = 2.0 * (b.value[1] - a.value[1]);
            const double cz = 2.0 * (c.value[1] - a.value[1]);
            const double dx = (bz * cy - cz * by) / area, dy = (bx * cz - cx * bz) / area;
            return {dx, dy, 2.0 * a.value[1] - 1 - dx * a.x / 16 - dy * a.y / 16};
        }
        return {};
    }
    static void emit(std::vector<RasterMeshVertex>& out, double x16, int y16,
                     const Values& value, std::uint32_t metadata, const DepthPlane& plane) {
        const float w = 1 / value[0];
        const float x = static_cast<float>(x16 / 16), y = static_cast<float>(y16) / 16;
        dsr::Gpu3DVertex v;
        v.clip = {(x / 128 - 1) * w, (1 - y / 96) * w, (value[1] * 2 - 1) * w, w};
        v.u = value[2] * w; v.v = value[3] * w;
        for (unsigned i = 0; i < 3; ++i) v.color[i] = value[4 + i] * w;
        out.push_back({v, metadata, {static_cast<float>(plane[0]), static_cast<float>(plane[1]),
            static_cast<float>(plane[0] * x + plane[1] * y + plane[2])}});
    }
public:
#ifdef DQ9_GPU_SCENE_TESTS
    // A native sample bounding box can omit a fractional tip entirely. Tests
    // use the actual clipped/projected perimeter, plus the documented nudge
    // and conservative margin, rather than relaxing a low-resolution box.
    static std::array<float, 4> projected_bounds(const GpuSceneFrame& frame) {
        if (((frame.clear_color >> 16U) & 31U) != 0) return {0, 0, 256, 192};
        std::array<float, 4> bounds{256, 192, 0, 0};
        std::array<dsr::Gpu3DVertex, 16> clipped;
        for (std::size_t p = 0; p < frame.polygon_count; ++p) {
            const auto& polygon = frame.polygons[p];
            const auto count = clip(frame, polygon, clipped);
            for (std::size_t i = 0; i < count; ++i) {
                const auto v = project(clipped[i], polygon.viewport);
                const float x = v.x / 16.0F, y = v.y / 16.0F;
                bounds[0] = std::min(bounds[0], x); bounds[1] = std::min(bounds[1], y);
                bounds[2] = std::max(bounds[2], x); bounds[3] = std::max(bounds[3], y);
            }
        }
        return bounds;
    }
#endif
    bool append(const GpuSceneFrame& frame, const dsr::Gpu3DPolygon& polygon,
                       bool translucent, std::vector<RasterMeshVertex>& out) {
        if (polygon.count < 3 || polygon.count > 4 || ((polygon.attr >> 16U) & 31U) == 0) return false;
        std::array<dsr::Gpu3DVertex, 16> clipped{};
        const auto count = clip(frame, polygon, clipped);
        if (count < 3 || count > 10) return false;
        std::array<Vertex, 10> vertices{};
        for (std::size_t i = 0; i < count; ++i) vertices[i] = project(clipped[i], polygon.viewport);
        const auto last = count - 1;
        double facing = static_cast<double>(vertices[0].y + vertices[last].y) * (vertices[0].x - vertices[last].x);
        for (std::size_t i = 0; i < last; ++i)
            facing += static_cast<double>(vertices[i + 1].y + vertices[i].y) * (vertices[i + 1].x - vertices[i].x);
        const bool back = facing < 0;
        if ((polygon.attr & (1U << (back ? 6 : 7))) == 0) return false;
        const auto begin = vertices.begin(), end = begin + count;
        if (!back) std::reverse(begin, end);
        for (std::size_t guard = 0; guard < count; ++guard) {
            bool above = false;
            for (std::size_t i = 1; i < count; ++i) above = above || vertices[0].y > vertices[i].y;
            if (!above) break;
            std::rotate(begin, begin + 1, end);
        }
        for (std::size_t guard = 0; guard < count && vertices[0].y == vertices[1].y && vertices[0].x > vertices[1].x; ++guard)
            std::rotate(begin, begin + 1, end);
        auto& spans = spans_;
        for (auto& span : spans) span.present = false;
        std::size_t l = count, r = 0;
        Edge left, right;
        bool next_l = true, next_r = true;
        for (std::size_t guard = 0; guard < count * 2; ++guard) {
            if (next_l && !edge(vertices[l == count ? 0 : l], vertices[l - 1], left)) return false;
            if (next_r && !edge(vertices[r], vertices[r + 1], right)) return false;
            next_l = next_r = false;
            for (int rows = std::min(left.height, right.height); rows > 0; --rows) {
                const int y = left.y;
                if (y >= 0 && y < 192 && left.x < right.x) {
                    spans[y] = {left.x, right.x, left.value, right.value, true};
                }
                step(left); step(right);
            }
            if (right.height == 0) { next_r = true; ++r; }
            if (left.height == 0) { next_l = true; --l; }
            if (l <= r + 1) break;
        }
        const auto metadata = ((polygon.attr >> 24U) & 63U) | ((polygon.attr & (1U << 15U)) != 0 ? 64U : 0U) |
                              (translucent ? 128U : 0U) | (((polygon.attr >> 16U) & 31U) << 8U);
        const auto plane = depth_plane(vertices, count);
        const auto initial_size = out.size();
        const Span* first = nullptr;
        for (const auto& span : spans) if (span.present) { first = &span; break; }
        if (!first) return false;
        std::array<const Span*, 192> rows;
        for (unsigned y = 0; y < rows.size(); ++y) {
            if (spans[y].present) first = &spans[y];
            rows[y] = first;
        }
        // Native row boundaries and the polygon's exact 28.4 vertex events.
        // This is a fixed-size, allocation-free partition of a convex polygon.
        std::array<int, 204> cuts;
        std::size_t cut_count = 0;
        int top = 3072, bottom = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto y = std::clamp(vertices[i].y, 0, 3072);
            top = std::min(top, y); bottom = std::max(bottom, y);
            cuts[cut_count++] = y;
        }
        for (int y = (top / 16 + 1) * 16; y < bottom; y += 16) cuts[cut_count++] = y;
        std::sort(cuts.begin(), cuts.begin() + cut_count);
        cut_count = std::unique(cuts.begin(), cuts.begin() + cut_count) - cuts.begin();
        for (std::size_t i = 1; i < cut_count; ++i) {
            const int y0 = cuts[i - 1], y1 = cuts[i];
            double l0, r0, l1, r1;
            boundary(vertices, count, y0, l0, r0);
            boundary(vertices, count, y1, l1, r1);
            if (l0 > r0 || l1 > r1 || (l0 == r0 && l1 == r1)) continue;
            // UV2.y's spare high bits identify strip corners, for a tiny
            // high-resolution-only conservative overlap in the vertex shader.
            emit(out, l0, y0, sample(rows, l0, y0), metadata, plane);
            emit(out, r0, y0, sample(rows, r0, y0), metadata | (1U << 13U), plane);
            emit(out, r1, y1, sample(rows, r1, y1), metadata | (2U << 13U), plane);
            emit(out, l1, y1, sample(rows, l1, y1), metadata | (3U << 13U), plane);
        }
        return out.size() != initial_size;
    }
};

} // namespace dq9
