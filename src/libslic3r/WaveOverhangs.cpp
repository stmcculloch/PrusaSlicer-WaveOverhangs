///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "WaveOverhangs.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Algorithm/RegionExpansion.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "Point.hpp"
#include "Polyline.hpp"
#include "libslic3r.h"

namespace Slic3r::WaveOverhangs {
namespace {

constexpr double toolpath_fit_tolerance_factor = 0.035;

#define EXTRA_PERIMETER_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

enum class PropagationMode {
    DirectDistance,
    Cumulative
};

bool try_merge_polylines(Polyline &base, Polyline &other)
{
    if (base.empty() || other.empty())
        return false;

    if (base.last_point() == other.first_point()) {
        polylines_merge(base.points, false, std::move(other.points), true);
    } else if (base.last_point() == other.last_point()) {
        polylines_merge(base.points, false, std::move(other.points), false);
    } else if (base.first_point() == other.last_point()) {
        polylines_merge(base.points, true, std::move(other.points), false);
    } else if (base.first_point() == other.first_point()) {
        polylines_merge(base.points, true, std::move(other.points), true);
    } else {
        return false;
    }

    remove_same_neighbor(base);
    return true;
}

Polylines merge_connected_linework(Polylines polylines)
{
    Polylines merged;
    std::vector<unsigned char> consumed(polylines.size(), 0);
    merged.reserve(polylines.size());

    for (size_t i = 0; i < polylines.size(); ++i) {
        if (consumed[i] || polylines[i].points.size() < 2)
            continue;

        consumed[i] = 1;
        Polyline current = std::move(polylines[i]);

        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t j = 0; j < polylines.size(); ++j) {
                if (consumed[j] || polylines[j].points.size() < 2)
                    continue;
                if (try_merge_polylines(current, polylines[j])) {
                    consumed[j] = 1;
                    changed = true;
                }
            }
        }

        if (current.points.size() >= 2)
            merged.emplace_back(std::move(current));
    }

    remove_degenerate(merged);
    return merged;
}

ExPolygons build_inset_boundary(const ExPolygon &boundary, const coord_t inset)
{
    if (inset <= 0)
        return ExPolygons{ boundary };

    ExPolygons inset_boundary = shrink_ex(ExPolygons{ boundary }, inset, jtRound, 0.);
    return inset_boundary.empty() ? ExPolygons{ boundary } : inset_boundary;
}

std::vector<Polylines> generate_seed_lines(const ExPolygons &boundary_a, const Polygons &anchoring, const coord_t tiny_expansion)
{
    std::vector<Polylines> seeds_per_boundary(boundary_a.size());
    if (boundary_a.empty() || anchoring.empty())
        return seeds_per_boundary;

    for (const Algorithm::WaveSeed &seed : Algorithm::wave_seeds(to_expolygons(anchoring), boundary_a, float(tiny_expansion), true)) {
        if (seed.boundary < boundary_a.size() && seed.path.size() >= 2)
            seeds_per_boundary[seed.boundary].emplace_back(seed.path);
    }

    Polygons expanded_anchoring = offset(anchoring, float(tiny_expansion), jtRound, 0.);
    for (size_t boundary_idx = 0; boundary_idx < boundary_a.size(); ++boundary_idx) {
        if (! seeds_per_boundary[boundary_idx].empty())
            continue;
        seeds_per_boundary[boundary_idx] = intersection_pl(to_polylines(boundary_a[boundary_idx]), expanded_anchoring);
    }

    return seeds_per_boundary;
}

Polylines trim_front_to_toolpaths(const ExPolygons &region, const ExPolygons &trim_boundary, const double fit_tolerance, const double min_length)
{
    Polylines contour_linework = intersection_pl(to_polylines(region), trim_boundary);
    contour_linework = merge_connected_linework(std::move(contour_linework));

    for (Polyline &line : contour_linework) {
        line.simplify(fit_tolerance);
        remove_same_neighbor(line);
    }

    contour_linework.erase(
        std::remove_if(contour_linework.begin(), contour_linework.end(), [min_length](const Polyline &line) {
            return line.points.size() < 2 || line.length() <= min_length;
        }),
        contour_linework.end());

    return contour_linework;
}

void append_fronts_to_extrusions(ExtrusionPaths    &overhang_region,
                                 Polygons          &filled_area,
                                 const ExPolygon   &overhang,
                                 const ExPolygons  &region,
                                 const ExPolygons  &trim_boundary,
                                 const Flow        &overhang_flow,
                                 const double       fit_tolerance,
                                 const double       min_length)
{
    Polylines fronts = trim_front_to_toolpaths(region, trim_boundary, fit_tolerance, min_length);
    if (fronts.empty())
        return;

    extrusion_paths_append(overhang_region, fronts, ExtrusionAttributes{ ExtrusionRole::OverhangPerimeter, overhang_flow });
    append(filled_area, intersection(offset(fronts, float(0.5 * overhang_flow.scaled_width()), jtRound, 0., ClipperLib::etOpenRound), ExPolygons{ overhang }));
}

void generate_direct_toolpath_lines(const ExPolygon  &overhang,
                                    const ExPolygon  &boundary_a,
                                    const ExPolygons &trim_boundary,
                                    const Polylines  &seed_lines,
                                    ExtrusionPaths   &overhang_region,
                                    Polygons         &filled_area,
                                    const Flow       &overhang_flow,
                                    const double      fit_tolerance,
                                    const double      min_length)
{
    const size_t max_iterations = std::max<size_t>(
        3, size_t(std::ceil(get_extents(boundary_a).radius() / std::max(1.0, double(overhang_flow.scaled_spacing())))) + 2);

    for (size_t iteration = 1; iteration <= max_iterations; ++iteration) {
        const float distance = float(iteration * overhang_flow.scaled_spacing());
        ExPolygons region = intersection_ex(
            offset(seed_lines, distance, jtRound, 0., ClipperLib::etOpenRound),
            ExPolygons{ boundary_a });
        if (region.empty())
            break;

        const size_t num_paths_before = overhang_region.size();
        append_fronts_to_extrusions(overhang_region, filled_area, overhang, region, trim_boundary, overhang_flow, fit_tolerance, min_length);
        if (overhang_region.size() == num_paths_before && iteration > 1)
            break;
    }
}

void generate_cumulative_toolpath_lines(const ExPolygon  &overhang,
                                        const ExPolygon  &boundary_a,
                                        const ExPolygons &trim_boundary,
                                        const Polylines  &seed_lines,
                                        const coord_t     line_buffer_eps,
                                        ExtrusionPaths   &overhang_region,
                                        Polygons         &filled_area,
                                        const Flow       &overhang_flow,
                                        const double      fit_tolerance,
                                        const double      min_length,
                                        const double      min_new_area)
{
    ExPolygons accumulated_region = intersection_ex(
        offset(seed_lines, float(line_buffer_eps), jtRound, 0., ClipperLib::etOpenRound),
        ExPolygons{ boundary_a });
    if (accumulated_region.empty())
        return;

    double       accumulated_area = area(accumulated_region);
    const size_t max_iterations = std::max<size_t>(
        3, size_t(std::ceil(get_extents(boundary_a).radius() / std::max(1.0, double(overhang_flow.scaled_spacing())))) + 2);

    for (size_t iteration = 0; iteration < max_iterations; ++iteration) {
        ExPolygons next_region = intersection_ex(
            offset(accumulated_region, float(overhang_flow.scaled_spacing()), jtRound, 0.),
            ExPolygons{ boundary_a });
        if (next_region.empty())
            break;

        double next_area = area(next_region);
        if (next_area <= accumulated_area + min_new_area)
            break;

        append_fronts_to_extrusions(overhang_region, filled_area, overhang, next_region, trim_boundary, overhang_flow, fit_tolerance, min_length);
        accumulated_region = std::move(next_region);
        accumulated_area   = next_area;
    }
}

} // namespace

std::tuple<std::vector<ExtrusionPaths>, Polygons> generate(
    ExPolygons      infill_area,
    const Polygons &lower_slices_polygons,
    int             perimeter_count,
    const Flow     &overhang_flow,
    double          scaled_resolution)
{
    const coord_t anchors_size       = std::min(coord_t(scale_(EXTERNAL_INFILL_MARGIN)), overhang_flow.scaled_spacing() * (perimeter_count + 1));
    const coord_t tiny_expansion     = std::max<coord_t>(1, overhang_flow.scaled_spacing() / 20);
    const coord_t line_buffer_eps    = std::max<coord_t>(1, overhang_flow.scaled_spacing() / 50);
    const coord_t inset_a_distance   = std::max<coord_t>(1, overhang_flow.scaled_spacing() / 2);
    const coord_t inset_b_distance   = std::max<coord_t>(1, overhang_flow.scaled_spacing());
    const double  fit_tolerance      = std::max(1.0, std::min(scaled_resolution, toolpath_fit_tolerance_factor * double(overhang_flow.scaled_spacing())));
    const double  min_length         = 0.6 * double(overhang_flow.scaled_spacing());
    const double  min_new_area       = std::max(1.0, 3.0 * double(line_buffer_eps) * double(line_buffer_eps));

    BoundingBox infill_area_bb       = get_extents(infill_area).inflated(SCALED_EPSILON);
    Polygons    optimized_lower      = ClipperUtils::clip_clipper_polygons_with_subject_bbox(lower_slices_polygons, infill_area_bb);
    Polygons    overhangs            = diff(infill_area, optimized_lower);

    if (overhangs.empty())
        return {};

    Polygons   anchors             = intersection(infill_area, optimized_lower);
    Polygons   inset_anchors       = diff(anchors, expand(overhangs, anchors_size + 0.1 * overhang_flow.scaled_width(), EXTRA_PERIMETER_OFFSET_PARAMETERS));
    ExPolygons inset_overhang_area = diff_ex(infill_area, inset_anchors);

    std::vector<ExtrusionPaths> wave_paths;
    Polygons                    filled_area;

    for (const ExPolygon &overhang : union_ex(to_expolygons(inset_overhang_area))) {
        if (intersection(to_polygons(overhang), overhangs).empty())
            continue;

        ExPolygons boundary_a = build_inset_boundary(overhang, inset_a_distance);
        ExPolygons boundary_b = build_inset_boundary(overhang, inset_b_distance);
        Polygons   anchoring  = intersection(expand(to_polygons(overhang), 1.1 * overhang_flow.scaled_spacing(), jtRound, 0.), inset_anchors);

        std::vector<Polylines> seeds_per_boundary = generate_seed_lines(boundary_a, anchoring, tiny_expansion);
        for (size_t boundary_idx = 0; boundary_idx < boundary_a.size(); ++boundary_idx) {
            Polylines &seed_lines = seeds_per_boundary[boundary_idx];
            if (seed_lines.empty())
                continue;

            ExPolygons trim_boundary = intersection_ex(ExPolygons{ boundary_a[boundary_idx] }, boundary_b);
            if (trim_boundary.empty())
                trim_boundary = ExPolygons{ boundary_a[boundary_idx] };

            ExtrusionPaths &overhang_region = wave_paths.emplace_back();
            const PropagationMode mode = boundary_a[boundary_idx].holes.empty() ? PropagationMode::DirectDistance : PropagationMode::Cumulative;

            if (mode == PropagationMode::DirectDistance) {
                generate_direct_toolpath_lines(
                    overhang, boundary_a[boundary_idx], trim_boundary, seed_lines, overhang_region, filled_area,
                    overhang_flow, fit_tolerance, min_length);
            } else {
                generate_cumulative_toolpath_lines(
                    overhang, boundary_a[boundary_idx], trim_boundary, seed_lines, line_buffer_eps, overhang_region,
                    filled_area, overhang_flow, fit_tolerance, min_length, min_new_area);
            }

            overhang_region.erase(
                std::remove_if(overhang_region.begin(), overhang_region.end(), [](const ExtrusionPath &path) { return path.empty(); }),
                overhang_region.end());
            if (overhang_region.empty())
                wave_paths.pop_back();
        }
    }

    return { wave_paths, union_(filled_area) };
}

} // namespace Slic3r::WaveOverhangs
