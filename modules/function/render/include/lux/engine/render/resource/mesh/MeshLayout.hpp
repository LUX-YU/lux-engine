#pragma once

#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    class RasterProgramSelection;
    struct DrawStreamSlot;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<MeshDescription>
    readMeshLayout(const RasterProgramSelection&, DrawStreamSlot, MeshId) noexcept;
} // namespace lux::render
