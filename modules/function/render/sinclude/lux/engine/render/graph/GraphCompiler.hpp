#pragma once

#include <lux/engine/render/graph/CompiledGraph.hpp>

namespace lux::render::detail
{
    // Borrowed program index in GraphPassId order. Call once after authoring; programs must outlive compilation.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<std::vector<const PassProgram*>>
    indexPrograms(const RenderGraphDefinition&, std::span<const PassProgram>) noexcept;

    // INVARIANT: analysis belongs to this immutable definition; never analyze again here.
    // Program/Target/device capabilities are the cold BOUNDARY checked by this compiler.
    // External Storage declarations have already been qualified by the caller's installation/assembly boundary.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<CompiledGraph> compileGraph(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::QueueType queue_type,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        const RenderGraphDefinition& graph,
        const GraphAnalysis& analysis,
        GraphStorageBindings external_bindings,
        std::span<const PassProgram* const> programs,
        const TargetDesc& target,
        std::vector<FeatureFrameContribution> contexts = {}
    ) noexcept;
} // namespace lux::render::detail
