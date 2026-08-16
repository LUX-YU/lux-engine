#pragma once

#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>

#include <memory>

namespace lux::ecs
{
    class PixelFieldRuntime;

    /// Domain-only construction primitive. Runtime composition decides
    /// whether to publish the returned probe and how long it lives.
    [[nodiscard]] std::unique_ptr<ICollision2DProbe>
    makePixelCollisionProbe(PixelFieldRuntime& runtime);
}
