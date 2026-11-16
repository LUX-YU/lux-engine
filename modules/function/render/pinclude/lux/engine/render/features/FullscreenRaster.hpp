#pragma once

#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<RasterWork>
    fullscreenRaster(FeatureBuildContext&, rdesc::ShaderProgramArtifact fragment) noexcept;
} // namespace lux::render::detail
