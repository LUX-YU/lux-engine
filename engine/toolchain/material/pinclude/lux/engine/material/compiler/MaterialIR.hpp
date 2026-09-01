#pragma once

#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/material/compiler/ShaderIR.hpp>

#include <cstdint>

namespace lux::material::compiler
{
    struct MaterialIR final
    {
        lux::shadergen::ShaderIR shader;
        lux::rdesc::ELightingTechnique shading_model{lux::rdesc::ELightingTechnique::PbrMetallicRoughness};
        lux::rdesc::EAlphaMode alpha_mode{lux::rdesc::EAlphaMode::Opaque};
        float alpha_cutoff{0.5F};
        bool double_sided{};
        std::uint64_t combined_fingerprint{};
    };
} // namespace lux::material::compiler
