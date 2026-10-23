#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    void selectQueues(const RenderGraph&, std::array<const QueueProperties*, 3>, RecordingProgram&) noexcept;

    void planSubmissions(
        const RenderGraph&,
        const RenderGraphAnalysis&,
        std::span<const CompiledResource>,
        const RecordingProgram&,
        std::array<const QueueProperties*, 3>,
        std::vector<SubmissionBatch>&,
        std::vector<ImportedAccess>&,
        std::uint32_t output
    ) noexcept;
} // namespace lux::render::detail
