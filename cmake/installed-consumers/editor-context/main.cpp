#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <stdexec/execution.hpp>

#include <utility>

namespace
{
    struct InstalledProbeTool final
    {
        int value{42};
    };
}

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    int result = 1;
    auto runtime_created = lux::process::ExecutionRuntime::create({2U, 8U, 8U, {8U}});
    if (!runtime_created)
    {
        lux::meta::ReflectionRegistry::destroyRegistry();
        return result;
    }
    auto runtime = std::move(*runtime_created);
    lux::ui::UISession ui;
    lux::asset::AssetVfs vfs;
    lux::editor::EditorSelection selection{ui.dispatcherRef()};
    {
        auto schemas = lux::simulation::ecs::ComponentSchemaSet::build({});
        if (!schemas)
        {
            lux::meta::ReflectionRegistry::destroyRegistry();
            return result;
        }
        auto scene_meta = lux::scene::SceneMetaManager::build({
            std::move(*schemas),
            lux::simulation::SimulationSystemRegistry{},
            {},
            {},
            {}
        });
        if (!scene_meta)
        {
            lux::meta::ReflectionRegistry::destroyRegistry();
            return result;
        }

        lux::process::TaskScope tasks;
        lux::editor::Toolset toolset;
        {
            lux::editor::EditorContext context{lux::editor::EditorContextCreateInfo{
                toolset,
                vfs.view(),
                {},
                runtime,
                tasks,
                selection,
                ui,
                *scene_meta
            }};
            auto tool = context.toolchain().install<InstalledProbeTool>();
            context.toolchain().freeze();
            const bool capabilities_available = tool && tool->get().value == 42 && context.vfs() &&
                context.sceneMeta().components().empty() && !context.toolchain().stopping();
            result = capabilities_available ? 0 : 1;
        }
        const auto closed = stdexec::sync_wait(tasks.close());
        toolset.requestStop();
        if (!closed)
            result = 1;
    }
    runtime.requestStop();
    if (!runtime.join())
        result = 1;
    lux::meta::ReflectionRegistry::destroyRegistry();
    return result;
}
