#pragma once

#include <lux/engine/render/extensions/resource/ResourceProgramUses.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    class RasterProgramSelection;
    struct DrawStreamSlot;

    struct SelectedMaterialProgram final
    {
        const rdesc::MaterialProgramSet& definition;
        ResourceProgramSource source;
    };

    // The definition is borrowed only during selection. The source is a generational publication address.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<SelectedMaterialProgram>
    readMaterialProgram(const RasterProgramSelection&, DrawStreamSlot, MaterialProgramId) noexcept;
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<SelectedMaterialProgram>
    readMaterialProgram(const RasterProgramSelection&, DrawStreamSlot, MaterialId) noexcept;
} // namespace lux::render
