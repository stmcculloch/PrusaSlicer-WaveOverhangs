///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "FillAuxetic.hpp"

#include <algorithm>
#include <cmath>

#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"

namespace Slic3r {
namespace {

inline void add_segment(Polylines &polylines, const Point &a, const Point &b)
{
    if (a != b)
        polylines.emplace_back(a, b);
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

    // Keep the full signed bridge / infill direction so a pi flip reverses which
    // side receives the horizontal braces.
    const float pattern_angle = this->_infill_direction(surface).first - float(M_PI / 2.);

    ExPolygon rotated_expolygon = surface->expolygon;
    rotated_expolygon.rotate(-pattern_angle);

    BoundingBox bounding_box = rotated_expolygon.contour.bounding_box();
    if (! empty(this->bounding_box)) {
        Polygon object_bb = this->bounding_box.polygon();
        object_bb.rotate(-pattern_angle);
        bounding_box.merge(align_to_grid(
            bounding_box.min,
            Point(cache.row_step, cache.column_step),
            object_bb.bounding_box().center()));
    }

    const coord_t x_min = bounding_box.min.x() - cache.row_step - cache.x_outer;
    const coord_t x_max = bounding_box.max.x() + cache.row_step + cache.x_outer;
    const coord_t y_min = bounding_box.min.y() - cache.column_step - cache.y_outer;
    const coord_t y_max = bounding_box.max.y() + cache.column_step + cache.y_outer;

    const Point object_center = empty(this->bounding_box) ?
        bounding_box.center() :
        this->bounding_box.center().rotated(-pattern_angle);
    const coord_t ref_x = object_center.x();
    const coord_t ref_y = object_center.y();

    Polylines all_polylines;
    for (int64_t column = int64_t(std::floor(double(y_min - ref_y) / cache.column_step)) - 2;
         column <= int64_t(std::ceil(double(y_max - ref_y) / cache.column_step)) + 2;
         ++column) {
        const coord_t center_y = coord_t(std::llround(double(ref_y) + double(column) * double(cache.column_step)));
        const coord_t stagger  = (std::llabs(column) & 1) ? cache.row_step / 2 : 0;

        for (int64_t row = int64_t(std::floor(double(x_min - ref_x - stagger) / cache.row_step)) - 2;
             row <= int64_t(std::ceil(double(x_max - ref_x - stagger) / cache.row_step)) + 2;
             ++row) {
            const coord_t center_x = coord_t(std::llround(double(ref_x + stagger) + double(row) * double(cache.row_step)));

            const Point top_inner    (center_x,                 center_y + cache.y_inner);
            const Point top_left     (center_x - cache.x_outer, center_y + cache.y_outer);
            const Point bottom_left  (center_x - cache.x_outer, center_y - cache.y_outer);
            const Point bottom_inner (center_x,                 center_y - cache.y_inner);
            const Point bottom_right (center_x + cache.x_outer, center_y - cache.y_outer);
            const Point top_right    (center_x + cache.x_outer, center_y + cache.y_outer);
            const Point top_brace    (center_x + cache.x_outer, center_y + cache.y_inner);
            const Point bottom_brace (center_x + cache.x_outer, center_y - cache.y_inner);

            add_segment(all_polylines, top_inner, top_left);
            add_segment(all_polylines, top_left, bottom_left);
            add_segment(all_polylines, bottom_left, bottom_inner);
            add_segment(all_polylines, bottom_inner, bottom_right);
            add_segment(all_polylines, bottom_right, top_right);
            add_segment(all_polylines, top_right, top_inner);
            add_segment(all_polylines, top_inner, top_brace);
            add_segment(all_polylines, bottom_inner, bottom_brace);
        }
    }

    all_polylines = chain_polylines(intersection_pl(std::move(all_polylines), rotated_expolygon));
    for (Polyline &polyline : all_polylines)
        polyline.rotate(pattern_angle);

    if (params.dont_connect() || all_polylines.size() <= 1)
        append(polylines_out, std::move(all_polylines));
    else
        connect_infill(std::move(all_polylines), surface->expolygon, polylines_out, this->spacing, params);

    return polylines_out;
}

} // namespace Slic3r
