#pragma once

#include <entt/entity/entity.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class Entity : std::uint32_t
    {
    };

    inline constexpr Entity NullEntity = entt::null;

    [[nodiscard]] constexpr std::uint32_t entityBits(Entity entity) noexcept
    {
        return entt::to_integral(entity);
    }
} // namespace lux::ecs
