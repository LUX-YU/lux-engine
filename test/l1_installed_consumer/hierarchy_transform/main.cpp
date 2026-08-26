#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemTaskResources.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <exception>
#include <memory>
#include <utility>

int main()
{
    lux::ecs::World world({{4096U, 1024U * 1024U}});
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

    auto hierarchy_system = systems.retain<lux::ecs::HierarchySystem>(
        *hierarchy_id
    );
    auto transform_system = systems.retain<lux::ecs::Transform3DSystem>(
        *transform_id
    );
    if (!hierarchy_system || !transform_system)
        return 2;

    auto hierarchy_changes = std::make_shared<lux::ecs::WorldChangeBatch>();
    auto transform_changes = std::make_shared<lux::ecs::WorldChangeBatch>();
    if (!hierarchy_changes->prepare(
            lux::ecs::HierarchySystem::TaskAccess.writeStorages()
        ) ||
        !transform_changes->prepare(
            lux::ecs::Transform3DSystem::TaskAccess.writeStorages()
        ))
    {
        return 3;
    }
    lux::ecs::WorldCommandBatch commands;
    if (!commands.prepare(2U))
        return 4;

    lux::task::TaskGraphBuilder builder;
    const auto hierarchy_update = builder.add(
        lux::ecs::systemTaskResources<lux::ecs::HierarchySystem>(),
        [&, system = std::move(*hierarchy_system), hierarchy_changes]() noexcept
        {
            auto recording = commands.begin(0U);
            if (!recording)
                std::terminate();
            auto scope = std::move(*recording);
            system->invokeTask(world, *hierarchy_changes, scope.commands());
        }
    );
    if (!hierarchy_update)
        return 5;
    const auto hierarchy_publish = builder.add(
        lux::task::dependsOn(*hierarchy_update),
        lux::ecs::worldChangesWrite(),
        [&, hierarchy_changes]() noexcept
        {
            (void)hierarchy_changes->publish(world);
        }
    );
    if (!hierarchy_publish)
        return 6;

    const auto transform_update = builder.add(
        lux::task::dependsOn(*hierarchy_publish),
        lux::ecs::systemTaskResources<lux::ecs::Transform3DSystem>(),
        [&, system = std::move(*transform_system), transform_changes]() noexcept
        {
            auto recording = commands.begin(1U);
            if (!recording)
                std::terminate();
            auto scope = std::move(*recording);
            system->invokeTask(world, *transform_changes, scope.commands());
        }
    );
    if (!transform_update)
        return 7;
    const auto transform_publish = builder.add(
        lux::task::dependsOn(*transform_update),
        lux::ecs::worldChangesWrite(),
        [&, transform_changes]() noexcept
        {
            (void)transform_changes->publish(world);
        }
    );
    if (!transform_publish)
        return 8;

    if (!builder.add(
            lux::task::dependsOn(*transform_publish),
            lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
            lux::ecs::worldCommandsWrite(),
            [&]() noexcept { lux::ecs::applyWorldCommands(world, commands); }
        ))
    {
        return 9;
    }
    auto graph = std::move(builder).build();
    if (!graph)
        return 10;

    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    auto execution = world.beginTaskExecution();
    if (!execution || !executor.execute(*graph))
        return 11;
    return 0;
}
