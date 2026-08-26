#pragma once

#include <lux/engine/simulation/ecs/Entity.hpp>

#include <entt/entity/registry.hpp>

namespace lux::simulation::ecs
{
    using Registry = entt::basic_registry<Entity>;
}
