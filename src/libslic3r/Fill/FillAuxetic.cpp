///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FillAuxetic.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"

namespace Slic3r {
namespace {

struct PointKey {
    coord_t x;
    coord_t y;

    bool operator==(const PointKey &rhs) const
    {
        return x == rhs.x && y == rhs.y;
    }
};

struct PointKeyHash {
    size_t operator()(const PointKey &key) const
    {
        return std::hash<long long>()((long long(key.x) << 32) ^ long long(uint32_t(key.y)));
    }
};

inline size_t merge_vertex(std::unordered_map<PointKey, size_t, PointKeyHash> &vertex_lookup, Points &positions, const Point &pt)
{
    const PointKey key{ pt.x(), pt.y() };
    const auto existing = vertex_lookup.find(key);
    if (existing != vertex_lookup.end())
        return existing->second;

    const size_t index = positions.size();
    vertex_lookup.emplace(key, index);
    positions.push_back(pt);
    return index;
}

inline void add_unique_edge(std::vector<std::pair<size_t, size_t>> &edges,
                            std::unordered_map<uint64_t, bool> &edge_set,
                            size_t a,
                            size_t b)
{
    if (a == b)
        return;

    if (a > b)
        std::swap(a, b);

    const uint64_t key = (uint64_t(a) << 32) | uint64_t(b);
    if (edge_set.emplace(key, true).second)
        edges.emplace_back(a, b);
}

} // namespace

Polylines FillAuxetic::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    if (params.density <= 0.f || surface->expolygon.empty())
        return polylines_out;

    CacheID cache_id{ params.density, this->spacing };
    CacheData &cache = m_cache.try_emplace(cache_id).first->second;
    if (cache.column_step == 0) {
        const coord_t distance = std::max<coord_t>(1, coord_t(scale_(this->spacing) / params.density));
        const coord_t bar_length = distance;
        const coord_t flat_half = std::max<coord_t>(1, bar_length / 2);
        const coord_t half_height = std::max<coord_t>(1, distance / 2);
        const double  shoulder = std::clamp(double(half_height), 0.10 * double(bar_length), 0.35 * double(bar_length));
        const coord_t corner_x = std::max<coord_t>(
            1,
            coord_t(std::llround(std::clamp(double(flat_half) - shoulder, 0.12 * double(bar_length), 0.90 * double(flat_half))))
        );

        cache.column_step = std::max<coord_t>(1, corner_x + flat_half);
        cache.row_step    = std::max<coord_t>(1, distance);
        cache.x_outer     = half_height;
        cache.y_outer     = flat_half;
        cache.y_inner     = corner_x;
    }

    // The prototype defines the auxetic cell with its straight bars horizontal.
    // Align that axis with the bridge / seed-derived straight-line direction.
    const float base_angle = surface->bridge_angle >= 0. ? float(surface->bridge_angle) : this->angle;
    const float pattern_angle = base_angle;

    ExPolygon rotated_expolygon = surface->expolygon;
    rotated_expolygon.rotate(-pattern_angle);

    BoundingBox bounding_box = rotated_expolygon.contour.bounding_box();
    const Point surface_center = bounding_box.center();
    const coord_t flat_half   = cache.y_outer;
    const coord_t half_height = cache.row_step / 2;
    const coord_t corner_x    = cache.y_inner;

    bounding_box.merge(align_to_grid(
        bounding_box.min,
        Point(cache.column_step, cache.row_step),
        surface_center));

    const coord_t x_min = bounding_box.min.x() - cache.column_step - flat_half;
    const coord_t x_max = bounding_box.max.x() + cache.column_step + flat_half;
    const coord_t y_min = bounding_box.min.y() - cache.row_step - half_height;
    const coord_t y_max = bounding_box.max.y() + cache.row_step + half_height;

    const coord_t ref_x = surface_center.x();
    const coord_t ref_y = surface_center.y();

    Points positions;
    positions.reserve(1024);
    std::unordered_map<PointKey, size_t, PointKeyHash> vertex_lookup;
    vertex_lookup.reserve(1024);
    std::vector<std::pair<size_t, size_t>> edges;
    edges.reserve(2048);
    std::unordered_map<uint64_t, bool> edge_set;
    edge_set.reserve(2048);

    for (int64_t column = int64_t(std::floor(double(x_min - ref_x) / cache.column_step)) - 3;
         column <= int64_t(std::ceil(double(x_max - ref_x) / cache.column_step)) + 3;
         ++column) {
        const coord_t center_x = coord_t(std::llround(double(ref_x) + double(column) * double(cache.column_step)));
        const coord_t stagger  = (std::llabs(column) & 1) ? cache.row_step / 2 : 0;

        for (int64_t row = int64_t(std::floor(double(y_min - ref_y - stagger) / cache.row_step)) - 3;
             row <= int64_t(std::ceil(double(y_max - ref_y - stagger) / cache.row_step)) + 3;
             ++row) {
            const coord_t center_y = coord_t(std::llround(double(ref_y + stagger) + double(row) * double(cache.row_step)));

            const std::array<Point, 6> vertices{{
                Point(center_x + corner_x, center_y),
                Point(center_x + flat_half, center_y + half_height),
                Point(center_x - flat_half, center_y + half_height),
                Point(center_x - corner_x, center_y),
                Point(center_x - flat_half, center_y - half_height),
                Point(center_x + flat_half, center_y - half_height)
            }};

            std::array<size_t, 6> indices{};
            for (size_t i = 0; i < vertices.size(); ++i)
                indices[i] = merge_vertex(vertex_lookup, positions, vertices[i]);

            for (size_t i = 0; i < indices.size(); ++i)
                add_unique_edge(edges, edge_set, indices[i], indices[(i + 1) % indices.size()]);
        }
    }

    Polylines all_polylines;
    all_polylines.reserve(edges.size());
    for (const auto &[a, b] : edges)
        all_polylines.emplace_back(positions[a], positions[b]);

    all_polylines = intersection_pl(std::move(all_polylines), rotated_expolygon);
    for (Polyline &polyline : all_polylines)
        polyline.rotate(pattern_angle);

    if (params.dont_connect() || all_polylines.size() <= 1 || all_polylines.size() > 256)
        append(polylines_out, std::move(all_polylines));
    else
        connect_infill(std::move(all_polylines), surface->expolygon, polylines_out, this->spacing, params);

    return polylines_out;
}

} // namespace Slic3r
