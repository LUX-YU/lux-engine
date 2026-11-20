#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    class RenderServer;
}

namespace lux::render::detail
{
    [[nodiscard]] RenderResult<void> registerDepthPyramid(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerLighting(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerMeshVisibility(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerGBuffer(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerMeshRaster(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerShadow(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerToneMapping(RenderServer&) noexcept;
}
