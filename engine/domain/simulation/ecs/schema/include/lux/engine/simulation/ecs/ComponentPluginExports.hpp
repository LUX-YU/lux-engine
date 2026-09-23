#pragma once

#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <cstdint>

namespace lux::simulation::ecs
{
struct ComponentPluginExports final
{
    std::uint32_t structure_size{sizeof(ComponentPluginExports)};
    std::uint32_t interface_version{1};
    const ComponentSchema *entries{};
    std::uint32_t count{};
};
using GetComponentPluginExports = const ComponentPluginExports *() noexcept;
inline constexpr const char *kComponentPluginExportsSymbol = "lux_component_exports_v1";
} // namespace lux::simulation::ecs
