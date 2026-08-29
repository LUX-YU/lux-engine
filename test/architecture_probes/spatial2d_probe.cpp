#include <lux/engine/scene/LatestSpscExchange.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

namespace
{
    struct Spatial2DStreamingSource final
    {
        Eigen::Vector2d position;
    };

    struct Tile2DPartitionIndex final
    {
        [[nodiscard]] std::int64_t query(const Eigen::Vector2d& value) const noexcept
        {
            return static_cast<std::int64_t>(std::floor(value.x() / 256.0));
        }
    };
}

int main()
{
    const Spatial2DStreamingSource source{Eigen::Vector2d{1'000'000'000'000.5, -4.0}};
    const Tile2DPartitionIndex index;
    const auto partition = index.query(source.position);
    assert(partition == 3'906'250'000LL);
    lux::simulation::ecs::Transform2D transform;
    transform.translation = source.position;
    assert(transform.translation.x() == source.position.x());
    return 0;
}
