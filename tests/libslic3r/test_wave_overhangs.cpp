#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/WaveOverhangs.hpp"

using namespace Slic3r;

static size_t count_paths(const std::vector<ExtrusionPaths> &regions)
{
    size_t count = 0;
    for (const ExtrusionPaths &paths : regions)
        count += paths.size();
    return count;
}

TEST_CASE("Wave overhangs generate fronts for a hole-free overhang", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 12.5, flow, scale_(0.01));

    REQUIRE(! regions.empty());
    CHECK(! filled_area.empty());
    for (const ExtrusionPaths &paths : regions)
        for (const ExtrusionPath &path : paths)
            CHECK(path.attributes().wave_overhang);
}

TEST_CASE("Wave overhangs preserve holes while generating fronts", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 50, 0 }, { 50, 30 }, { 0, 30 } }) };
    infill.holes.emplace_back(Polygon::new_scale({ { 18, 8 }, { 32, 8 }, { 32, 22 }, { 18, 22 } }));

    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 14, 0 }, { 14, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 12.5, flow, scale_(0.01));

    REQUIRE(! regions.empty());

    Polygons swept_paths;
    for (const ExtrusionPaths &paths : regions)
        for (const ExtrusionPath &path : paths)
            append(swept_paths, offset(path.polyline, float(0.5 * flow.scaled_width()), jtRound, 0., ClipperLib::etOpenRound));

    Polygons hole = { infill.holes.front() };
    CHECK(intersection(filled_area, hole).empty());
    CHECK(intersection(swept_paths, hole).empty());
}

TEST_CASE("Wave overhang nozzle overlap increases wave density", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [baseline_regions, baseline_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.0, flow, scale_(0.01));
    auto [dense_regions, dense_filled]       = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 25.0, flow, scale_(0.01));

    REQUIRE(! baseline_regions.empty());
    REQUIRE(! dense_regions.empty());
    CHECK(count_paths(dense_regions) >= count_paths(baseline_regions));
    CHECK(area(dense_filled) >= area(baseline_filled));
}

TEST_CASE("Wave overhang outer perimeter reserve reduces filled area", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [one_outer_regions, one_outer_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 12.5, flow, scale_(0.01));
    auto [two_outer_regions, two_outer_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 2, 12.5, flow, scale_(0.01));

    REQUIRE(! one_outer_regions.empty());
    REQUIRE(! two_outer_regions.empty());
    CHECK(area(two_outer_filled) < area(one_outer_filled));
}
