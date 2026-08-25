#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>

#include <utility>

int main()
{
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy(world);
    lux::ecs::SystemRegistry systems;
    const auto hierarchy_id = systems.emplace<lux::ecs::HierarchySystem>(
        world,
        hierarchy
    );
    const auto transform_id = systems.emplace<lux::ecs::Transform3DSystem>(
        hierarchy
    );
    if (!hierarchy_id || !transform_id)
        return 1;

    lux::ecs::SystemRelations relations;
    if (!relations.before(*hierarchy_id, *transform_id))
        return 2;
    auto compilation = lux::ecs::compileSystemTaskGraph(systems, relations);
    if (!compilation)
        return 3;
    lux::ecs::SystemExecutionScratch scratch;
    if (!scratch.prepare(*compilation))
        return 4;
    lux::ecs::EcsExecutionContext context{
        world,
        systems,
        relations,
        scratch,
        1.0F / 60.0F,
        1U
    };
    return lux::ecs::executeSystemTaskGraph(
        lux::task::referenceTaskExecutionBackend(),
        *compilation,
        context
    ) ? 0 : 5;
}
