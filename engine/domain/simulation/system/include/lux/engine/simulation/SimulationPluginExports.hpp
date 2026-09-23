#pragma once

#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <cstdint>

namespace lux::simulation
{
struct SimulationPluginExports final
{
    std::uint32_t structure_size{sizeof(SimulationPluginExports)};
    std::uint32_t interface_version{1};
    const SimulationSystemRegistration *entries{};
    std::uint32_t count{};
};
using GetSimulationPluginExports = const SimulationPluginExports *() noexcept;
inline constexpr const char *kSimulationPluginExportsSymbol = "lux_simulation_exports_v1";
} // namespace lux::simulation
