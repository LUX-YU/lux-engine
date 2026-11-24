#pragma once

#include <array>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

#include <NRI.h>
#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/graph/CommandPackets.hpp>

namespace lux::render
{
    class GraphDefinition;
    struct RenderGraphAnalysis;
}

namespace lux::render::detail
{
    struct PassBindings final
    {
        [[nodiscard]] PassDescriptorBindings recordBindings() const noexcept
        {
            return {layout, private_set, shared_sets};
        }

        std::vector<ShaderBinding> bindings;
        std::vector<ShaderBinding> private_bindings;
        std::vector<DescriptorAccess> uses;
        nri::PipelineLayout* layout = nullptr;
        std::uint32_t private_set = 0;
        // Texture, sampler and buffer table positions; absent kinds consume no set.
        std::array<std::uint32_t, 3> shared_sets{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        std::uint32_t parameter_bytes = 0;
        nri::StageBits parameter_stages = nri::StageBits::NONE;
        std::vector<std::byte> initial_parameters;

        bool operator==(const PassBindings&) const noexcept = default;
    };

    // Cold shader relocation and native pipeline layout data. Fixed descriptor operands are
    // lowered into the executable after queue, barrier, and layout planning.
    struct ShaderBindingPlan final
    {
        std::vector<PassBindings> passes;
        std::vector<std::uint32_t> sampler_inputs;
    };

    [[nodiscard]] RenderResult<void> addPrivateDescriptors(nri::DescriptorPoolDesc&, const PassBindings&) noexcept;

    // The same reflected placement algorithm serves a fixed Graph pass and a material program.
    // Field indices and resource ranges have already been normalized at the authoring boundary.
    // Resource descriptions are cold borrowed inputs; this function owns no Graph or device state.
    [[nodiscard]] RenderResult<PassBindings> planProgramLayout(
        const nri::DeviceDesc& limits,
        const rdesc::ShaderInfo& shader,
        const PassInterface& input,
        std::span<const GraphResource> graph_resources
    ) noexcept;

    // Dynamic Raster inputs use stage-local slot indices instead of fixed Graph resources.
    // Reflected reads must be declared by the stage; Shader writes are not accepted here.
    [[nodiscard]] RenderResult<PassBindings>
    planRasterLayout(
        const nri::DeviceDesc&,
        const rdesc::ShaderInfo&,
        std::span<const RasterPassTemplate::Input>,
        const ParameterBinding&
    ) noexcept;

    // Qualifies the native descriptor table and root constants. The cache retains the remaining fragment color budget.
    [[nodiscard]] RenderResult<std::uint32_t>
    qualifyPipelineLayout(const nri::DeviceDesc&, const nri::PipelineLayoutDesc&, std::uint32_t color_count) noexcept;

    [[nodiscard]] RenderResult<ShaderBindingPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const GraphDefinition& graph,
        const RenderGraphAnalysis& analysis,
        bool device_addresses = false
    ) noexcept;

    [[nodiscard]] bool
    requiresShaderRelocation(const rdesc::ShaderProgramArtifact&, std::span<const ShaderBinding>) noexcept;

    [[nodiscard]] RenderResult<rdesc::ShaderProgramArtifact>
    relocateSpirv(rdesc::ShaderProgramArtifact shader, std::span<const ShaderBinding> locations) noexcept;
} // namespace lux::render::detail
