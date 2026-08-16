#pragma once

#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>

namespace lux::ecs
{
    class Schedule;
    class World;
}

namespace lux::runtime::spatial2d::testing
{
    enum class EInfinite2DCheckpoint : unsigned char
    {
        ORIGIN_READY,
        FAR_READY,
        ORIGIN_RECOVERED
    };

    /// Test-only leaf seam. The scenario remains the sole owner of loading,
    /// generation, publication, simulation, persistence and close. A device
    /// probe may add presentation behavior and observe exact checkpoints, but
    /// cannot replace the EntityScene -> SpatialPartition -> ECS path.
    class Infinite2DTestExtension
    {
    public:
        virtual ~Infinite2DTestExtension() = default;

        virtual bool install(
            lux::ecs::World& world,
            lux::ecs::Schedule& schedule,
            lux::ecs::PixelFieldRuntime& pixels) = 0;

        virtual void beforeTick(lux::ecs::World&) noexcept {}
        virtual void afterTick(lux::ecs::World&) noexcept {}

        virtual bool checkpoint(
            EInfinite2DCheckpoint checkpoint,
            lux::ecs::World& world,
            lux::ecs::PixelFieldRuntime& pixels,
            lux::ecs::PixelFieldHandle field,
            lux::spatial::GridCoord2i64 center) = 0;

        virtual void shutdown(lux::ecs::World&) noexcept = 0;
    };

    int runInfinite2DScenario(Infinite2DTestExtension* extension = nullptr);
}
