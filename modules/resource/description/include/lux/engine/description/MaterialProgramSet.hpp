#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/description/SpirvArtifact.hpp>

namespace lux::rdesc
{
    enum class EMaterialPurpose : std::uint8_t
    {
        FORWARD,
        GBUFFER,
        SHADOW
    };

    struct MaterialShader final
    {
        EMaterialPurpose purpose;
        std::string output_semantic;
        SpirvArtifact shader;
        bool operator==(const MaterialShader&) const noexcept = default;
    };

    // Cooked values only. Layout and defaults are common to every use; no author graph or native Pipeline.
    struct MaterialProgramSet final
    {
        ShaderBlock parameters;
        std::vector<std::array<float, 4>> parameter_defaults;
        std::uint32_t texture_count = 0;
        ELightingTechnique shading_model = ELightingTechnique::UNLIT;
        EAlphaMode alpha_mode = EAlphaMode::OPAQUE_SURFACE;
        bool double_sided = false;
        std::vector<MaterialShader> programs;

        [[nodiscard]] const MaterialShader* find(EMaterialPurpose purpose) const noexcept
        {
            const auto entry = std::ranges::find(programs, purpose, &MaterialShader::purpose);
            return entry == programs.end() ? nullptr : &*entry;
        }

        [[nodiscard]] const ShaderBlock* parameterLayout() const noexcept
        {
            return parameters.size ? &parameters : nullptr;
        }

        bool operator==(const MaterialProgramSet&) const noexcept = default;
    };
} // namespace lux::rdesc
