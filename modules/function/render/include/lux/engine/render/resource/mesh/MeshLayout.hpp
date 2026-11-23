#pragma once

#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    class RasterProgramBuildContext;
    struct DrawStreamSlot;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<MeshDescription>
    readMeshLayout(const RasterProgramBuildContext&, DrawStreamSlot, MeshId) noexcept;
} // namespace lux::render
