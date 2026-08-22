#pragma once

#include <lux/engine/ecs/render/components/VisualTransitionComponent.hpp>
#include <lux/engine/ecs/Registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lux::ecs
{
    struct VisualTransitionParameters final
    {
        std::uint32_t duration_milliseconds{0u};
        std::uint32_t seed{0u};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return duration_milliseconds != 0u && seed != 0u;
        }
    };

    [[nodiscard]] inline VisualTransitionParameters visualTransitionOf(
        const lux::ecs::Registry& registry,
        lux::ecs::Entity entity) noexcept
    {
        const auto* component =
            registry.try_get<VisualTransitionComponent>(entity);
        if (!component || !std::isfinite(component->duration_seconds) ||
            component->duration_seconds <= 0.0f)
        {
            return {};
        }

        constexpr double kMaximumMilliseconds = static_cast<double>(
            std::numeric_limits<std::uint32_t>::max());
        const double milliseconds = std::min(
            static_cast<double>(component->duration_seconds) * 1000.0,
            kMaximumMilliseconds);
        const auto duration = static_cast<std::uint32_t>(
            std::max(1.0, std::round(milliseconds)));
        return VisualTransitionParameters{
            duration,
            component->seed == 0u ? 1u : component->seed};
    }
}
