#pragma once

#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    struct DescriptorUse final
    {
        std::uint32_t resource;
        std::uint32_t set;
        std::uint32_t binding;
        EDescriptorKind kind;
    };

    struct ProgramLayout final
    {
        std::vector<ShaderBinding> bindings;
        std::vector<DescriptorUse> uses;
    };

    // The sole cold physical placement decision. Shader relocation, native layouts and Context writes
    // consume this same value. Private resources are packed; a resource never consumes an entire set.
    struct LayoutPlan final
    {
        std::vector<ShaderBinding> shared_bindings;
        std::vector<ShaderBinding> private_bindings;
        std::vector<ProgramLayout> programs; // Original GraphPassId indices; pruned entries stay empty.
        std::uint32_t shared_set = kGraphInvalidIndex;
        std::uint32_t private_set = kGraphInvalidIndex;
    };

    [[nodiscard]] const ShaderBinding& sampledImageTableBinding() noexcept;

    [[nodiscard]] RenderResult<LayoutPlan> planLayouts(
        const VulkanDevice& device, const RenderGraphDefinition& graph, const GraphAnalysis& analysis,
        std::span<const PassProgram* const> programs
    ) noexcept;

    [[nodiscard]] RenderResult<SpirvArtifact>
    relocateShader(const SpirvArtifact& original, std::span<const ShaderBinding> locations) noexcept;
}
