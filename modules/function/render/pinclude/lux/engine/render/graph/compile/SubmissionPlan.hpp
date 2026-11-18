#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    void selectQueues(
        const RenderGraph&,
        std::span<const GraphPassId>,
        std::array<const QueueProperties*, 3>,
        RecordingProgram&
    ) noexcept;

    void planSubmissions(
        const RenderGraph&,
        const RenderGraphAnalysis&,
        std::span<const CompiledResource>,
        RecordingProgram&,
        const std::vector<std::vector<std::pair<std::uint32_t, nri::StageBits>>>& use_stages,
        std::array<const QueueProperties*, 3>,
        std::vector<SubmissionBatch>&,
        std::vector<ImportedAccess>&,
        std::uint32_t output
    ) noexcept;
} // namespace lux::render::detail
