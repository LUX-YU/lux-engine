#pragma once
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/asset/AssetImporter.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

struct ModelPlacementChecks final
{
    using Scene = lux::editor::scene::SceneEditor;
    std::unique_ptr<lux::editor::assets::AssetImporter> importer;
    lux::editor::assets::AssetImportId import;
    lux::editor::scene::ModelPlacementId request;
    lux::asset::AssetId model;
    lux::world::WorldObjectId object;
    lux::editor::editing::StateId before;
    std::size_t count{}, stage{}, notices{};
    lux::object::ScopedConnection completion;

    void begin(Scene &scene, lux::process::ExecutionRuntime &runtime)
    {
        using namespace lux;
        model = asset::AssetId(*uuids::uuid::from_string("00000000-0000-0000-0000-0000000000ac"));
        importer = std::make_unique<editor::assets::AssetImporter>(scene.project(), runtime);
        auto accepted = importer->requestModel({model, scene.project().root() / "Triangle.obj", "Models/Triangle", {}});
        assert(accepted);
        import = *accepted;
        count = scene.objects().size();
        completion = scene.observeScoped<Scene::modelPlacementFinished>(
            [this, &scene](auto id) noexcept
            {
                ++notices;
                assert(scene.modelPlacementStatus(id));
                auto reentrant = scene.acknowledgeModelPlacement(id);
                assert(!reentrant && reentrant.error().code == editor::EEditorError::BUSY);
            });
    }
    bool poll(Scene &scene, lux::editor::PollBudget &budget)
    {
        using namespace lux;
        using namespace editor;
        if (stage == 0)
        {
            importer->poll(budget);
            auto status = importer->status(import);
            assert(status);
            if (std::holds_alternative<assets::AssetImportPending>(*status))
            {
                return false;
            }
            if (const auto *failure = std::get_if<EditorFailure>(&*status))
            {
                std::printf("placement import failure %s:%llu %s\n", failure->domain.c_str(), failure->reason,
                            failure->message.c_str());
            }
            assert(std::holds_alternative<assets::AssetImportSucceeded>(*status));
            assert(importer->acknowledge(import));
            importer->requestClose();
            stage = 1;
        }
        if (stage == 1)
        {
            importer->poll(budget);
            if (importer->closeStatus().state != ECloseState::CLOSED)
            {
                return false;
            }
            importer.reset();
            auto reference = scene.project().reference(model);
            auto foreign = reference;
            ++foreign.project_instance;
            auto refused =
                scene.requestModelPlacement(foreign, Eigen::Vector3d::Zero(), partition::PartitionOrdinal{0});
            assert(!refused && refused.error().domain == "project.asset-reference");
            auto accepted =
                scene.requestModelPlacement(reference, Eigen::Vector3d::Zero(), partition::PartitionOrdinal{0});
            assert(accepted);
            request = *accepted;
            assert(!scene.requestModelPlacement(reference, Eigen::Vector3d::Zero(), partition::PartitionOrdinal{0}));
            using Transform = simulation::ecs::Transform3D;
            auto target = scene.writeTarget(scene.objects().front().object);
            assert(target);
            assert(scene.setField<Transform>(
                *target, "translation", "Move during model load",
                [](auto &value) noexcept
                {
                    return &value.translation;
                },
                Eigen::Vector3d{1, 2, 3}));
            before = scene.historyView()->history.current;
            stage = 2;
        }
        if (stage == 2)
        {
            auto status = scene.modelPlacementStatus(request);
            assert(status);
            if (std::holds_alternative<editor::scene::ModelPlacementPending>(*status))
            {
                return false;
            }
            const auto *failure = std::get_if<EditorFailure>(&*status);
            assert(failure && failure->domain == "model.place");
            const auto *cause = std::any_cast<editing::EditFailure>(&failure->cause);
            assert(cause && cause->code == editing::EEditError::STALE_BASE);
            assert(scene.objects().size() == count && scene.historyView()->history.current == before && notices == 1);
            assert(scene.retryModelPlacement(request, before));
            stage = 3;
        }
        if (stage == 3)
        {
            auto status = scene.modelPlacementStatus(request);
            assert(status);
            if (std::holds_alternative<editor::scene::ModelPlacementPending>(*status))
            {
                return false;
            }
            if (const auto *failure = std::get_if<EditorFailure>(&*status))
            {
                std::printf("placement failure %s:%llu %s\n", failure->domain.c_str(), failure->reason,
                            failure->message.c_str());
            }
            const auto *success = std::get_if<editor::scene::ModelPlacementSucceeded>(&*status);
            assert(success && success->objects == 1 && notices == 2);
            object = success->root;
            assert(scene.objects().size() == count + 1);
            const auto current = scene.historyView()->history.current;
            assert(scene.undo() && scene.objects().size() == count);
            assert(scene.redo() && scene.objects().size() == count + 1 &&
                   scene.historyView()->history.current == current);
            assert(scene.acknowledgeModelPlacement(request));
            assert(!scene.modelPlacementStatus(request));
            auto accepted = scene.requestModelPlacement(scene.project().reference(model), Eigen::Vector3d::Zero(),
                                                        partition::PartitionOrdinal{0});
            assert(accepted);
            request = *accepted;
            before = scene.historyView()->history.current;
            assert(scene.cancelModelPlacement(request));
            stage = 4;
        }
        if (stage == 4)
        {
            auto status = scene.modelPlacementStatus(request);
            assert(status);
            if (std::holds_alternative<editor::scene::ModelPlacementPending>(*status))
            {
                return false;
            }
            assert(std::holds_alternative<editor::scene::ModelPlacementCancelled>(*status) && notices == 3);
            assert(scene.objects().size() == count + 1 && scene.historyView()->history.current == before);
            assert(scene.acknowledgeModelPlacement(request));
            stage = 5;
        }
        if (stage == 5)
        {
            using Mesh = simulation::ecs::Mesh3D;
            const auto *mesh = static_cast<const Mesh *>(scene.component(object, cxx::typeToken<Mesh>()));
            assert(mesh);
            const auto resources = scene.resources();
            if (!resources)
            {
                return false;
            }
            for (const auto &row : resources->rows)
            {
                if (row.key.mesh == mesh->value.mesh && row.state == editor::scene::ESceneResourceState::READY)
                {
                    std::puts("actual import -> Process read/CPU decode/Main placement: stale-base retained + retry, "
                              "unique request, Undo/Redo, cancellation and imported GPU resource READY");
                    stage = 6;
                    return true;
                }
            }
        }
        return false;
    }
    void closeWithPending(Scene &scene)
    {
        using namespace lux;
        auto accepted = scene.requestModelPlacement(scene.project().reference(model), Eigen::Vector3d::Zero(),
                                                    partition::PartitionOrdinal{0});
        assert(accepted);
        request = *accepted;
        const auto count = scene.objects().size();
        completion = scene.observeScoped<Scene::modelPlacementFinished>(
            [count, &scene](auto id) noexcept
            {
                auto status = scene.modelPlacementStatus(id);
                assert(status && std::holds_alternative<editor::scene::ModelPlacementCancelled>(*status));
                assert(scene.objects().size() == count);
                std::puts("pending placement close: retained sender delivered cancellation; no late model insertion");
            });
        scene.requestClose();
        assert(scene.closeStatus().state == editor::ECloseState::CLOSING);
    }
};
