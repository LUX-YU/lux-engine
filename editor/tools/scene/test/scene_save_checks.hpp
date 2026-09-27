#include "ToolTestAccess.hpp"
#pragma once

#include "entity_checks.hpp"
#include "scene_structure_checks.hpp"

#include <cassert>
#include <cstdio>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

struct SceneSaveChecks final
{
    using Transform = lux::simulation::ecs::Transform3D;
    using Value = decltype(Transform::translation);
    lux::editor::SaveRequestId request;
    lux::editor::scene::FieldEditToken retired_preview;
    lux::editor::editing::StateId captured;
    lux::scene::SceneInstanceId original_instance;
    lux::simulation::ecs::Entity object{lux::simulation::ecs::NullEntity};
    HANDLE denied{INVALID_HANDLE_VALUE};
    std::string mode;
    bool retried{};
    std::size_t model_count{};
    lux::simulation::ecs::Entity structure_parent{lux::simulation::ecs::NullEntity}, structure_deleted{lux::simulation::ecs::NullEntity};
    std::vector<lux::simulation::ecs::Entity> placed;
    std::size_t saved_object_count{};
    Value deleted_position{};
    lux::asset::AssetId deleted_mesh;

    static auto access()
    {
        return [](auto& component) noexcept { return &component.translation; };
    }
    void begin(
        lux::editor::scene::SceneEditor& scene,
        std::string requested_mode,
        lux::simulation::ecs::Entity selected = lux::simulation::ecs::NullEntity
    )
    {
        mode = std::move(requested_mode);
        // Structural regression restores an entity with a new generation. The
        // imported selection is not the fixture's first object erased there.
        checkSceneStructure(scene, mode == "save-preservation");
        original_instance = toolTest(scene).instance();
        object = selected != lux::simulation::ecs::NullEntity ? selected : sceneObjects(scene).front().object;
        if (mode == "save-model" || mode == "save-hierarchy" || mode == "save-preservation")
        {
            using namespace lux;
            using Mesh = simulation::ecs::Mesh3D;
            const auto* mesh = static_cast<const Mesh*>(toolTest(scene).component(object, cxx::typeToken<Mesh>()));
            assert(mesh);
            auto model = std::make_shared<rdesc::ModelDescription>();
            model->primitives.push_back({mesh->value.mesh, mesh->value.material});
            model->nodes.resize(2);
            model->nodes[0].children.push_back(1);
            model->nodes[0].local_transform.translation() = Eigen::Vector3f{1, 0, 0};
            model->nodes[1].local_transform.translation() = Eigen::Vector3f{0, 2, 0};
            model->nodes[1].primitives.push_back(0);
            const auto id = asset::AssetId(*uuids::uuid::from_string("00000000-0000-0000-0000-0000000000ab"));
            auto source = asset::ModelAsset::create({id, asset::ModelAsset::asset_type}, model);
            assert(source);
            model_count = sceneObjects(scene).size();
            const auto before = scene.historyView()->history;
            const auto old_selection = toolTest(scene).selection().object;
            const auto old_entities = entitySnapshot(scene);
            auto failed = toolTest(scene).createEntitiesFromModel(
                before.current,
                **source,
                {0, 0, 0},
                partition::PartitionOrdinal{static_cast<std::uint32_t>(toolTest(scene).partitionCount())}
            );
            assert(
                !failed && failed.error().domain_code ==
                               static_cast<std::uint64_t>(editor::scene::EModelCreationError::INVALID_PARTITION)
            );
            assert(sceneObjects(scene).size() == model_count && scene.historyView()->history.current == before.current);
            std::size_t notices{};
            auto observer = lux::object::LuxObject::connect(
                                std::addressof(scene),
                                &editor::scene::SceneEditor::objectsChanged,
                                [&](editor::editing::Revision revision) noexcept {
                                    ++notices;
                                    assert(scene.historyView()->history.revision == revision);
                                    for (const auto& row : sceneObjects(scene))
                                    {
                                        assert(toolTest(scene).component(row.object, cxx::typeToken<Transform>()));
                                    }
                                    auto reentrant = toolTest(scene).select(lux::simulation::ecs::NullEntity);
                                    assert(!reentrant && reentrant.error().code == editor::EEditorError::BUSY);
                                }
            ).value();
            auto created = toolTest(scene).createEntitiesFromModel(
                before.current,
                **source,
                {3, 0, 0},
                partition::PartitionOrdinal{mode == "save-preservation" ? 1U : 0U}
            );
            if (!created)
            {
                std::printf(
                    "place failure=%u domain=%llu\n",
                    unsigned(created.error().code),
                    created.error().domain_code
                );
            }
            const std::size_t added = mode == "save-hierarchy" ? 2 : 1;
            assert(
                created && created->size() == added && sceneObjects(scene).size() == model_count + added && notices == 1
            );
            placed = *created;
            object = placed.back();
            const auto* value = static_cast<const Transform*>(toolTest(scene).component(object, cxx::typeToken<Transform>()));
            if (mode == "save-hierarchy")
            {
                assert(value && value->translation == Value(0, 2, 0));
                for (std::size_t index{}; index < placed.size(); ++index)
                {
                    const auto snapshot = sceneObjects(scene);
                    const auto row = std::ranges::find(snapshot, placed[index], &TestObjectRow::object);
                    assert(row != snapshot.end());
                    assert(index ? row->parent == placed[index - 1] : row->parent == lux::simulation::ecs::NullEntity);
                    assert(toolTest(scene).component(placed[index], cxx::typeToken<simulation::ecs::Parent>()));
                }
            }
            else
            {
                assert(value && value->translation == Value(4, 2, 0));
                assert(!toolTest(scene).component(object, cxx::typeToken<simulation::ecs::Parent>()));
            }
            assert(
                scene.undo() && sceneObjects(scene).size() == model_count && notices == 2 &&
                toolTest(scene).selection().object == old_selection
            );
            assert(scene.redo() && sceneObjects(scene).size() == model_count + added && notices == 3);
            assert(!toolTest(scene).component(object, cxx::typeToken<Mesh>()));
            placed = newEntities(scene, old_entities);
            if (mode == "save-hierarchy")
            {
                std::ranges::sort(placed, [&](auto left, auto right) {
                    const auto has_mesh = [&](auto entity) {
                        return toolTest(scene).component(entity, cxx::typeToken<Mesh>()) != nullptr;
                    };
                    return has_mesh(left) < has_mesh(right);
                });
            }
            object = placed.back();
            assert(toolTest(scene).component(object, cxx::typeToken<Mesh>()));
            auto stale =
                toolTest(scene).createEntitiesFromModel(before.current, **source, {0, 0, 0}, partition::PartitionOrdinal{0});
            assert(
                !stale && stale.error().code == editor::editing::EEditError::STALE_BASE &&
                sceneObjects(scene).size() == model_count + added
            );
            std::printf(
                "model placement: hierarchy=%d added=%zu exact invalid-partition/stale failures, one history "
                "entry, IDs/selection/notifications restored\n",
                mode == "save-hierarchy",
                added
            );
        }
        if (mode == "save-structure")
        {
            using namespace lux;
            const auto parent = toolTest(scene).createObject(
                scene.historyView()->history.current,
                partition::PartitionOrdinal{0},
                editor::scene::EObjectSpace::SPACE_3D
            );
            assert(parent);
            structure_parent = *parent;
            auto target = toolTest(scene).writeTarget(object);
            assert(target && toolTest(scene).reparent(*target, structure_parent));
            assert(scene.undo());
            assert(!toolTest(scene).component(object, cxx::typeToken<simulation::ecs::Parent>()));
            assert(scene.redo());
            const auto snapshot = sceneObjects(scene);
            const auto other = std::ranges::find_if(snapshot, [&](const auto& row) {
                return row.object != object && row.object != structure_parent;
            });
            assert(other != snapshot.end());
            structure_deleted = other->object;
            deleted_position =
                static_cast<const Transform*>(toolTest(scene).component(structure_deleted, cxx::typeToken<Transform>()))
                    ->translation;
            if (const auto* mesh = static_cast<const simulation::ecs::Mesh3D*>(
                    toolTest(scene).component(structure_deleted, cxx::typeToken<simulation::ecs::Mesh3D>())
                ))
            {
                deleted_mesh = mesh->value.mesh;
            }
            assert(toolTest(scene).eraseObjects(scene.historyView()->history.current, std::span(&structure_deleted, 1)));
        }
        saved_object_count = sceneObjects(scene).size();
        auto target = toolTest(scene).writeTarget(object);
        assert(target);
        auto preview =
            toolTest(scene).beginFieldEdit<Transform, Value>(*target, "retired-pane", "translation", "Translation", access());
        assert(preview && toolTest(scene).finishFieldEdit(*preview));
        retired_preview = *preview;
        const Value first{4, 5, 6};
        assert(toolTest(scene).setField<Transform>(*target, "Transform.translation", "Translation", access(), first));
        captured = scene.historyView()->history.current;
        if (mode == "save-retry" || mode == "save-partial")
        {
            const auto file = toolTest(scene).project().root() / (mode == "save-retry" ? "Main.luxscene" : "Project.luxproject");
            denied = CreateFileW(
                file.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            assert(denied != INVALID_HANDLE_VALUE);
        }
        const auto accepted = scene.requestSave("d2-save-verification");
        assert(accepted);
        request = *accepted;
        const auto duplicate = scene.requestSave("second-request");
        assert(!duplicate && duplicate.error().code == lux::editor::EEditorError::BUSY);
        target = toolTest(scene).writeTarget(object);
        assert(target);
        const Value second{7, 8, 9};
        assert(toolTest(scene).setField<Transform>(*target, "Transform.translation", "Translation", access(), second));
        assert(scene.historyView()->history.current != captured);
        std::printf(
            "save begin: captured=%llu current=%llu pending=%d\n",
            captured.serial,
            scene.historyView()->history.current.serial,
            scene.historyView()->history.save_pending
        );
    }
    bool finished(lux::editor::scene::SceneEditor& scene)
    {
        const auto current = scene.saveStatus(request);
        assert(current);
        const auto* transform =
            static_cast<const Transform*>(toolTest(scene).component(object, lux::cxx::typeToken<Transform>()));
        assert(transform && transform->translation == Value(7, 8, 9));
        if (const auto* failure = std::get_if<lux::editor::SaveRetryable>(&*current))
        {
            std::printf(
                "save failed: domain=%s reason=%llu attempt=%llu state=%llu\n",
                failure->failure.domain.c_str(),
                failure->failure.reason,
                failure->attempt,
                failure->captured.serial
            );
            std::printf("failure path: %s\n", failure->failure.message.c_str());
            assert(!retried && (mode == "save-retry" || mode == "save-partial"));
            const auto* cause = std::any_cast<lux::editor::ProjectPublicationFailure>(&failure->failure.cause);
            assert(cause && cause->code == lux::editor::EProjectPublicationError::REPLACE);
            assert(cause->published_files == (mode == "save-partial" ? 1 : 0));
            assert(failure->captured == captured && failure->attempt == 1);
            assert(scene.historyView()->history.save_pending);
            std::printf(
                "accurate replacement failure: files_published=%zu live_S2_unchanged=1 ticket_S1_retained=1\n",
                cause->published_files
            );
            CloseHandle(denied);
            denied = INVALID_HANDLE_VALUE;
            assert(scene.retrySave(request));
            retried = true;
            return false;
        }
        if (const auto* success = std::get_if<lux::editor::SaveSucceeded>(&*current))
        {
            assert(success->captured == captured && success->cleanup);
            const auto state = scene.historyView()->history;
            assert(state.saved == captured && !state.clean && !state.save_pending);
            assert(
                mode == "save" || mode == "save-spatial" || mode == "save-structure" || mode == "save-preservation" ||
                mode == "save-hierarchy" || mode == "save-model" || mode == "save-import-model" || retried
            );
            std::printf(
                "save success: saved=%llu current=%llu dirty=%d same_capture=1\n",
                state.saved->serial,
                state.current.serial,
                !state.clean
            );
            assert(scene.acknowledgeSave(request));
            const auto stale = scene.saveStatus(request);
            assert(!stale && stale.error().code == lux::editor::EEditorError::STALE_REQUEST);
            return true;
        }
        assert(std::holds_alternative<lux::editor::SavePending>(*current));
        return false;
    }
    void reopened(lux::editor::scene::SceneEditor& scene)
    {
        assert(original_instance != toolTest(scene).instance());
        object = lux::simulation::ecs::NullEntity;
        for (const auto& row : sceneObjects(scene))
        {
            const auto* candidate =
                static_cast<const Transform*>(toolTest(scene).component(row.object, lux::cxx::typeToken<Transform>()));
            if (candidate && candidate->translation == Value(4, 5, 6))
            {
                assert(object == lux::simulation::ecs::NullEntity);
                object = row.object;
            }
        }
        assert(object != lux::simulation::ecs::NullEntity && sceneObjects(scene).size() == saved_object_count);
        if (!placed.empty())
        {
            auto current = object;
            for (std::size_t index = placed.size(); index > 0; --index)
            {
                placed[index - 1] = current;
                const auto snapshot = sceneObjects(scene);
                const auto row = std::ranges::find(snapshot, current, &TestObjectRow::object);
                assert(row != snapshot.end());
                current = row->parent;
            }
        }
        const auto* value = static_cast<const Transform*>(toolTest(scene).component(object, lux::cxx::typeToken<Transform>()));
        assert(value && value->translation == Value(4, 5, 6));
        assert(scene.historyView()->history.clean);
        const auto before_late = scene.historyView()->history;
        const auto late = toolTest(scene).finishFieldEdit(retired_preview);
        assert(!late && late.error().code == lux::editor::editing::EEditError::STALE_TARGET);
        assert(
            scene.historyView()->history.current == before_late.current &&
            scene.historyView()->history.revision == before_late.revision
        );

        if (mode == "save-model" || mode == "save-hierarchy" || mode == "save-preservation")
        {
            assert(sceneObjects(scene).size() == model_count + placed.size());
            assert(toolTest(scene).component(object, lux::cxx::typeToken<lux::simulation::ecs::Mesh3D>()));
            if (mode == "save-hierarchy")
            {
                for (std::size_t index{}; index < placed.size(); ++index)
                {
                    const auto snapshot = sceneObjects(scene);
                    const auto row = std::ranges::find(snapshot, placed[index], &TestObjectRow::object);
                    assert(row != snapshot.end() && row->partition.value == 0);
                    assert(index ? row->parent == placed[index - 1] : row->parent == lux::simulation::ecs::NullEntity);
                }
            }
            std::puts("model reopen: persistent object identity, author Parent links, Mesh3D and partition membership "
                      "survived real source save");
        }
        if (mode == "save-structure")
        {
            using namespace lux;
            const auto snapshot = sceneObjects(scene);
            const auto row = std::ranges::find(snapshot, object, &TestObjectRow::object);
            assert(row != snapshot.end() && row->parent != lux::simulation::ecs::NullEntity);
            structure_parent = row->parent;
            assert(toolTest(scene).component(structure_parent, cxx::typeToken<Transform>()));
            assert(!toolTest(scene).component(structure_deleted, cxx::typeToken<Transform>()));
            for (const auto& entry : sceneObjects(scene))
            {
                const auto* transform =
                    static_cast<const Transform*>(toolTest(scene).component(entry.object, cxx::typeToken<Transform>()));
                const auto* mesh = static_cast<const simulation::ecs::Mesh3D*>(
                    toolTest(scene).component(entry.object, cxx::typeToken<simulation::ecs::Mesh3D>())
                );
                assert(
                    !transform || transform->translation != deleted_position ||
                    (mesh ? mesh->value.mesh : asset::AssetId{}) != deleted_mesh
                );
            }
            assert(toolTest(scene).component(object, cxx::typeToken<simulation::ecs::Parent>()));
            std::puts("structure reopen: created identity and newly added Parent survived; deleted original object "
                      "remains absent");
        }
        std::puts("save reopen: actual author value=(4,5,6), newer unsaved (7,8,9) was not written; clean=1");
    }
};
