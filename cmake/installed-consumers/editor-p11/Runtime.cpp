#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#if defined(_WIN32)
#define PROBE_EXPORT __declspec(dllexport)
#else
#define PROBE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PROBE_EXPORT const lux::simulation::ecs::ComponentPluginExports* lux_component_exports_v1() noexcept
{
    static const auto schema = lux::simulation::ecs::transformComponentSchemas().front();
    static const lux::simulation::ecs::ComponentPluginExports exports{sizeof(exports), 1, &schema, 1};
    return &exports;
}
