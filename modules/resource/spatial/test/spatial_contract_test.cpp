#include <lux/engine/resource/spatial/Spatial.hpp>

#include <cassert>
#include <cmath>
#include <limits>

int main()
{
    using namespace lux::spatial;

    constexpr Position3D far_origin{1.0e12, -1.0e12, 5.0e11};
    constexpr Position3D millimetre_step{
        far_origin.x + 0.001,
        far_origin.y - 0.001,
        far_origin.z + 0.002};
    const auto relative = relativeFloat(millimetre_step, far_origin, 1.0f);
    assert(relative);
    assert(std::abs((*relative)[0] - 0.0009765625f) < 1.0e-7f);
    assert(std::abs((*relative)[1] + 0.0009765625f) < 1.0e-7f);
    assert(std::abs((*relative)[2] - 0.00201416015625f) < 1.0e-7f);

    assert(!relativeFloat(
        Position2D{std::numeric_limits<double>::infinity(), 0.0},
        Position2D{},
        1.0f));
    assert(!relativeFloat(Position2D{2.0, 0.0}, Position2D{}, 1.0f));

    constexpr GridCoord2i64 negative{-1'000'000, 1'000'000};
    constexpr GridCoord2i64 origin{};
    static_assert(negative < origin);

    return 0;
}
