#include <lux/engine/ecs/integration/physics2d_pixel/PixelCollisionProbe.hpp>

#include <FieldCollisionAdapter.hpp>

namespace lux::ecs
{
    std::unique_ptr<ICollision2DProbe> makePixelCollisionProbe(
        PixelFieldRuntime& runtime)
    {
        return std::make_unique<FieldCollisionAdapter>(&runtime);
    }
}
