#pragma once

#include <array>
#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <NRI.h>
#include <lux/engine/render/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/graph/Work.hpp>

namespace lux::render::detail
{
    struct DescriptorUse final
    {
        std::uint32_t resource;
        std::uint32_t set;
        std::uint32_t binding;
        EDescriptorKind kind;
        VGraphRange range;
        std::uint32_t field = 0;
        std::uint32_t element = 0;
        std::uint32_t sampler = kGraphInvalidIndex;
        bool whole_resource = false;
        std::uint32_t fallback = kGraphInvalidIndex;
    };

    struct ParameterUse final
    {
        std::uint32_t resource;
        std::string source_field;
        rdesc::ShaderMember destination;
    };

    struct PassLayout final
    {
        std::vector<ShaderBinding> bindings;
        std::vector<ShaderBinding> private_bindings;
        std::vector<DescriptorUse> uses;
        nri::PipelineLayout* layout = nullptr;
        std::uint32_t private_set = 0;
        bool shared = false;
        std::uint32_t parameter_bytes = 0;
        nri::StageBits parameter_stages = nri::StageBits::NONE;
        std::vector<std::byte> initial_parameters;
        std::vector<ParameterUse> parameters;
    };

    // The sole cold physical placement decision. Shader relocation, native layouts and Context writes
    // consume this same value. Private resources are packed; a resource never consumes an entire set.
    struct PipelineLayoutPlan final
    {
        std::vector<ShaderBinding> shared_bindings;
        std::vector<PassLayout> passes; // Original GraphPassId indices; pruned entries stay empty.
        rdesc::ShaderBlock instance_parameters;
    };

    [[nodiscard]] RenderResult<PipelineLayoutPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis
    ) noexcept;

    [[nodiscard]] RenderResult<rdesc::SpirvArtifact> relocateShader(
        const rdesc::SpirvArtifact& original,
        std::span<const ShaderBinding> locations
    ) noexcept;
} // namespace lux::render::detail
