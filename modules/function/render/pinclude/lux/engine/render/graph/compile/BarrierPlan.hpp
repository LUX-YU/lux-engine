#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/StageScope.hpp>

namespace lux::render::detail
{
    // Compact only fixed subresource ranges; publish reverse indexes after all planning mutations.
    void compactImageTransitions(std::span<CompiledResource>, RecordingProgram&) noexcept;
} // namespace lux::render::detail
