#pragma once

#include <cstdint>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>

namespace lux::render
{
    inline constexpr std::uint32_t kRenderPluginExportsVersion = 2;

    struct RenderPluginExports final
    {
        std::uint32_t structure_size{sizeof(RenderPluginExports)};
        std::uint32_t interface_version{kRenderPluginExportsVersion};
        const RenderFeatureRegistration* entries{};
        std::uint32_t count{};
    };

    using GetRenderPluginExports = const RenderPluginExports*() noexcept;
    inline constexpr const char* kRenderPluginExportsSymbol = "lux_render_exports_v2";
} // namespace lux::render
