#pragma once

#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/core/ResourceId.hpp>

namespace lux::render::detail
{
    struct MaterialProgramTag;
    using MaterialProgramId = ResourceId<MaterialProgramTag>;

    struct MaterialProgramRecord final
    {
        MaterialProgramId id;
        rdesc::MaterialProgramSet data;
        std::size_t digest;
        std::array<bool, 3> samples_textures;
    };
} // namespace lux::render::detail
