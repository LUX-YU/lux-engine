#pragma once

#include <array>
#include <lux/engine/render/graph/ShaderInterface.hpp>
#include <map>

#include <NRI.h>
#include <lux/engine/render/graph/Analysis.hpp>
#include <lux/engine/render/graph/Program.hpp>

namespace lux::render::detail
{
    using ResourceNames = std::map<std::string_view, std::uint32_t, std::less<>>;

    struct DescriptorUse final
    {
        std::uint32_t resource;
        std::uint32_t set;
        std::uint32_t binding;
        EDescriptorKind kind;
    };

    struct ParameterUse final
    {
        std::uint32_t resource;
        std::string source_field;
        rdesc::ShaderMember destination;
    };

    struct ProgramLayout final
    {
        std::vector<ShaderBinding> bindings;
        std::vector<DescriptorUse> uses;
        std::vector<std::byte> initial_parameters;
        std::vector<ParameterUse> parameters;
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
        rdesc::ShaderBlock context_parameters;
        std::uint32_t parameter_bytes = 0;
        nri::StageBits parameter_stages = nri::StageBits::NONE;
    };

    [[nodiscard]] const std::array<ShaderBinding, 2>& sampledImageTableBindings() noexcept;

    [[nodiscard]] RenderResult<LayoutPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        std::span<const PassProgram* const> programs,
        const ResourceNames& resource_names
    ) noexcept;

    [[nodiscard]] RenderResult<rdesc::SpirvArtifact> relocateShader(
        const rdesc::SpirvArtifact& original,
        std::span<const ShaderBinding> locations
    ) noexcept;
} // namespace lux::render::detail
