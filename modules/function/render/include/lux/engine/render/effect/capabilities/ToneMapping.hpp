#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/Lighting.hpp>
#include <lux/engine/render/effect/resources/Color.hpp>
#include <array>

namespace lux::render
{
    namespace ToneMappingPorts
    {
        inline constexpr auto hdr_color = inputPort("hdr_color", {HdrColorBundle::type, HdrColorBundle::version}, true);
        inline constexpr auto color = outputPort("color", {kDisplayColorType, 1});
    }

    namespace capabilities
    {
        inline constexpr std::array kToneMappingPorts{
            ToneMappingPorts::hdr_color, ToneMappingPorts::color
        };

        inline constexpr EffectCapability ToneMapping{
            .id = graph::nodeTypeId("lux.render.tone_mapping"),
            .canonical_name = "lux.render.tone_mapping",
            .display_name = "Tone Mapping",
            .ports = kToneMappingPorts
        };
    }
}
