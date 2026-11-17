#pragma once

#include <lux/engine/description/EmbeddedShader.hpp>
#include <algorithm>

namespace lux::render
{
    // One logical program, retaining each requested target's own code and reflected ABI.
    // Selection belongs to device compilation; Feature construction never chooses a backend.
    struct GraphProgram final
    {
        std::vector<rdesc::ShaderProgramArtifact> targets;

        GraphProgram() noexcept = default;

        GraphProgram(rdesc::ShaderProgramArtifact artifact) noexcept
        {
            targets.push_back(std::move(artifact));
        }

        explicit GraphProgram(const rdesc::EmbeddedShader& shader) noexcept
        {
            for (auto target : {rdesc::EShaderTarget::SPIRV, rdesc::EShaderTarget::DXIL})
                targets.push_back(shader.load(target));
        }

        [[nodiscard]] const rdesc::ShaderProgramArtifact* find(rdesc::EShaderTarget target) const noexcept
        {
            const auto found = std::ranges::find(targets, target, &rdesc::ShaderProgramArtifact::target);
            return found == targets.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            if (targets.empty()) return false;
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                const auto& program = targets[i];
                const bool is_invalid_target = program.target > rdesc::EShaderTarget::DXIL;
                const bool is_empty_program = program.stages.empty();
                if (is_invalid_target || is_empty_program) return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (targets[j].target == program.target) return false;
                if (!sameResources(targets.front().interface, program.interface)) return false;
            }
            return true;
        }

    private:
        static bool sameResources(const rdesc::ShaderInfo& first, const rdesc::ShaderInfo& second) noexcept
        {
            std::size_t count = 0, matched = 0;
            for (const auto& set : second.sets) count += set.bindings.size();
            for (const auto& set : first.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    const rdesc::EDescriptorBindingInfo* other = nullptr;
                    for (const auto& candidate_set : second.sets)
                    {
                        for (const auto& candidate : candidate_set.bindings)
                        {
                            if (candidate.name != binding.name) continue;
                            if (other) return false;
                            other = &candidate;
                        }
                    }
                    if (!other) return false;
                    const bool is_type_mismatch = binding.type != other->type || binding.count != other->count;
                    const bool is_access_mismatch = binding.readable != other->readable ||
                        binding.writable != other->writable || binding.stages != other->stages;
                    const bool is_shape_mismatch = binding.image_arrayed != other->image_arrayed ||
                        binding.image_depth != other->image_depth;
                    const bool is_mismatch = is_type_mismatch || is_access_mismatch || is_shape_mismatch;
                    if (is_mismatch) return false;
                    ++matched;
                }
            }
            return matched == count;
        }
    };
} // namespace lux::render
