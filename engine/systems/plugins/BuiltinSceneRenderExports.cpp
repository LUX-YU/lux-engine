#include <lux/engine/scene/ViewSchema.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/ecs/ComponentPluginExports.hpp>

#include <array>

#if defined(_WIN32)
#define LUX_BUILTIN_EXPORT __declspec(dllexport)
#else
#define LUX_BUILTIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" LUX_BUILTIN_EXPORT const lux::scene::ScenePluginExports* lux_scene_exports_v3() noexcept
{
    static const std::array values{::lux::engine::render::renderSystemRegistration()};
    static const lux::scene::ScenePluginExports table{.entries = values.data(), .count = values.size()};
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT const lux::ecs::ComponentPluginExports* lux_component_exports_v2() noexcept
{
    static const auto schemas = lux::scene::viewComponentSchemas();
    static const lux::ecs::ComponentPluginExports
        table{sizeof(table), 2, schemas.data(), static_cast<std::uint32_t>(schemas.size())};
    return &table;
}
