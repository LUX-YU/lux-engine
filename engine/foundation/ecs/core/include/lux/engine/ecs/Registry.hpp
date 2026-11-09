#pragma once

#include <lux/engine/ecs/Entity.hpp>

#include <entt/entity/registry.hpp>

namespace lux::ecs
{
    using Registry = entt::basic_registry<Entity>;
}
