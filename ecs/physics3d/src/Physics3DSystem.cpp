#include <lux/engine/ecs/physics3d/systems/Physics3DSystem.hpp>

namespace lux::ecs
{
    void Physics3DSystem::update(const SystemUpdateContext& context)
    {
        if (scene_)
        {
            scene_->advance(context.registry(), context.dt());
        }
    }
} // namespace lux::ecs
