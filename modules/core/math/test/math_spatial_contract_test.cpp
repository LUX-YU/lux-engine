#include <lux/engine/math/Grid.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/RelativePosition.hpp>

#include <cassert>
#include <cstddef>
#include <cmath>
#include <limits>
#include <type_traits>

int main()
{
    using namespace lux::math;

    static_assert(std::is_standard_layout_v<Position2d>);
    static_assert(std::is_trivially_copyable_v<Position2d>);
    static_assert(sizeof(Position2d) == 16u);
    static_assert(offsetof(Position2d, x) == 0u);
    static_assert(offsetof(Position2d, y) == 8u);

    static_assert(std::is_standard_layout_v<Position3d>);
    static_assert(std::is_trivially_copyable_v<Position3d>);
    static_assert(sizeof(Position3d) == 24u);
    static_assert(offsetof(Position3d, x) == 0u);
    static_assert(offsetof(Position3d, y) == 8u);
    static_assert(offsetof(Position3d, z) == 16u);

    static_assert(std::is_standard_layout_v<GridCoord2i64>);
    static_assert(std::is_trivially_copyable_v<GridCoord2i64>);
    static_assert(sizeof(GridCoord2i64) == 16u);
    static_assert(offsetof(GridCoord2i64, x) == 0u);
    static_assert(offsetof(GridCoord2i64, y) == 8u);

    static_assert(std::is_standard_layout_v<GridCoord3i64>);
    static_assert(std::is_trivially_copyable_v<GridCoord3i64>);
    static_assert(sizeof(GridCoord3i64) == 24u);
    static_assert(offsetof(GridCoord3i64, x) == 0u);
    static_assert(offsetof(GridCoord3i64, y) == 8u);
    static_assert(offsetof(GridCoord3i64, z) == 16u);

    constexpr Position3d far_origin{1.0e12, -1.0e12, 5.0e11};
    constexpr Position3d millimetre_step{
        far_origin.x + 0.001,
        far_origin.y - 0.001,
        far_origin.z + 0.002};
    const auto relative = relativeFloat(millimetre_step, far_origin, 1.0f);
    assert(relative);
    assert(std::abs((*relative)[0] - 0.0009765625f) < 1.0e-7f);
    assert(std::abs((*relative)[1] + 0.0009765625f) < 1.0e-7f);
    assert(std::abs((*relative)[2] - 0.00201416015625f) < 1.0e-7f);

    assert(!relativeFloat(
        Position2d{std::numeric_limits<double>::infinity(), 0.0},
        Position2d{},
        1.0f));
    assert(!relativeFloat(
        Position3d{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0},
        Position3d{},
        1.0f));
    assert(!relativeFloat(Position2d{2.0, 0.0}, Position2d{}, 1.0f));
    assert(!relativeFloat(Position2d{}, Position2d{}, -1.0f));
    assert(!relativeFloat(
        Position2d{},
        Position2d{},
        std::numeric_limits<float>::infinity()));

    constexpr GridCoord2i64 negative{-1'000'000, 1'000'000};
    constexpr GridCoord2i64 origin{};
    static_assert(negative < origin);
    static_assert(GridCoord2i64{1, 2} == GridCoord2i64{1, 2});
    static_assert(GridCoord3i64{1, 2, 3} < GridCoord3i64{1, 3, 0});
    static_assert(Position2d{1.0, 2.0} == Position2d{1.0, 2.0});
    static_assert(Position3d{1.0, 2.0, 3.0} == Position3d{1.0, 2.0, 3.0});

    return 0;
}
