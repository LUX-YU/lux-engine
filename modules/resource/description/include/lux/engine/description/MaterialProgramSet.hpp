#pragma once

#include <algorithm>
#include <array>
#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/description/MaterialLayout.hpp>
#include <lux/engine/description/ShaderProgramArtifact.hpp>

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
        ShaderProgramArtifact shader;
        bool operator==(const MaterialShader&) const noexcept = default;
    };

    // One target per set. Layout/default projection belongs to this target, never to the author graph.
    struct MaterialProgramSet final
    {
        ShaderBlock parameters;
        std::vector<std::array<float, 4>> parameter_defaults;
        std::uint32_t texture_count = 0;
        ELightingTechnique shading_model = ELightingTechnique::UNLIT;
        EAlphaMode alpha_mode = EAlphaMode::OPAQUE_SURFACE;
        bool double_sided = false;
        std::vector<MaterialShader> programs;
        EShaderTarget target = EShaderTarget::SPIRV;

        [[nodiscard]] const MaterialShader* find(EMaterialPurpose purpose) const noexcept
        {
            const auto entry = std::ranges::find(programs, purpose, &MaterialShader::purpose);
            return entry == programs.end() ? nullptr : &*entry;
        }

        [[nodiscard]] const ShaderBlock* parameterLayout() const noexcept
        {
            return parameters.size ? &parameters : nullptr;
        }

        [[nodiscard]] bool hasEmptyRecord() const noexcept
        {
            return parameters == ShaderBlock{} && parameter_defaults.empty() && texture_count == 0;
        }

        // Default values and texture objects are material data. This digest only selects
        // candidate tables; hasSameStructure still verifies the complete ABI and code.
        [[nodiscard]] std::size_t structureDigest() const noexcept
        {
            std::size_t result = 0;
            const auto combine = [&]<class T>(const T& value) noexcept
            {
                result ^= std::hash<T>{}(value) + 0x9e3779b9u + (result << 6) + (result >> 2);
            };
            combine(target);
            combine(parameters.size);
            combine(parameters.alignment);
            combine(parameter_defaults.size());
            combine(texture_count);
            combine(shading_model);
            combine(alpha_mode);
            combine(double_sided);
            combine(programs.size());
            for (const auto& program : programs)
            {
                combine(program.purpose);
                combine(program.output_semantic);
                combine(program.shader.binaryDigest());
            }
            return result;
        }

        // Check small structural fields before comparing the reflected programs.
        [[nodiscard]] bool hasSameStructure(const MaterialProgramSet& other) const noexcept
        {
            return target == other.target && parameters == other.parameters &&
                parameter_defaults.size() == other.parameter_defaults.size() && texture_count == other.texture_count &&
                shading_model == other.shading_model && alpha_mode == other.alpha_mode &&
                double_sided == other.double_sided && programs == other.programs;
        }

        bool operator==(const MaterialProgramSet&) const noexcept = default;
    };
} // namespace lux::rdesc
