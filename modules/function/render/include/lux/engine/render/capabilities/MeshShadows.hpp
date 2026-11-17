#pragma once

#include <cmath>
#include <lux/engine/description/LightDescription.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resources/lighting/Data.hpp>

namespace lux::render
{

    struct MeshShadowsConfiguration final
    {
        std::uint32_t resolution = 1024;
        std::uint32_t filter_radius = 1;
        float distance = 100;
        bool operator==(const MeshShadowsConfiguration&) const noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            const bool valid_resolution = filter_radius <= 4 && resolution > 2 * (filter_radius + 2);
            return valid_resolution && std::isfinite(distance) && distance > 0;
        }
    };

    struct ShadowLightInput final
    {
        LightId light;
        rdesc::LightDescription description;
        bool operator==(const ShadowLightInput&) const noexcept = default;
    };

    // Cold selection and clipping used to compile shadow slices. Moving lights and
    // camera pose remain frame data; they do not recreate the Effect.
} // namespace lux::render
