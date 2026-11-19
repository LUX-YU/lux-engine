#pragma once

#include <array>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/GraphAssemblyPlan.hpp>

#include <NRI.h>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/graph/execute/PassRecordInput.hpp>

namespace lux::render
{
    class RenderGraph;
    struct RenderGraphAnalysis;
    struct InstanceDataRecipe;
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
        std::vector<DescriptorUse> uses;
        nri::PipelineLayout* layout = nullptr;
        std::uint32_t private_set = 0;
        std::uint32_t instance_region = 0;
        // Texture, sampler and buffer table positions; absent kinds consume no set.
        std::array<std::uint32_t, 3> shared_sets{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        std::uint32_t parameter_bytes = 0;
        nri::StageBits parameter_stages = nri::StageBits::NONE;
        std::vector<std::byte> initial_parameters;
        std::vector<ParameterUse> parameters;
        std::vector<std::pair<std::uint32_t, EDeviceParameter>> device_parameters;
    };

    struct ParameterRegion final
    {
        rdesc::ShaderBlock layout;
        std::uint32_t group = 0, offset = 0;
        std::vector<std::uint32_t> passes;
        EParameterSource source = EParameterSource::INSTANCE;
    };

    // Cold shader relocation and native pipeline layout data. Fixed descriptor operands are
    // lowered into the executable after queue, barrier, and layout planning.
    struct ShaderBindingPlan final
    {
        std::vector<PassBindings> passes;

        std::vector<ParameterRegion> parameter_regions;
        std::uint32_t local_parameter_bytes = 0;
        std::uint32_t instance_parameter_bytes = 0;
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
    planRasterLayout(const nri::DeviceDesc&, const rdesc::ShaderInfo&, const RasterPassTemplate&) noexcept;

    [[nodiscard]] RenderResult<nri::PipelineLayout*> createPipelineLayout(
        nri::Device& device,
        const nri::CoreInterface& core,
        const PassBindings& program,
        std::span<const nri::DescriptorRangeDesc> local,
        const std::array<nri::DescriptorRangeDesc, 3>& shared,
        std::uint32_t color_count,
        std::uint32_t& max_colors
    ) noexcept;

    [[nodiscard]] RenderResult<ShaderBindingPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        std::span<const std::uint32_t> parameter_groups = {},
        bool device_addresses = false,
        std::span<const InstanceDataRecipe* const> instance_data = {}
    ) noexcept;

    [[nodiscard]] RenderResult<rdesc::ShaderProgramArtifact>
    relocateShader(const rdesc::ShaderProgramArtifact& original, std::span<const ShaderBinding> locations) noexcept;
} // namespace lux::render::detail
