#pragma once

#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <lux/engine/render/resource/lighting/Data.hpp>

namespace lux::render
{
    inline constexpr ResourceNameView kLightResourceName{"lux.render.resource.lights"};
    inline constexpr ResourceRequirement kSceneLights{
        .binding = "scene.lights", .type = kLightResourceName, .domain = EResourceDomain::SCENE, .scope_default = true
    };

    inline constexpr std::array<std::string_view, 4> kLightBindings{
        "directional_lights", "point_lights", "spot_lights", "light_counts"
    };

    struct DirectLightingBinding final
    {
        EDirectLightingBuffer field;
        bool operator==(const DirectLightingBinding&) const noexcept = default;
    };

} // namespace lux::render
