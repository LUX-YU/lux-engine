#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    [[nodiscard]] PhysicalResourcePlan planPhysicalResources(
        const RenderGraph& graph,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const SubmissionBatch> submissions,
        std::array<const QueueProperties*, 3> queues,
        ETransientAllocation allocation
    ) noexcept;
}
