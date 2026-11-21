#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    // Compact only fixed subresource ranges; publish reverse indexes after all planning mutations.
    void compactImageTransitions(std::span<CompiledResource>, RecordingProgram&) noexcept;
} // namespace lux::render::detail
