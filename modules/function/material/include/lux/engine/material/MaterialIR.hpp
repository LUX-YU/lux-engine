#pragma once

#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/material/ShaderIR.hpp>

#include <cstdint>

namespace lux::material
{
    // Fully owned domain compilation result; shader backend and device objects are absent.
    struct MaterialIR final
    {
        lux::shadergen::ShaderIR shader;
        lux::rdesc::ELightingTechnique shading_model{lux::rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS};
        lux::rdesc::EAlphaMode alpha_mode{lux::rdesc::EAlphaMode::OPAQUE_SURFACE};
        float alpha_cutoff{0.5F};
        bool double_sided{};
        std::uint64_t combined_fingerprint{};
    };

    [[nodiscard]] std::uint64_t computeFingerprint(const MaterialIR& ir) noexcept;
    [[nodiscard]] bool validateMaterialIR(const MaterialIR& ir) noexcept;
} // namespace lux::material
