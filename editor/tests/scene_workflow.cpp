#include "support/ModelSource.hpp"
#include "support/ProjectRequests.hpp"
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ModelPlacement.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/ui/Pane.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <source_location>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    assert(argc == 4);
    std::mt19937 random{std::random_device{}()};
    uuids::uuid_random_generator generate{random};
    const auto root = std::filesystem::absolute(argv[3]) / uuids::to_string(generate());
    std::filesystem::create_directories(root);
    const auto project_path = root / "Workflow.luxproj";
    ProjectManifest manifest{1, generate(), "Scene workflow", {{"lux.builtin.scene_render", 2}}};
    assert(writeProjectManifestAtomic(project_path, manifest, EProjectWrite::CREATE));
    EditorConfig config{"Scene product workflow", 900, 600};
    config.project_file = project_path;
    config.enable_validation = true;
    config.enable_vsync = false;
    config.execution.task_capacity = 8;
    config.plugin_locations = {{argv[1], argv[2]}};
    config.layout = {{"scene.creation", "scenes", "Scenes"}, {"scene.viewport", "view", "View"}};
    auto made = LuxEngine::create(
        std::move(config),
        [](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            assert(composition.registerSceneProfile(sceneProfile3D()));
            assert(composition.registerServiceFactory<ProjectAssets>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<ProjectAssets>>
                {
                    auto assets = std::make_unique<ProjectAssets>(context);
                    assert(assets->open());
                    return assets;
                }
            ));
            assert(composition.registerServiceFactory<ProjectScenes>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<ProjectScenes>>
                { return std::make_unique<ProjectScenes>(context, context.service<ProjectAssets>()->get()); }
            ));
            assert(composition.registerUiFactory(
                "scene.creation",
                [](EditorContext& context,
                   const PaneDescription& pane) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    auto scenes = context.service<ProjectScenes>();
                    assert(scenes);
                    return std::make_unique<SceneCreationPane>(context, scenes->get(), pane.title);
                }
            ));
            return composition.registerUiFactory(
                "scene.viewport",
                [](EditorContext& context,
                   const PaneDescription& pane) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    auto scenes = context.service<ProjectScenes>();
                    assert(scenes);
                    return std::make_unique<SceneViewportPane>(
                        scenes->get(),
                        context.service<ProjectAssets>()->get(),
                        context.engine().sceneRuntime(),
                        pane.title
                    );
                }
            );
        }
    );
    assert(made);
    auto& host = **made;
    std::vector<SceneViewportId> observed_views;
    auto until = [&](auto predicate, std::source_location location = std::source_location::current())
    {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fprintf(
                    stderr,
                    "Timeout at workflow line %u; instances=%zu\n",
                    location.line(),
                    host.engine().sceneRuntime().instanceCount()
                );
                for (const auto& task : host.engine().execution().taskInfos())
                {
                    std::fprintf(
                        stderr,
                        "Task %s: state=%u phase=%s\n",
                        task.name.c_str(),
                        unsigned(task.state),
                        task.phase.c_str()
                    );
                }
                {
                    if (auto* collection = host.project().services().constructed<ProjectScenes>(); collection)
                    {
                        if (collection->failure())
                        {
                            std::fprintf(stderr, "%s\n", describeProjectSceneFailure(*collection->failure()).c_str());
                        }
                        for (auto id : observed_views)
                        {
                            const auto view = collection->viewport(id);
                            std::fprintf(
                                stderr,
                                "Viewport %u: %ux%u pending=%d ready=%d image=%llu error=%llu\n",
                                id.index,
                                view.description.width,
                                view.description.height,
                                view.pending,
                                view.ready,
                                static_cast<unsigned long long>(view.image.value),
                                static_cast<unsigned long long>(view.error.type)
                            );
                            const auto registry =
                                std::as_const(host.engine().sceneRuntime()).borrowInstance(view.instance);
                            if (registry && registry->get().valid(view.camera))
                            {
                                if (const auto* targets =
                                        registry->get().template try_get<scene::CameraTargetsComponent>(view.camera))
                                {
                                    for (const auto& target : targets->targets)
                                    {
                                        std::fprintf(
                                            stderr,
                                            "Camera target %llu: %ux%u revision=%llu epoch=%llu\n",
                                            static_cast<unsigned long long>(target.id.value),
                                            target.description.width,
                                            target.description.height,
                                            static_cast<unsigned long long>(target.revision),
                                            static_cast<unsigned long long>(target.epoch)
                                        );
                                    }
                                }
                            }
                        }
                    }
                }
                assert(false);
            }
            auto advanced = detail::LuxEngineTestAccess::pumpOnce(host);
            if (!advanced)
            {
                std::fprintf(stderr, "Pump error %llu\n", static_cast<unsigned long long>(advanced.error().type));
            }
            assert(advanced && *advanced);
            std::this_thread::yield();
        }
    };
    auto& context = host.project();
    until([&] { return context.service<ProjectAssets>()->get().ready(); });
    auto service = context.service<ProjectScenes>();
    assert(service);
    auto& scenes = service->get();
    scene::RenderSystem* scene_projection = nullptr;
    auto projection_connection = object::LuxObject::connect(
        &scenes,
        &ProjectScenes::opened,
        &host,
        [&](asset::AssetId opened) noexcept
        {
            auto system =
                context.engine().sceneRuntime().borrowSystem<scene::RenderSystem>(scenes.find(opened)->instance());
            assert(system);
            scene_projection = *system; // Cold construction borrow; ends before that Scene's retirement.
        }
    );
    assert(projection_connection);
    const auto selection = [&]
    {
        auto package =
            sceneProfile3D().create({asset::AssetId{generate()}, "Defaults", context.sceneRegistrations(), {}});
        assert(package);
        SceneCreateSelection selected;
        selected.schemas.assign(package->world->data().schemas().begin(), package->world->data().schemas().end());
        selected.simulation = {package->simulation, &package->simulation->data()};
        selected.systems = {package->scene, &package->scene->data()};
        return selected;
    };
    auto incompatible = selection();
    std::erase_if(incompatible.schemas, [](const auto& value) { return value.name == "lux.ecs.Transform3D"; });
    const auto bad_id = asset::AssetId{generate()};
    assert(scenes.create({bad_id, "Invalid", "invalid.luxscene", "lux.editor.scene.3d"}, std::move(incompatible)));
    until([&] { return scenes.failure().has_value(); });
    assert(!std::filesystem::exists(root / "invalid.luxscene"));
    assert(host.engine().sceneRuntime().instanceCount() == 0);

    const auto id = asset::AssetId{generate()};
    assert(scenes.create({id, "Authored", "Scenes/authored.luxscene", "lux.editor.scene.3d"}, selection()));
    until([&] { return scenes.find(id) != nullptr; });
    assert(!scenes.failure());
    assert(host.engine().sceneRuntime().instanceCount() == 1);
    auto persisted = readProjectManifest(project_path);
    assert(persisted && persisted->scenes.size() == 1 && persisted->startup_scene == id);
    auto package = scene::readScenePackageFile(root / "Scenes/authored.luxscene", 1U << 24);
    assert(package && package->scene->id() == id);
    const auto editing_instance = scenes.find(id)->instance();

    simulation::ecs::Transform3D left, right;
    left.translation = Eigen::Vector3d{0, 2, 8};
    right.translation = Eigen::Vector3d{5, 3, 8};
    auto a = scenes.addViewport(id, 320, 240);
    auto b = scenes.addViewport(id, 480, 240);
    assert(a && b);
    observed_views = {*a, *b};
    until([&] { return scenes.setViewportCameraTransform(*a, left); });
    until([&] { return scenes.setViewportCameraTransform(*b, right); });
    until(
        [&]
        {
            const auto first = scenes.viewport(*a), second = scenes.viewport(*b);
            if (first.error.type || second.error.type)
            {
                std::fprintf(
                    stderr,
                    "View errors %llu / %llu\n",
                    static_cast<unsigned long long>(first.error.type),
                    static_cast<unsigned long long>(second.error.type)
                );
                rendering::reportRenderFailure(first.error, "First viewport");
                rendering::reportRenderFailure(second.error, "Second viewport");
                assert(false);
            }
            return first.ready && second.ready;
        }
    );
    const auto original_target = scenes.viewport(*a).target;
    assert(scenes.viewport(*b).target != original_target);
    assert(scenes.updateViewport(*a, 640, 320));
    assert(!scenes.viewport(*a).ready);
    until([&] { return scenes.viewport(*a).ready; });
    assert(
        scenes.viewport(*a).target == original_target
    ); // Resize adopts physical candidates, not new logical identity.
    assert(scenes.selectViewportCamera(*a, {EViewportCamera::PRIMARY}));
    until([&] { return scenes.viewport(*a).error.type != 0 && !scenes.viewport(*a).ready; });
    assert(scenes.updateViewport(*a, 640, 320));
    assert(scenes.selectViewportCamera(*a, {}));
    until([&] { return scenes.viewport(*a).ready && scenes.viewport(*b).ready; });

    auto& assets = context.service<ProjectAssets>()->get();
    const auto source = fixture::modelSource(root / "Sources");
    assert(assets.importModel(source));
    until([&] { return !assets.busy(); });
    if (assets.failure())
    {
        std::fprintf(stderr, "Import: %s\n", assets.failure()->detail.c_str());
    }
    assert(!assets.failure() && assets.records().size() == 1);
    const auto imported = assets.records().front();
    std::fprintf(stderr, "Model imported: %zu products\n", imported.products.size());
    const auto* model = assets.model(imported.id);
    assert(model && model->data().primitives.size() == 2 && imported.products.size() >= 7);
    // Keep admitted work on Main pending, then prove temporary admission pressure is not an asset failure.
    std::size_t pending_tasks = 0;
    for (;;)
    {
        auto task = context.tasks().submit(
            {"Hold Main completion", "Verification"},
            [main = context.engine().execution().mainScheduler()](execution::TaskReporter) noexcept
            { return stdexec::schedule(main); }
        );
        if (!task)
        {
            assert(task.error() == execution::EExecutionError::CAPACITY_EXCEEDED);
            break;
        }
        ++pending_tasks;
        assert(pending_tasks <= 8);
    }
    assert(pending_tasks != 0);
    const auto query_mesh = model->data().primitives.front().mesh;
    const auto busy_geometry = assets.prepareMeshGeometry(query_mesh);
    assert(!busy_geometry && busy_geometry.error().code == EAssetImportError::BUSY);
    assert(context.engine().execution().dispatchMainCompletions());
    assert(assets.prepareMeshGeometry(query_mesh));
    until([&] { return assets.meshGeometry(query_mesh) != nullptr; });
    auto* document = scenes.find(id);
    auto placement = prepareModelPlacement(*document, *model, Eigen::Affine3d::Identity(), {0});
    std::fprintf(stderr, "Model placement prepared\n");
    assert(placement);
    const auto before_drop = document->objects()->size();
    until(
        [&]
        {
            auto result = document->apply(*placement);
            if (!result)
            {
                assert(isSceneEditBusy(result.error()));
            }
            return result.has_value();
        }
    );
    std::fprintf(stderr, "Model placement adopted\n");
    const auto after_drop = document->objects()->size();
    assert(after_drop > before_drop);
    assert(document->undo() && document->objects()->size() == before_drop);
    std::fprintf(stderr, "Model placement undo complete\n");
    assert(document->redo() && document->objects()->size() == after_drop);
    std::fprintf(stderr, "Model placement redo complete\n");
    auto& runtime = context.engine().sceneRuntime();
    auto projected = [&]()
    {
        auto borrowed = std::as_const(runtime).borrowInstance(document->instance());
        assert(borrowed);
        assert(scene_projection);
        std::size_t count = 0;
        for (auto entity : borrowed->get().view<const simulation::ecs::Mesh3D>())
        {
            assert(scene_projection->assetError(entity).type == 0);
            count += scene_projection->visualReady(entity);
        }
        return count;
    };
    until([&] { return projected() == 2 && scenes.viewport(*a).ready && scenes.viewport(*b).ready; });
    until(
        [&]
        {
            auto registry = std::as_const(runtime).borrowInstance(document->instance());
            const auto* query = registry ? registry->get().ctx().find<scene::MeshQuery>() : nullptr;
            return query && query->statistics().entities == 2;
        }
    );
    {
        auto registry = std::as_const(runtime).borrowInstance(document->instance());
        const auto& query = registry->get().ctx().get<scene::MeshQuery>();
        scene::RayHit3D hit;
        const auto picked = query.raycastNearest({{-1, 1, 3}, {0, 0, -1}}, 100, hit);
        assert(picked && *picked && std::abs(hit.distance - 3.0) < 1e-5);
    }
    const auto old_snapshot = context.assets().view().capture();
    assert(assets.reimport(imported.id));
    until([&] { return !assets.busy(); });
    assert(!assets.failure() && assets.records().front().revision == imported.revision + 1);
    assert(assets.records().front().products == imported.products);
    assert(assets.records().front().package != imported.package);
    assert(old_snapshot.open(imported.id)); // Old readers still own their exact published provider.
    const auto accepted_catalog = assets.revision();
    fixture::writeText(source, "this is not a model");
    assert(assets.reimport(imported.id));
    until([&] { return !assets.busy(); });
    assert(assets.failure() && !assets.failure()->catalog_published && assets.revision() == accepted_catalog);
    assert(assets.model(imported.id) && projected() == 2);
    assert(assets.importModel(root / "Sources" / "missing.obj"));
    until([&] { return !assets.busy(); });
    assert(assets.failure() && assets.revision() == accepted_catalog);
    assert(scenes.viewport(*a).ready && scenes.viewport(*b).ready);

    const auto root_object = std::get<SceneCreateObjects>(placement->operation).objects.front().id;
    assert(document->select(root_object));
    const auto transform_schema = simulation::ecs::componentSchemaId("lux.ecs.Transform3D");
    auto initial = document->component(root_object, transform_schema);
    assert(initial);
    auto edited_transform = *static_cast<const simulation::ecs::Transform3D*>(initial->data());
    assert(scenes.save(id));
    edited_transform.translation.x() += 0.75;
    auto edit = document->prepareComponentEdit(root_object, document->revision(), edited_transform);
    assert(edit);
    until(
        [&]
        {
            auto applied = document->apply(*edit);
            if (!applied)
            {
                assert(isSceneEditBusy(applied.error()));
            }
            return applied.has_value();
        }
    );
    until([&] { return !scenes.saving(id); });
    assert(!scenes.failure() && document->dirty()); // R reached disk; the intervening R+1 is still unsaved.
    assert(scenes.save(id));
    until([&] { return !scenes.saving(id); });
    assert(!scenes.failure() && !document->dirty());

    auto headless = scenes.startPlay(id, ESceneRunMode::HEADLESS);
    assert(headless && *headless != document->instance() && document->readOnly());
    assert(!runtime.borrowSystem<scene::RenderSystem>(*headless));
    until(
        [&]
        {
            auto clock = std::as_const(runtime).borrowClock(*headless);
            return clock && std::visit([](const auto& value) { return value.snapshot().step_index > 0; }, clock->get());
        }
    );
    bool mutated = false;
    until(
        [&]
        {
            auto change = [&](simulation::ecs::Registry& registry) noexcept
            {
                const auto* residency = registry.ctx().find<scene::WorldResidency>();
                const auto* identities = residency ? &residency->identities() : nullptr;
                if (!identities)
                {
                    return;
                }
                const auto entity = identities->entity(root_object);
                if (entity == simulation::ecs::NullEntity)
                {
                    return;
                }
                registry.patch<simulation::ecs::Transform3D>(entity, [](auto& value) { value.translation.x() = 900; });
                mutated = true;
            };
            static_cast<void>(runtime.editInstance(*headless, change));
            return mutated;
        }
    );
    auto author = document->component(root_object, transform_schema);
    assert(
        author && static_cast<const simulation::ecs::Transform3D*>(author->data())->translation.x() ==
                      edited_transform.translation.x()
    );
    scenes.stopPlay(id);
    until([&] { return !scenes.playInstance(id).valid(); });
    assert(!document->readOnly());

    auto graphical = scenes.startPlay(id, ESceneRunMode::GRAPHICAL);
    assert(graphical);
    auto play_view = scenes.addViewport(id, 200, 150, true);
    assert(play_view);
    until([&] { return scenes.setViewportCameraTransform(*play_view, left); });
    until([&] { return scenes.viewport(*play_view).ready; });
    scenes.stopPlay(id);
    until([&] { return !scenes.playInstance(id).valid(); });
    assert(scenes.viewport(*play_view).target.isNull() && scenes.viewport(*b).ready);

    scenes.closeViewport(*a);
    until([&] { return scenes.viewport(*a).target.isNull(); });
    assert(scenes.viewport(*b).ready);
    assert(scenes.close(id));
    scene_projection = nullptr;
    until([&] { return host.engine().sceneRuntime().instanceCount() == 0 && scenes.settled(); });
    assert(scenes.open(root / "Scenes/authored.luxscene"));
    until([&] { return scenes.find(id) != nullptr; });
    assert(scenes.find(id)->instance() != editing_instance);
    auto reopened_transform = scenes.find(id)->component(root_object, transform_schema);
    assert(
        reopened_transform &&
        static_cast<const simulation::ecs::Transform3D*>(reopened_transform->data())->translation ==
            edited_transform.translation
    );
    assert(scenes.open(root / "Scenes/authored.luxscene"));
    fixture::modelSource(root / "Sources");
    assert(assets.reimport(imported.id));
    fixture::close(host); // A decode may return after closure; it cannot adopt into the next Context.
    assert(host.run());
    assert(host.engine().sceneRuntime().instanceCount() == 0);
    made->reset();
    std::puts("Scene creation, incompatibility, multi-view, resize, reopen and late close passed");
}
