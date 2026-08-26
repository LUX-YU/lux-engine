#pragma once

#include <entt/entity/entity.hpp>

#include <cstdint>

namespace lux::simulation::ecs
{
    enum class Entity : std::uint32_t
    {
    };

    inline constexpr Entity NullEntity = entt::null;

    // A runtime Entity can only be serialized by an ECS-aware archive that
    // remaps identity. Generic enum serialization must not persist its bits.
    constexpr void luxBinarySemanticArchiveOnly(Entity) noexcept {}

    [[nodiscard]] constexpr std::uint32_t entityBits(Entity entity) noexcept
    {
        return entt::to_integral(entity);
    }
} // namespace lux::simulation::ecs
