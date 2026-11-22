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
            // The bundle compiler serializes the selected entry's reflection alongside its target code.
            const auto& entries = result.interface.entry_points;
            const bool named_entry = entries.size() == 1 &&
                entries.front().stage == stage && !entries.front().name.empty();
            result.stages.push_back({
                stage, named_entry ? entries.front().name : "main", {binary.words.begin(), binary.words.end()}
            });
            return result;
        }
    };
} // namespace lux::rdesc
