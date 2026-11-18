#pragma once

#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<RasterWork>
    fullscreenRaster(FeatureBuildContext&, GraphProgram fragment) noexcept;
} // namespace lux::render::detail
