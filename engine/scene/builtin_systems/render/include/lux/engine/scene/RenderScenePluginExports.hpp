#pragma once

#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <cstdint>

namespace lux::scene
{
struct RenderScenePluginExports final
{
    std::uint32_t structure_size{sizeof(RenderScenePluginExports)};
    std::uint32_t interface_version{1};
    const RenderFeatureSceneBinding *entries{};
    std::uint32_t count{};
};
using GetRenderScenePluginExports = const RenderScenePluginExports *() noexcept;
inline constexpr const char *kRenderScenePluginExportsSymbol = "lux_render_scene_exports_v1";
} // namespace lux::scene
