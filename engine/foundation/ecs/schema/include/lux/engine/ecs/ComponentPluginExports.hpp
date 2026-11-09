#pragma once

#include <cstdint>
#include <lux/engine/ecs/ComponentSchema.hpp>

namespace lux::ecs
{
    struct ComponentPluginExports final
    {
        std::uint32_t structure_size{sizeof(ComponentPluginExports)};
        std::uint32_t interface_version{2};
        const ComponentSchema* entries{};
        std::uint32_t count{};
    };

    using GetComponentPluginExports = const ComponentPluginExports*() noexcept;
    inline constexpr const char* kComponentPluginExportsSymbol = "lux_component_exports_v2";
} // namespace lux::ecs
