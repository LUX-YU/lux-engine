#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    void selectQueues(const RenderGraph&, std::array<const QueueProperties*, 3>, RecordingProgram&) noexcept;

    void
    planSubmissions(const RenderGraph&, const RenderGraphAnalysis&, std::span<const CompiledResource>, const RecordingProgram&, std::vector<SubmissionBatch>&, std::vector<ImportedAccess>&) noexcept;
} // namespace lux::render::detail
