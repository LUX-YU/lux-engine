#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    class LUX_RENDER_PUBLIC GraphCompiler final
    {
    public:
        // Cold compilation consumes the caller's exact analysis. It never discovers business resources.
        [[nodiscard]] static RenderResult<RenderPlan> compile(
            nri::Device& device,
            const nri::CoreInterface& nri_core,
            const EnabledDeviceFeatures& enabled,
            std::span<const nri::FormatSupportBits> formats,
            std::array<std::uint32_t, 2> vertex_limits,
            std::array<const QueueProperties*, 3> queues,
            std::uint32_t sampled_image_capacity,
            std::uint32_t sampler_capacity,
            const RenderGraph& graph,
            const RenderGraphAnalysis& analysis,
            const TargetDesc* target = nullptr,
            ETransientAllocation allocation = ETransientAllocation::REUSE_OBJECTS
        ) noexcept;
    };
} // namespace lux::render::detail
