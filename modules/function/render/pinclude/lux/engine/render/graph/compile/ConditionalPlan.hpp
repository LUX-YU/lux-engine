#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    void planConditions(const RenderGraph&, std::span<const GraphPassId>, RecordingProgram&) noexcept;
}
