#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/WaveOverhangs.hpp"

using namespace Slic3r;

TEST_CASE("Wave overhangs generate fronts for a hole-free overhang", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, flow, scale_(0.01));

    REQUIRE(! regions.empty());
    CHECK(! filled_area.empty());
}

TEST_CASE("Wave overhangs preserve holes while generating fronts", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 50, 0 }, { 50, 30 }, { 0, 30 } }) };
    infill.holes.emplace_back(Polygon::new_scale({ { 18, 8 }, { 32, 8 }, { 32, 22 }, { 18, 22 } }));

    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 14, 0 }, { 14, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, flow, scale_(0.01));

    REQUIRE(! regions.empty());

    Polygons swept_paths;
    for (const ExtrusionPaths &paths : regions)
        for (const ExtrusionPath &path : paths)
            append(swept_paths, offset(path.polyline, float(0.5 * flow.scaled_width()), jtRound, 0., ClipperLib::etOpenRound));

    Polygons hole = { infill.holes.front() };
    CHECK(intersection(filled_area, hole).empty());
    CHECK(intersection(swept_paths, hole).empty());
}
