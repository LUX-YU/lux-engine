#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/physics2d/Physics2DComponents.ecs_schema.hpp>
#include <lux/engine/simulation/SimulationPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>

extern "C" LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC const lux::simulation::SimulationPluginExports *
lux_simulation_exports_v1() noexcept
{
    static const auto entries = lux::physics2d::physics2DSystemRegistrations();
    static const lux::simulation::SimulationPluginExports exports{
        sizeof(lux::simulation::SimulationPluginExports), 1, entries.data(), static_cast<std::uint32_t>(entries.size())
    };
    return &exports;
}

extern "C" LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC const lux::simulation::ecs::ComponentPluginExports *
lux_component_exports_v1() noexcept
{
    static const auto entries = lux::simulation::ecs::generated::physics2dComponentSchemas();
    static const lux::simulation::ecs::ComponentPluginExports exports{
        sizeof(lux::simulation::ecs::ComponentPluginExports), 1, entries.data(), static_cast<std::uint32_t>(entries.size())
    };
    return &exports;
}
