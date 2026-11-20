#pragma once

#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<RasterWork>
    fullscreenRaster(RenderGraphBuilder&, GraphProgram fragment) noexcept;
} // namespace lux::render::detail
