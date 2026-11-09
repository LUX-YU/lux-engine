#include "support/ModelSource.hpp"
#include "support/ProjectRequests.hpp"
#include "support/RenderBindingExtensions.hpp"
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/WorldEntityMap.hpp>
#include <lux/engine/editor/CameraViewport.hpp>
#include <lux/engine/editor/ComponentEditors3D.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/MeshQueryAssets.hpp>
#include <lux/engine/editor/MeshViewportTool.hpp>
#include <lux/engine/editor/ModelImporter.hpp>
#include <lux/engine/editor/ModelPlacement.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/RenderConfigurationEditor.hpp>
#include <lux/engine/editor/RenderProjectInputs.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/systems/render/CameraViewReaction.hpp>
#include <lux/engine/systems/render/LightRenderResourceComponent.hpp>
#include <lux/engine/systems/render/MeshRenderBinding.hpp>
#include <lux/engine/systems/render/MeshRenderResourceComponent.hpp>
#include <lux/engine/systems/render/OffscreenTargetReaction.hpp>
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#include <lux/engine/systems/render/RenderExecution.hpp>
#include <lux/engine/systems/render/RenderScene.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>
#include <lux/engine/systems/render/RenderTargetComponent.hpp>
#include <lux/engine/systems/render/RenderViewComponent.hpp>
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
    unsigned inspected_documents{}, inspection_updates{};

    class InspectSceneTool final : public SceneViewportTool
    {
    public:
        ui::Element& controls() noexcept override
        {
            return label_;
        }

        void observe(asset::AssetId id, scene::SceneInstanceId instance, bool) noexcept override
        {
            assert(!id.isNull() && instance.valid());
            ++inspected_documents;
            label_.setText("Independent inspection tool");
        }

        void update() noexcept override
        {
            ++inspection_updates;
        }

        void draw(const ui::ImageInteraction&) noexcept override {}

        SceneViewportId viewport() const noexcept override
        {
            return {};
        }

        std::string_view status() const noexcept override
        {
            return "Inspection";
        }

    private:
        ui::Label label_;
    };

    SceneViewportToolDeclaration inspectSceneTool()
    {
        return {
            {},
            "Inspect document",
            {},
            +[](EditorContext&,
                ProjectScenes&,
                SceneViewports&) noexcept -> FrameworkResult<std::unique_ptr<SceneViewportTool>>
            { return std::unique_ptr<SceneViewportTool>{std::make_unique<InspectSceneTool>()}; }
        };
    }

    std::atomic_uint prepared_profiles{};

    SceneCreateResult countedProfile(const SceneCreateInfo& info) noexcept
    {
        if (info.selection)
        {
            ++prepared_profiles;
        }
        return sceneProfile3D().create(info);
    }

    SceneCreateResult
    versionedProfile(const SceneCreateInfo& info, bool invalid_version, bool invalid_configuration = false) noexcept
    {
        auto package = sceneProfile3D().create(info);
        if (!package || (info.selection && !invalid_version && !invalid_configuration))
        {
            return package;
        }
        // Current configuration round-trips through the real creation UI. Unsupported
        // versions and malformed bytes are separate rejection cases, not legacy migration.
        render::DeferredLightingConfiguration lighting;
        lighting.shadow = {64, 2, 37.F};
        lighting.clusters.index_capacity = 2048;
        ::lux::engine::render::RenderSystemConfiguration render_configuration;
        render_configuration.composition = render::deferredRenderComposition();
        auto selected = std::ranges::find(
            render_configuration.composition.contributions,
            render::kDeferredLightingCapability,
            &render::CapabilityContributionDescription::capability
        );
        selected->configuration = *render::encodeDeferredLightingConfiguration(lighting);
        std::vector<std::byte> bytes;
        assert(::lux::engine::render::renderConfigurationCodec().encode(&render_configuration, bytes));
        const auto& original = package->scene->data();
        scene::SceneDescriptionBuilder builder;
        for (std::size_t i = 0; i < original.systemCount(); ++i)
        {
            const auto system = original.systemAt(i);
            const bool rendering =
                system.type().name == ::lux::engine::render::RenderSystem::Description.canonical_name;
            if (rendering && invalid_configuration)
            {
                ::lux::engine::render::RenderSystemConfiguration configuration;
                assert(::lux::engine::render::renderConfigurationCodec()
                           .decode(system.configurationPayload(), &configuration));
                configuration.composition.contributions.front().configuration.bytes = {std::byte{0xff}};
                bytes.clear();
                assert(::lux::engine::render::renderConfigurationCodec().encode(&configuration, bytes));
            }
            assert(builder.addSystem(
                system.instanceId(),
                system.instanceName(),
                system.type(),
                rendering && invalid_version ? system.version() + 1 : system.version(),
                system.configurationSchemaName(),
                system.configurationSchemaVersion(),
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
    {
        plugins::PluginDescription dependency, root_plugin;
        dependency.identity = {"test.geometry", 1};
        dependency.geometries.push_back({{"test.geometry.mesh", 1}});
        dependency.partitions.push_back({{"test.partition.single", 1}});
        root_plugin.identity = {"test.product", 1};
        root_plugin.dependencies.push_back(dependency.identity);
        const std::array catalog{dependency, root_plugin};
        const std::array selected{ProjectManifest::PluginRecord{"test.product", 1}};
        const auto allowed = availableProjectContent(catalog, selected);
        assert(allowed.geometries.size() == 1 && allowed.partitions.size() == 1);
        assert(availableProjectContent(catalog, {}).geometries.empty());
    }

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
    config.layout = defaultProjectLayout({{"scene.creation", "scenes", "Scenes"}, {"scene.viewport", "view", "View"}});
    SceneCreationPane* creation_pane = nullptr;
    SceneViewportPane* viewport_pane = nullptr;
    auto made = fixture::createLoadedEditor(
        std::move(config),
        [&creation_pane, &viewport_pane](EditorComposition& composition) noexcept -> FrameworkResult<void>
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
            auto counted = sceneProfile3D();
            counted.id = "test.counted.scene";
            counted.create = &countedProfile;
            assert(composition.registerSceneProfile(std::move(counted)));
            assert(registerComponentEditors3D(composition));
            assert(registerRenderConfigurationEditor(composition));
            assert(composition.registerServiceFactory<ProjectAssets>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<ProjectAssets>>
                {
                    auto importer = declareAssetImporter<ModelImporter>();
                    assert(importer);
                    auto assets = std::make_unique<ProjectAssets>(context, std::vector{std::move(*importer)});
                    assert(assets->open());
                    return assets;
                }
            ));
            assert(composition.registerServiceFactory<MeshQueryAssets>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<MeshQueryAssets>>
                { return std::make_unique<MeshQueryAssets>(context, context.service<ProjectAssets>()->get()); }
            ));
            assert(composition.registerServiceFactory<SceneViewports>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<SceneViewports>>
                { return std::make_unique<SceneViewports>(context); }
            ));
            assert(composition.registerServiceFactory<RenderProjectInputs>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<RenderProjectInputs>>
                { return RenderProjectInputs::create(context, context.service<ProjectAssets>()->get()); }
            ));
            assert(composition.registerServiceFactory<ProjectScenes>(
                [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<ProjectScenes>>
                {
                    assert(context.service<MeshQueryAssets>());
                    assert(context.service<SceneViewports>());
                    return std::make_unique<ProjectScenes>(
                        context,
                        context.service<RenderProjectInputs>()->get().providers()
                    );
                }
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
                [&viewport_pane](EditorContext& context, const PaneDescription& pane) noexcept
                    -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    auto scenes = context.service<ProjectScenes>();
                    assert(scenes);
                    auto result = std::make_unique<SceneViewportPane>(
                        context,
                        scenes->get(),
                        context.service<SceneViewports>()->get(),
                        std::vector{meshViewportTool(), inspectSceneTool()},
                        pane.title
                    );
                    viewport_pane = result.get();
                    return result;
                }
            );
        }
    );
    assert(made);
    auto& host = **made;
    std::vector<SceneViewportId> observed_views;
    auto& outputs = host.project().service<SceneViewports>()->get();
    auto cameraOutput = [&](SceneViewportId id) noexcept { return static_cast<CameraViewport*>(outputs.find(id)); };
    auto outputStatus = [&](SceneViewportId id) noexcept
    {
        struct CameraStatus
        {
            rdesc::UiImageToken image;
            render::TargetId target;
            render::TargetDesc description;
            scene::SceneInstanceId instance;
            render::RenderError error;
            bool ready{}, pending{};
            ecs::Entity camera{ecs::NullEntity};
        };
        auto state = outputs.status(id);
        return CameraStatus{
            state.image,
            state.target,
            state.description,
            state.instance,
            state.error,
            state.ready,
            state.pending,
            cameraOutput(id) ? cameraOutput(id)->camera() : ecs::NullEntity
        };
    };
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
                            const auto view = outputStatus(id);
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
                                const auto* target =
                                    registry->get()
                                        .template try_get<::lux::engine::render::detail::RenderTargetComponent>(
                                            view.camera
                                        );
                                if (target)
                                {
                                    std::fprintf(stderr, "Camera target: %ux%u epoch=%llu\n",
                                        target->description.width, target->description.height,
                                        static_cast<unsigned long long>(target->epoch));
                                }
                            }
                        }
                    }
                }
                assert(false);
            }
            auto advanced = fixture::stepEditor(host);
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
        registration.cpp_type = cxx::make_basic_type_info<fixture::ViewObservation>();
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
    const auto application_scene_count = host.engine().sceneRuntime().instanceCount();
    auto& bindings = context.service<RenderProjectInputs>()->get().declarations();
    const auto type = cxx::make_basic_type_info<render::Mesh3dCapability>();
    const auto* builtin = bindings.find(type);
    assert(builtin);
    fixture::ViewBindings external{*builtin, host.engine().execution()};
    assert(!bindings.add(*builtin)); // One recipe per capability, explicit composition rather than role strings.
    assert(bindings.replace(
        {type,
         [&external](::lux::engine::render::RenderSystem& system, const render::CommandDestination& inputs) noexcept
         { return fixture::ViewBindings::install(system, inputs, &external); }}
    ));
    ::lux::engine::render::RenderBindingDeclarations isolated;
    assert(!isolated.find(type));
    assert(isolated.add(*builtin) && isolated.find(type));

    until([&] { return context.service<ProjectAssets>()->get().ready(); });
    auto service = context.service<ProjectScenes>();
    assert(service);
    auto& scenes = service->get();
    ::lux::engine::render::MeshRenderBinding* scene_projection = nullptr;
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
    assert(
        context.manifest() == before_dynamic && host.engine().sceneRuntime().instanceCount() == application_scene_count
    );

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
    assert(host.engine().sceneRuntime().instanceCount() == application_scene_count);

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
            const auto* failure = std::get_if<scene::SceneRuntimeFailure>(&*scenes.failure());
            assert(failure);
            const auto* build = std::get_if<scene::SceneBuildFailure>(&failure->cause);
            assert(build && build->code == scene::ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE);
            assert(build->scene_system.code == scene::ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE);
            assert(build->scene_system.cause.type == render::kInvalidConfig);
            assert(scene::toError(*failure).type == render::kInvalidConfig);
        }
        else
        {
            const auto* failure = std::get_if<scene::SceneRuntimeFailure>(&*scenes.failure());
            assert(failure);
            const auto* build = std::get_if<scene::SceneBuildFailure>(&failure->cause);
            assert(build && build->simulation.code == simulation::ESimulationSystemBuildError::UNKNOWN_SYSTEM_TYPE);
        }
        assert(!std::filesystem::exists(root / "bad-config.luxscene"));
        assert(context.manifest() == before && host.engine().sceneRuntime().instanceCount() == application_scene_count);
    }
    assert(
        context.manifest() == before_dynamic && host.engine().sceneRuntime().instanceCount() == application_scene_count
    );

    const auto id = asset::AssetId{generate()};
    std::unique_ptr<SceneCreationCandidate> qualified;
    bool qualification_done = false;
    assert(scenes.prepare(
        {id, "Authored", "Scenes/authored.luxscene", "test.counted.scene"},
        selection(),
        [&](SceneCreationResult result) noexcept
        {
            assert(result);
            qualified = std::move(*result);
            qualification_done = true;
        }
    ));
    until([&] { return qualification_done; });
    assert(qualified && prepared_profiles == 1);
    assert(
        host.engine().sceneRuntime().instanceCount() == application_scene_count && context.manifest() == before_dynamic
    );
    assert(!std::filesystem::exists(root / "Scenes/authored.luxscene"));
    std::unique_ptr<SceneCreationCandidate> stale_candidate;
    qualification_done = false;
    assert(scenes.prepare(
        {asset::AssetId{generate()}, "Stale", "Scenes/stale.luxscene", "lux.editor.scene.3d"},
        selection(),
        [&](SceneCreationResult result) noexcept
        {
            assert(result);
            stale_candidate = std::move(*result);
            qualification_done = true;
        }
    ));
    until([&] { return qualification_done; });
    const auto creation_task = scenes.create(std::move(qualified));
    assert(creation_task);
    until([&] { return scenes.find(id) != nullptr; });
    assert(!scenes.failure());
    assert(host.engine().sceneRuntime().instanceCount() == application_scene_count + 1);
    assert(prepared_profiles == 1); // Publication reuses the exact qualified package and Builder.
    until(
        [&]
        {
            const auto tasks = context.engine().execution().taskInfos();
            const auto task = std::ranges::find(tasks, *creation_task, &execution::TaskInfo::id);
            return task != tasks.end() && task->state == execution::ETaskState::SUCCEEDED;
        }
    );
    auto stale = scenes.create(std::move(stale_candidate));
    assert(!stale);
    const auto* stale_failure = std::get_if<ProjectFailure>(&stale.error());
    assert(stale_failure && stale_failure->code == EProjectError::STALE_REVISION);
    assert(!std::filesystem::exists(root / "Scenes/stale.luxscene"));
    std::puts("PASS creation candidate: pure qualification, exact adoption, stale catalog rejected");
    auto persisted = readProjectManifest(project_path);
    assert(persisted && persisted->scenes.size() == 1 && persisted->startup_scene == id);
    auto package = scene::readScenePackageFile(root / "Scenes/authored.luxscene", 1U << 24);
    assert(package && package->scene->id() == id);
    const auto editing_instance = scenes.find(id)->instance();
    until(
        [&]
        {
            auto system =
                context.engine().sceneRuntime().borrowSystem<::lux::engine::render::RenderSystem>(editing_instance);
            if (!system)
            {
                const auto* error = std::get_if<scene::ESceneRuntimeError>(&system.error().cause);
                assert(error && *error == scene::ESceneRuntimeError::BUSY);
                return false;
            }
            scene_projection = (*system)->binding<::lux::engine::render::MeshRenderBinding>();
            const render::VRenderScope unrelated_view = render::ViewRequest{};
            assert(!(*system)->binding<::lux::engine::render::MeshRenderBinding>(&unrelated_view));
            assert(scene_projection); // Borrow ends before this Scene retires.
            return true;
        }
    );

    assert(host.renderAssets().install(fixture::AssetTableProbe{11}, scene_projection->destination()));
    assert(!host.renderAssets().install(fixture::AssetTableProbe{12}, scene_projection->destination()));
    const auto tables_destroyed = fixture::AssetTableProbe::destroyed;
    ecs::Transform3D left, right;
    left.translation = Eigen::Vector3d{0, 2, 8};
    right.translation = Eigen::Vector3d{5, 3, 8};
    assert(viewport_pane && viewport_pane->activateTool("Inspect document"));
    until([&] { return inspected_documents != 0 && inspection_updates != 0; });
    assert(!viewport_pane->activateTool("Undeclared tool"));
    assert(viewport_pane->activateTool("Mesh editing"));
    for (int i = 0; i != 8; ++i)
    {
        auto advanced = fixture::stepEditor(host);
        assert(advanced);
    }

    auto a = outputs.add(std::make_unique<CameraViewport>(outputs, scenes.find(id)->instance(), 320, 240));
    auto b = outputs.add(std::make_unique<CameraViewport>(outputs, scenes.find(id)->instance(), 480, 240));
    assert(a && b);
    observed_views = {*a, *b};
    until([&] { return cameraOutput(*a)->setTransform(left); });
    until([&] { return cameraOutput(*b)->setTransform(right); });
    until(
        [&]
        {
            const auto first = outputStatus(*a), second = outputStatus(*b);
            if (first.error.type || second.error.type)
            {
                std::fprintf(
                    stderr,
                    "View errors %llu / %llu\n",
                    static_cast<unsigned long long>(first.error.type),
                    static_cast<unsigned long long>(second.error.type)
                );
                ::lux::engine::render::reportRenderFailure(first.error, "First viewport");
                ::lux::engine::render::reportRenderFailure(second.error, "Second viewport");
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
        for (const auto camera : registry->get().view<const ::lux::engine::render::detail::RenderViewComponent>())
        {
            const auto view = registry->get().get<::lux::engine::render::detail::RenderViewComponent>(camera).view;
            if (!view.isNull())
            {
                adopted_views.push_back(view);
            }
        }
        assert(adopted_views.size() >= 2 && fixture::ViewOperation::observations.size() == adopted_views.size());
        for (const auto view : adopted_views)
        {
            assert(
                std::ranges::count_if(
                    fixture::ViewOperation::observations,
                    [&](const auto& observed) noexcept { return observed.first.view == view; }
                ) == 1
            );
        }
    }
    const auto observed_camera = outputStatus(*a).camera;
    auto observe = [&](ecs::Registry& registry) noexcept
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
    const auto original_target = outputStatus(*a).target;
    assert(outputStatus(*b).target != original_target);
    auto nativeView = [&](SceneViewportId id) noexcept
    {
        const auto output = outputStatus(id);
        auto registry = std::as_const(context.engine().sceneRuntime()).borrowInstance(output.instance);
        assert(registry);
        const auto& binding = registry->get().get<::lux::engine::render::detail::RenderViewComponent>(output.camera);
        assert(binding.target == output.target);
        return binding.view;
    };
    const auto view_before_resize = nativeView(*a);
    assert(outputs.find(*a)->resize(640, 320));
    assert(!outputStatus(*a).ready);
    until([&] { return outputStatus(*a).ready; });
    assert(nativeView(*a) == view_before_resize);
    assert(outputStatus(*a).target == original_target); // Resize adopts physical candidates, not new logical identity.
    const auto sampled_image = outputStatus(*a).image;
    assert(outputs.find(*a)->resize(0, 0));
    assert(outputStatus(*a).image == sampled_image);
    until([&] { return !outputStatus(*a).pending; });
    assert(!outputStatus(*a).ready && outputStatus(*a).target == original_target);
    assert(outputs.find(*a)->resize(700, 350));
    assert(outputs.find(*a)->resize(640, 320));
    until([&] { return outputStatus(*a).ready; });
    // A separately activated Pane may finish its first output concurrently. The affected View/Target
    // and sampling identity must remain unchanged; unrelated creations are not Resize work.
    assert(outputStatus(*a).image == sampled_image && nativeView(*a) == view_before_resize);

    const auto primary_camera = outputStatus(*a).camera;
    assert(cameraOutput(*a)->selectCamera({EViewportCamera::PRIMARY}));
    until([&] { return outputStatus(*a).error.type != 0 && !outputStatus(*a).ready; });
    auto& scene_runtime = host.engine().sceneRuntime();
    auto set_primary = [&](bool value)
    {
        auto change = [&](ecs::Registry& registry) noexcept
        {
            registry.patch<scene::ViewComponent>(
                primary_camera,
                [value](scene::ViewComponent& camera) noexcept { camera.primary = value; }
            );
        };
        until([&] { return scene_runtime.editInstance(editing_instance, change).has_value(); });
    };
    set_primary(true);
    assert(cameraOutput(*b)->selectCamera({EViewportCamera::PRIMARY}));
    until([&] { return outputStatus(*a).ready && outputStatus(*b).error.type != 0; });
    assert(outputStatus(*a).camera == primary_camera);
    assert(outputStatus(*b).error.type == render::kCommandContract);
    assert(outputStatus(*b).camera != primary_camera); // One entity cannot own two independent outputs.
    set_primary(false);
    until([&] { return outputStatus(*a).error.type != 0 && outputStatus(*b).error.type != 0; });
    assert(cameraOutput(*b)->selectCamera({}));
    assert(outputs.find(*a)->resize(640, 320));
    assert(cameraOutput(*a)->selectCamera({}));
    until([&] { return outputStatus(*a).ready && outputStatus(*b).ready; });

    auto& assets = context.service<ProjectAssets>()->get();
    const auto source = fixture::modelSource(root / "Sources");
    assert(assets.importAsset(ModelImporter::name, ModelImporter::version, source, *ModelImporter::encode({})));
    until([&] { return !assets.busy(); });
    if (assets.failure())
    {
        std::fprintf(stderr, "Import: %s\n", assets.failure()->detail.c_str());
    }
    assert(!assets.failure() && assets.records().size() == 1);
    const auto imported = assets.records().front();
    std::fprintf(stderr, "Model imported: %zu products\n", imported.products.size());
    process::assets::AssetDataRef<asset::ModelAsset::data_type> model;
    auto use = (*context.prepareAssets())->acquire<asset::ModelAsset>(imported.id);
    assert(use);
    auto loaded = (*context.prepareAssets())->read(*use);
    assert(context.engine().execution().spawn(stdexec::then(
        stdexec::continues_on(
            execution::asResult(std::move(loaded)),
            context.engine().execution().mainScheduler()
        ),
        [&](auto result) noexcept
        {
            assert(result);
            model = std::move(*result);
        }
    )));
    until([&] { return bool(model); });
    assert(model && model->primitives.size() == 2 && imported.products.size() >= 7);
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
    const auto query_mesh = model->primitives.front().mesh;
    assert(context.service<MeshQueryAssets>()->get().prepare(query_mesh)
    ); // Demand accepted, actual task waits for capacity.
    assert(!context.service<MeshQueryAssets>()->get().geometry(query_mesh));
    assert(context.engine().execution().dispatchMainCompletions());
    // No second prepare call: the production capacity continuation must resume this demand.
    until([&] { return context.service<MeshQueryAssets>()->get().geometry(query_mesh) != nullptr; });
    auto& queries = context.service<MeshQueryAssets>()->get();
    const auto geometry_before = queries.geometry(query_mesh);
    const auto query_tasks = [&]()
    {
        const auto tasks = context.engine().execution().taskInfos();
        return std::ranges::count(tasks, std::string{"Prepare mesh query"}, &execution::TaskInfo::name);
    };
    const auto prepared_tasks = query_tasks();
    assert(queries.prepare(query_mesh) && queries.prepare(query_mesh));
    assert(queries.geometry(query_mesh) == geometry_before && query_tasks() == prepared_tasks);

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
        for (auto entity : borrowed->get().view<const ecs::Mesh3D>())
        {
            assert(scene_projection->assetError(entity).type == 0);
            const bool ready = scene_projection->visualReady(entity);
            if (ready)
            {
                const auto* resources =
                    borrowed->get().try_get<::lux::engine::render::detail::MeshRenderResourceComponent>(entity);
                assert(resources && resources->roles.size() == 1);
                assert(!resources->roles[0].destination.effect.isNull() && !resources->roles[0].instance.isNull());
                // This recipe installs one Mesh3D representation. Wait for its actual
                // adopted asset revision, not merely an older ready resource.
                count += std::ranges::all_of(
                    resources->roles,
                    [&](const auto& role)
                    {
                        return role.mesh_source.revision == assets.revision() &&
                               role.material_source.revision == assets.revision();
                    }
                );
            }
        }
        return count;
    };
    until([&] { return projected() == 2 && outputStatus(*a).ready && outputStatus(*b).ready; });

    // Exercise the real Editor Registry -> registered binding -> Render response -> safe adoption path.
    // The private result type is inspected by this fixture only; no test getter enters production APIs.
    using LightResource = ::lux::engine::render::detail::LightRenderResourceComponent;
    ecs::Entity reactive_light = ecs::NullEntity;
    auto edit_light = [&](auto edit)
    { until([&] { return runtime.editInstance(editing_instance, edit).has_value(); }); };
    auto resource_id = [&]() -> lux::render::LightId
    {
        const auto borrowed = std::as_const(runtime).borrowInstance(editing_instance);
        assert(borrowed);
        const auto& registry = borrowed->get();
        const auto* resource =
            registry.valid(reactive_light) ? registry.try_get<const LightResource>(reactive_light) : nullptr;
        if (!resource)
        {
            return {};
        }
        assert(resource->roles.size() == 1);
        return resource->roles[0].light;
    };
    edit_light(
        [&](ecs::Registry& registry) noexcept
        {
            reactive_light = registry.create();
            ecs::Transform3D transform;
            transform.translation = {0, 4, 2};
            registry.emplace<ecs::Transform3D>(reactive_light, transform);
            rdesc::LightDescription light;
            light.cast_shadow = true;
            registry.emplace<ecs::Light3D>(reactive_light, light);
        }
    );
    until([&] { return !resource_id().isNull(); });
    const auto first_light_id = resource_id();
    edit_light(
        [&](ecs::Registry& registry) noexcept
        {
            registry.patch<ecs::Light3D>(
                reactive_light,
                [](auto& light) noexcept
                {
                    light.value.intensity = 4.F;
                    light.value.color = {0.8F, 0.6F, 0.4F};
                }
            );
            registry.patch<ecs::Transform3D>(
                reactive_light,
                [](auto& transform) noexcept { transform.translation.x() = 2; }
            );
        }
    );
    const auto before_light_update = host.rendering().stats().commands;
    until([&] { return host.rendering().stats().commands > before_light_update + 4; });
    assert(resource_id() == first_light_id); // Parameter updates must not recreate the resource.
    edit_light(
        [&](ecs::Registry& registry) noexcept
        {
            registry.remove<ecs::Light3D>(reactive_light);
            registry.emplace<ecs::Light3D>(reactive_light, rdesc::LightDescription{});
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
        [&](ecs::Registry& registry) noexcept
        {
            registry.patch<ecs::Light3D>(
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
    edit_light([&](ecs::Registry& registry) noexcept { registry.destroy(reactive_light); });
    assert(resource_id().isNull());
    edit_light(
        [&](ecs::Registry& registry) noexcept
        {
            reactive_light = registry.create();
            assert(reactive_light != removed_entity);
            registry.emplace<ecs::Transform3D>(reactive_light);
            registry.emplace<ecs::Light3D>(reactive_light, rdesc::LightDescription{});
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
    until([&] { return projected() == 2 && outputStatus(*a).ready && outputStatus(*b).ready; });
    const auto accepted_catalog = assets.revision();
    fixture::writeText(source, "this is not a model");
    assert(assets.reimport(imported.id));
    until([&] { return !assets.busy(); });
    assert(assets.failure() && !assets.failure()->catalog_published && assets.revision() == accepted_catalog);
    assert(assets.find(imported.id) && projected() == 2);
    assert(assets.importAsset(
        ModelImporter::name,
        ModelImporter::version,
        root / "Sources" / "missing.obj",
        *ModelImporter::encode({})
    ));
    until([&] { return !assets.busy(); });
    assert(assets.failure() && assets.revision() == accepted_catalog);
    assert(outputStatus(*a).ready && outputStatus(*b).ready);

    const auto root_object = std::get<SceneCreateObjects>(placement->operation).objects.front().id;
    assert(document->select(root_object));
    const auto transform_schema = ecs::componentSchemaId("lux.ecs.Transform3D");
    auto initial = document->component(root_object, transform_schema);
    assert(initial);
    auto edited_transform = *static_cast<const ecs::Transform3D*>(initial->data());
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
        assert(!runtime.borrowSystem<::lux::engine::render::RenderSystem>(*headless));
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
                auto change = [&](ecs::Registry& registry) noexcept
                {
                    const auto* residency = registry.ctx().find<scene::WorldResidency>();
                    const auto* identities = residency ? &residency->identities() : nullptr;
                    if (!identities)
                    {
                        return;
                    }
                    const auto entity = identities->entity(root_object);
                    if (entity == ecs::NullEntity)
                    {
                        return;
                    }
                    registry.patch<ecs::Transform3D>(
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
            author && static_cast<const ecs::Transform3D*>(author->data())->translation.x() ==
                          edited_transform.translation.x()
        );
        scenes.stopPlay(id);
        until([&] { return !scenes.playInstance(id).valid(); });
        assert(!document->readOnly());
    }

    auto graphical = scenes.startPlay(id, ESceneRunMode::GRAPHICAL);
    assert(graphical);
    until([&] { return scenes.playInstance(id).valid(); });
    auto play_view = outputs.add(std::make_unique<CameraViewport>(outputs, scenes.playInstance(id), 200, 150));
    assert(play_view);
    until([&] { return cameraOutput(*play_view)->setTransform(left); });
    until([&] { return outputStatus(*play_view).ready; });
    {
        auto play_system =
            host.engine().sceneRuntime().borrowSystem<::lux::engine::render::RenderSystem>(scenes.playInstance(id));
        assert(play_system);
        const auto* play_mesh = (*play_system)->binding<::lux::engine::render::MeshRenderBinding>();
        assert(play_mesh);
        const auto editing_destination = scene_projection->destination();
        const auto playing_destination = play_mesh->destination();
        assert(editing_destination != playing_destination);
        const auto command =
            host.rendering().mainClient().commandBinding<render::Mesh3dCommands::CreateInstance>(playing_destination);
        assert(command && command->destination == playing_destination);
    }
    scenes.stopPlay(id);
    until([&] { return !scenes.playInstance(id).valid(); });
    assert(outputStatus(*play_view).target.isNull() && outputStatus(*b).ready);

    {
        auto system = host.engine().sceneRuntime().borrowSystem<::lux::engine::render::RenderSystem>(editing_instance);
        assert(system);
        auto* cameras = (*system)->binding<::lux::engine::render::CameraViewReaction>();
        assert(cameras);
        const auto output = outputStatus(*a);
        auto registry = std::as_const(host.engine().sceneRuntime()).borrowInstance(editing_instance);
        assert(registry);
        const auto native = registry->get().get<::lux::engine::render::detail::RenderViewComponent>(output.camera);
        fixture::ViewOperation::fail_retirement = {
            (*system)->binding<::lux::engine::render::RenderScene>()->id(),
            native.view
        };
        bool completed = false;
        auto failed_exit = [&]() noexcept -> exec::task<render::RenderResult<void>>
        {
            auto result = co_await (*system)->binding<::lux::engine::render::RenderScene>()->detach(
                **system,
                native.view,
                native.attachment
            );
            assert(!result && result.error().type == render::kRuntimeState);
            completed = true;
            co_return render::RenderResult<void>{};
        };
        assert(host.engine().execution().spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(host.engine().execution().mainScheduler(), failed_exit())),
            [](auto result) noexcept { assert(result); }
        )));
        until([&] { return completed; });
        assert(fixture::ViewOperation::fail_retirement.isNull());
        // Actual close state remains for the output's ordinary retry; no Camera error mirror.

    }
    outputs.close(*a);
    until([&] { return outputStatus(*a).target.isNull(); });
    assert(outputStatus(*b).ready);
    const auto signals_after_close = fixture::StatelessObservation::signals;
    const auto active_view_count = fixture::ViewOperation::observations.size();
    const auto remaining_camera = outputStatus(*b).camera;
    auto notify_remaining = [&](ecs::Registry& registry) noexcept
    { registry.emplace_or_replace<fixture::ViewObservation>(remaining_camera, 51u); };
    until([&] { return context.engine().sceneRuntime().editInstance(editing_instance, notify_remaining).has_value(); });
    assert(fixture::StatelessObservation::signals - signals_after_close == 2 * active_view_count);
    assert(scenes.close(id));
    scene_projection = nullptr;
    until([&] { return host.engine().sceneRuntime().instanceCount() == application_scene_count && scenes.settled(); });
    assert(fixture::AssetTableProbe::destroyed == tables_destroyed + 1);
    assert(scenes.open(root / "Scenes/authored.luxscene"));
    until([&] { return scenes.find(id) != nullptr; });
    assert(scenes.find(id)->instance() != editing_instance);
    auto reopened_transform = scenes.find(id)->component(root_object, transform_schema);
    assert(
        reopened_transform &&
        static_cast<const ecs::Transform3D*>(reopened_transform->data())->translation ==
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
    const auto saved_system = migrated_package->scene->data().findSystem(system::SystemInstanceId{3});
    assert(saved_system && saved_system.instanceName() == "render");
    assert(saved_system.type().name == ::lux::engine::render::RenderSystem::Description.canonical_name);
    assert(
        saved_system && saved_system.configurationSchemaVersion() ==
                            ::lux::engine::render::RenderSystem::Description.configuration_schema_version
    );
    ::lux::engine::render::RenderSystemConfiguration migrated_config;
    assert(
        ::lux::engine::render::renderConfigurationCodec().decode(saved_system.configurationPayload(), &migrated_config)
    );
    const auto lighting = std::ranges::find(
        migrated_config.composition.contributions,
        lux::render::kDeferredLightingCapability,
        &lux::render::CapabilityContributionDescription::capability
    );
    assert(lighting != migrated_config.composition.contributions.end());
    const auto migrated_lighting = lux::render::decodeDeferredLightingConfiguration(lighting->configuration.bytes);
    assert(
        migrated_lighting && migrated_lighting->shadow.resolution == 64 &&
        migrated_lighting->shadow.filter_radius == 2 && migrated_lighting->shadow.distance == 37.F &&
        migrated_lighting->clusters.index_capacity == 2048
    );
    until(
        [&]
        {
            const auto tasks = context.engine().execution().taskInfos();
            return std::ranges::all_of(
                tasks,
                [](const auto& task) { return task.name != "Create Scene" || task.finished.has_value(); }
            );
        }
    );
    // Another Scene in this same Editor chooses no rendering or streaming System.
    // Its author data and lifecycle must not inherit the other Scene's providers.
    SceneCreateSelection neutral;
    simulation::SimulationDescriptionBuilder neutral_simulation;
    scene::SceneDescriptionBuilder neutral_systems;
    auto neutral_simulation_value = std::move(neutral_simulation).build();
    auto neutral_systems_value = std::move(neutral_systems).buildResolved();
    assert(neutral_simulation_value && neutral_systems_value);
    neutral.simulation =
        std::make_shared<const simulation::SimulationDescription>(std::move(*neutral_simulation_value));
    neutral.systems = std::make_shared<const scene::SceneDescription>(std::move(*neutral_systems_value));
    const auto neutral_id = asset::AssetId{generate()};
    const auto instance_count = host.engine().sceneRuntime().instanceCount();
    assert(scenes.create({neutral_id, "Neutral", "Scenes/neutral.luxscene", "lux.editor.scene.3d"}, std::move(neutral))
    );
    until([&] { return scenes.find(neutral_id) != nullptr; });
    assert(host.engine().sceneRuntime().instanceCount() == instance_count + 1);
    auto neutral_package = scene::readScenePackageFile(root / "Scenes/neutral.luxscene", 1U << 24);
    assert(neutral_package && neutral_package->scene->data().systemCount() == 0);
    assert(neutral_package->scene->data().geometries().empty());
    assert(scenes.find(migrated_package->scene->id()));
    assert(scenes.close(neutral_id));
    until([&] { return host.engine().sceneRuntime().instanceCount() == instance_count; });
    std::puts("PASS Editor multiple selections: rendering and neutral author Scene coexist independently");
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
        fixture::ProjectFacts loading{host};
        assert(fixture::open(host, project_path));
        until([&] { return loading.completed == 1; });
        assert(host.ready() && loading.failed == 0);
        auto startup_scenes = host.project().service<ProjectScenes>();
        assert(startup_scenes && startup_scenes->get().find(id));
        assert(host.engine().sceneRuntime().instanceCount() == application_scene_count + 1);
        std::puts("PASS startup Scene opens and reaches initial readiness before project load success");
        fixture::close(host); // The new Context follows the same close transaction as the original one.
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
