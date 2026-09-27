#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <array>

#if defined(_WIN32)
#define LUX_BUILTIN_EXPORT __declspec(dllexport)
#else
#define LUX_BUILTIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" LUX_BUILTIN_EXPORT const lux::scene::ScenePluginExports* lux_scene_exports_v1() noexcept
{
    static const std::array values{lux::scene::builtinRenderSystemRegistration()};
    static const lux::scene::ScenePluginExports table{sizeof(table), 1, values.data(), values.size()};
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT const lux::simulation::ecs::ComponentPluginExports* lux_component_exports_v1() noexcept
{
    static const auto values = lux::scene::sceneRenderComponentSchemas();
    static const lux::simulation::ecs::ComponentPluginExports
        table{sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())};
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT const lux::scene::RenderScenePluginExports* lux_render_scene_exports_v1() noexcept
{
    static const auto values = lux::scene::builtinRenderFeatureSceneBindings();
    static const lux::scene::RenderScenePluginExports
        table{sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())};
    return &table;
}
