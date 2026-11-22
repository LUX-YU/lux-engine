#pragma once

#include <lux/engine/render/effect/EffectCompileContext.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<RasterWork>
    fullscreenRaster(EffectCompileContext&, GraphProgram fragment) noexcept;
} // namespace lux::render::detail
