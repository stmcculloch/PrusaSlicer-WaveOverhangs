///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FillAuxetic.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../ClipperUtils.hpp"
#include "../PrincipalComponents2D.hpp"
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

inline PointKey point_key(const Point &pt)
{
    return PointKey{ pt.x(), pt.y() };
}

inline bool is_horizontal_segment(const Polyline &polyline)
{
    if (polyline.points.size() < 2)
        return false;

    const Point &a = polyline.points.front();
    const Point &b = polyline.points.back();
    const coord_t dx = std::abs(b.x() - a.x());
    const coord_t dy = std::abs(b.y() - a.y());
    return dy <= std::max<coord_t>(SCALED_EPSILON, dx / 8);
}

inline bool is_horizontal_edge(const Point &a, const Point &b)
{
    const coord_t dx = std::abs(b.x() - a.x());
    const coord_t dy = std::abs(b.y() - a.y());
    return dy <= std::max<coord_t>(SCALED_EPSILON, dx / 8);
}

inline bool point_less_xy(const Point &lhs, const Point &rhs)
{
    return lhs.x() < rhs.x() || (lhs.x() == rhs.x() && lhs.y() < rhs.y());
}

double recommended_transverse_angle_bias(const Points &positions, const std::vector<std::pair<size_t, size_t>> &edges)
{
    if (positions.empty() || edges.empty())
        return 0.;

    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const Point &pt : positions) {
        y_min = std::min(y_min, double(pt.y()));
        y_max = std::max(y_max, double(pt.y()));
    }

    const double y_mid       = 0.5 * (y_min + y_max);
    const double y_half_span = std::max(0.5 * (y_max - y_min), 1e-9);
    const double tol         = std::max(1e-9, 0.03 * (y_max - y_min));

    struct EdgeMid {
        size_t index;
        double y_midpoint;
    };
    std::vector<EdgeMid> angled_edges;
    angled_edges.reserve(edges.size());
    double edge_y_min = std::numeric_limits<double>::infinity();
    double edge_y_max = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto &[a_idx, b_idx] = edges[i];
        const Point &a = positions[a_idx];
        const Point &b = positions[b_idx];
        if (is_horizontal_edge(a, b))
            continue;

        const double mid = 0.5 * (double(a.y()) + double(b.y()));
        angled_edges.push_back({ i, mid });
        edge_y_min = std::min(edge_y_min, mid);
        edge_y_max = std::max(edge_y_max, mid);
    }

    if (angled_edges.empty())
        return 0.;

    std::vector<double> biases;
    biases.reserve(angled_edges.size());
    for (const EdgeMid &edge_mid : angled_edges) {
        if (edge_mid.y_midpoint < edge_y_min + tol && edge_mid.y_midpoint > edge_y_max - tol)
            continue;
        if (edge_mid.y_midpoint > edge_y_min + tol && edge_mid.y_midpoint < edge_y_max - tol)
            continue;

        const auto &[a_idx, b_idx] = edges[edge_mid.index];
        const double y0 = double(positions[a_idx].y());
        const double y1 = double(positions[b_idx].y());
        const double denom = (((y0 - y_mid) * (y0 - y_mid) * (y0 - y_mid)) -
                              ((y1 - y_mid) * (y1 - y_mid) * (y1 - y_mid))) /
                             (y_half_span * y_half_span);
        if (std::abs(denom) > 1e-9)
            biases.push_back(-(y0 - y1) / denom);
    }

    if (biases.empty())
        return 0.;

    std::sort(biases.begin(), biases.end());
    const double median = biases[biases.size() / 2];
    return std::clamp(median, -0.80, 1.20);
}

std::vector<Vec2d> apply_transverse_angle_bias(const Points &positions, double bias_strength)
{
    std::vector<Vec2d> biased;
    biased.reserve(positions.size());
    if (positions.empty())
        return biased;

    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const Point &pt : positions) {
        y_min = std::min(y_min, double(pt.y()));
        y_max = std::max(y_max, double(pt.y()));
    }

    const double y_mid       = 0.5 * (y_min + y_max);
    const double y_half_span = std::max(0.5 * (y_max - y_min), 1e-9);

    for (const Point &pt : positions) {
        const double x    = double(pt.x());
        const double y    = double(pt.y());
        const double eta  = (y - y_mid) / y_half_span;
        const double gain = 1.0 + bias_strength * eta * eta;
        biased.emplace_back(x, y_mid + (y - y_mid) * gain);
    }

    return biased;
}

Polylines chain_zigzag_segments(Polylines segments)
{
    struct SegmentRef {
        size_t index;
        bool   at_front;
    };

    std::unordered_map<PointKey, std::vector<SegmentRef>, PointKeyHash> adjacency;
    adjacency.reserve(segments.size() * 2);
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].points.size() < 2)
            continue;
        adjacency[point_key(segments[i].first_point())].push_back({ i, true });
        adjacency[point_key(segments[i].last_point())].push_back({ i, false });
    }

    std::vector<char> visited(segments.size(), false);
    std::vector<Point> starts;
    starts.reserve(adjacency.size());
    for (const auto &[key, refs] : adjacency)
        if (refs.size() == 1)
            starts.emplace_back(key.x, key.y);
    std::sort(starts.begin(), starts.end(), point_less_xy);

    auto follow_chain = [&](const Point &start_point) -> Polyline {
        Polyline chain;
        chain.points.push_back(start_point);
        Point current = start_point;

        for (;;) {
            auto it = adjacency.find(point_key(current));
            if (it == adjacency.end())
                break;

            size_t next_idx = std::numeric_limits<size_t>::max();
            bool   current_at_front = false;
            for (const SegmentRef &ref : it->second) {
                if (! visited[ref.index]) {
                    next_idx = ref.index;
                    current_at_front = ref.at_front;
                    break;
                }
            }

            if (next_idx == std::numeric_limits<size_t>::max())
                break;

            visited[next_idx] = true;
            const Polyline &segment = segments[next_idx];
            if (current_at_front) {
                chain.append(segment.points.begin() + 1, segment.points.end());
                current = segment.last_point();
            } else {
                for (auto rit = segment.points.rbegin() + 1; rit != segment.points.rend(); ++rit)
                    chain.points.push_back(*rit);
                current = segment.first_point();
            }
        }

        return chain;
    };

    Polylines chained;
    chained.reserve(segments.size());
    for (const Point &start : starts) {
        Polyline chain = follow_chain(start);
        if (chain.points.size() > 1)
            chained.push_back(std::move(chain));
    }

    for (size_t i = 0; i < segments.size(); ++i) {
        if (visited[i] || segments[i].points.size() < 2)
            continue;

        visited[i] = true;
        Polyline chain = segments[i];
        Point current = chain.last_point();
        for (;;) {
            auto it = adjacency.find(point_key(current));
            if (it == adjacency.end())
                break;

            size_t next_idx = std::numeric_limits<size_t>::max();
            bool   current_at_front = false;
            for (const SegmentRef &ref : it->second) {
                if (! visited[ref.index]) {
                    next_idx = ref.index;
                    current_at_front = ref.at_front;
                    break;
                }
            }

            if (next_idx == std::numeric_limits<size_t>::max())
                break;

            visited[next_idx] = true;
            const Polyline &segment = segments[next_idx];
            if (current_at_front) {
                chain.append(segment.points.begin() + 1, segment.points.end());
                current = segment.last_point();
            } else {
                for (auto rit = segment.points.rbegin() + 1; rit != segment.points.rend(); ++rit)
                    chain.points.push_back(*rit);
                current = segment.first_point();
            }
        }

        if (chain.points.size() > 1)
            chained.push_back(std::move(chain));
    }

    std::sort(chained.begin(), chained.end(), [](const Polyline &lhs, const Polyline &rhs) {
        return point_less_xy(lhs.first_point(), rhs.first_point());
    });
    return chained;
}

} // namespace

Polylines FillAuxetic::take_deferred_straights()
{
    Polylines out = std::move(m_deferred_straights);
    m_deferred_straights.clear();
    return out;
}

Polylines FillAuxetic::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    m_deferred_straights.clear();
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
    // Prefer the explicit bridge / anchor-derived direction. If this surface does
    // not carry one, derive a local best-fit axis from the filled region and
    // keep the straight bars parallel to that axis.
    float base_angle = this->angle;
    if (surface->bridge_angle >= 0.) {
        base_angle = float(surface->bridge_angle);
    } else {
        auto [pc1, pc2] = compute_principal_components(to_polygons(surface->expolygon));
        if (pc1 != Vec2f::Zero())
            base_angle = float(std::atan2(pc1.y(), pc1.x()));
    }
    const float pattern_angle = base_angle + float(0.5 * PI);

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

    const double transverse_angle_bias = recommended_transverse_angle_bias(positions, edges);
    const std::vector<Vec2d> biased_positions = apply_transverse_angle_bias(positions, transverse_angle_bias);

    Polylines all_polylines;
    all_polylines.reserve(edges.size());
    for (const auto &[a, b] : edges) {
        all_polylines.emplace_back(
            Point(coord_t(std::llround(biased_positions[a].x())), coord_t(std::llround(biased_positions[a].y()))),
            Point(coord_t(std::llround(biased_positions[b].x())), coord_t(std::llround(biased_positions[b].y())))
        );
    }

    all_polylines = intersection_pl(std::move(all_polylines), rotated_expolygon);

    Polylines zigzag_segments;
    Polylines horizontal_segments;
    zigzag_segments.reserve(all_polylines.size());
    horizontal_segments.reserve(all_polylines.size());
    for (Polyline &polyline : all_polylines) {
        if (polyline.points.size() < 2)
            continue;
        if (is_horizontal_segment(polyline))
            horizontal_segments.push_back(std::move(polyline));
        else
            zigzag_segments.push_back(std::move(polyline));
    }

    std::sort(horizontal_segments.begin(), horizontal_segments.end(), [](const Polyline &lhs, const Polyline &rhs) {
        const Point &la = lhs.first_point();
        const Point &ra = rhs.first_point();
        return la.y() < ra.y() || (la.y() == ra.y() && la.x() < ra.x());
    });

    all_polylines = chain_zigzag_segments(std::move(zigzag_segments));
    m_deferred_straights = std::move(horizontal_segments);

    for (Polyline &polyline : all_polylines)
        polyline.rotate(pattern_angle);
    for (Polyline &polyline : m_deferred_straights)
        polyline.rotate(pattern_angle);

    append(polylines_out, std::move(all_polylines));
    return polylines_out;
}

} // namespace Slic3r
