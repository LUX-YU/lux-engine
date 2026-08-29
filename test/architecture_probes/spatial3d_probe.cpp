#include <lux/engine/scene/LatestSpscExchange.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

namespace
{
    struct Spatial3DStreamingSource final
    {
        Eigen::Vector3d position;
        bool enabled{true};
        std::int32_t priority{};
    };

    struct Spatial3DPartitionIndex final
    {
        [[nodiscard]] std::uint32_t query(const Eigen::Vector3d& value) const noexcept
        {
            return static_cast<std::uint32_t>(std::floor(value.x() / 1000.0));
        }
    };

    struct Presentation final
    {
        Eigen::Vector3f relative;
        std::uint64_t revision{};
    };
}

int main()
{
    const Eigen::Vector3d origin{6'371'000.0, 0.0, 0.0};
    const Spatial3DStreamingSource source{origin + Eigen::Vector3d{250.125, 0.0, 0.0}};
    const Spatial3DPartitionIndex index;
    assert(index.query(source.position) == 6371U);

    lux::scene::LatestSpscExchange<Presentation> exchange;
    const Eigen::Vector3d relative = source.position - origin;
    exchange.write() = Presentation{relative.cast<float>(), 1U};
    exchange.publish();
    assert(exchange.acquireLatest());
    assert(std::abs(exchange.read().relative.x() - 250.125F) < 0.001F);
    return 0;
}
