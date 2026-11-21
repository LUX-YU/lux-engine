#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    [[nodiscard]] double estimateScheduleNs(
        const RenderGraph&, std::span<const GraphPassId>, std::span<const SubmissionBatch>, GraphQueueCosts
    ) noexcept;

    [[nodiscard]] bool preservesObjectReuse(const PhysicalResourcePlan&, const PhysicalResourcePlan&) noexcept;
} // namespace lux::render::detail
