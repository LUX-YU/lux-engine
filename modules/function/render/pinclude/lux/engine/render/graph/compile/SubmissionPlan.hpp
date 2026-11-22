#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    [[nodiscard]] std::array<std::size_t, 3>
    physicalQueueIndices(const std::array<const QueueProperties*, 3>& queues) noexcept;

    [[nodiscard]] double estimateScheduleNs(
        const RenderGraph&, std::span<const GraphPassId>, std::span<const SubmissionBatch>, GraphQueueCosts
    ) noexcept;

    [[nodiscard]] bool preservesObjectReuse(const PhysicalResourcePlan&, const PhysicalResourcePlan&) noexcept;
} // namespace lux::render::detail
