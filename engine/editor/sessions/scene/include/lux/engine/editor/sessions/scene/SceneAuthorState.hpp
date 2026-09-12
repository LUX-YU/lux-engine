#pragma once
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <optional>
namespace lux::editor::sessions
{
    // Finite, explicitly supplied author values; derived WorldTransform/resources are never author state.
    struct SceneAuthorObject final
    {
        lux::world::WorldObjectId object;
        lux::simulation::ecs::Entity entity{lux::simulation::ecs::NullEntity};
        std::optional<lux::simulation::ecs::Transform3D> transform;
        std::optional<lux::simulation::ecs::Light3D> light;
    };
}
