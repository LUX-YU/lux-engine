#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <utility>

namespace
{
    [[nodiscard]] lux::scene::SceneMetaManager emptySceneMeta()
    {
        auto schemas = lux::simulation::ecs::ComponentSchemaSet::build({});
        auto meta = lux::scene::SceneMetaManager::build({
            std::move(*schemas),
            lux::simulation::SimulationSystemRegistry{},
            {},
            {},
            {}
        });
        return std::move(*meta);
    }
}

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    auto application = lux::editor::EditorApplication::create({
        {2U, 64U, 64U, {64U}, lux::process::BlockingSchedulerConfig{2U, 64U}},
        {64U},
        emptySceneMeta(),
        {}
    });
    int result = 1;
    if (application && (*application)->start())
    {
        const auto drained = (*application)->drainMain(64U);
        result = drained && (*application)->shutdown() ? 0 : 1;
    }
    else if (application)
    {
        static_cast<void>((*application)->shutdown());
    }
    if (application)
        application->reset();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return result;
}
