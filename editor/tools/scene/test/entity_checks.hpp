#include "ToolTestAccess.hpp"
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#pragma once

#include <algorithm>
#include <vector>
#include <lux/engine/editor/scene/SceneEditor.hpp>

#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>

inline lux::scene::SceneRuntime& testSceneRuntime(const lux::editor::scene::SceneEditor& scene)
{
    return static_cast<lux::editor::Editor&>(scene.root()).context().engine().sceneRuntime();
}

// Test-only snapshots. Production owners and saving do not consume this directory.
struct TestObjectRow final
{
    lux::simulation::ecs::Entity object{lux::simulation::ecs::NullEntity}, parent{lux::simulation::ecs::NullEntity};
    lux::partition::PartitionOrdinal partition;
};
inline std::vector<TestObjectRow> sceneObjects(const lux::editor::scene::SceneEditor& document)
{
    namespace ecs = lux::simulation::ecs;
    std::vector<TestObjectRow> rows;
    const auto instance = toolTest(document).instance();
    const auto borrowed = std::as_const(testSceneRuntime(document)).borrowInstance(instance);
    if (!borrowed)
        return rows;
    const auto& registry = borrowed->get();
    auto* loader = registry.ctx().find<lux::scene::WorldResidency>();
    const auto state = document.runStatus().state;
    using RunState = lux::editor::scene::EPlaybackState;
    const bool inspecting_run = state == RunState::RUNNING || state == RunState::PAUSED || state == RunState::STOPPING;
    const bool author = !inspecting_run;
    for (const auto [entity] : registry.storage<ecs::Entity>()->each())
    {
        const auto* request = registry.try_get<lux::scene::RenderViewRequest>(entity);
        if (request && request->destroy_entity_on_stop)
            continue;
        if (author && (!loader || !loader->identities().object(entity).valid()))
            continue;
        const auto* parent = registry.try_get<ecs::Parent>(entity);
        lux::partition::PartitionOrdinal partition;
        if (loader)
            static_cast<void>(loader->partitionOf(entity, partition));
        rows.push_back({entity, parent ? parent->entity : ecs::NullEntity, partition});
    }
    std::ranges::sort(rows, {}, &TestObjectRow::object);
    return rows;
}

inline std::vector<lux::simulation::ecs::Entity> entitySnapshot(lux::editor::scene::SceneEditor& scene)
{
    std::vector<lux::simulation::ecs::Entity> result;
    for (const auto& row : sceneObjects(scene))
    {
        result.push_back(row.object);
    }
    return result;
}

inline std::vector<lux::simulation::ecs::Entity> newEntities(
    lux::editor::scene::SceneEditor& scene,
    const std::vector<lux::simulation::ecs::Entity>& previous
)
{
    std::vector<lux::simulation::ecs::Entity> result;
    for (const auto& row : sceneObjects(scene))
    {
        if (std::ranges::find(previous, row.object) == previous.end())
        {
            result.push_back(row.object);
        }
    }
    return result;
}
