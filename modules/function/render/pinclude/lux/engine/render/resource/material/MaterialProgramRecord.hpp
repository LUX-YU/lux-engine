#pragma once

#include <lux/engine/render/resources/material/Data.hpp>

namespace lux::render::detail
{
    struct MaterialProgramRecord final
    {
        MaterialProgramId id;
        rdesc::MaterialProgramSet data;
        std::size_t digest;
        std::array<bool, 3> samples_textures;
    };
} // namespace lux::render::detail
