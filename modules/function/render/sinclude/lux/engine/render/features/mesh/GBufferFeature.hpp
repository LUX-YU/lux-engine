#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>

namespace lux::render
{
    inline constexpr std::array<std::string_view, 4> kGBufferPorts{
        "albedo_metallic", "normal_roughness", "emissive_ao", "depth"
    };

    struct GBufferFeatureConfig final
    {
        EffectOutputDesc output;
        bool front_counter_clockwise = true;
        bool operator==(const GBufferFeatureConfig&) const noexcept = default;
    };

} // namespace lux::render
