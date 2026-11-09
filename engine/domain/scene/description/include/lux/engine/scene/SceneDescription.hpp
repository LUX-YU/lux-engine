#pragma once

#include <lux/engine/scene/SceneSystemDescription.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <vector>

namespace lux::scene
{
    // Complete authoring values, independent of asset identities and runtime Scene IDs.
    // External assembly validates these values; Engine schedules the assembled Scene, not this description.
    struct SceneDescription final
    {
        world::WorldDescription             world;
        simulation::SimulationDescription   simulation;
        std::vector<SceneSystemDescription> systems;
    };
} // namespace lux::scene
