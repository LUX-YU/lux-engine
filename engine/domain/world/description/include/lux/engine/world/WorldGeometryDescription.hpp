#pragma once

#include <lux/engine/world/PartitionerDescription.hpp>
#include <string>

namespace lux::world
{
    // One supported geometry type and its own partition configuration.
    // Reusing an algorithm name does not imply sharing a runtime partition instance.
    struct WorldGeometryDescription final
    {
        std::string            name;
        PartitionerDescription partitioner;
    };
} // namespace lux::world
