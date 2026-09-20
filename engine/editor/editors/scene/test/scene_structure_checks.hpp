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

inline void checkSceneStructure(lux::editor::scene::SceneEditor &scene, bool opaque)
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    const auto measured_begin = std::chrono::steady_clock::now();
    const auto initial = scene.historyView()->history;
    const auto count = scene.objects().size();
    auto original = scene.objects().front().object;
    const auto state = [&]
    {
        return scene.historyView()->history.current;
    };
    const auto original_entities = entitySnapshot(scene);
    const auto row = [&](SceneEntityRef id) -> const SceneObjectRow &
    {
        const auto found = std::ranges::find(scene.objects(), id, &SceneObjectRow::object);
        assert(found != scene.objects().end());
        return *found;
    };
    const auto invalid = scene.createObject(state(), partition::PartitionOrdinal{999}, EObjectSpace::NONE);
    assert(!invalid && invalid.error().domain_code == static_cast<unsigned>(ESceneStructureError::INVALID_PARTITION));
    assert(!scene.supportsObjectSpace(EObjectSpace::SPACE_2D));
    auto unsupported = scene.createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::SPACE_2D);
    assert(!unsupported &&
           unsupported.error().domain_code == static_cast<unsigned>(ESceneStructureError::MISSING_PROVIDER));
    assert(state() == initial.current && scene.objects().size() == count);

    auto first = scene.createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::SPACE_3D);
    assert(first && scene.component(*first, cxx::typeToken<simulation::ecs::Transform3D>()));
    auto stale = scene.createObject(initial.current, partition::PartitionOrdinal{0}, EObjectSpace::NONE);
    assert(!stale && stale.error().code == editing::EEditError::STALE_BASE);
    auto second = scene.createObject(state(), partition::PartitionOrdinal{0}, EObjectSpace::NONE);
    assert(second && !scene.component(*second, cxx::typeToken<simulation::ecs::Transform3D>()));
    const auto restore = [&]
    {
        const auto old = *first;
        const auto restored = newEntities(scene, original_entities);
        assert(restored.size() == 2);
        for (const auto entity : restored)
        {
            if (scene.component(entity, cxx::typeToken<simulation::ecs::Transform3D>()))
            {
                *first = entity;
            }
            else
            {
                *second = entity;
            }
        }
        assert(*first != old && !scene.writeTarget(old));
    };
    const auto target = scene.writeTarget(*second);
    assert(target);
    if (scene.supportsHierarchy())
    {
        auto attached = scene.reparent(*target, *first);
        assert(attached && row(*second).parent == *first);
        const auto before = scene.historyView()->history;
        auto cycle = scene.reparent(*scene.writeTarget(*first), *second);
        assert(!cycle && cycle.error().domain_code == static_cast<unsigned>(ESceneStructureError::HIERARCHY_CYCLE));
        auto dangling = scene.eraseObjects(state(), std::span(&*first, 1));
        assert(!dangling &&
               dangling.error().domain_code == static_cast<unsigned>(ESceneStructureError::REFERENCE_IN_USE));
        assert(state() == before.current && scene.historyView()->history.revision == before.revision);
        assert(row(*second).parent == *first && scene.objects().size() == count + 2);
        const std::array removed{*first, *second};
        assert(scene.eraseObjects(state(), removed));
        assert(scene.objects().size() == count);
        for (unsigned iteration{}; iteration < 32; ++iteration)
        {
            assert(scene.undo());
            restore();
            assert(row(*second).parent == *first);
            const auto *child = static_cast<const simulation::ecs::Parent *>(
                scene.component(*second, cxx::typeToken<simulation::ecs::Parent>()));
            assert(child && child->entity != simulation::ecs::NullEntity);
            assert(scene.component(*first, cxx::typeToken<simulation::ecs::Transform3D>()));
            assert(scene.redo() && scene.objects().size() == count);
        }
        assert(scene.undo());
        restore();
        assert(scene.undo() && !row(*second).parent.valid());
        assert(scene.redo() && row(*second).parent == *first);
        assert(scene.undo() && !row(*second).parent.valid());
    }
    else
    {
        auto rejected = scene.reparent(*target, *first);
        assert(!rejected &&
               rejected.error().domain_code == static_cast<unsigned>(ESceneStructureError::HIERARCHY_UNSUPPORTED));
    }
    assert(scene.undo() && scene.undo() && scene.objects().size() == count);
    assert(state() == initial.current);

    const auto deletion_base = scene.historyView()->history;
    const std::array removed{original};
    auto deletion = scene.eraseObjects(state(), removed);
    if (opaque)
    {
        assert(!deletion &&
               deletion.error().domain_code == static_cast<unsigned>(ESceneStructureError::MISSING_PROVIDER));
        assert(scene.objects().size() == count && state() == deletion_base.current);
        assert(scene.historyView()->history.revision == deletion_base.revision);
    }
    else
    {
        assert(deletion && scene.objects().size() == count - 1);
        assert(scene.undo() && scene.objects().size() == count);
        assert(!scene.component(original, cxx::typeToken<simulation::ecs::Transform3D>()));
        const auto restored = newEntities(scene, original_entities);
        assert(restored.size() == 1);
        original = restored.front();
        assert(scene.component(original, cxx::typeToken<simulation::ecs::Transform3D>()));
        assert(state() == deletion_base.current);
    }
    std::printf("scene structure: hierarchy=%d opaque=%d create/delete/restore, exact stale/provider/reference/cycle "
                "rejection; owner content/history preserved\n",
                scene.supportsHierarchy(), opaque);
    std::printf("structure active: objects=%zu hierarchy=%d opaque=%d elapsed_us=%.3f revision_delta=%llu retained=%zu\n",
                count, scene.supportsHierarchy(), opaque,
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - measured_begin).count(),
                static_cast<unsigned long long>(scene.historyView()->history.revision.value - initial.revision.value),
                scene.historyView()->history.charged_retained_bytes);
}
