#include "support/ModelSource.hpp"
#include "support/ProjectRequests.hpp"
#include "support/RenderBindingExtensions.hpp"
#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/context/render/RenderExecution.hpp>
#include <lux/engine/editor/ComponentEditors3D.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ModelPlacement.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/RenderConfigurationEditor.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/project/SceneRegistrations.hpp>
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/render/CameraRenderResourceComponent.hpp>
#include <lux/engine/scene/render/CameraViewBinding.hpp>
#include <lux/engine/scene/render/LightRenderResourceComponent.hpp>
#include <lux/engine/scene/render/MeshRenderBinding.hpp>
#include <lux/engine/scene/render/MeshRenderResourceComponent.hpp>
#include <lux/engine/scene/render/RasterTransformOperation.hpp>
#include <lux/engine/scene/render/RenderBindingPlan.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/ui/Controls.hpp>
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

namespace lux::ui
{
    struct ControlsTestAccess final
    {
        static void click(Button& button) noexcept
        {
            static_cast<void>(button.emit(button.activated));
        }
    };
} // namespace lux::ui

namespace
{
    SceneCreateResult versionedProfile(
        const SceneCreateInfo& info,
        bool invalid_version,
        bool invalid_configuration = false
    ) noexcept
    {
        auto package = sceneProfile3D().create(info);
        if (!package || (info.selection && !invalid_version && !invalid_configuration))
        {
            return package;
        }
        // Real version-1 bytes enter through the Profile, then pass through the Pane's current editor and save.
        std::vector<std::byte> bytes;
        serialization::BinaryWriter writer(bytes);
        const auto old_fields = std::tuple{
            std::string{"lux.scene.clustered"},
            std::uint32_t{1},
            true,
            std::uint32_t{64},
            std::uint32_t{2},
            37.F,
            std::uint32_t{2048}
        };
        assert(serialization::write(writer, old_fields, serialization::kPortableMetadataBudget));
        const auto& original = package->scene->data();
        scene::SceneDescriptionBuilder builder;
        for (std::size_t i = 0; i < original.systemCount(); ++i)
        {
            const auto system = original.systemAt(i);
            const bool rendering = system.type().name == scene::RenderSystem::Description.canonical_name;
            if (rendering && invalid_configuration)
            {
                scene::RenderSystemConfiguration configuration;
                assert(scene::renderConfigurationCodec().decode(system.configurationPayload(), &configuration));
                configuration.configuration.bytes = {std::byte{0xff}};
                bytes.clear();
                assert(scene::renderConfigurationCodec().encode(&configuration, bytes));
            }
            assert(builder.addSystem(
                system.instanceId(),
                system.instanceName(),
                system.type(),
                rendering && invalid_version ? system.version() + 1 : system.version(),
                system.configurationSchemaName(),
                rendering && !invalid_version && !invalid_configuration ? 1 : system.configurationSchemaVersion(),
                rendering && !invalid_version ? std::span<const std::byte>{bytes} : system.configurationPayload()
            ));
            for (std::size_t j = 0; j < system.requirementBindingCount(); ++j)
            {
                const auto binding = system.requirementBindingAt(j);
                assert(builder.bindRequirement(system.instanceId(), binding.requirement(), binding.provider()));
            }
        }
        for (std::size_t i = 0; i < original.dependencyCount(); ++i)
        {
            const auto dependency = original.dependencyAt(i);
            assert(builder.addDependency(dependency.before(), dependency.after()));
        }
        auto description = std::move(builder).buildResolved();
        assert(description);
        return scene::createScenePackage(
            info.id,
            info.name,
            package->world->data().schemas(),
            {package->simulation, &package->simulation->data()},
            *description
        );
    }

    SceneCreateResult legacyProfile(const SceneCreateInfo& info) noexcept
    {
        return versionedProfile(info, false);
    }

    SceneCreateResult invalidConfigurationProfile(const SceneCreateInfo& info) noexcept
    {
        return versionedProfile(info, false, true);
    }

    SceneCreateResult invalidSimulationProfile(const SceneCreateInfo& info) noexcept
    {
        auto package = sceneProfile3D().create(info);
        if (!package)
        {
            return package;
        }
        simulation::SimulationDescriptionBuilder builder;
        simulation::SimulationSystemDescription unavailable{{"test.unavailable.simulation", 1}};
        assert(builder.addSystem(system::SystemInstanceId{1}, "unavailable", unavailable));
        auto simulation = std::move(builder).build();
        assert(simulation);
        return scene::createScenePackage(
            info.id,
            info.name,
            package->world->data().schemas(),
            std::make_shared<const simulation::SimulationDescription>(std::move(*simulation)),
            package->scene->data()
        );
    }

    SceneCreateResult invalidVersionProfile(const SceneCreateInfo& info) noexcept
    {
        return versionedProfile(info, true);
    }
} // namespace

int main(int argc, char** argv)
{
    assert(argc >= 4 && argc <= 6);
    bool render_only = false, fail_close = false;
    for (int i = 4; i < argc; ++i)
    {
        const std::string_view option{argv[i]};
        render_only |= option == "--render-only";
        fail_close |= option == "--fault-close";
        assert(option == "--render-only" || option == "--fault-close");
    }
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
    SceneCreationPane* creation_pane = nullptr;
    auto made = LuxEngine::create(
        std::move(config),
        [&creation_pane](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            auto legacy = sceneProfile3D();
            legacy.id = "test.legacy.scene.3d";
            legacy.create = &legacyProfile;
            assert(composition.registerSceneProfile(std::move(legacy)));
            auto invalid = sceneProfile3D();
            invalid.id = "test.invalid.scene.version";
            invalid.create = &invalidVersionProfile;
            assert(composition.registerSceneProfile(std::move(invalid)));
            auto configuration = sceneProfile3D();
            configuration.id = "test.invalid.render.configuration";
            configuration.create = &invalidConfigurationProfile;
            assert(composition.registerSceneProfile(std::move(configuration)));
            auto simulation = sceneProfile3D();
            simulation.id = "test.invalid.simulation.type";
            simulation.create = &invalidSimulationProfile;
            assert(composition.registerSceneProfile(std::move(simulation)));
            assert(composition.registerSceneProfile(sceneProfile3D()));
            assert(registerComponentEditors3D(composition));
            assert(registerRenderConfigurationEditor(composition));
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
                [&creation_pane](EditorContext& context, const PaneDescription& pane) noexcept
                    -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    auto scenes = context.service<ProjectScenes>();
                    assert(scenes);
                    auto created = std::make_unique<SceneCreationPane>(context, scenes->get(), pane.title);
                    creation_pane = created.get();
                    return created;
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
    {
        // The shipped Scene profile has no Simulation systems. Qualify an external declaration
        // using the same pure function that Scene preflight and Simulation construction now share.
        simulation::SimulationSystemDescription description{{"test.simulation.qualifier", 1}};
        simulation::SimulationSystemRegistry registrations;
        simulation::SimulationSystemRegistration registration;
        registration.type = system::systemTypeId(description.type.canonical_name);
        registration.cpp_type = cxx::typeToken<fixture::ViewObservation>();
        registration.description = &description;
        registration.install =
            +[](simulation::SimulationSystemInstaller&, simulation::SimulationSystemView
             ) noexcept -> cxx::expected<void, simulation::SimulationSystemBuildFailure> { std::terminate(); };
        assert(registrations.add(std::move(registration)));
        simulation::SimulationDescriptionBuilder builder;
        auto invalid = description;
        ++invalid.type.version;
        assert(builder.addSystem(system::SystemInstanceId{1}, "invalid", invalid));
        auto value = std::move(builder).build();
        assert(value);
        auto qualified = simulation::Simulation::validate(*value, registrations);
        assert(!qualified && qualified.error().code == simulation::ESimulationSystemBuildError::VERSION_MISMATCH);
    }
    auto& context = host.project();
    auto* bindings = context.extensions().find<scene::RenderBindingDeclarations>();
    assert(bindings);
    assert(scene::registerCapabilityOperation<fixture::ViewDependency>(*bindings));
    assert(scene::registerCapabilityOperation<fixture::ViewOperation>(*bindings));
    assert(scene::registerCapabilityOperation<fixture::StatelessObservation>(*bindings, {}, "test.signal.a"));
    assert(scene::registerCapabilityOperation<fixture::StatelessObservation>(*bindings, {}, "test.signal.b"));
    {
        auto alternatives = *bindings;
        const std::array names{std::string_view{"test.alternative.camera"}};
        assert(scene::registerComponentBinding<
               scene::CameraViewBinding>(alternatives, {}, "test.camera.alternative", {}, names));
        render::RenderCapability isolated;
        isolated.name = names.front();
        isolated.kind = render::ERenderCapabilityKind::EFFECT;
        auto alternate = scene::resolveRenderBindings(alternatives, isolated);
        assert(alternate && alternate->declarations.size() == 1);
        // Registration is not activation. Two actual writers still fail selected-Plan qualification.
        auto contracts = render::RenderCapability{};
        contracts.name = render::kLitSceneCapability;
        // Both view providers explicitly activated: no fabricated GPU or System construction.
        auto first = scene::detail::renderBindingDeclaration<scene::CameraViewBinding>();
        first.role = "first";
        first.capabilities = {contracts.name};
        auto second = first;
        second.role = "second";
        scene::RenderBindingDeclarations writers;
        assert(writers.add(std::move(first)) && writers.add(std::move(second)));
        assert(!scene::resolveRenderBindings(writers, contracts));
    }
    const std::array unused_capabilities{std::string_view{"test.unselected.capability"}};
    assert(scene::registerComponentBinding<
           scene::MeshRenderBinding>(*bindings, {}, "test.unselected.mesh", {}, unused_capabilities));
    assert(scene::registerComponentBinding<scene::MeshRenderBinding>(*bindings, {}, "test.secondary.mesh"));
    assert(scene::registerCapabilityOperation<scene::RasterTransformOperation>(*bindings, {}, "test.secondary.mesh"));

    render::RenderCapability empty_effect;
    empty_effect.name = "test.empty.effect";
    empty_effect.kind = render::ERenderCapabilityKind::EFFECT;
    auto empty_plan = scene::resolveRenderBindings(*bindings, empty_effect);
    assert(empty_plan && empty_plan->declarations.empty() && empty_plan->data_inputs.empty());

    until([&] { return context.service<ProjectAssets>()->get().ready(); });
    auto service = context.service<ProjectScenes>();
    assert(service);
    auto& scenes = service->get();
    scene::MeshRenderBinding* scene_projection = nullptr;
    scene::MeshRenderBinding* secondary_projection = nullptr;
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
    auto missing_camera = selection();
    std::erase_if(missing_camera.schemas, [](const auto& value) { return value.name == "lux.scene.Camera"; });
    const auto before_dynamic = context.manifest();
    const auto invalid_camera = asset::AssetId{generate()};
    assert(scenes.create(
        {invalid_camera, "Missing Camera", "invalid-camera.luxscene", "lux.editor.scene.3d"},
        std::move(missing_camera)
    ));
    until([&] { return scenes.failure().has_value(); });
    const auto* dynamic_failure = std::get_if<scene::SceneRuntimeFailure>(&*scenes.failure());
    assert(dynamic_failure);
    const auto* build_failure = std::get_if<scene::SceneBuildFailure>(&dynamic_failure->cause);
    assert(build_failure && build_failure->code == scene::ESceneBuildError::INCOMPATIBLE_DATA);
    assert(!std::filesystem::exists(root / "invalid-camera.luxscene"));
    assert(context.manifest() == before_dynamic && host.engine().sceneRuntime().instanceCount() == 0);

    auto incompatible = selection();
    std::erase_if(incompatible.schemas, [](const auto& value) { return value.name == "lux.ecs.Transform3D"; });
    const auto bad_id = asset::AssetId{generate()};
    bool invalid_finished = false;
    auto invalid_connection = object::LuxObject::connect(
        &scenes,
        &ProjectScenes::changed,
        &host,
        [&]() noexcept { invalid_finished = true; }
    );
    assert(invalid_connection);
    assert(scenes.create({bad_id, "Invalid", "invalid.luxscene", "lux.editor.scene.3d"}, std::move(incompatible)));
    until([&] { return invalid_finished; });
    assert(!std::filesystem::exists(root / "invalid.luxscene"));
    assert(host.engine().sceneRuntime().instanceCount() == 0);

    invalid_finished = false;
    assert(scenes.create(
        {asset::AssetId{generate()}, "Bad version", "bad-version.luxscene", "test.invalid.scene.version"},
        selection()
    ));
    until([&] { return invalid_finished; });
    const auto* version_failure = std::get_if<scene::SceneRuntimeFailure>(&*scenes.failure());
    assert(version_failure);
    const auto* version_build = std::get_if<scene::SceneBuildFailure>(&version_failure->cause);
    assert(version_build && version_build->scene_system.code == scene::ESceneSystemBuildError::VERSION_MISMATCH);
    assert(!std::filesystem::exists(root / "bad-version.luxscene"));
    for (const auto profile : {"test.invalid.render.configuration", "test.invalid.simulation.type"})
    {
        const auto before = context.manifest();
        invalid_finished = false;
        assert(scenes.create(
            {asset::AssetId{generate()}, "Invalid configuration", "bad-config.luxscene", profile},
            selection()
        ));
        until([&] { return invalid_finished; });
        if (std::string_view{profile} == "test.invalid.render.configuration")
        {
            const auto* failure = std::get_if<render::RenderError>(&*scenes.failure());
            assert(failure && failure->type == render::kInvalidConfig);
        }
        else
        {
            const auto* failure = std::get_if<scene::SceneRuntimeFailure>(&*scenes.failure());
            assert(failure);
            const auto* build = std::get_if<scene::SceneBuildFailure>(&failure->cause);
            assert(build && build->simulation.code == simulation::ESimulationSystemBuildError::UNKNOWN_SYSTEM_TYPE);
        }
        assert(!std::filesystem::exists(root / "bad-config.luxscene"));
        assert(context.manifest() == before && host.engine().sceneRuntime().instanceCount() == 0);
    }
    assert(context.manifest() == before_dynamic && host.engine().sceneRuntime().instanceCount() == 0);

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
    until(
        [&]
        {
            auto system = context.engine().sceneRuntime().borrowSystem<scene::RenderSystem>(editing_instance);
            if (!system)
            {
                const auto* error = std::get_if<scene::ESceneRuntimeError>(&system.error().cause);
                assert(error && *error == scene::ESceneRuntimeError::BUSY);
                return false;
            }
            scene_projection = (*system)->binding<scene::MeshRenderBinding>("raster.mesh");
            secondary_projection = (*system)->binding<scene::MeshRenderBinding>("test.secondary.mesh");
            assert(!(*system)->binding<scene::MeshRenderBinding>("test.unselected.mesh"));
            assert(scene_projection && secondary_projection); // Borrow ends before this Scene retires.
            return true;
        }
    );

    assert(host.renderAssets().install(fixture::AssetTableProbe{11}, scene_projection->instanceInput()));
    assert(!host.renderAssets().install(fixture::AssetTableProbe{12}, scene_projection->instanceInput()));
    const auto tables_destroyed = fixture::AssetTableProbe::destroyed;
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
                engine::render::reportRenderFailure(first.error, "First viewport");
                engine::render::reportRenderFailure(second.error, "Second viewport");
                assert(false);
            }
            return first.ready && second.ready;
        }
    );
    {
        // The mounted SceneViewportPane also creates an output. Compare actual adopted Views,
        // rather than assuming only the two explicit viewport requests exist.
        auto registry = std::as_const(context.engine().sceneRuntime()).borrowInstance(editing_instance);
        assert(registry);
        std::vector<render::ViewId> adopted_views;
        for (const auto camera : registry->get().view<const scene::detail::CameraRenderResourceComponent>())
        {
            for (const auto& target :
                 registry->get().get<scene::detail::CameraRenderResourceComponent>(camera).bindings)
            {
                if (!target.view.isNull())
                {
                    adopted_views.push_back(target.view);
                }
            }
        }
        assert(adopted_views.size() >= 2 && fixture::ViewOperation::observations.size() == adopted_views.size());
        for (const auto view : adopted_views)
        {
            assert(
                std::ranges::count_if(
                    fixture::ViewOperation::observations,
                    [&](const auto& observed) noexcept { return observed.first == view; }
                ) == 1
            );
        }
    }
    const auto observed_camera = scenes.viewport(*a).camera;
    auto observe = [&](simulation::ecs::Registry& registry) noexcept
    { registry.emplace_or_replace<fixture::ViewObservation>(observed_camera, 41u); };
    const auto before_signals = fixture::StatelessObservation::signals;
    until([&] { return context.engine().sceneRuntime().editInstance(editing_instance, observe).has_value(); });
    assert(fixture::StatelessObservation::signals - before_signals == 2 * fixture::ViewOperation::observations.size());
    assert(
        std::ranges::count_if(
            fixture::ViewOperation::observations,
            [](const auto& item) { return item.second == 41; }
        ) == 1
    );
    const auto original_target = scenes.viewport(*a).target;
    assert(scenes.viewport(*b).target != original_target);
    const auto views_before_resize = fixture::ViewOperation::created;
    assert(scenes.updateViewport(*a, 640, 320));
    assert(!scenes.viewport(*a).ready);
    until([&] { return scenes.viewport(*a).ready; });
    assert(fixture::ViewOperation::created == views_before_resize);
    assert(
        scenes.viewport(*a).target == original_target
    ); // Resize adopts physical candidates, not new logical identity.
    const auto primary_camera = scenes.viewport(*a).camera;
    assert(scenes.selectViewportCamera(*a, {EViewportCamera::PRIMARY}));
    until([&] { return scenes.viewport(*a).error.type != 0 && !scenes.viewport(*a).ready; });
    auto& scene_runtime = host.engine().sceneRuntime();
    auto set_primary = [&](bool value)
    {
        auto change = [&](simulation::ecs::Registry& registry) noexcept
        {
            registry.patch<scene::Camera>(
                primary_camera,
                [value](scene::Camera& camera) noexcept { camera.primary = value; }
            );
        };
        until([&] { return scene_runtime.editInstance(editing_instance, change).has_value(); });
    };
    set_primary(true);
    assert(scenes.selectViewportCamera(*b, {EViewportCamera::PRIMARY}));
    until([&] { return scenes.viewport(*a).ready && scenes.viewport(*b).ready; });
    assert(scenes.viewport(*a).camera == primary_camera && scenes.viewport(*b).camera == primary_camera);
    set_primary(false);
    until([&] { return scenes.viewport(*a).error.type != 0 && scenes.viewport(*b).error.type != 0; });
    assert(scenes.selectViewportCamera(*b, {}));
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
    assert(assets.prepareMeshGeometry(query_mesh)); // Demand accepted, actual task waits for capacity.
    assert(!assets.meshGeometry(query_mesh));
    assert(context.engine().execution().dispatchMainCompletions());
    // No second prepare call: the production capacity continuation must resume this demand.
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
            assert(secondary_projection->assetError(entity).type == 0);
            const bool ready = scene_projection->visualReady(entity) && secondary_projection->visualReady(entity);
            count += ready;
            if (ready)
            {
                const auto* resources = borrowed->get().try_get<scene::detail::MeshRenderResourceComponent>(entity);
                assert(resources && resources->roles.size() == 2);
                const auto& first = resources->roles[0];
                const auto& second = resources->roles[1];
                assert(first.role != second.role && first.instance != second.instance);
                assert(first.mesh == second.mesh && first.material == second.material);
            }
        }
        return count;
    };
    until([&] { return projected() == 2 && scenes.viewport(*a).ready && scenes.viewport(*b).ready; });

    // Exercise the real Editor Registry -> registered binding -> Render response -> safe adoption path.
    // The private result type is inspected by this fixture only; no test getter enters production APIs.
    using LightResource = scene::detail::LightRenderResourceComponent;
    simulation::ecs::Entity reactive_light = simulation::ecs::NullEntity;
    auto edit_light = [&](auto edit)
    { until([&] { return runtime.editInstance(editing_instance, edit).has_value(); }); };
    auto resource_id = [&]() -> lux::render::LightId
    {
        const auto borrowed = std::as_const(runtime).borrowInstance(editing_instance);
        assert(borrowed);
        const auto& registry = borrowed->get();
        const auto* resource =
            registry.valid(reactive_light) ? registry.try_get<const LightResource>(reactive_light) : nullptr;
        return resource ? resource->light : lux::render::LightId{};
    };
    edit_light(
        [&](simulation::ecs::Registry& registry) noexcept
        {
            reactive_light = registry.create();
            simulation::ecs::Transform3D transform;
            transform.translation = {0, 4, 2};
            registry.emplace<simulation::ecs::Transform3D>(reactive_light, transform);
            rdesc::LightDescription light;
            light.cast_shadow = true;
            registry.emplace<simulation::ecs::Light3D>(reactive_light, light);
        }
    );
    until([&] { return !resource_id().isNull(); });
    const auto first_light_id = resource_id();
    edit_light(
        [&](simulation::ecs::Registry& registry) noexcept
        {
            registry.patch<simulation::ecs::Light3D>(
                reactive_light,
                [](auto& light) noexcept
                {
                    light.value.intensity = 4.F;
                    light.value.color = {0.8F, 0.6F, 0.4F};
                }
            );
            registry.patch<simulation::ecs::Transform3D>(
                reactive_light,
                [](auto& transform) noexcept { transform.translation.x() = 2; }
            );
        }
    );
    const auto before_light_update = host.rendering().stats().commands;
    until([&] { return host.rendering().stats().commands > before_light_update + 4; });
    assert(resource_id() == first_light_id); // Parameter updates must not recreate the resource.
    edit_light(
        [&](simulation::ecs::Registry& registry) noexcept
        {
            registry.remove<simulation::ecs::Light3D>(reactive_light);
            registry.emplace<simulation::ecs::Light3D>(reactive_light, rdesc::LightDescription{});
        }
    );
    until(
        [&]
        {
            const auto current = resource_id();
            return !current.isNull() && current != first_light_id;
        }
    );
    const auto recreated_light_id = resource_id();
    edit_light(
        [&](simulation::ecs::Registry& registry) noexcept
        {
            registry.patch<simulation::ecs::Light3D>(
                reactive_light,
                [](auto& light) noexcept { light.value.type = rdesc::ELightType::SPOT; }
            );
        }
    );
    until(
        [&]
        {
            const auto current = resource_id();
            return !current.isNull() && current != recreated_light_id;
        }
    );
    const auto removed_entity = reactive_light;
    edit_light([&](simulation::ecs::Registry& registry) noexcept { registry.destroy(reactive_light); });
    assert(resource_id().isNull());
    edit_light(
        [&](simulation::ecs::Registry& registry) noexcept
        {
            reactive_light = registry.create();
            assert(reactive_light != removed_entity);
            registry.emplace<simulation::ecs::Transform3D>(reactive_light);
            registry.emplace<simulation::ecs::Light3D>(reactive_light, rdesc::LightDescription{});
        }
    );
    until([&] { return !resource_id().isNull(); });
    assert(host.rendering().stats().validation_errors == 0);
    // This live relation is deliberately left for the ordinary Scene/Editor close path below.

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
    // Catalog publication precedes asynchronous resource adoption for both representation roles.
    until([&] { return projected() == 2 && scenes.viewport(*a).ready && scenes.viewport(*b).ready; });
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

    if (!render_only)
    {
        auto admitted = scenes.startPlay(id, ESceneRunMode::HEADLESS);
        assert(admitted);
        until([&] { return scenes.playInstance(id).valid(); });
        const auto headless = std::optional{scenes.playInstance(id)};
        assert(*headless != document->instance() && document->readOnly());
        assert(!runtime.borrowSystem<scene::RenderSystem>(*headless));
        until(
            [&]
            {
                auto clock = std::as_const(runtime).borrowClock(*headless);
                return clock &&
                       std::visit([](const auto& value) { return value.snapshot().step_index > 0; }, clock->get());
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
                    registry.patch<simulation::ecs::Transform3D>(
                        entity,
                        [](auto& value) { value.translation.x() = 900; }
                    );
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
    }

    auto graphical = scenes.startPlay(id, ESceneRunMode::GRAPHICAL);
    assert(graphical);
    until([&] { return scenes.playInstance(id).valid(); });
    auto play_view = scenes.addViewport(id, 200, 150, true);
    assert(play_view);
    until([&] { return scenes.setViewportCameraTransform(*play_view, left); });
    until([&] { return scenes.viewport(*play_view).ready; });
    {
        auto play_system = host.engine().sceneRuntime().borrowSystem<scene::RenderSystem>(scenes.playInstance(id));
        assert(play_system);
        const auto* play_mesh = (*play_system)->binding<scene::MeshRenderBinding>("raster.mesh");
        assert(play_mesh);
        const std::array inputs{
            render::CapabilitiesInstalled::Input{
                scene::MeshRenderBinding::inputs().front().schema,
                1,
                scene_projection->instanceInput()
            },
            render::CapabilitiesInstalled::Input{
                scene::MeshRenderBinding::inputs().front().schema,
                1,
                play_mesh->instanceInput()
            }
        };
        assert(inputs[0].destination != inputs[1].destination);
        scene::RenderBindingDeclaration declaration;
        declaration.inputs.push_back(scene::MeshRenderBinding::inputs().front());
        declaration.resource_inputs.push_back({declaration.inputs.front().schema});
        assert(!scene::resolveRenderBindingInputs(declaration, inputs, {}, {}));
        // Actual construction supplies only the current scope, never another Scene's candidates.
        auto resolved = scene::resolveRenderBindingInputs(declaration, std::span{inputs}.subspan(1), {}, {});
        assert(resolved && resolved->front().destination == inputs[1].destination);
    }
    scenes.stopPlay(id);
    until([&] { return !scenes.playInstance(id).valid(); });
    assert(scenes.viewport(*play_view).target.isNull() && scenes.viewport(*b).ready);

    {
        auto system = host.engine().sceneRuntime().borrowSystem<scene::RenderSystem>(editing_instance);
        assert(system);
        auto* cameras = (*system)->binding<scene::CameraViewBinding>();
        assert(cameras);
        const auto output = scenes.viewport(*a);
        bool observed = false;
        auto retire = [&](simulation::ecs::Registry& registry) noexcept
        {
            auto& resources = registry.get<scene::detail::CameraRenderResourceComponent>(output.camera);
            const auto binding = std::ranges::find_if(
                resources.bindings,
                [&](const auto& value) noexcept { return value.adopted.target == output.target; }
            );
            assert(binding != resources.bindings.end());
            fixture::ViewOperation::fail_retirement = binding->view;
            assert(cameras->removeCameraTarget(output.camera, binding->id));
            scene::SceneStageContext stage;
            auto result = cameras->maintain(stage);
            observed = !result && result.error().cause.type == 0;
        };
        until([&] { return host.engine().sceneRuntime().editInstance(editing_instance, retire).has_value(); });
        assert(observed && fixture::ViewOperation::fail_retirement.isNull());
    }
    scenes.closeViewport(*a);
    until([&] { return scenes.viewport(*a).target.isNull(); });
    assert(scenes.viewport(*b).ready);
    const auto signals_after_close = fixture::StatelessObservation::signals;
    const auto active_view_count = fixture::ViewOperation::observations.size();
    const auto remaining_camera = scenes.viewport(*b).camera;
    auto notify_remaining = [&](simulation::ecs::Registry& registry) noexcept
    { registry.emplace_or_replace<fixture::ViewObservation>(remaining_camera, 51u); };
    until([&] { return context.engine().sceneRuntime().editInstance(editing_instance, notify_remaining).has_value(); });
    assert(fixture::StatelessObservation::signals - signals_after_close == 2 * active_view_count);
    assert(scenes.close(id));
    scene_projection = nullptr;
    until([&] { return host.engine().sceneRuntime().instanceCount() == 0 && scenes.settled(); });
    assert(fixture::AssetTableProbe::destroyed == tables_destroyed + 1);
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
    assert(creation_pane);
    // Layout's last two controls are Create and Status. Drive the actual public signal;
    // no production backdoor calls selection() or fabricates a migrated configuration.
    object::LuxObject* create_control = nullptr;
    for (auto* child = creation_pane->content()->firstChild(); child && child->nextSibling();
         child = child->nextSibling())
    {
        create_control = child;
    }
    assert(create_control);
    ui::ControlsTestAccess::click(*static_cast<ui::Button*>(create_control));
    until(
        [&]
        {
            const auto& records = context.manifest().scenes;
            const auto created =
                std::ranges::find(records, "Scenes/NewScene.luxscene", &ProjectManifest::SceneRecord::path);
            return created != records.end() && scenes.find(created->id) != nullptr;
        }
    );
    auto migrated_package = scene::readScenePackageFile(root / "Scenes/NewScene.luxscene", 1U << 24);
    assert(migrated_package);
    const auto saved_system =
        migrated_package->scene->data().findSystem(scene::RenderSystem::Description.canonical_name);
    assert(
        saved_system &&
        saved_system.configurationSchemaVersion() == scene::RenderSystem::Description.configuration_schema_version
    );
    scene::RenderSystemConfiguration migrated_config;
    assert(scene::renderConfigurationCodec().decode(saved_system.configurationPayload(), &migrated_config));
    const auto migrated_lighting = lux::render::decodeLitSceneConfiguration(migrated_config.configuration.bytes);
    assert(
        migrated_lighting && migrated_lighting->shadow_resolution == 64 &&
        migrated_lighting->shadow_filter_radius == 2 && migrated_lighting->shadow_distance == 37.F &&
        migrated_lighting->cluster_index_capacity == 2048
    );
    assert(scenes.close(migrated_package->scene->id()));
    until([&] { return !scenes.find(migrated_package->scene->id()); });
    scene_projection = nullptr;
    fixture::modelSource(root / "Sources");
    assert(assets.reimport(imported.id));
    if (fail_close)
    {
        // A real Server execution failure while a project and an accepted source decode are alive.
        // No test backdoor fabricates a successful release or destroys a borrowed Registry.
        auto& rendering = host.rendering();
        auto& client = rendering.mainClient();
        const auto stop = client.bind<lux::render::Stop>();
        assert(stop);
        lux::render::EmptyRequest input;
        bool acknowledged = false;
        assert(rendering.execution().spawn(stdexec::then(
            client.request(*stop, input),
            [&](lux::render::AckResponse response) noexcept
            {
                assert(response.error.type == 0);
                acknowledged = true;
            }
        )));
        const auto closed = host.run();
        assert(!closed && closed.error().type == lux::render::kCommandCancelled && acknowledged);
        assert(client.pendingRequests() == 0);
    }
    else
    {
        fixture::close(host); // A decode may return after closure; it cannot adopt into the next Context.
        assert(host.run());
    }
    assert(host.engine().sceneRuntime().instanceCount() == 0);
    made->reset();
    assert(fixture::StatelessObservation::signals > 0);
    assert(fixture::ViewOperation::observations.empty());
    assert(fixture::ViewDependency::alive.empty());
    assert(fixture::ViewOperation::created == fixture::ViewOperation::destroyed);
    assert(fixture::ViewOperation::created == fixture::ViewOperation::drained);
    std::puts("Scene creation, incompatibility, multi-view, resize, reopen and late close passed");
}
