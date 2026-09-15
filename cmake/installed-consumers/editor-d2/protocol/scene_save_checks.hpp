#pragma once
#include "scene_structure_checks.hpp"

#include <cassert>
#include <cstdio>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
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
    lux::editor::scene::PreviewToken retired_preview;
    lux::editor::editing::StateId captured;
    lux::world::WorldObjectId object;
    HANDLE denied{INVALID_HANDLE_VALUE};
    std::string mode;
    bool retried{};
    std::size_t model_count{};
    lux::world::WorldObjectId structure_parent, structure_deleted;
    std::vector<lux::world::WorldObjectId> placed;

    static auto access()
    {
        return [](auto &component) noexcept
        {
            return &component.translation;
        };
    }
    void begin(lux::editor::scene::SceneEditor &scene, std::string requested_mode,
               lux::world::WorldObjectId selected = {})
    {
        mode = std::move(requested_mode);
        checkSceneStructure(scene, mode == "save-preservation");
        object = selected.valid() ? selected : scene.objects().front().object;
        if (mode == "save-model" || mode == "save-hierarchy" || mode == "save-preservation")
        {
            using namespace lux;
            using Mesh = simulation::ecs::Mesh3D;
            const auto *mesh = static_cast<const Mesh *>(scene.component(object, cxx::typeToken<Mesh>()));
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
            model_count = scene.objects().size();
            const auto before = scene.historyView()->history;
            const auto old_selection = scene.selection().object;
            auto failed =
                scene.placeModel(before.current, **source, {0, 0, 0},
                                 partition::PartitionOrdinal{static_cast<std::uint32_t>(scene.partitionCount())});
            assert(!failed && failed.error().domain_code ==
                                  static_cast<std::uint64_t>(editor::scene::EModelPlacementError::INVALID_PARTITION));
            assert(scene.objects().size() == model_count && scene.historyView()->history.current == before.current);
            std::size_t notices{};
            auto observer = scene.observeScoped<editor::scene::SceneEditor::objectsChanged>(
                [&](editor::editing::Revision revision) noexcept
                {
                    ++notices;
                    assert(scene.historyView()->history.revision == revision);
                    for (const auto &row : scene.objects())
                    {
                        assert(scene.component(row.object, cxx::typeToken<Transform>()));
                    }
                    auto reentrant = scene.select({});
                    assert(!reentrant && reentrant.error().code == editor::EEditorError::BUSY);
                });
            auto created = scene.placeModel(before.current, **source, {3, 0, 0},
                                            partition::PartitionOrdinal{mode == "save-preservation" ? 1U : 0U});
            if (!created)
            {
                std::printf("place failure=%u domain=%llu\n", unsigned(created.error().code),
                            created.error().domain_code);
            }
            const std::size_t added = mode == "save-hierarchy" ? 3 : 1;
            assert(created && created->size() == added && scene.objects().size() == model_count + added &&
                   notices == 1);
            placed = *created;
            object = placed.back();
            const auto *value = static_cast<const Transform *>(scene.component(object, cxx::typeToken<Transform>()));
            if (mode == "save-hierarchy")
            {
                assert(value && value->translation == Value::Zero());
                for (std::size_t index{}; index < placed.size(); ++index)
                {
                    const auto row =
                        std::ranges::find(scene.objects(), placed[index], &editor::scene::SceneObjectRow::object);
                    assert(row != scene.objects().end());
                    assert(row->parent == (index ? placed[index - 1] : world::WorldObjectId{}));
                    assert(scene.component(placed[index], cxx::typeToken<simulation::ecs::Parent>()));
                }
            }
            else
            {
                assert(value && value->translation == Value(4, 2, 0));
                assert(!scene.component(object, cxx::typeToken<simulation::ecs::Parent>()));
            }
            assert(scene.undo() && scene.objects().size() == model_count && notices == 2 &&
                   scene.selection().object == old_selection);
            assert(scene.redo() && scene.objects().size() == model_count + added && notices == 3);
            assert(scene.component(object, cxx::typeToken<Mesh>()));
            auto stale = scene.placeModel(before.current, **source, {0, 0, 0}, partition::PartitionOrdinal{0});
            assert(!stale && stale.error().code == editor::editing::EEditError::STALE_BASE &&
                   scene.objects().size() == model_count + added);
            std::printf("model placement: hierarchy=%d added=%zu exact invalid-partition/stale failures, one history "
                        "entry, IDs/selection/notifications restored\n",
                        mode == "save-hierarchy", added);
        }
        if (mode == "save-structure")
        {
            using namespace lux;
            const auto parent = scene.createObject(scene.historyView()->history.current, partition::PartitionOrdinal{0},
                                                   editor::scene::EObjectSpace::SPACE_3D);
            assert(parent);
            structure_parent = *parent;
            auto target = scene.writeTarget(object);
            assert(target && scene.reparent(*target, structure_parent));
            assert(scene.undo());
            assert(!scene.component(object, cxx::typeToken<simulation::ecs::Parent>()));
            assert(scene.redo());
            const auto other = std::ranges::find_if(scene.objects(),
                                                    [&](const auto &row)
                                                    {
                                                        return row.object != object && row.object != structure_parent;
                                                    });
            assert(other != scene.objects().end());
            structure_deleted = other->object;
            assert(scene.eraseObjects(scene.historyView()->history.current, std::span(&structure_deleted, 1)));
        }
        auto target = scene.writeTarget(object);
        assert(target);
        auto preview =
            scene.beginPreview<Transform, Value>(*target, "retired-pane", "translation", "Translation", access());
        assert(preview && scene.cancelPreview(*preview));
        retired_preview = *preview;
        const Value first{4, 5, 6};
        assert(scene.setField<Transform>(*target, "Transform.translation", "Translation", access(), first));
        captured = scene.historyView()->history.current;
        if (mode == "save-retry" || mode == "save-partial")
        {
            const auto file = scene.project().root() / (mode == "save-retry" ? "Main.luxscene" : "Project.luxproject");
            denied = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            assert(denied != INVALID_HANDLE_VALUE);
        }
        const auto accepted = scene.requestSave("d2-save-verification");
        assert(accepted);
        request = *accepted;
        const auto duplicate = scene.requestSave("second-request");
        assert(!duplicate && duplicate.error().code == lux::editor::EEditorError::BUSY);
        target = scene.writeTarget(object);
        assert(target);
        const Value second{7, 8, 9};
        assert(scene.setField<Transform>(*target, "Transform.translation", "Translation", access(), second));
        assert(scene.historyView()->history.current != captured);
        std::printf("save begin: captured=%llu current=%llu pending=%d\n", captured.serial,
                    scene.historyView()->history.current.serial, scene.historyView()->history.save_pending);
    }
    bool finished(lux::editor::scene::SceneEditor &scene)
    {
        const auto current = scene.saveStatus(request);
        assert(current);
        const auto *transform =
            static_cast<const Transform *>(scene.component(object, lux::cxx::typeToken<Transform>()));
        assert(transform && transform->translation == Value(7, 8, 9));
        if (const auto *failure = std::get_if<lux::editor::SaveRetryable>(&*current))
        {
            std::printf("save failed: domain=%s reason=%llu attempt=%llu state=%llu\n", failure->failure.domain.c_str(),
                        failure->failure.reason, failure->attempt, failure->captured.serial);
            std::printf("failure path: %s\n", failure->failure.message.c_str());
            assert(!retried && (mode == "save-retry" || mode == "save-partial"));
            const auto *cause = std::any_cast<lux::editor::ProjectPublicationFailure>(&failure->failure.cause);
            assert(cause && cause->code == lux::editor::EProjectPublicationError::REPLACE);
            assert(cause->published_files == (mode == "save-partial" ? 1 : 0));
            assert(failure->captured == captured && failure->attempt == 1);
            assert(scene.historyView()->history.save_pending);
            std::printf("accurate replacement failure: files_published=%zu live_S2_unchanged=1 ticket_S1_retained=1\n",
                        cause->published_files);
            CloseHandle(denied);
            denied = INVALID_HANDLE_VALUE;
            assert(scene.retrySave(request));
            retried = true;
            return false;
        }
        if (const auto *success = std::get_if<lux::editor::SaveSucceeded>(&*current))
        {
            assert(success->captured == captured && success->cleanup);
            const auto state = scene.historyView()->history;
            assert(state.saved == captured && !state.clean && !state.save_pending);
            assert(mode == "save" || mode == "save-structure" || mode == "save-preservation" ||
                   mode == "save-hierarchy" || mode == "save-model" || mode == "save-import-model" || retried);
            std::printf("save success: saved=%llu current=%llu dirty=%d same_capture=1\n", state.saved->serial,
                        state.current.serial, !state.clean);
            assert(scene.acknowledgeSave(request));
            const auto stale = scene.saveStatus(request);
            assert(!stale && stale.error().code == lux::editor::EEditorError::STALE_REQUEST);
            return true;
        }
        assert(std::holds_alternative<lux::editor::SavePending>(*current));
        return false;
    }
    void reopened(lux::editor::scene::SceneEditor &scene)
    {
        const auto *value = static_cast<const Transform *>(scene.component(object, lux::cxx::typeToken<Transform>()));
        assert(value && value->translation == Value(4, 5, 6));
        assert(scene.historyView()->history.clean);
        const auto before_late = scene.historyView()->history;
        const auto late = scene.commitPreview(retired_preview);
        assert(!late && late.error().code == lux::editor::editing::EEditError::STALE_TARGET);
        assert(scene.historyView()->history.current == before_late.current &&
               scene.historyView()->history.revision == before_late.revision);

        if (mode == "save-model" || mode == "save-hierarchy" || mode == "save-preservation")
        {
            assert(scene.objects().size() == model_count + placed.size());
            assert(scene.component(object, lux::cxx::typeToken<lux::simulation::ecs::Mesh3D>()));
            if (mode == "save-hierarchy")
            {
                for (std::size_t index{}; index < placed.size(); ++index)
                {
                    const auto row =
                        std::ranges::find(scene.objects(), placed[index], &lux::editor::scene::SceneObjectRow::object);
                    assert(row != scene.objects().end() && row->partition.value == 0);
                    assert(row->parent == (index ? placed[index - 1] : lux::world::WorldObjectId{}));
                }
            }
            std::puts("model reopen: persistent object identity, author Parent links, Mesh3D and partition membership "
                      "survived real source save");
        }
        if (mode == "save-structure")
        {
            using namespace lux;
            const auto row = std::ranges::find(scene.objects(), object, &editor::scene::SceneObjectRow::object);
            assert(row != scene.objects().end() && row->parent == structure_parent);
            assert(scene.component(structure_parent, cxx::typeToken<Transform>()));
            assert(!scene.component(structure_deleted, cxx::typeToken<Transform>()));
            assert(scene.component(object, cxx::typeToken<simulation::ecs::Parent>()));
            std::puts("structure reopen: created identity and newly added Parent survived; deleted original object "
                      "remains absent");
        }
        std::puts("save reopen: actual author value=(4,5,6), newer unsaved (7,8,9) was not written; clean=1");
    }
};
