#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::ZigZag, 1.0, 0.75, flow, scale_(0.01));

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
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::ZigZag, 1.0, 0.75, flow, scale_(0.01));

    REQUIRE(! regions.empty());

    Polygons swept_paths;
    for (const ExtrusionPaths &paths : regions)
        for (const ExtrusionPath &path : paths)
            append(swept_paths, offset(path.polyline, float(0.5 * path.width()), jtRound, 0., ClipperLib::etOpenRound));

    Polygons hole = { infill.holes.front() };
    CHECK(intersection(filled_area, hole).empty());
    CHECK(intersection(swept_paths, hole).empty());
}

TEST_CASE("Wave overhang geometry modifiers do not suppress wave generation", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [baseline_regions, baseline_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::Monotonic, 1.0, 1.0, flow, scale_(0.01));
    auto [modified_regions, modified_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 2, 0.1, 2.0, WaveOverhangPattern::ZigZag, 0.75, 0.5, flow, scale_(0.01));

    REQUIRE(! baseline_regions.empty());
    REQUIRE(! modified_regions.empty());
    CHECK(! baseline_filled.empty());
    CHECK(! modified_filled.empty());

    bool saw_wave_width_path = false;
    bool saw_shell_width_path = false;
    for (const ExtrusionPaths &paths : modified_regions) {
        for (const ExtrusionPath &path : paths) {
            if (path.width() == Catch::Approx(0.5f))
                saw_wave_width_path = true;
            if (path.width() == Catch::Approx(1.0f))
                saw_shell_width_path = true;
        }
    }

    CHECK(saw_wave_width_path);
    CHECK(saw_shell_width_path);
}

TEST_CASE("Wave overhang zig-zag connects adjacent fronts into fewer paths", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [monotonic_regions, monotonic_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::Monotonic, 1.0, 0.75, flow, scale_(0.01));
    auto [zigzag_regions, zigzag_filled]       = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::ZigZag, 1.0, 0.75, flow, scale_(0.01));

    REQUIRE(! monotonic_regions.empty());
    REQUIRE(! zigzag_regions.empty());
    CHECK(! monotonic_filled.empty());
    CHECK(! zigzag_filled.empty());

    size_t monotonic_count = 0;
    for (const ExtrusionPaths &paths : monotonic_regions)
        monotonic_count += paths.size();
    size_t zigzag_count = 0;
    for (const ExtrusionPaths &paths : zigzag_regions)
        zigzag_count += paths.size();

    CHECK(zigzag_count < monotonic_count);
}

TEST_CASE("Wave overhang zig-zag stays depth-first when fronts split around a hole", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 50, 0 }, { 50, 30 }, { 0, 30 } }) };
    infill.holes.emplace_back(Polygon::new_scale({ { 18, 8 }, { 32, 8 }, { 32, 22 }, { 18, 22 } }));

    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 14, 0 }, { 14, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [monotonic_regions, monotonic_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::Monotonic, 1.0, 0.75, flow, scale_(0.01));
    auto [zigzag_regions, zigzag_filled]       = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::ZigZag, 1.0, 0.75, flow, scale_(0.01));

    REQUIRE(! monotonic_regions.empty());
    REQUIRE(! zigzag_regions.empty());
    CHECK(! monotonic_filled.empty());
    CHECK(! zigzag_filled.empty());

    size_t monotonic_count = 0;
    for (const ExtrusionPaths &paths : monotonic_regions)
        monotonic_count += paths.size();
    size_t zigzag_count = 0;
    for (const ExtrusionPaths &paths : zigzag_regions)
        zigzag_count += paths.size();

    CHECK(zigzag_count < monotonic_count);
}

TEST_CASE("Wave overhang smart mode still generates supported fronts", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 30 }, { 0, 30 } })
    };

    Flow flow(1., 1., 1.);
    auto [smart_regions, smart_filled] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::Smart, 1.0, 0.75, flow, scale_(0.01));

    REQUIRE(! smart_regions.empty());
    CHECK(! smart_filled.empty());
}

TEST_CASE("Wave overhangs leave bridgeable spans to regular bridging", "[WaveOverhangs]")
{
    ExPolygon infill{ Polygon::new_scale({ { 0, 0 }, { 60, 0 }, { 60, 20 }, { 0, 20 } }) };
    Polygons lower_support = {
        Polygon::new_scale({ { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 } }),
        Polygon::new_scale({ { 40, 0 }, { 60, 0 }, { 60, 20 }, { 40, 20 } })
    };

    Flow flow(1., 1., 1.);
    auto [regions, filled_area] = WaveOverhangs::generate({ infill }, lower_support, 2, 1, 0.1, 2.0, WaveOverhangPattern::Smart, 1.0, 0.75, flow, scale_(0.01));

    CHECK(regions.empty());
    CHECK(filled_area.empty());
}
