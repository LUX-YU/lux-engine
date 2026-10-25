#pragma once

#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<RasterWork> fullscreenRaster(
        FeatureBuildContext&,
        std::span<const std::uint32_t> fragment,
        std::span<const std::byte> reflection
    ) noexcept;
} // namespace lux::render::detail
