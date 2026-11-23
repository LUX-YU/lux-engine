#pragma once

#include <lux/engine/render/resource/material/Data.hpp>

namespace lux::render::detail
{
    struct MaterialProgramRecord final
    {
        MaterialProgramId id;
        rdesc::MaterialProgramSet data;
    };
} // namespace lux::render::detail
