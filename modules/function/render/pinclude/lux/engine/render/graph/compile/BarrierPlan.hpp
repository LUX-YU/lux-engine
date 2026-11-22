#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    // NRI encodes ALL as zero. Once a selected scope is ALL, union must preserve it.
    [[nodiscard]] inline nri::StageBits combineStages(nri::StageBits a, nri::StageBits b) noexcept
    {
        return a == nri::StageBits::ALL || b == nri::StageBits::ALL ? nri::StageBits::ALL : a | b;
    }

    [[nodiscard]] inline bool coversScope(nri::AccessStage offered, nri::AccessStage required) noexcept
    {
        const bool covers_access = (offered.access & required.access) == static_cast<std::uint32_t>(required.access);
        const bool covers_stages = offered.stages == nri::StageBits::ALL ||
            (required.stages != nri::StageBits::ALL &&
             (offered.stages & required.stages) == static_cast<std::uint32_t>(required.stages));
        return covers_access && covers_stages;
    }

    // Compact only fixed subresource ranges; publish reverse indexes after all planning mutations.
    void compactImageTransitions(std::span<CompiledResource>, RecordingProgram&) noexcept;
} // namespace lux::render::detail
