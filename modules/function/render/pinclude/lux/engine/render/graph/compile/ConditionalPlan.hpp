#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    void planConditions(
        const RenderGraph&, std::span<const GraphPassId>, std::span<const CompiledResource>, RecordingProgram&
    ) noexcept;
}
