#pragma once
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <span>

namespace lux::scene
{
    [[nodiscard]] std::span<const simulation::ecs::ComponentSchema> cameraComponentSchemas() noexcept;
}
