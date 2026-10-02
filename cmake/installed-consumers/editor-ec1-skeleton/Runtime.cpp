#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#if defined(_WIN32)
#define SKELETON_EXPORT __declspec(dllexport)
#else
#define SKELETON_EXPORT __attribute__((visibility("default")))
#endif
// This editor-only authoring extension adds no runtime component or duplicate Skeleton codec.
extern "C" SKELETON_EXPORT const lux::simulation::ecs::ComponentPluginExports *lux_component_exports_v1() noexcept
{
    static const lux::simulation::ecs::ComponentPluginExports table{sizeof(table), 1, nullptr, 0};
    return &table;
}
