///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "WaveOverhangs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

#include "Algorithm/RegionExpansion.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "Line.hpp"
#include "Polyline.hpp"
#include "libslic3r.h"

namespace Slic3r::WaveOverhangs {
namespace {

#define EXTRA_PERIMETER_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

Polylines reconnect_polylines(const Polylines &polylines, double limit_distance)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t i = 0; i < polylines.size(); ++i) {
        if (! polylines[i].empty())
            connected.emplace(i, polylines[i]);
    }

    for (size_t a = 0; a < polylines.size(); ++a) {
        auto base_it = connected.find(a);
        if (base_it == connected.end())
            continue;

        Polyline &base = base_it->second;
        for (size_t b = a + 1; b < polylines.size(); ++b) {
            auto next_it = connected.find(b);
            if (next_it == connected.end())
                continue;

            Polyline &next = next_it->second;
            if ((base.last_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.append(std::move(next));
                connected.erase(next_it);
            } else if ((base.last_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(next_it);
            } else if ((base.first_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                next.append(std::move(base));
                base = std::move(next);
                base.reverse();
                connected.erase(next_it);
            } else if ((base.first_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next));
                base.reverse();
                connected.erase(next_it);
            }
        }
    }

    Polylines result;
    result.reserve(connected.size());
    for (auto &entry : connected)
        result.push_back(std::move(entry.second));
    return result;
}

Polylines generate_wave_overhang_seeds(const ExPolygon &boundary, const Polygons &anchoring, const coord_t seed_expansion)
{
    if (anchoring.empty())
        return {};

    Polylines seeds;
    for (const Algorithm::WaveSeed &seed : Algorithm::wave_seeds(to_expolygons(anchoring), ExPolygons{ boundary }, float(seed_expansion), true)) {
        if (seed.boundary == 0 && seed.path.size() >= 2)
            seeds.emplace_back(seed.path);
    }

    if (seeds.empty())
        seeds = intersection_pl(to_polylines(boundary), offset(anchoring, float(seed_expansion), jtRound, 0.));

    return seeds;
}

void tag_wave_overhang_paths(std::vector<ExtrusionPaths> &wave_paths)
{
    for (ExtrusionPaths &region : wave_paths)
        for (ExtrusionPath &path : region)
            path.attributes_mutable().wave_overhang = true;
}

void append_shell_perimeters(ExtrusionPaths &overhang_region,
                             const Polygons &overhang_to_cover,
                             int             outer_perimeter_count,
                             coord_t         perimeter_spacing,
                             const Flow     &perimeter_flow,
                             double          scaled_resolution)
{
    if (outer_perimeter_count <= 0)
        return;

    Polygons shell_centerline = shrink(overhang_to_cover, std::max<coord_t>(1, perimeter_flow.scaled_width() / 2), jtRound, 0.);
    for (int i = 0; i < outer_perimeter_count && ! shell_centerline.empty(); ++i) {
        Polylines shell_loops = to_polylines(shell_centerline);
        for (Polyline &loop : shell_loops)
            loop.simplify(std::min(0.05 * perimeter_spacing, scaled_resolution));
        shell_loops.erase(
            std::remove_if(shell_loops.begin(), shell_loops.end(), [](const Polyline &loop) { return loop.points.size() < 2; }),
            shell_loops.end());

        if (! shell_loops.empty())
            extrusion_paths_append(overhang_region, shell_loops, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, perimeter_flow });

        shell_centerline = shrink(shell_centerline, perimeter_spacing, jtRound, 0.);
    }
}

void append_wave_fronts(ExtrusionPaths &overhang_region,
                        const Polylines &fronts,
                        const Flow      &wave_flow,
                        coord_t          connector_limit,
                        WaveOverhangPattern wave_pattern)
{
    if (fronts.empty())
        return;

    if (wave_pattern == WaveOverhangPattern::Monotonic) {
        Polylines monotonic_fronts = fronts;
        extrusion_paths_append(overhang_region, monotonic_fronts, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
        return;
    }

    if (wave_pattern == WaveOverhangPattern::ZigZag) {
        Polylines merged;
        merged.reserve(fronts.size());
        for (const Polyline &source_front : fronts) {
            Polyline front = source_front;
            if (front.points.size() < 2)
                continue;

            if (merged.empty()) {
                merged.emplace_back(std::move(front));
                continue;
            }

            Polyline &current = merged.back();
            const double d_keep = (current.last_point() - front.first_point()).cast<double>().norm();
            const double d_flip = (current.last_point() - front.last_point()).cast<double>().norm();
            const double best_d = std::min(d_keep, d_flip);

            if (best_d > connector_limit) {
                merged.emplace_back(std::move(front));
                continue;
            }

            if (d_flip < d_keep)
                front.reverse();
            if (current.last_point() == front.first_point())
                current.append(front.points.begin() + 1, front.points.end());
            else
                current.append(std::move(front));
        }

        extrusion_paths_append(overhang_region, merged, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
        return;
    }

    auto point_at_distance = [](const Polyline &line, double distance) {
        if (line.points.empty())
            return Point{};
        if (distance <= 0. || line.points.size() == 1)
            return line.first_point();

        double walked = 0.;
        for (size_t i = 1; i < line.points.size(); ++i) {
            const Vec2d a = line.points[i - 1].cast<double>();
            const Vec2d b = line.points[i].cast<double>();
            const Vec2d segment = b - a;
            const double segment_length = segment.norm();
            if (segment_length <= 0.)
                continue;
            if (walked + segment_length >= distance) {
                const double t = (distance - walked) / segment_length;
                return Point((a + t * segment).cast<coord_t>());
            }
            walked += segment_length;
        }
        return line.last_point();
    };

    auto support_score = [&point_at_distance](const Polyline &candidate, const ExtrusionPaths &support_paths, coord_t support_reach, coord_t prefix_length) {
        if (support_paths.empty() || candidate.points.size() < 2)
            return -1.;

        const double candidate_length = candidate.length();
        if (candidate_length <= 0.)
            return -1.;

        const double sample_length = std::min(candidate_length, double(std::max<coord_t>(1, prefix_length)));
        const std::array<std::pair<double, double>, 3> samples = {{
            { 0.0,               3.0 },
            { 0.5 * sample_length, 2.0 },
            { sample_length,     1.0 }
        }};

        double best_score = -1.;
        for (auto it = support_paths.rbegin(); it != support_paths.rend(); ++it) {
            if (it->polyline.points.size() < 2)
                continue;

            double score = 0.;
            for (const auto &[distance_along, weight] : samples) {
                Point sample = point_at_distance(candidate, distance_along);
                auto [seg_idx, foot] = foot_pt(it->polyline.points, sample);
                if (seg_idx < 0 || size_t(seg_idx + 1) >= it->polyline.points.size())
                    continue;

                const Point &a = it->polyline.points[size_t(seg_idx)];
                const Point &b = it->polyline.points[size_t(seg_idx + 1)];
                const bool interior_projection = foot != a && foot != b;
                const double distance_to_support = (sample - foot).cast<double>().norm();
                const double normalized_support = std::max(0.0, 1.0 - distance_to_support / double(std::max<coord_t>(1, support_reach)));

                score += weight * (3.0 * normalized_support + (interior_projection ? 1.5 : 0.2));
            }

            best_score = std::max(best_score, score);
        }

        return best_score;
    };

    ExtrusionPaths support_paths = overhang_region;
    const coord_t support_reach = std::max<coord_t>(wave_flow.scaled_width(), connector_limit);
    const coord_t prefix_length = std::max<coord_t>(wave_flow.scaled_width(), connector_limit / 2);

    for (const Polyline &source_front : fronts) {
        Polyline front = source_front;
        if (front.points.size() < 2)
            continue;

        Polyline reversed = front;
        reversed.reverse();
        const double forward_score = support_score(front, support_paths, support_reach, prefix_length);
        const double reverse_score = support_score(reversed, support_paths, support_reach, prefix_length);
        if (reverse_score > forward_score)
            front.reverse();

        overhang_region.emplace_back(front, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
        support_paths.emplace_back(front, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
    }
}

void append_zig_zag_front_levels(ExtrusionPaths               &overhang_region,
                                 const std::vector<Polylines> &front_levels,
                                 const Flow                   &wave_flow,
                                 coord_t                       connector_limit)
{
    if (front_levels.empty())
        return;

    std::vector<std::vector<bool>> used;
    used.reserve(front_levels.size());
    for (const Polylines &level : front_levels)
        used.emplace_back(level.size(), false);

    const double max_connector_distance_sq = double(connector_limit) * double(connector_limit);

    auto append_or_start = [&](Polyline &&front) {
        if (overhang_region.empty()) {
            overhang_region.emplace_back(front, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
            return;
        }

        ExtrusionPath &current = overhang_region.back();
        const double d_keep = (current.last_point() - front.first_point()).cast<double>().squaredNorm();
        const double d_flip = (current.last_point() - front.last_point()).cast<double>().squaredNorm();
        const double best_d = std::min(d_keep, d_flip);

        if (best_d > max_connector_distance_sq) {
            overhang_region.emplace_back(front, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, wave_flow });
            return;
        }

        if (d_flip < d_keep)
            front.reverse();
        if (current.last_point() == front.first_point())
            current.polyline.append(front.points.begin() + 1, front.points.end());
        else
            current.polyline.append(std::move(front));
    };

    std::function<void(size_t, size_t, bool)> follow_branch = [&](size_t level_idx, size_t front_idx, bool reverse_front) {
        used[level_idx][front_idx] = true;
        Polyline current = front_levels[level_idx][front_idx];
        if (current.points.size() < 2)
            return;
        if (reverse_front)
            current.reverse();

        append_or_start(std::move(current));

        for (size_t next_level = level_idx + 1; next_level < front_levels.size(); ++next_level) {
            size_t best_idx = size_t(-1);
            bool   reverse_child = false;
            double best_d = max_connector_distance_sq;

            const Point anchor = overhang_region.back().last_point();
            for (size_t candidate_idx = 0; candidate_idx < front_levels[next_level].size(); ++candidate_idx) {
                if (used[next_level][candidate_idx])
                    continue;

                const Polyline &candidate = front_levels[next_level][candidate_idx];
                if (candidate.points.size() < 2)
                    continue;

                const double d_keep = (anchor - candidate.first_point()).cast<double>().squaredNorm();
                if (d_keep <= best_d) {
                    best_d = d_keep;
                    best_idx = candidate_idx;
                    reverse_child = false;
                }

                const double d_flip = (anchor - candidate.last_point()).cast<double>().squaredNorm();
                if (d_flip <= best_d) {
                    best_d = d_flip;
                    best_idx = candidate_idx;
                    reverse_child = true;
                }
            }

            if (best_idx == size_t(-1) || best_d > max_connector_distance_sq)
                break;

            follow_branch(next_level, best_idx, reverse_child);
            return;
        }
    };

    for (size_t level_idx = 0; level_idx < front_levels.size(); ++level_idx) {
        for (size_t front_idx = 0; front_idx < front_levels[level_idx].size(); ++front_idx) {
            if (! used[level_idx][front_idx])
                follow_branch(level_idx, front_idx, false);
        }
    }
}

} // namespace

std::tuple<std::vector<ExtrusionPaths>, Polygons> generate(
    ExPolygons      infill_area,
    const Polygons &lower_slices_polygons,
    int             perimeter_count,
    int             additional_shell_count,
    double          wave_perimeter_overlap,
    WaveOverhangPattern wave_pattern,
    double          wave_line_spacing,
    double          wave_line_width,
    const Flow     &overhang_flow,
    double          scaled_resolution)
{
    const coord_t base_spacing       = overhang_flow.scaled_spacing();
    const Flow    wave_flow          = wave_line_width > 0. ? overhang_flow.with_width(float(wave_line_width)) : overhang_flow;
    const coord_t perimeter_overlap  = std::max<coord_t>(0, wave_perimeter_overlap > 0. ? coord_t(scale_(wave_perimeter_overlap)) : 0);
    const coord_t wave_spacing       = std::max<coord_t>(1, wave_line_spacing > 0. ? coord_t(scale_(wave_line_spacing)) : base_spacing);
    const coord_t anchors_size       = std::min(coord_t(scale_(EXTERNAL_INFILL_MARGIN)), base_spacing * (perimeter_count + 1));
    const coord_t seed_expansion     = std::max<coord_t>(1, base_spacing / 10);
    const coord_t shell_inner_edge   = additional_shell_count > 0 ? overhang_flow.scaled_width() + (additional_shell_count - 1) * base_spacing : 0;
    const coord_t filled_area_regularization = std::max<coord_t>(1, base_spacing / 2);
    const coord_t zig_zag_connector_limit = std::max<coord_t>(wave_spacing, wave_flow.scaled_width()) + perimeter_overlap;
    const double  min_area_growth    = 0.05 * double(wave_spacing) * double(wave_spacing);

    BoundingBox infill_area_bb       = get_extents(infill_area).inflated(SCALED_EPSILON);
    Polygons    optimized_lower      = ClipperUtils::clip_clipper_polygons_with_subject_bbox(lower_slices_polygons, infill_area_bb);
    Polygons    overhangs            = diff(infill_area, optimized_lower);

    if (overhangs.empty())
        return {};

    Polygons anchors             = intersection(infill_area, optimized_lower);
    Polygons inset_anchors       = diff(anchors, expand(overhangs, anchors_size + 0.1 * overhang_flow.scaled_width(), EXTRA_PERIMETER_OFFSET_PARAMETERS));
    Polygons inset_overhang_area = diff(infill_area, inset_anchors);

    std::vector<ExtrusionPaths> wave_paths;
    Polygons                    filled_area;

    for (const ExPolygon &overhang : union_ex(to_expolygons(inset_overhang_area))) {
        Polygons overhang_to_cover = to_polygons(overhang);
        Polygons wave_cover_area   = additional_shell_count > 0 ?
            shrink(overhang_to_cover, std::max<coord_t>(0, shell_inner_edge - perimeter_overlap), jtRound, 0.) :
            expand(overhang_to_cover, perimeter_overlap, jtRound, 0.);
        Polygons real_overhang     = intersection(wave_cover_area, overhangs);
        if (real_overhang.empty())
            wave_cover_area.clear();

        ExtrusionPaths &overhang_region = wave_paths.emplace_back();

        for (const ExPolygon &wave_cover : union_ex(to_expolygons(wave_cover_area))) {
            Polygons wave_cover_polygons = to_polygons(wave_cover);
            const Polygons &seed_cover_polygons = additional_shell_count > 0 ? overhang_to_cover : wave_cover_polygons;
            const ExPolygon &seed_boundary      = additional_shell_count > 0 ? overhang : wave_cover;
            Polygons anchoring = intersection(expand(seed_cover_polygons, 1.1 * base_spacing, jtRound, 0.), inset_anchors);
            Polylines seeds    = generate_wave_overhang_seeds(seed_boundary, anchoring, seed_expansion);
            if (seeds.empty())
                continue;

            Polygons trim_boundary = shrink(wave_cover_polygons, std::max<coord_t>(1, wave_flow.scaled_width() / 2), jtRound, 0.);
            if (trim_boundary.empty())
                trim_boundary = shrink(wave_cover_polygons, 0.1 * base_spacing);
            if (trim_boundary.empty())
                trim_boundary = wave_cover_polygons;

            const coord_t seed_offset = additional_shell_count > 0 ? shell_inner_edge + seed_expansion : seed_expansion;
            Polygons accumulated_region = intersection(offset(seeds, float(seed_offset), jtRound, 0., ClipperLib::etOpenRound), wave_cover_polygons);
            if (accumulated_region.empty())
                continue;

            std::vector<Polylines> front_levels;
            double accumulated_area = area(accumulated_region);
            for (;;) {
                Polygons next_region = intersection(offset(accumulated_region, float(wave_spacing), jtRound, 0.), wave_cover_polygons);
                if (next_region.empty())
                    break;

                double next_area = area(next_region);
                if (next_area <= accumulated_area + min_area_growth)
                    break;

                Polylines fronts = intersection_pl(to_polylines(next_region), trim_boundary);
                for (Polyline &front : fronts)
                    front.simplify(std::min(0.05 * wave_spacing, scaled_resolution));
                fronts.erase(
                    std::remove_if(fronts.begin(), fronts.end(), [](const Polyline &front) { return front.points.size() < 2; }),
                    fronts.end());
                fronts = reconnect_polylines(fronts, wave_spacing);

                if (! fronts.empty())
                    front_levels.emplace_back(std::move(fronts));

                accumulated_region = std::move(next_region);
                accumulated_area   = next_area;
            }

            if (! front_levels.empty()) {
                if (wave_pattern == WaveOverhangPattern::ZigZag) {
                    append_zig_zag_front_levels(overhang_region, front_levels, wave_flow, zig_zag_connector_limit);
                } else {
                    Polylines collected_fronts;
                    for (const Polylines &level : front_levels)
                        collected_fronts.insert(collected_fronts.end(), level.begin(), level.end());
                    append_wave_fronts(overhang_region, collected_fronts, wave_flow, zig_zag_connector_limit, wave_pattern);
                }
            }
        }

        overhang_region.erase(
            std::remove_if(overhang_region.begin(), overhang_region.end(), [](const ExtrusionPath &path) { return path.empty(); }),
            overhang_region.end());
        append_shell_perimeters(overhang_region, overhang_to_cover, additional_shell_count, base_spacing, overhang_flow, scaled_resolution);
        if (! overhang_region.empty())
            append(filled_area, additional_shell_count > 0 ? overhang_to_cover : wave_cover_area);
        if (overhang_region.empty())
            wave_paths.pop_back();
    }

    tag_wave_overhang_paths(wave_paths);
    return { wave_paths, union_safety_offset(closing_ex(filled_area, float(filled_area_regularization), jtRound, 0.)) };
}

} // namespace Slic3r::WaveOverhangs
