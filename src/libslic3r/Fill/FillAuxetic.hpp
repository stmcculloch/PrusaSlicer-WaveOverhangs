///|/ Copyright (c) Prusa Research 2026
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_FillAuxetic_hpp_
#define slic3r_FillAuxetic_hpp_

#include <map>

#include "FillBase.hpp"

namespace Slic3r {

class FillAuxetic : public Fill
{
public:
    Fill* clone() const override { return new FillAuxetic(*this); }
    ~FillAuxetic() override = default;

    Polylines fill_surface(const Surface *surface, const FillParams &params) override;
    Polylines take_deferred_straights();
    bool no_sort() const override { return true; }
    bool is_self_crossing() override { return false; }

private:
    struct CacheID {
        float    density;
        coordf_t spacing;

        bool operator<(const CacheID &rhs) const
        {
            return density < rhs.density || (density == rhs.density && spacing < rhs.spacing);
        }
    };

    struct CacheData {
        coord_t column_step{0};
        coord_t row_step{0};
        coord_t x_outer{0};
        coord_t y_outer{0};
        coord_t y_inner{0};
    };

    using Cache = std::map<CacheID, CacheData>;
    mutable Cache m_cache;
    Polylines     m_deferred_straights;

protected:
    float _layer_angle(size_t) const override { return 0.f; }
};

} // namespace Slic3r

#endif // slic3r_FillAuxetic_hpp_
