#pragma once

#include <array>
#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <NRI.h>
#include <lux/engine/render/extensions/graph/RenderGraphAnalysis.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>

namespace lux::render::detail
{
    struct EnabledDeviceFeatures;

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
        bool comparison_sampler = false;
    };

    struct ParameterUse final
    {
        std::uint32_t resource;
        std::string source_field;
        rdesc::ShaderMember destination;
        std::uint32_t source_offset = 0;
    };

    struct PassBindings final
    {
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
    };

    // The sole cold physical placement decision. Shader relocation, native layouts and Context writes
    // consume this same value. Private resources are packed; a resource never consumes an entire set.
    struct ShaderBindingPlan final
    {
        std::vector<PassBindings> passes; // Original GraphPassId indices; pruned entries stay empty.

        struct ParameterRegion final
        {
            rdesc::ShaderBlock layout;
            std::uint32_t group = 0, offset = 0;
            std::vector<std::uint32_t> passes;
        };

        std::vector<ParameterRegion> parameter_regions;
        std::uint32_t instance_parameter_bytes = 0;
        std::vector<std::vector<std::uint32_t>> parameter_uses;
        std::vector<std::uint32_t> instance_parameter_uses;
        std::vector<std::uint32_t> parameter_resources;
    };

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
        const EnabledDeviceFeatures& enabled,
        const std::array<std::uint32_t, 3>& capacities,
        std::uint32_t color_count = 0
    ) noexcept;

    [[nodiscard]] RenderResult<ShaderBindingPlan> planLayouts(
        const nri::DeviceDesc& limits,
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        std::span<const std::uint32_t> parameter_groups = {}
    ) noexcept;

    [[nodiscard]] RenderResult<rdesc::ShaderProgramArtifact>
    relocateShader(const rdesc::ShaderProgramArtifact& original, std::span<const ShaderBinding> locations) noexcept;
} // namespace lux::render::detail
