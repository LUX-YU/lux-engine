#pragma once

#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <cstdint>

namespace lux::scene
{
struct ScenePluginExports final
{
    std::uint32_t structure_size{sizeof(ScenePluginExports)};
    std::uint32_t interface_version{1};
    const SceneSystemRegistration *entries{};
    std::uint32_t count{};
};
using GetScenePluginExports = const ScenePluginExports *() noexcept;
inline constexpr const char *kScenePluginExportsSymbol = "lux_scene_exports_v1";
} // namespace lux::scene
