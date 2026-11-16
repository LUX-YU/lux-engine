#pragma once

#include <lux/engine/description/ShaderProgramArtifact.hpp>

namespace lux::rdesc
{
    // Build-time embedded target code. Each binary keeps the reflection generated for that exact target.
    struct EmbeddedShader final
    {
        struct Binary final
        {
            std::span<const std::uint32_t> words;
            std::span<const std::byte> reflection;
        };

        EShaderType stage;
        Binary spirv, dxil;

        [[nodiscard]] ShaderProgramArtifact load(EShaderTarget target) const noexcept
        {
            ShaderProgramArtifact result;
            result.target = target;
            const auto& binary = target == EShaderTarget::SPIRV ? spirv : dxil;
            if (!validShaderContainer(target, binary.words) ||
                !ShaderInfo::deserialize(binary.reflection, result.interface))
            {
                return {};
            }
            result.stages.push_back({stage, "main", {binary.words.begin(), binary.words.end()}});
            return result;
        }
    };
} // namespace lux::rdesc
