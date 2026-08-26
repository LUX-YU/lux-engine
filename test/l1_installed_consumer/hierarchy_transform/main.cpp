#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/HierarchySystem.hpp>
#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/simulation/ecs/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <exception>
#include <memory>
#include <utility>

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::EcsChangeJournal journal(
        lux::simulation::ecs::EcsChangeHistoryBudget{4096U, 1024U * 1024U}
    );
    lux::simulation::ecs::HierarchyIndex hierarchy(world);
    lux::simulation::SystemRegistry systems;
    const auto hierarchy_id = systems.emplace<lux::simulation::ecs::HierarchySystem>(
        world,
        hierarchy
    );
    const auto transform_id = systems.emplace<lux::simulation::ecs::Transform3DSystem>(
        hierarchy
    );
    if (!hierarchy_id || !transform_id)
        return 1;

    auto hierarchy_system = systems.retain<lux::simulation::ecs::HierarchySystem>(
        *hierarchy_id
    );
    auto transform_system = systems.retain<lux::simulation::ecs::Transform3DSystem>(
        *transform_id
    );
    if (!hierarchy_system || !transform_system)
        return 2;

    auto hierarchy_changes = std::make_shared<lux::simulation::ecs::EcsChangeBatch>();
    auto transform_changes = std::make_shared<lux::simulation::ecs::EcsChangeBatch>();
    if (!hierarchy_changes->prepare(
            lux::simulation::ecs::HierarchySystem::TaskAccess.writeStorages()
        ) ||
        !transform_changes->prepare(
            lux::simulation::ecs::Transform3DSystem::TaskAccess.writeStorages()
        ))
    {
        return 3;
    }
    lux::simulation::ecs::EcsCommandBatch commands;
    if (!commands.prepare(2U))
        return 4;

    lux::task::TaskGraphBuilder builder;
    const auto hierarchy_update = builder.add(
        lux::simulation::ecs::systemTaskResources<lux::simulation::ecs::HierarchySystem>(),
        [&, system = std::move(*hierarchy_system), hierarchy_changes]() noexcept
        {
            auto recording = commands.begin(0U);
            if (!recording)
                std::terminate();
            auto scope = std::move(*recording);
            system->invokeTask(
                world,
                journal,
                *hierarchy_changes,
                scope.commands()
            );
        }
    );
    if (!hierarchy_update)
        return 5;
    const auto hierarchy_publish = builder.add(
        lux::task::dependsOn(*hierarchy_update),
        lux::simulation::ecs::ecsChangesWrite(),
        [&, hierarchy_changes]() noexcept
        {
            (void)hierarchy_changes->publish(journal);
        }
    );
    if (!hierarchy_publish)
        return 6;

    const auto transform_update = builder.add(
        lux::task::dependsOn(*hierarchy_publish),
        lux::simulation::ecs::systemTaskResources<lux::simulation::ecs::Transform3DSystem>(),
        [&, system = std::move(*transform_system), transform_changes]() noexcept
        {
            auto recording = commands.begin(1U);
            if (!recording)
                std::terminate();
            auto scope = std::move(*recording);
            system->invokeTask(
                world,
                journal,
                *transform_changes,
                scope.commands()
            );
        }
    );
    if (!transform_update)
        return 7;
    const auto transform_publish = builder.add(
        lux::task::dependsOn(*transform_update),
        lux::simulation::ecs::ecsChangesWrite(),
        [&, transform_changes]() noexcept
        {
            (void)transform_changes->publish(journal);
        }
    );
    if (!transform_publish)
        return 8;

    if (!builder.add(
            lux::task::dependsOn(*transform_publish),
            lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD),
            lux::simulation::ecs::ecsCommandsWrite(),
            [&]() noexcept
            {
                lux::simulation::ecs::applyEcsCommands(
                    world,
                    journal,
                    commands
                );
            }
        ))
    {
        return 9;
    }
    auto graph = std::move(builder).build();
    if (!graph)
        return 10;

    lux::task::TaskExecutor executor({0U, graph->taskCount()});
    if (!executor.execute(*graph))
        return 11;
    return 0;
}
