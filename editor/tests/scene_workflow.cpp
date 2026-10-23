#include "support/ProjectRequests.hpp"
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
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
    config.enable_validation = true;
    config.enable_vsync = false;
    config.plugin_locations = {{argv[1], argv[2]}};
    config.layout = {{"scene.creation", "scenes", "Scenes"}, {"scene.viewport", "view", "View"}};
    auto made = LuxEngine::create(
        std::move(config),
        [](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            assert(composition.registerSceneProfile(sceneProfile3D()));
            assert(composition.registerServiceFactory<ProjectScenes>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<ProjectScenes>>
                { return std::make_unique<ProjectScenes>(context); }
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
                        context.engine().sceneRuntime(),
                        pane.title
                    );
                }
            );
        }
    );
    assert(made);
    auto& host = **made;
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
                if (host.project())
                {
                    if (auto* collection = host.project()->services().constructed<ProjectScenes>();
                        collection && collection->failure())
                    {
                        std::fprintf(stderr, "%s\n", describeProjectSceneFailure(*collection->failure()).c_str());
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
    assert(fixture::open(host, project_path));
    until([&] { return host.project() != nullptr; });
    auto& context = *host.project();
    auto service = context.service<ProjectScenes>();
    assert(service);
    auto& scenes = service->get();
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
    auto package = readScenePackageFile(root / "Scenes/authored.luxscene", 1U << 24);
    assert(package && package->scene->id() == id);
    const auto editing_instance = scenes.find(id)->instance();

    scene::SceneCameraInput left, right;
    left.transform.translation() = Eigen::Vector3d{0, 2, 8};
    right.transform.translation() = Eigen::Vector3d{5, 3, 8};
    auto a = scenes.addViewport(id, left, 320, 240);
    auto b = scenes.addViewport(id, right, 480, 240);
    assert(a && b);
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
                assert(false);
            }
            return first.ready && second.ready;
        }
    );
    const auto original_target = scenes.viewport(*a).target;
    assert(scenes.viewport(*b).target != original_target);
    assert(scenes.updateViewport(*a, left, 640, 320));
    assert(!scenes.viewport(*a).ready);
    until([&] { return scenes.viewport(*a).ready; });
    assert(
        scenes.viewport(*a).target == original_target
    ); // Resize adopts physical candidates, not new logical identity.
    assert(scenes.updateViewport(*a, scene::PrimarySceneCamera{}, 640, 320));
    until([&] { return scenes.viewport(*a).error.type != 0 && !scenes.viewport(*a).ready; });
    assert(scenes.updateViewport(*a, left, 640, 320));
    until([&] { return scenes.viewport(*a).ready && scenes.viewport(*b).ready; });
    scenes.closeViewport(*a);
    until([&] { return scenes.viewport(*a).target.isNull(); });
    assert(scenes.viewport(*b).ready);
    assert(scenes.close(id));
    until([&] { return host.engine().sceneRuntime().instanceCount() == 0 && scenes.settled(); });
    assert(scenes.open(root / "Scenes/authored.luxscene"));
    until([&] { return scenes.find(id) != nullptr; });
    assert(scenes.find(id)->instance() != editing_instance);
    assert(scenes.open(root / "Scenes/authored.luxscene"));
    fixture::close(host); // A decode may return after closure; it cannot adopt into the next Context.
    until([&] { return host.project() == nullptr && host.engine().sceneRuntime().instanceCount() == 0; });
    made->reset();
    std::puts("Scene creation, incompatibility, multi-view, resize, reopen and late close passed");
}
