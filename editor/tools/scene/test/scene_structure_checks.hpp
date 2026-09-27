#include "ToolTestAccess.hpp"
#pragma once
#include "entity_checks.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

inline void checkSceneStructure(lux::editor::scene::SceneEditor& scene, bool opaque)
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    const auto measured_begin = std::chrono::steady_clock::now();
    const auto initial = scene.historyView()->history;
    const auto count = sceneObjects(scene).size();
    auto original = sceneObjects(scene).front().object;
    const auto state = [&] { return scene.historyView()->history.current; };
    const auto original_entities = entitySnapshot(scene);
    const auto row = [&](lux::simulation::ecs::Entity id) -> TestObjectRow {
        const auto snapshot = sceneObjects(scene);
        const auto found = std::ranges::find(snapshot, id, &TestObjectRow::object);
        assert(found != snapshot.end());
        return *found;
    };
    const auto invalid = toolTest(scene).createObject(state(), partition::PartitionOrdinal{999}, EObjectSpace::NONE);
    assert(!invalid && invalid.error().domain_code == static_cast<unsigned>(ESceneStructureError::INVALID_PARTITION));
    assert(!toolTest(scene).supportsObjectSpace(EObjectSpace::SPACE_2D));
    auto unsupported = toolTest(scene).createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::SPACE_2D);
    assert(
        !unsupported && unsupported.error().domain_code == static_cast<unsigned>(ESceneStructureError::MISSING_PROVIDER)
    );
    assert(state() == initial.current && sceneObjects(scene).size() == count);

    auto first = toolTest(scene).createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::SPACE_3D);
    assert(first && toolTest(scene).component(*first, cxx::typeToken<simulation::ecs::Transform3D>()));
    auto stale = toolTest(scene).createObject(initial.current, partition::PartitionOrdinal{0}, EObjectSpace::NONE);
    assert(!stale && stale.error().code == editing::EEditError::STALE_BASE);
    auto second = toolTest(scene).createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::NONE);
    assert(second && !toolTest(scene).component(*second, cxx::typeToken<simulation::ecs::Transform3D>()));
    const auto restore = [&] {
        const auto old = *first;
        const auto restored = newEntities(scene, original_entities);
        assert(restored.size() == 2);
        for (const auto entity : restored)
        {
            if (toolTest(scene).component(entity, cxx::typeToken<simulation::ecs::Transform3D>()))
            {
                *first = entity;
            }
            else
            {
                *second = entity;
            }
        }
        assert(*first != old && !toolTest(scene).writeTarget(old));
    };
    const auto target = toolTest(scene).writeTarget(*second);
    assert(target);
    if (toolTest(scene).supportsHierarchy())
    {
        auto attached = toolTest(scene).reparent(*target, *first);
        assert(attached && row(*second).parent == *first);
        const auto before = scene.historyView()->history;
        auto cycle = toolTest(scene).reparent(*toolTest(scene).writeTarget(*first), *second);
        assert(!cycle && cycle.error().domain_code == static_cast<unsigned>(ESceneStructureError::HIERARCHY_CYCLE));
        auto dangling = toolTest(scene).eraseObjects(state(), std::span(&*first, 1));
        assert(
            !dangling && dangling.error().domain_code == static_cast<unsigned>(ESceneStructureError::REFERENCE_IN_USE)
        );
        assert(state() == before.current && scene.historyView()->history.revision == before.revision);
        assert(row(*second).parent == *first && sceneObjects(scene).size() == count + 2);
        const std::array removed{*first, *second};
        assert(toolTest(scene).eraseObjects(state(), removed));
        assert(sceneObjects(scene).size() == count);
        for (unsigned iteration{}; iteration < 32; ++iteration)
        {
            assert(scene.undo());
            restore();
            assert(row(*second).parent == *first);
            const auto* child = static_cast<const simulation::ecs::Parent*>(
                toolTest(scene).component(*second, cxx::typeToken<simulation::ecs::Parent>())
            );
            assert(child && child->entity != simulation::ecs::NullEntity);
            assert(toolTest(scene).component(*first, cxx::typeToken<simulation::ecs::Transform3D>()));
            assert(scene.redo() && sceneObjects(scene).size() == count);
        }
        assert(scene.undo());
        restore();
        assert(scene.undo() && row(*second).parent == lux::simulation::ecs::NullEntity);
        assert(scene.redo() && row(*second).parent == *first);
        assert(scene.undo() && row(*second).parent == lux::simulation::ecs::NullEntity);
    }
    else
    {
        auto rejected = toolTest(scene).reparent(*target, *first);
        assert(
            !rejected &&
            rejected.error().domain_code == static_cast<unsigned>(ESceneStructureError::HIERARCHY_UNSUPPORTED)
        );
    }
    assert(scene.undo() && scene.undo() && sceneObjects(scene).size() == count);
    assert(state() == initial.current);

    const auto deletion_base = scene.historyView()->history;
    const std::array removed{original};
    auto deletion = toolTest(scene).eraseObjects(state(), removed);
    if (opaque)
    {
        assert(
            !deletion && deletion.error().domain_code == static_cast<unsigned>(ESceneStructureError::MISSING_PROVIDER)
        );
        assert(sceneObjects(scene).size() == count && state() == deletion_base.current);
        assert(scene.historyView()->history.revision == deletion_base.revision);
    }
    else
    {
        assert(deletion && sceneObjects(scene).size() == count - 1);
        assert(scene.undo() && sceneObjects(scene).size() == count);
        assert(!toolTest(scene).component(original, cxx::typeToken<simulation::ecs::Transform3D>()));
        const auto restored = newEntities(scene, original_entities);
        assert(restored.size() == 1);
        original = restored.front();
        assert(toolTest(scene).component(original, cxx::typeToken<simulation::ecs::Transform3D>()));
        assert(state() == deletion_base.current);
    }
    std::printf(
        "scene structure: hierarchy=%d opaque=%d create/delete/restore, exact stale/provider/reference/cycle "
        "rejection; owner content/history preserved\n",
        toolTest(scene).supportsHierarchy(),
        opaque
    );
    std::printf(
        "structure active: objects=%zu hierarchy=%d opaque=%d elapsed_us=%.3f revision_delta=%llu retained=%zu\n",
        count,
        toolTest(scene).supportsHierarchy(),
        opaque,
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - measured_begin).count(),
        static_cast<unsigned long long>(scene.historyView()->history.revision.value - initial.revision.value),
        scene.historyView()->history.charged_retained_bytes
    );
}
