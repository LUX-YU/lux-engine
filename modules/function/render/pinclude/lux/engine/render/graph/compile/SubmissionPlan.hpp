#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>

namespace lux::render::detail
{
    [[nodiscard]] bool assignAsyncCompute(
        const RenderGraph&, std::span<const GraphPassId>, std::array<const QueueProperties*, 3>,
        std::uint32_t surface_output, RecordingProgram&
    ) noexcept;

    [[nodiscard]] double estimateScheduleNs(
        const RenderGraph&, std::span<const GraphPassId>, std::span<const SubmissionBatch>, GraphQueueCosts
    ) noexcept;

    [[nodiscard]] bool preservesObjectReuse(const PhysicalResourcePlan&, const PhysicalResourcePlan&) noexcept;

    void planSubmissions(
        const RenderGraph&,
        const RenderGraphAnalysis&,
        std::span<const CompiledResource>,
        RecordingProgram&,
        const std::vector<std::vector<std::pair<std::uint32_t, nri::StageBits>>>& use_stages,
        std::array<const QueueProperties*, 3>,
        std::vector<SubmissionBatch>&,
        std::vector<ImportedAccess>&,
        std::uint32_t output,
        bool presents
    ) noexcept;
} // namespace lux::render::detail
