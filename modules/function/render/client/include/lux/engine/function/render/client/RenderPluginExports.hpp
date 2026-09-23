#pragma once

#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <cstdint>

namespace lux::render
{
struct RenderPluginExports final
{
    std::uint32_t structure_size{sizeof(RenderPluginExports)};
    std::uint32_t interface_version{1};
    const RenderFeatureRegistration *entries{};
    std::uint32_t count{};
};
using GetRenderPluginExports = const RenderPluginExports *() noexcept;
inline constexpr const char *kRenderPluginExportsSymbol = "lux_render_exports_v1";
} // namespace lux::render
