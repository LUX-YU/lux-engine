#pragma once

#include <lux/engine/world/WorldGeometryDescription.hpp>
#include <string>
#include <vector>

namespace lux::world
{
    // Caller-authored data. Creation validates geometry names and partition compatibility.
    // Storage bundles, streaming state and constructed indexes are not part of this value.
    struct WorldDescription final
    {
        std::string                           name;
        std::vector<WorldGeometryDescription> supported_geometries;
    };
} // namespace lux::world
