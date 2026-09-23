#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/simulation/SimulationPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <array>
#include <vector>

#if defined(_WIN32)
#define LUX_BUILTIN_EXPORT __declspec(dllexport)
#else
#define LUX_BUILTIN_EXPORT __attribute__((visibility("default")))
#endif

// This composition is consumed by both the DLL and its build-time description writer.
// No Editor or application maintains a corresponding list.
extern "C" LUX_BUILTIN_EXPORT
const lux::simulation::SimulationPluginExports *lux_simulation_exports_v1() noexcept
{
    static const auto values = lux::simulation::transformSystemRegistrations();
    static const lux::simulation::SimulationPluginExports table{
        sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())
    };
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT
const lux::scene::ScenePluginExports *lux_scene_exports_v1() noexcept
{
    static const std::array values{
        lux::scene::builtinRenderSystemRegistration(), lux::scene::builtinMeshQuerySystemRegistration(),
        lux::scene::worldLoadingSystemRegistration()
    };
    static const lux::scene::ScenePluginExports table{
        sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())
    };
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT
const lux::simulation::ecs::ComponentPluginExports *lux_component_exports_v1() noexcept
{
    static const auto values = [] {
        using namespace lux::simulation::ecs;
        std::vector<ComponentSchema> result;
        const auto append = [&](auto range) { result.insert(result.end(), range.begin(), range.end()); };
        append(transformComponentSchemas());
        append(hierarchyComponentSchemas());
        append(visualComponentSchemas());
        append(lux::scene::sceneRenderComponentSchemas());
        append(lux::scene::worldLoadingComponentSchemas());
        return result;
    }();
    static const lux::simulation::ecs::ComponentPluginExports table{
        sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())
    };
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT
const lux::scene::RenderScenePluginExports *lux_render_scene_exports_v1() noexcept
{
    static const auto values = lux::scene::builtinRenderFeatureSceneBindings();
    static const lux::scene::RenderScenePluginExports table{
        sizeof(table), 1, values.data(), static_cast<std::uint32_t>(values.size())
    };
    return &table;
}
