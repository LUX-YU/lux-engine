#include "../../../../../cmake/installed-consumers/common/ControlsTestAccess.hpp"
#include "ObjectQueue.hpp"
#include <fstream>
#include <imgui_internal.h>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/flowforge/FlowModule.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/material/MaterialCodec.hpp>
#include <lux/engine/editor/material/MaterialModule.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/SceneModule.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <source_location>
#ifdef LUX_P10_R1_NATIVE
#include "../../../authoring/flow/src/FlowSessionData.hpp"
#include "../../../authoring/material/src/MaterialSessionData.hpp"
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#endif
#include <imgui.h>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif
#include <cassert>
#include <cstdio>
#include <source_location>
#include <thread>

namespace registered_views
{
    using namespace lux::editor;
    template <class Value>
    views::ViewFactoryResult<views::DetachedView> prepare(
        lux::object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        std::shared_ptr<views::ViewFactoryEntry> entry,
        Value value
    )
    {
        auto snapshot = views::ViewFactorySnapshot::create({entry});
        if (!snapshot)
        {
            return lux::cxx::unexpected(snapshot.error());
        }
        const views::ViewFactoryInput input{
            dispatcher,
            std::move(id),
            lux::object::CodeLease::builtin(),
            lux::cxx::typeToken<Value>(),
            std::make_shared<const Value>(std::move(value))
        };
        return snapshot->prepare(views::ViewTypeId{entry->descriptor().type.name()}, input);
    }
    // These fixtures deliberately borrow an explicit interaction to inspect its gesture lifetime.
    // Production content factories instead construct the complete owner from ContentViewInput.
    template <class Create> auto fixtureFactory(views::ViewTypeId type, Create create)
    {
        return views::ViewFactoryEntry::create(
            lux::object::CodeLease::builtin(),
            views::ViewFactoryDescriptor{type.view(), "Borrowed fixture", lux::cxx::typeToken<std::monostate>()},
            [create = std::move(create)](const views::ViewFactoryInput& input
            ) mutable -> views::ViewFactoryResult<views::DetachedView>
            {
                auto view = create(input);
                if (!view)
                {
                    return lux::cxx::unexpected(
                        views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "fixture"}
                    );
                }
                return std::move(*view);
            }
        );
    }
    auto scene(
        lux::object::ObjectDispatcherRef dispatcher,
        scene::SceneViewServices services,
        scene::SceneViewCreateInfo info
    )
    {
        const auto id = info.id;
        return prepare(
            dispatcher,
            id,
            fixtureFactory(
                views::ViewTypeId{"lux.editor.scene.view"},
                [services, info = std::move(info)](const views::ViewFactoryInput& input) mutable
                { return scene::makeSceneView(input.dispatcher(), services, std::move(info)); }
            ),
            std::monostate{}
        );
    }
    auto material(
        lux::object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        material::MaterialViewServices services,
        std::optional<material::MaterialViewBinding> binding,
        material::MaterialViewState state
    )
    {
        return prepare(
            dispatcher,
            id,
            fixtureFactory(
                views::ViewTypeId{"lux.editor.material"},
                [services, binding, state](const views::ViewFactoryInput& input)
                { return material::makeMaterialView(input.dispatcher(), input.paneId(), services, binding, state); }
            ),
            std::monostate{}
        );
    }
    auto flow(
        lux::object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        flowforge::FlowViewServices services,
        std::optional<flowforge::FlowViewBinding> binding,
        flowforge::FlowViewState state
    )
    {
        return prepare(
            dispatcher,
            id,
            fixtureFactory(
                views::ViewTypeId{"lux.editor.flowforge"},
                [services, binding, state](const views::ViewFactoryInput& input)
                { return flowforge::makeFlowView(input.dispatcher(), input.paneId(), services, binding, state); }
            ),
            std::monostate{}
        );
    }

} // namespace registered_views
namespace
{
    using namespace lux;
    using namespace lux::editor;
    namespace ecs = simulation::ecs;
    namespace author = editor::scene;
    template <class T> void printFailure(const T& failure)
    {
        if constexpr (requires { failure.scene_system; })
        {
            printFailure(failure.code);
            printFailure(failure.scene_system);
        }
        else if constexpr (requires {
                               failure.subject_hash;
                               failure.configuration;
                           })
        {
            printFailure(failure.code);
            std::fprintf(stderr, "system=%llu subject=%llu\n", failure.system.value, failure.subject_hash);
        }
        else if constexpr (requires { failure.cause; })
        {
            printFailure(failure.cause);
        }
        else if constexpr (requires { failure.code; })
        {
            printFailure(failure.code);
        }
        else if constexpr (std::is_enum_v<T>)
        {
            std::fprintf(stderr, "%s=%u\n", typeid(T).name(), static_cast<unsigned>(failure));
        }
        else if constexpr (requires { std::variant_size<T>::value; })
        {
            std::visit([](const auto& value) { printFailure(value); }, failure);
        }
        else
        {
            std::fprintf(stderr, "Failure type: %s\n", typeid(T).name());
        }
    }
    template <class T> auto take(T value, std::source_location where = std::source_location::current())
    {
        if (!value)
        {
            std::fprintf(stderr, "Unexpected failure at %s:%u\n", where.file_name(), where.line());
            printFailure(value.error());
        }
        assert(value);
        return std::move(*value);
    }
    template <class View> void viewConfiguration(View& view)
    {
        const auto original = take(view.captureState());
        assert(original.schema == 1 && !original.bytes.empty());
        assert(!view.prepareState(99, original.bytes));
        auto truncated = original.bytes;
        truncated.pop_back();
        assert(!view.prepareState(original.schema, truncated));
        assert(take(view.captureState()) == original);
        auto prepared = take(view.prepareState(original.schema, original.bytes));
        assert(take(view.captureState()) == original);
        prepared();
        assert(take(view.captureState()) == original);
    }
    uuids::uuid uuid(std::string_view name)
    {
        return uuids::uuid_name_generator(*uuids::uuid::from_string("01234567-89ab-cdef-0123-456789abcdef"))(name);
    }
    struct Fixture final
    {
        process::ExecutionRuntime execution{
            take(process::ExecutionRuntime::create({2, 256, 256, {128}, process::BlockingSchedulerConfig{1, 32}}))
        };
        process::TaskScope tasks{execution};
        std::unique_ptr<render::RenderRuntime> renderer;
        std::unique_ptr<lux::scene::RenderResources> resources;
        std::unique_ptr<lux::scene::SceneRuntime> runtime;
        std::shared_ptr<author::ScenePresentationHub> hub;
        lux::test::ObjectQueue store_messages;
        sessions::SessionStore store{store_messages.dispatcherRef(), 4};
        object::ObjectMessageQueue messages{take(object::ObjectMessageQueue::create(256))};
        editor::commands::CommandRegistry commands;
        editor::commands::CommandDispatcher dispatcher{commands};
        std::unique_ptr<desktop::DesktopShell> desktop;
        std::unique_ptr<desktop::ViewHost> legacy_host;
        author::ProjectionEnvironment environment;
        std::optional<sessions::TSessionKey<author::SceneSession>> key;
        author::SceneSession* session{};
        world::WorldObjectId object{uuid("object")};
        std::uint64_t frames{};
        editor::material::MaterialPreview* material_preview{};
        author::RunStore* runs{};
        std::filesystem::path files{
            std::filesystem::temp_directory_path() /
            ("lux-p10-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
        };
        storage::FileArtifactStore disk{files};
        persistence::WriteCoordinator writes;
        persistence::SaveService saves{writes};
        persistence::SaveExecution transfer{execution, saves, writes, disk};
        ProjectContentSaving* publication{};

        window::LuxWindow* window_{};
        input::Input input_;

        explicit Fixture(window::LuxWindow* native = nullptr) : window_(native)
        {
            std::filesystem::create_directories(files);
            render::RendererConfig config;
            config.validation = true;
            if (window_)
            {
                for (auto extension : window::LuxWindow::requiredVulkanInstanceExtensions())
                {
                    config.instance_extensions.emplace_back(extension);
                }
            }
            renderer = take(render::RenderRuntime::create(
                config,
                [](auto severity, auto message)
                {
                    if (severity == 2)
                    {
                        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
                    }
                }
            ));
            std::vector<render::RenderFeatureRegistration> features;
            for (const auto& entry : render::builtinRenderFeatureRegistrations())
            {
                features.push_back(entry);
            }
            features.push_back(render::kUiRenderRenderFeatureRegistration);
            assert(renderer->beginFeatureRegistration(std::move(features)));
            wait(
                [&]
                {
                    return renderer->featureRegistrationStatus().state !=
                           render::EFeatureRegistrationState::REGISTERING;
                },
                false
            );
            assert(renderer->featureRegistrationStatus().state == render::EFeatureRegistrationState::READY);
            assert(renderer->commitFeatureRegistration());
            resources = take(lux::scene::RenderResources::create(*renderer, tasks, execution.cpu()));
            runtime = take(lux::scene::SceneRuntime::create(execution, {0, 2048}));
            hub = std::make_shared<author::ScenePresentationHub>(*runtime, execution);
            environment.renderer = renderer.get();
            environment.resources = resources.get();
            environment.simulation_systems = std::make_shared<simulation::SimulationSystemRegistry>();
            for (const auto& binding : lux::scene::builtinRenderFeatureSceneBindings())
            {
                environment.render_bindings.push_back(binding);
            }
            std::vector<ecs::ComponentSchema> types;
            for (auto group : {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas()})
            {
                for (auto schema : group)
                {
                    if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                    {
                        types.push_back(std::move(schema));
                    }
                }
            }
            environment.components = take(ecs::ComponentSchemaSet::build(std::move(types)));
            environment.scene_systems = {
                lux::scene::worldLoadingSystemRegistration(),
                lux::scene::transformSystemRegistration(),
                lux::scene::builtinRenderSystemRegistration()
            };
            lux::scene::SceneDescriptionBuilder builder;
            lux::scene::WorldLoadingConfiguration loading{{{0}}};
            std::vector<std::byte> loading_bytes;
            assert(environment.scene_systems[0].configuration.encode(&loading, loading_bytes));
            const auto add = [&](std::size_t index, std::span<const std::byte> bytes)
            {
                const auto& registration = environment.scene_systems[index];
                assert(builder.addSystem(
                    {static_cast<std::uint64_t>(index + 1)},
                    std::to_string(index),
                    registration.type,
                    registration.description->version,
                    registration.description->configuration_schema_name,
                    registration.description->configuration_schema_version,
                    bytes
                ));
            };
            add(0, loading_bytes);
            assert(builder.bindRequirement({1}, "world_loading", "world-storage"));
            add(1, take(lux::scene::makeTransformSystemConfiguration(64, {1024, 65536})));
            lux::scene::RenderSystemConfiguration render_configuration;
            for (const auto& feature : render::builtinRenderFeatureRegistrations())
            {
                const auto name = feature.factory.descriptor.canonical_name;
                if (name.find("camera") == std::string_view::npos && name.find("grid") == std::string_view::npos)
                {
                    continue;
                }
                std::vector<std::byte> defaults;
                assert(feature.configuration.portable.encode_default(defaults));
                render_configuration.features.push_back(
                    {feature.factory.descriptor.type,
                     std::move(defaults),
                     std::string(feature.configuration.schema),
                     feature.configuration.schema_version}
                );
            }
            assert(!render_configuration.features.empty());
            std::vector<std::byte> render_bytes;
            assert(environment.scene_systems[2].configuration.encode(&render_configuration, render_bytes));
            add(2, render_bytes);
            assert(builder.bindRequirement({3}, "render_runtime", "main-window"));
            assert(builder.bindRequirement({3}, "render_bindings", "render-bindings"));
            assert(builder.bindRequirement({3}, "render_resources", "resources"));
            std::vector<world::WorldDataSchemaId> schemas;
            for (const auto& schema : environment.components.all())
            {
                schemas.push_back(world::worldDataSchemaId(schema.id.name));
            }
            auto package = take(lux::scene::createScenePackage(
                asset::AssetId{uuid("scene")},
                "P10",
                schemas,
                std::make_shared<const simulation::SimulationDescription>(
                    take(std::move(simulation::SimulationDescriptionBuilder{}).build())
                ),
                take(std::move(builder).buildResolved())
            ));
            auto reservation =
                take(store.reserve<author::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
            auto model = take(author::SceneSession::create(
                reservation.id(),
                sessions::BoundSource{package.scene->id(), "scene.lux"},
                take(author::SceneSource::create(package, environment.components))
            ));
            session = model.get();
            assert(store.prepare(reservation, model));
            key = take(store.key<author::SceneSession>(take(store.publish(reservation))));
            ecs::WorldEntityMap identities;
            author::SceneEditBatch initial{session->describe().current, "initial", {}};
            initial.edits.push_back(author::SceneCreateObject{
                {object,
                 {0},
                 {take(author::encodeSceneValue(ecs::Transform3D{}, environment.components, identities, 4096))}}
            });
            assert(session->apply(std::move(initial)));
            desktop = take(desktop::DesktopShell::create(
                messages.dispatcherRef(),
                execution,
                *runtime,
                *renderer,
                *resources,
                window_,
                {.docking = false}
            ));
            legacy_host = std::make_unique<desktop::ViewHost>(desktop->root());
        }
        void frame(bool draw = true)
        {
            assert(renderer->collectCompletions(128));
            std::size_t controls = 128, programs = 32;
            assert(renderer->submitPending(controls, programs));
            assert(transfer.submitReady());
            assert(execution.collectCompletions());
            assert(execution.dispatchTaskEvents());
            saves.adoptCompletions();
            if (publication)
            {
                assert(publication->update());
            }
            if (material_preview)
            {
                material_preview->update();
            }
            if (desktop)
            {
                if (window_)
                {
                    window::LuxWindow::pollEvents();
                    input_.sample(*window_);
                    assert(desktop->feedInput(input_.snapshot()));
                }
                assert(legacy_host->drain());
                assert(desktop->update(
                    draw && window_ ? std::nullopt
                                    : std::optional{ui::FrameInfo{draw ? ui::Size{1000, 650} : ui::Size{}, 1.F / 60.F}}
                ));
            }
            if (runtime)
            {
                const auto driven = take(runtime->driveFrame());
                assert(driven.empty());
                ++frames;
                if (runs)
                {
                    assert(runs->update());
                }
            }
            if (hub)
            {
                hub->collectReleased();
            }
            // Match the product safe point: unmounted owning panes retire before borrowed services.
            static_cast<void>(messages.collectRetired());
        }
        template <class Fn>
        void wait(Fn condition, bool draw = true, std::source_location location = std::source_location::current())
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!condition())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    std::fprintf(
                        stderr,
                        "wait expired at %s:%u (%s)\n",
                        location.file_name(),
                        location.line(),
                        location.function_name()
                    );
                    std::abort();
                }
                frame(draw);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        author::SceneViewServices services()
        {
            return {store.access<author::SceneSession>(), hub, *runtime, *resources, *renderer, environment, {}};
        }
        author::SceneViewCreateInfo info(const char* name, author::SceneInteractionGroup& interaction)
        {
            author::SceneViewCreateInfo info{ui::PaneId{name}, name, author::EditedSceneBinding{*key, &interaction}};
            info.render_system = {3};
            info.state.camera.transform.translation = {0, 3, 8};
            return info;
        }
        ~Fixture()
        {
            assert(transfer.tasks().join());
            saves.adoptCompletions();
            legacy_host.reset();
            desktop.reset();
            hub.reset();
            wait([&] { return resources->empty(); }, false);
            assert(tasks.join());
            assert(renderer->statistics().validation_errors == 0);
            assert(renderer->beginClose());
            wait(
                [&]
                {
                    std::size_t replies = 128, controls = 128, programs = 32;
                    return take(renderer->advanceClose(replies, controls, programs)) == render::ERenderClose::COMPLETE;
                },
                false
            );
            assert(renderer->joinStopped());
        }
    };

    void inspectorView(Fixture& f)
    {
        auto before = f.session->describe();
        author::SceneInteractionGroup group(f.store.access<author::SceneSession>(), *f.key, {55});
        const author::SceneObjectRef target{f.key->id(), before.current.state.history, f.object};
        auto detached = take(author::makeInspectorView(
            f.messages.dispatcherRef(),
            lux::ui::PaneId{"inspector"},
            f.store.access<author::SceneSession>(),
            {*f.key, &group},
            target,
            f.environment.components,
            author::sceneInspectorComponents()
        ));
        auto* inspector = static_cast<author::InspectorView*>(detached.pane());
        const auto id = take((*f.legacy_host).adopt(detached, views::ViewRestoreKey{"inspector"})).id;
        f.frame(false);
        const auto find = [&](auto&& self, object::LuxObject& object) -> lux::ui::NumericEdit*
        {
            if (auto* control = dynamic_cast<lux::ui::NumericEdit*>(&object))
            {
                return control;
            }
            for (auto* child = object.firstChild(); child; child = child->nextSibling())
            {
                if (auto* result = self(self, *child))
                {
                    return result;
                }
            }
            return nullptr;
        };
        auto* control = find(find, *inspector);
        assert(control && std::get<double>(control->value()) == 0.0);
        control->setValue(2.0);
        assert(f.session->describe().current == before.current);
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
        assert(f.session->describe().current == before.current);
        f.frame(false);
        assert(group.overlay() && f.session->describe().current == before.current);
        auto invalid = target;
        invalid.object = world::WorldObjectId{uuid("missing")};
        assert(!inspector->rebind({*f.key, &group}, invalid));
        assert(inspector->target() == target && group.overlay());
        assert(inspector->finishEditing());
        assert(f.session->describe().current != before.current && !group.overlay());
        assert(f.session->undo());
        f.frame(false);
        assert(f.session->describe().current == before.current && std::get<double>(control->value()) == 0.0);
        assert(f.session->redo());
        f.frame(false);
        assert(std::get<double>(control->value()) == 2.0);
        assert(f.session->undo());
        f.frame(false);
        assert(inspector->removeComponent(ecs::componentSchemaId("lux.ecs.Transform3D")));
        f.frame(false);
        assert(!find(find, *inspector));
        assert(f.session->undo());
        f.frame(false);
        control = find(find, *inspector);
        assert(control && std::get<double>(control->value()) == 0.0);
        control->setValue(4.0);
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
        f.frame(false);
        auto read = take(f.session->read());
        auto busy = read.withRead(
            [&](const author::SceneReadView&) -> author::SceneEditResult<void>
            {
                assert(!inspector->prepareClose());
                assert(inspector->target() == target && group.overlay());
                assert((*f.legacy_host).close(id));
                auto drain = take((*f.legacy_host).drain());
                assert(drain.pending == 1 && (*f.legacy_host).describe(id));
                return {};
            }
        );
        assert(busy);
        f.wait([&] { return !(*f.legacy_host).describe(id); }, false);
        assert(!group.overlay() && f.session->describe().current == before.current);
    }
    void ownedSceneTools(Fixture& f)
    {
        const auto before = f.session->describe();
        const author::SceneObjectRef target{f.key->id(), before.current.state.history, f.object};
        services::ServiceRegistry services(f.messages.dispatcherRef());
        auto scope = take(services.createScope());
        auto module = take(extensions::EditorExtension::fromStatic(author::sceneModule()));
        auto declared = take(module.contributions());
        assert(services.publish(std::move(declared.services)));
        assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
        assert(scope.provide(services::ServiceNameView{"lux.simulation.components"}, f.environment.components));
        assert(scope.provide(services::ServiceNameView{"lux.scene.runtime"}, *f.runtime));
        assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, f.execution));
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.projection.environment"}, f.environment));
        desktop::UiRegistry windows(f.messages.dispatcherRef(), services);
        auto catalog = take(desktop::UiCatalog::prepare(std::move(declared.ui)));
        assert(windows.publish(catalog));
        auto& root = f.desktop->root();
        const views::ViewContent content{{f.key->id()}, f.key->id()};
        auto detached = take(windows.create(
            take(catalog.find(author::kSceneView.type)), scope,
            {f.messages.dispatcherRef(), ui::PaneId{"owned-primary"}, content}
        ));
        auto* primary = static_cast<author::SceneView*>(detached.get());
        auto group = primary->interactionOwner();
        assert(group && group->select({{target}}));
        assert(root.addSubPane(std::move(detached)));
        auto primary_window = take(root.identify(*primary));
        f.wait([&] { return primary->image().isValid(); });
        ui::PaneHandle outline, inspector, resources;
        {
            auto local = take(services.createScope(&scope));
            assert(local.provide(services::ServiceNameView{"lux.editor.scene.interaction"}, group));
            assert(local.provide(services::ServiceNameView{"lux.ui.root"}, root));
            assert(local.provide(services::ServiceNameView{"lux.editor.scene.viewport"}, primary_window));
            const auto make = [&](const char* name, const desktop::UiDescriptor& descriptor)
            {
                auto owner = take(windows.create(
                    take(catalog.find(descriptor.type)), local,
                    {f.messages.dispatcherRef(), ui::PaneId{name}, content}
                ));
                auto* pointer = owner.get();
                assert(root.addSubPane(std::move(owner)));
                return take(root.identify(*pointer));
            };
            outline = make("owned-outline", author::kOutlinerView);
            inspector = make("owned-inspector", author::kInspectorView);
            resources = make("owned-resources", author::kResourceView);
            assert(local.release() && local.drained());
        }
        const auto close = [&](ui::PaneHandle id)
        {
            auto removal = take(windows.prepareClose(root, std::span{&id, 1}));
            assert(root.commit(removal) && !root.findPane(id));
            f.frame(false); // Original Object/SceneRuntime retirement owners reclaim at their safe points.
        };
        std::weak_ptr<author::SceneInteractionGroup> lifetime = group;
        group.reset();
        close(primary_window);
        assert(!lifetime.expired());
        assert(!author::shareSceneInteraction(root, primary_window));
        assert(take(windows.content(root, outline)).primary == f.key->id());
        assert(take(windows.content(root, inspector)).primary == f.key->id());
        f.frame(false);
        assert(take(windows.content(root, resources)).sessions.empty());
        assert(lifetime.lock()->select({}));
        f.frame(false);
        const auto no_target = [&](ui::Pane& pane) { assert(!static_cast<author::InspectorView&>(pane).target()); };
        assert(root.withPane(inspector, no_target));
        // An empty selection does not detach the still-live author's content association.
        assert(take(windows.content(root, inspector)).primary == f.key->id());
        assert(lifetime.lock()->select({{target}}));
        auto read = take(f.session->read());
        assert(read.withRead(
            [&](const author::SceneReadView&) -> author::SceneEditResult<void>
            {
                f.frame(false);
                const auto unchanged = [&](ui::Pane& pane)
                {
                    auto& view = static_cast<author::InspectorView&>(pane);
                    assert(!view.target() && !view.status());
                };
                assert(root.withPane(inspector, unchanged));
                assert(lifetime.lock()->selection().objects == std::vector<author::VSceneSelectionTarget>{target});
                return {};
            }
        ));
        f.frame(false);
        const auto restored = [&](ui::Pane& pane)
        {
            auto& view = static_cast<author::InspectorView&>(pane);
            assert(view.status() && view.target() == target);
        };
        assert(root.withPane(inspector, restored));
        close(outline);
        assert(!lifetime.expired());
        close(inspector);
        close(resources);
        assert(lifetime.expired());
        auto hub = take(services.get<author::ScenePresentationHub>(scope));
        f.wait([&] { hub->collectReleased(); return hub->size() == 0; });
        hub.reset();
        assert(scope.release());
        (void)f.messages.collectRetired();
        assert(scope.drained() && services.drained());
        assert(f.session->describe().current == before.current && f.session->describe().dirty == before.dirty);
        std::puts("EC1 complete Scene tools: owner lifetime, no-target association, BUSY retry and release");
    }
    void declaredSceneTools(Fixture& f)
    {
        author::SceneView::ModelDrop model_drop;
        std::optional<author::ModelPlacement> received_drop;
        unsigned model_drops{};
        services::ServiceRegistry services(f.messages.dispatcherRef());
        auto scope = take(services.createScope());
        auto module = take(extensions::EditorExtension::fromStatic(author::sceneModule()));
        auto declared = take(module.contributions());
        assert(declared.ui.size() == 7);
        assert(services.publish(std::move(declared.services)));
        assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
        assert(scope.provide(services::ServiceNameView{"lux.simulation.components"}, f.environment.components));
        assert(scope.provide(services::ServiceNameView{"lux.scene.runtime"}, *f.runtime));
        assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, f.execution));
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.projection.environment"}, f.environment));
        desktop::UiRegistry windows(f.messages.dispatcherRef(), services);
        auto catalog = take(desktop::UiCatalog::prepare(std::move(declared.ui)));
        assert(windows.publish(catalog));
        auto& root = f.desktop->root();
        const auto before = f.session->describe();
        const author::SceneObjectRef target{f.key->id(), before.current.state.history, f.object};
        const views::ViewContent content{{f.key->id()}, f.key->id()};
        desktop::UiCreateInfo input{f.messages.dispatcherRef(), ui::PaneId{"declared-source"}, content, {}};
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.model-drop"}, model_drop));
        auto rejected_drop = windows.create(take(catalog.find(author::kSceneView.type)), scope, input);
        assert(!rejected_drop && rejected_drop.error().code == desktop::EUiError::INVALID_CONFIGURATION);
        assert(f.session->describe().current == before.current && !received_drop);
        model_drop = [&](const author::ModelPlacement& value) noexcept
        {
            ++model_drops;
            received_drop = value;
        };
        auto primary = take(windows.create(take(catalog.find(author::kSceneView.type)), scope, input));
        auto* scene = static_cast<author::SceneView*>(primary.get());
        auto group = scene->interactionOwner();
        assert(group && group->select({{target}}));
        assert(root.addSubPane(std::move(primary)));
        auto source = take(root.identify(*scene));
        f.wait([&] { return scene->image().isValid(); });
        const AssetReference dropped_asset{17, 4, asset::AssetId{uuid("declared-model-drop")}};
        assert(model_drops == 0);
        assert(scene->dropModel(dropped_asset, {50, 90}, {100, 100}));
        assert(model_drops == 1 && received_drop);
        assert(received_drop->target.id() == f.key->id() && received_drop->based_on == before.current);
        assert(received_drop->asset.project_instance == dropped_asset.project_instance);
        assert(received_drop->asset.catalog_revision == dropped_asset.catalog_revision);
        assert(received_drop->asset.asset == dropped_asset.asset && received_drop->position.allFinite());
        assert(f.session->describe().current == before.current && f.session->describe().dirty == before.dirty);

        std::vector<ui::PaneHandle> handles;
        author::InspectorView* inspector{};
        author::ResourceView* resources{};
        {
            auto local = take(services.createScope(&scope));
            assert(local.provide(services::ServiceNameView{"lux.editor.scene.interaction"}, group));
            assert(local.provide(services::ServiceNameView{"lux.ui.root"}, root));
            assert(local.provide(services::ServiceNameView{"lux.editor.scene.viewport"}, source));
            const auto make = [&](const desktop::UiDescriptor& descriptor)
            {
                input.instance = ui::PaneId{descriptor.type.name()};
                auto owner = take(windows.create(take(catalog.find(descriptor.type)), local, input));
                auto* pointer = owner.get();
                assert(!pointer->parent() && !pointer->attachedRoot());
                assert(root.addSubPane(std::move(owner)));
                handles.push_back(take(root.identify(*pointer)));
                return pointer;
            };
            auto* outline = static_cast<author::OutlinerView*>(make(author::kOutlinerView));
            inspector = static_cast<author::InspectorView*>(make(author::kInspectorView));
            resources = static_cast<author::ResourceView*>(make(author::kResourceView));
            assert(outline->interactionOwner() == group && inspector->interactionOwner() == group);
            assert(outline->objects().size() == 1 && inspector->target() == target);
            assert(resources->content() == content && resources->snapshot().instance == scene->presentedInstance());
            input.instance = ui::PaneId{"declared-invalid"};
            input.configuration.bytes.push_back(std::byte{1});
            const auto invalid = windows.create(take(catalog.find(author::kInspectorView.type)), local, input);
            assert(!invalid && invalid.error().code == desktop::EUiError::INVALID_CONFIGURATION);
            input.configuration.bytes.clear();
            auto stale = f.key->id();
            ++stale.generation;
            input.content = {{stale}, stale};
            assert(!windows.create(take(catalog.find(author::kInspectorView.type)), local, input));
            input.content = content;
            assert(group->selection().objects == std::vector<author::VSceneSelectionTarget>{target});
            assert(local.release() && local.drained());
        }
        const auto find = [&](auto&& self, object::LuxObject& object) -> ui::NumericEdit*
        {
            if (auto* value = dynamic_cast<ui::NumericEdit*>(&object))
            {
                return value;
            }
            for (auto* child = object.firstChild(); child; child = child->nextSibling())
            {
                if (auto* value = self(self, *child))
                {
                    return value;
                }
            }
            return nullptr;
        };
        f.frame(false);
        auto* field = find(find, *inspector);
        assert(field && std::get<double>(field->value()) == 0.0);
        field->setValue(4.0);
        static_cast<void>(ui::ControlsTestAccess::edited(*field, {true, true, false, false}));
        f.frame(false);
        assert(group->overlay() && f.session->describe().current == before.current);
        assert(take(f.session->read())
                   .withRead(
                       [&](const author::SceneReadView&) -> author::SceneEditResult<void>
                       {
                           auto denied = windows.prepareClose(root, handles);
                           assert(!denied && denied.error().code == desktop::EUiError::BUSY);
                           for (const auto& id : handles)
                           {
                               assert(root.findPane(id));
                           }
                           assert(group->overlay() && inspector->target() == target);
                           return {};
                       }
                   ));
        assert(inspector->finishEditing() && f.session->describe().current != before.current);
        assert(f.session->undo());
        f.frame(false);
        assert(f.session->describe().current == before.current && std::get<double>(field->value()) == 0.0);
        // Closing only the viewport neither closes the model nor steals its interaction from auxiliary UI.
        auto detached = take(windows.prepareClose(root, std::span{&source, 1}));
        assert(root.commit(detached) && !root.findPane(source));
        std::weak_ptr<author::SceneInteractionGroup> retained = group;
        group.reset();
        f.frame(false);
        assert(!retained.expired() && resources->content().sessions.empty());
        assert(!author::shareSceneInteraction(root, source));
        assert(retained.lock()->select({}));
        f.frame(false);
        assert(!inspector->target() && inspector->content() == content);
        assert(retained.lock()->select({{target}}));
        assert(take(f.session->read())
                   .withRead(
                       [&](const author::SceneReadView&) -> author::SceneEditResult<void>
                       {
                           f.frame(false);
                           assert(!inspector->target() && !inspector->status());
                           assert(
                               retained.lock()->selection().objects ==
                               std::vector<author::VSceneSelectionTarget>{target}
                           );
                           return {};
                       }
                   ));
        f.frame(false);
        assert(inspector->target() == target && inspector->status());
        auto closed = take(windows.prepareClose(root, handles));
        assert(root.commit(closed));
        (void)f.messages.collectRetired();
        assert(retained.expired());
        auto hub = take(services.get<author::ScenePresentationHub>(scope));
        f.wait(
            [&]
            {
                hub->collectReleased();
                return hub->size() == 0;
            }
        );
        hub.reset();
        assert(scope.release());
        (void)f.messages.collectRetired();
        assert(scope.drained() && services.drained());
        assert(f.session->describe().current == before.current && f.session->describe().dirty == before.dirty);
        std::puts("EC4 declared Scene auxiliaries: off-tree factories, lexical input, shared selection, original gate, "
                  "undo, viewport close and independent content lifetime PASS");
    }
    void closeInspectorContent(Fixture& f)
    {
        auto snapshot = take(f.session->capture());
        auto package = take(author::buildSceneSnapshotPackage(snapshot));
        auto reservation =
            take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
        auto model = take(author::SceneSession::create(
            reservation.id(),
            {},
            take(author::SceneSource::create(package, f.environment.components))
        ));
        auto* source = model.get();
        assert(f.store.prepare(reservation, model));
        auto key = take(f.store.key<author::SceneSession>(take(f.store.publish(reservation))));
        author::SceneInteractionGroup group(f.store.access<author::SceneSession>(), key, {901});
        const auto before = source->describe();
        const author::SceneObjectRef target{key.id(), before.current.state.history, f.object};
        auto candidate = take(author::makeInspectorView(
            f.messages.dispatcherRef(),
            ui::PaneId{"closing-inspector"},
            f.store.access<author::SceneSession>(),
            {key, &group},
            target,
            f.environment.components,
            author::sceneInspectorComponents()
        ));
        auto* inspector = static_cast<author::InspectorView*>(candidate.pane());
        auto& host = (*f.legacy_host);
        const auto view = take(host.adopt(candidate, views::ViewRestoreKey{"closing-inspector"})).id;
        class BlockingPane final : public ui::Pane
        {
        public:
            using Pane::Pane;
            bool refuse{true};
        };
        auto blocker = std::make_unique<BlockingPane>(
            f.messages.dispatcherRef(),
            ui::PaneId{"close-blocker"},
            ui::PaneTypeId{"test.blocker"},
            "Blocker"
        );
        auto* blocking = blocker.get();
        views::DetachedView other{
            lux::object::CodeLease::builtin(),
            std::move(blocker),
            +[](ui::Pane& pane) -> views::ViewCloseResult
            {
                if (static_cast<BlockingPane&>(pane).refuse)
                {
                    return cxx::unexpected(views::ViewPreparationFailure{"test.close", 7, "Not ready", true});
                }
                return {};
            }
        };
        const auto other_id = take(host.adopt(other, views::ViewRestoreKey{"close-blocker"})).id;
        f.frame(false);
        const std::array ids{view, other_id};
        assert(!host.prepareClose(ids));
        assert(host.describe(view) && host.describe(other_id));
        assert(inspector->target() == target && source->describe().current == before.current);
        blocking->refuse = false;
        auto prepared = take(host.prepareClose(ids));
        assert(inspector->target() == target);
        std::array permits{take(f.store.prepareClose(before.current))};
        bool notified{};
        auto connection = take(object::LuxObject::connect(
            &f.desktop->root(),
            &ui::Root::attachmentChanged,
            [&](const ui::AttachmentChanged& change) noexcept
            {
                if (change.mounted)
                {
                    return;
                }
                notified = true;
                assert(take(f.store.describe(key.id())).admission == sessions::EEditAdmission::CLOSING);
                auto refused = source->apply({before.current, "reentrant close edit", {}});
                assert(!refused);
                assert(inspector->target() == target); // Still owned until the handoff completes.
            }
        ));
        const auto commit_content = [&]() noexcept
        {
            assert(notified && f.store.close(permits));
            assert(!f.store.describe(key.id()));
        };
        assert(host.commitClose(prepared, commit_content));
        assert(notified && !host.describe(view) && !host.describe(other_id));
        assert(f.store.describe(f.key->id()));
        std::puts("PASS P12 real Inspector abandonable close preparation and guarded content retirement handoff");
    }
    void runningView(Fixture& f)
    {
        author::RunStore runs(*f.runtime, f.execution);
        f.runs = &runs;
        author::RunController controller(runs);
        auto reads = take(process::asset_loading::makeAssetReadOverlay({}, {}));
        author::RunEnvironment environment{
            f.environment.components,
            f.environment.simulation_systems,
            f.environment.scene_systems,
            f.environment.render_bindings,
            f.renderer.get(),
            f.resources.get(),
            {{13, 1}, 1, std::move(reads), {}}
        };
        auto prepare = take(controller.prepare(*f.session, std::move(environment), {.viewport = {3}}));
        f.wait([&] { return prepare->ready(); });
        const auto run = take(controller.adopt(*prepare));
        prepare.reset();
        auto services = f.services();
        services.runs.emplace(runs.inspect());
        author::SceneInteractionGroup author_group(f.store.access<author::SceneSession>(), *f.key, {20});
        author::SceneInteractionGroup run_group(f.store.access<author::SceneSession>(), *f.key, {21}, runs.inspect());
        auto author_candidate =
            take(author::SceneView::create(f.messages.dispatcherRef(), services, f.info("author-run-pair", author_group))
            );
        auto info = f.info("running", run_group);
        info.binding = author::RunningSceneBinding{run, &run_group};
        auto running = take(author::SceneView::create(f.messages.dispatcherRef(), services, info));
        viewConfiguration(*author_candidate);
        viewConfiguration(*running);
        auto* a = author_candidate.get();
        auto* b = running.get();
        auto& root = f.desktop->root();
        assert(root.addSubPane(std::move(author_candidate)) && root.addSubPane(std::move(running)));
        const auto aid = take(root.identify(*a));
        const auto bid = take(root.identify(*b));
        author::SceneViewCreateInfo command_info;
        command_info.id = ui::PaneId{"run-commands"};
        command_info.title = "Run (frozen author content)";
        command_info.state.camera.transform.translation = {0, 3, 8};
        command_info.render_system = {3};
        auto command_candidate = take(author::SceneView::create(
            f.messages.dispatcherRef(), services, std::move(command_info)
        ));
        assert(command_candidate->rebindRun(run));
        auto* command_scene = command_candidate.get();
        assert(root.addSubPane(std::move(command_candidate)));
        const auto command_view = take(root.identify(*command_scene));
        // The installed tool provider binds real run/view identities without an Application owner.
        const auto available = [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
        { return commands::CommandState{true}; };
        std::optional<author::StepTicket> command_step;
        std::optional<author::StopTicket> command_stop;
        auto run_commands = author::makeRunViewCommands(
            available,
            f.desktop->root(),
            runs,
            [&](author::RunId id) -> commands::CommandResult<void>
            {
                assert(id == run && !command_step);
                command_step = take(runs.step(id));
                return {};
            },
            [&](author::RunId id) -> commands::CommandResult<void>
            {
                assert(id == run && !command_stop);
                command_stop = take(runs.stop(id));
                return {};
            }
        );
        std::optional<author::ESceneTool> requested_tool;
        auto tool_commands = author::makeSceneToolCommands(
            available,
            [&](ui::PaneHandle id, author::ESceneTool tool) -> commands::CommandResult<void>
            {
                assert(id == take(f.desktop->root().identify(*b)));
                requested_tool = tool;
                return {};
            }
        );
        auto other_tools = author::makeSceneToolCommands(
            available,
            [](ui::PaneHandle, author::ESceneTool) -> commands::CommandResult<void> { return {}; }
        );
        for (std::size_t i{}; i < tool_commands.size(); ++i)
        {
            assert(&tool_commands[i]->descriptor() == &other_tools[i]->descriptor());
        }
        run_commands.insert(run_commands.end(), tool_commands.begin(), tool_commands.end());
        commands::CommandRegistry controls;
        assert(controls.publish(take(commands::CommandRegistrySnapshot::create(std::move(run_commands)))));
        const auto author_handle = take(f.desktop->root().identify(*a));
        const auto running_handle = take(f.desktop->root().identify(*b));
        std::optional<ui::PaneHandle> command_handle;
        const auto identify_command = [&](ui::Pane& pane) { command_handle = take(f.desktop->root().identify(pane)); };
        assert(root.withPane(command_view, identify_command) && command_handle);
        const auto invoke = [&](const char* name, ui::PaneHandle target)
        {
            return controls.execute(
                take(controls.snapshot().find(commands::CommandIdView{name})),
                commands::CommandInvocation::forView(target, lux::object::CodeLease::builtin())
            );
        };
        constexpr std::pair<const char*, author::ESceneTool> tool_cases[]{
            {"lux.editor.scene.outliner", author::ESceneTool::OUTLINER},
            {"lux.editor.scene.inspector", author::ESceneTool::INSPECTOR},
            {"lux.editor.scene.resources", author::ESceneTool::RESOURCES},
            {"lux.editor.scene.configuration", author::ESceneTool::CONFIGURATION}
        };
        for (const auto& [name, tool] : tool_cases)
        {
            assert(invoke(name, running_handle));
            assert(requested_tool == tool);
        }
        const auto refused_author = invoke("lux.editor.scene.pause", author_handle);
        // This original fixture borrows an external group; tool sharing correctly refuses it.
        assert(!refused_author && refused_author.error().code == commands::ECommandError::DOMAIN_FAILURE);
        f.wait([&] { return a->image().isValid() && b->image().isValid(); });
        auto outline = std::make_unique<author::OutlinerView>(
            f.messages.dispatcherRef(),
            ui::PaneId{"outline"},
            f.store.access<author::SceneSession>(),
            author::EditedSceneBinding{*f.key, &author_group},
            std::optional<author::RunInspectAccess>{},
            f.environment.components
        );
        assert(outline->status());
        auto* tree = outline.get();
        assert(root.addSubPane(std::move(outline)));
        const auto tree_id = take(root.identify(*tree));
        assert(tree->objects().size() == 1 && tree->select(tree->objects().front()));
        const auto stable_root = tree->objects().front();
        assert(tree->setCollapsed(stable_root, true) && tree->isCollapsed(stable_root));
        const auto tree_initial = f.session->describe().current;
        const world::WorldObjectId child_id{uuid("outliner-created")};
        assert(tree->createObject(child_id, {0}, author::EObjectSpace::SPACE_3D));
        f.frame();
        assert(tree->objects().size() == 2 && tree->isCollapsed(stable_root));
        const author::SceneObjectRef child{f.key->id(), tree_initial.state.history, child_id};
        assert(tree->setCollapsed(child, true) && tree->isCollapsed(child));
        auto stale_child = child;
        ++stale_child.session.generation;
        assert(!tree->isCollapsed(stale_child) && !tree->setCollapsed(stale_child, true));
        assert(tree->reparent(child, f.object));
        assert(take(take(f.session->read()).parent(child)) == f.object);
        assert(tree->erase(std::span{&child, 1}));
        assert(f.session->undo() && f.session->undo() && f.session->undo());
        assert(f.session->describe().current == tree_initial);
        f.frame();
        assert(tree->objects().size() == 1 && tree->isCollapsed(stable_root) && !tree->isCollapsed(child));
        f.wait([&] { return a->image().isValid(); });
        assert(author_group.selection().objects.size() == 1 && run_group.selection().objects.empty());
        assert(!tree->rebind(author::EditedSceneBinding{*f.key, nullptr}));
        assert(tree->objects().size() == 1 && tree->select(tree->objects().front()));
        assert(root.removeSubPane(*tree));
        f.wait([&] { return !root.findPane(tree_id); });
        assert(a->presentedInstance() != b->presentedInstance());
        assert(b->presentedInstance() == take(runs.info(run)).instance);
        const auto stamp = f.session->describe();
        const auto camera = a->state().camera.transform.translation;
        lux::editor::views::CameraMotion motion;
        motion.local_translation.x() = 1;
        const auto run_camera = b->state().camera.transform.translation;
        f.wait(
            [&]
            {
                auto result = b->navigate(motion);
                if (!result)
                {
                    const auto* failure = std::get_if<render::RendererFailure>(&result.error().cause);
                    assert(failure && failure->code == render::ERendererError::BUSY);
                    assert(b->state().camera.transform.translation == run_camera);
                }
                return result.has_value();
            }
        );
        assert(a->state().camera.transform.translation == camera && f.session->describe().current == stamp.current);
        assert(!b->beginEdit("must not edit author") && !run_group.overlay());
        assert(invoke("lux.editor.scene.pause", *command_handle));
        f.wait([&] { return take(runs.info(run)).state == author::ERunState::PAUSED; });
        const auto registry = take(runs.inspect().borrow(run));
        const auto entity = registry.get().view<const simulation::ecs::Transform3D>().front();
        const auto target = take(runs.inspect().reference(run, entity));
        const auto inspect_command = [&](const simulation::ecs::Registry&,
                                         const std::optional<editing::HistorySnapshot>&) -> author::RunResult<void>
        {
            const auto refused = invoke("lux.editor.scene.resume", *command_handle);
            assert(!refused && refused.error().code == commands::ECommandError::BUSY);
            assert(refused.error().domain == cxx::typeToken<author::ERunError>().name());
            assert(refused.error().domain_code == static_cast<std::uint64_t>(author::ERunError::BUSY));
            return {};
        };
        assert(runs.withInspection(target, inspect_command));
        assert(take(runs.info(run)).state == author::ERunState::PAUSED);
        {
            services::ServiceRegistry registry(f.messages.dispatcherRef());
            auto scope = take(registry.createScope());
            assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
            assert(scope.provide(services::ServiceNameView{"lux.simulation.components"}, f.environment.components));
            assert(scope.provide(services::ServiceNameView{"lux.editor.scene.runs"}, runs));
            auto group =
                std::make_shared<author::SceneInteractionGroup>(runs.inspect(), run, author::InteractionGroupId{902});
            assert(group->select({{target}}));
            desktop::UiRegistry windows(f.messages.dispatcherRef(), registry);
            assert(windows.publish(take(desktop::UiCatalog::prepare(
                {desktop::UiEntry::bind<author::kRunInspectorView>(object::CodeLease::builtin())}
            ))));
            auto factory = take(windows.snapshot().at(0));
            auto local = take(registry.createScope(&scope));
            assert(local.provide(services::ServiceNameView{"lux.editor.scene.interaction"}, group));
            auto candidate =
                take(windows.create(factory, local, {f.messages.dispatcherRef(), ui::PaneId{"declared-run-inspector"}})
                );
            auto* inspector = static_cast<author::RunInspectorView*>(candidate.get());
            assert(inspector->target() == target && inspector->interactionOwner() == group);
            auto& root = f.desktop->root();
            assert(root.addSubPane(std::move(candidate)));
            const auto handle = take(root.identify(*inspector));
            assert(local.release() && local.drained());
            const auto paused = take(take(runs.debugHistory(run)).get().view()).snapshot;
            f.frame(false);
            assert(inspector->status() && inspector->target() == target);
            auto closed = take(windows.prepareClose(root, std::span{&handle, 1}));
            assert(root.commit(closed) && !root.findPane(handle));
            f.frame(false);
            assert(take(runs.info(run)).state == author::ERunState::PAUSED);
            assert(take(take(runs.debugHistory(run)).get().view()).snapshot.current == paused.current);
            assert(f.session->describe().current == stamp.current);
            assert(scope.release() && scope.drained());
        }
        auto run_inspector = std::make_unique<author::RunInspectorView>(
            f.messages.dispatcherRef(),
            ui::PaneId{"run-fields"},
            runs,
            f.environment.components,
            author::runInspectorComponents()
        );
        assert(run_inspector->status() && run_inspector->rebind(target));
        auto* run_fields_view = run_inspector.get();
        assert(root.addSubPane(std::move(run_inspector)));
        const auto inspector_id = take(root.identify(*run_fields_view));
        assert(run_fields_view->target() == target);
        {
            const auto components = author::runInspectorComponents();
            const auto type = cxx::typeToken<simulation::ecs::Transform3D>();
            const auto registration = std::ranges::find(components, type, &author::RunInspectorComponent::type);
            assert(registration != components.end());
            author::RunInspectorFields fields(runs, target, *f.environment.components.find(type), registration->copy);
            assert(fields.refresh() && fields.writeRestriction().empty());
            const auto axis = [](auto& value) { return &value.translation.x(); };
            const auto initial = fields.read<simulation::ecs::Transform3D>(target)->translation.x();
            const auto paused = take(take(runs.debugHistory(run)).get().view()).snapshot;
            assert(fields.apply<simulation::ecs::Transform3D>(
                target,
                "translation.x",
                "Move paused object",
                axis,
                initial + 2.,
                {true, true, false, false}
            ));
            assert(
                take(runs.inspect().borrow(run)).get().get<simulation::ecs::Transform3D>(entity).translation.x() ==
                initial
            );
            assert(fields.cancel() && fields.refresh());
            assert(take(take(runs.debugHistory(run)).get().view()).snapshot.current == paused.current);
            assert(fields.apply<simulation::ecs::Transform3D>(
                target,
                "translation.x",
                "Move paused object",
                axis,
                initial + 3.,
                {true, true, true, false}
            ));
            assert(fields.update());
            assert(
                take(runs.inspect().borrow(run)).get().get<simulation::ecs::Transform3D>(entity).translation.x() ==
                initial + 3.
            );
            assert(editing::EditExecutor{}.undo(take(runs.debugHistory(run)).get()));
            assert(
                take(runs.inspect().borrow(run)).get().get<simulation::ecs::Transform3D>(entity).translation.x() ==
                initial
            );
            assert(fields.refresh());
            const auto inspect = [&](const simulation::ecs::Registry&,
                                     const std::optional<editing::HistorySnapshot>&) -> author::RunResult<void>
            {
                const auto refused = runs.resume(run);
                assert(!refused && std::get<author::ERunError>(refused.error().cause) == author::ERunError::BUSY);
                return {};
            };
            assert(runs.withInspection(target, inspect));
            assert(fields.apply<simulation::ecs::Transform3D>(
                target,
                "translation.x",
                "Old pause epoch",
                axis,
                initial + 4.,
                {true, true, false, false}
            ));
            assert(invoke("lux.editor.scene.resume", *command_handle));
            assert(!fields.finish() && fields.active());
            assert(fields.cancel());
            f.wait([&] { return fields.refresh().has_value(); });
            assert(!fields.writeRestriction().empty());
            assert(invoke("lux.editor.scene.pause", *command_handle));
            f.wait([&] { return take(runs.info(run)).state == author::ERunState::PAUSED; });
            assert(fields.refresh());
            assert(take(take(runs.debugHistory(run)).get().view()).snapshot.history != paused.history);
            assert(f.session->describe().current == stamp.current);
        }
        assert(run_fields_view->prepareClose() && root.removeSubPane(*run_fields_view));
        f.wait([&] { return !root.findPane(inspector_id); });
        assert(invoke("lux.editor.scene.step", *command_handle) && command_step);
        f.wait([&] { return take(runs.stepStatus(*command_step)).state == lux::scene::ESceneStepState::COMPLETED; });
        assert(runs.acknowledgeStep(*command_step));
        const auto clock = take(runs.info(run)).progress.time.elapsed;
        for (int i{}; i < 3; ++i)
        {
            f.frame();
        }
        assert(take(runs.info(run)).progress.time.elapsed == clock);
        assert(b->cancelEdit() && root.removeSubPane(*b));
        f.wait([&] { return !root.findPane(bid); });
        assert(!invoke("lux.editor.scene.resume", running_handle));
        assert(take(runs.info(run)).state == author::ERunState::PAUSED && a->image().isValid());
        assert(invoke("lux.editor.scene.stop", *command_handle) && command_stop);
        assert(command_scene->cancelEdit() && root.removeSubPane(*command_scene));
        f.wait([&] { return !root.findPane(command_view); });
        const auto stopped = *command_stop;
        f.wait([&] { return stopped.complete(); });
        assert(runs.acknowledgeStop(run));
        assert(a->cancelEdit() && root.removeSubPane(*a));
        f.wait([&] { return !root.findPane(aid); });
        assert(f.session->describe().current == stamp.current);
        (void)f.messages.collectRetired();
        f.runs = nullptr;
    }

    struct ArtifactPublication final
    {
        Fixture& fixture;
        asset::AssetVfs assets;
        std::unique_ptr<ProjectStorage> project;
        services::ServiceRegistry dependencies;
        services::ServiceScope scope;
        sessions::SessionOpening opening;
        std::shared_ptr<ProjectContentSaving> saving;
        explicit ArtifactPublication(Fixture& f)
            : fixture(f), dependencies(f.messages.dispatcherRef()), scope(take(dependencies.createScope())),
              opening(f.execution, f.store, f.saves, dependencies, scope)
        {
            ProjectManifest manifest{asset::AssetId{uuid("flow-project")}, "Flow publication", {}, {}};
            for (const auto name : {"flow", "ec4-flow"})
            {
                const auto relative = std::string{name} + ".lux";
                manifest.assets.push_back({asset::AssetId{uuid(name)}, "lux.flowforge.source", relative});
                std::ofstream(f.files / relative) << "Initial author source";
            }
            manifest.assets.push_back({asset::AssetId{uuid("material")}, "lux.material.source", "material.lux"});
            if (!std::filesystem::exists(f.files / "material.lux"))
            {
                std::ofstream(f.files / "material.lux") << "Initial material source";
            }
            const auto path = f.files / "Project.luxproject";
            {
                std::ofstream file(path);
                file << take(encodeProjectManifest(manifest));
            }
            auto prepared = take(prepareProjectOpen(path));
            project = take(
                ProjectStorage::open(prepared, assets, *f.execution.blocking(), f.tasks, f.messages.dispatcherRef())
            );
            assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
            assert(scope.provide(services::ServiceNameView{"lux.editor.sessions.opening"}, opening));
            assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.saves"}, f.saves));
            assert(scope.provide(services::ServiceNameView{"lux.editor.project.storage"}, *project));
            assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.writes"}, f.writes));
            assert(scope.provide(
                services::ServiceNameView{"lux.editor.persistence.files"},
                static_cast<persistence::IArtifactStore&>(f.disk)
            ));
            assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, f.execution));
            assert(scope.provide(services::ServiceNameView{"lux.editor.persistence.execution"}, f.transfer));
        }
        void observe()
        {
            saving = take(dependencies.get<ProjectContentSaving>(scope));
            fixture.publication = saving.get();
        }
        ~ArtifactPublication()
        {
            assert(!saving || saving->settled());
            fixture.publication = nullptr;
            saving.reset();
            assert(scope.release());
            (void)fixture.messages.collectRetired();
            assert(scope.drained() && dependencies.drained());
            project->requestClose();
            assert(take(project->advanceClose()));
        }
    };

    void flowComposition(Fixture& f, const char* linker)
    {
        namespace ef = editor::flowforge;
        using namespace lux::services;
        ArtifactPublication publication(f);
        auto slot = take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, object::CodeLease::builtin()));
        ef::FlowAuthoringSource source{asset::AssetId{uuid("ec4-flow")}, "EC4 shared", {}};
        const auto node = source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("entry"));
        assert(source.graph.addExport(
            {lux::flowforge::FlowForgeExportNodeId{1}, source.graph.getNode(node).node->id(), 991}
        ));
        auto candidate = take(ef::FlowSession::create(
            slot.id(),
            sessions::BoundSource{asset::AssetId{uuid("ec4-flow")}, "ec4-flow.lux"},
            std::move(source)
        ));
        auto* model = candidate.get();
        assert(f.store.prepare(slot, candidate));
        const auto key = take(f.store.key<ef::FlowSession>(take(f.store.publish(slot))));
        const auto initial = model->describe();
        const auto bytes = take(take(model->read()).encode());
        std::weak_ptr<ef::FlowSession> weak_model = take(f.store.access<ef::FlowSession>().share(key));

        auto& services = publication.dependencies;
        auto module = take(extensions::EditorExtension::fromStatic(ef::flowModule()));
        auto declarations = take(module.contributions());
        assert(declarations.services.size() == 2 && declarations.ui.size() == 1);
        assert(declarations.sessions.size() == 1 && declarations.commands.size() == 1);
        declarations.services.push_back(ServiceEntry::bind<kProjectContentSavingService>(object::CodeLease::builtin()));
        assert(services.publish(declarations.services));
        auto& scope = publication.scope;
        desktop::UiRegistry ui(f.messages.dispatcherRef(), services);
        auto catalog = take(desktop::UiCatalog::prepare(std::move(declarations.ui)));
        assert(ui.publish(catalog) && services.drained());
        const auto factory = take(catalog.selectContent({"lux.editor.flowforge"}));
        desktop::UiCreateInfo
            input{f.messages.dispatcherRef(), lux::ui::PaneId{"ec4-flow-a"}, {{key.id()}, key.id()}, {}};
        input.configuration.schema = 99;
        auto invalid = ui.create(factory, scope, input);
        assert(!invalid && invalid.error().code == desktop::EUiError::INVALID_CONFIGURATION && services.drained());
        input.configuration.schema = 1;
        {
            serialization::BinaryWriter writer(input.configuration.bytes);
            constexpr std::string_view missing_linker{"EC4-deliberately-missing-linker.exe"};
            assert(writer.writeUnsigned(std::uint64_t{1}));
            assert(writer.writeUnsigned(static_cast<std::uint32_t>(missing_linker.size())));
            assert(writer.writeBytes(std::as_bytes(std::span(missing_linker))));
        }
        auto a_owner = take(ui.create(factory, scope, input));
        input.instance = lux::ui::PaneId{"ec4-flow-b"};
        auto b_owner = take(ui.create(factory, scope, input));
        publication.observe();
        auto* a = static_cast<ef::FlowView*>(a_owner.get());
        auto* b = static_cast<ef::FlowView*>(b_owner.get());
        assert(!a->parent() && !b->parent() && !a->attachedRoot() && !b->attachedRoot());
        assert(a->binding()->session == key && b->binding()->session == key);
        assert(a->binding()->interaction != b->binding()->interaction);
        std::array owners{std::move(a_owner), std::move(b_owner)};
        auto& root = f.desktop->root();
        const auto activeWindows = [&] { return std::ranges::count_if(root.panes(), [](auto* pane) { return pane; }); };
        const auto before = activeWindows();
        assert(root.addSubPanes(owners) && activeWindows() == before + 2);
        const auto a_handle = take(root.identify(*a));
        const auto b_handle = take(root.identify(*b));
        assert(a_handle != b_handle && take(root.findPane(a_handle)) == a && take(root.findPane(b_handle)) == b);
        f.frame();
        assert(a->beginEdit("local draft"));
        std::vector<ef::VFlowEdit> edits;
        edits.emplace_back(ef::FlowRename{"uncommitted"});
        assert(a->previewEdit(edits) && a->binding()->interaction->overlay());
        assert(!b->binding()->interaction->overlay());
        assert(model->describe().current == initial.current && take(take(model->read()).encode()) == bytes);
        const views::ViewContent binding{{key.id()}, key.id()};
        assert(take(ui.content(root, a_handle)) == binding && take(ui.content(root, b_handle)) == binding);
        const auto b_state = take(ui.captureState(root, b_handle));
        auto failed_binding = ui.rebind(root, b_handle, {{sessions::SessionId{}}, sessions::SessionId{}});
        assert(!failed_binding && b->binding()->session == key && a->binding()->interaction->overlay());
        assert(take(ui.captureState(root, b_handle)).bytes == b_state.bytes);
        assert(ui.rebind(root, b_handle, {}) && !b->binding());
        assert(ui.rebind(root, b_handle, binding) && b->binding()->session == key);
        assert(take(model->read())
                   .withRead(
                       [&]() -> ef::FlowEditResult<void>
                       {
                           auto busy_cancel = ui.cancelPreview(root, a_handle);
                           assert(!busy_cancel && busy_cancel.error().code == desktop::EUiError::BUSY);
                           assert(a->binding()->interaction->overlay() && a->binding()->session == key);
                           assert(!b->binding()->interaction->overlay() && b->binding()->session == key);
                           return {};
                       }
                   ));
        assert(model->describe().current == initial.current && model->describe().observed == initial.observed);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        assert(ui.cancelPreview(root, a_handle) && !a->binding()->interaction->overlay());
        assert(root.findPane(a_handle) && a->binding()->session == key);
        assert(a->beginEdit("local draft"));
        edits.clear();
        edits.emplace_back(ef::FlowRename{"uncommitted"});
        assert(a->previewEdit(edits) && a->binding()->interaction->overlay());
        const std::array closing{a_handle, b_handle};
        const auto observed_before_close = model->describe().observed;
        assert(take(model->read())
                   .withRead(
                       [&]() -> ef::FlowEditResult<void>
                       {
                           auto busy_close = ui.prepareClose(root, closing);
                           assert(!busy_close && busy_close.error().code == desktop::EUiError::BUSY);
                           assert(root.findPane(a_handle) && root.findPane(b_handle));
                           assert(a->binding()->interaction->overlay() && !b->binding()->interaction->overlay());
                           return {};
                       }
                   ));
        assert(model->describe().current == initial.current && model->describe().observed == observed_before_close);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        workspace::VersionedViewState changed_state{1, {}};
        {
            serialization::BinaryWriter writer(changed_state.bytes);
            constexpr std::string_view another_missing_linker{"EC4-another-missing-linker.exe"};
            assert(writer.writeUnsigned(std::uint64_t{1}));
            assert(writer.writeUnsigned(static_cast<std::uint32_t>(another_missing_linker.size())));
            assert(writer.writeBytes(std::as_bytes(std::span(another_missing_linker))));
        }
        const std::vector<desktop::UiStateRequest> state_change{{a_handle, changed_state, {}}};
        assert(take(model->read())
                   .withRead(
                       [&]() -> ef::FlowEditResult<void>
                       {
                           auto busy_state = ui.mount(root, scope, {}, {}, state_change);
                           assert(!busy_state && busy_state.error().code == desktop::EUiError::BUSY);
                           assert(a->binding()->interaction->overlay() && a->binding()->session == key);
                           assert(take(ui.captureState(root, a_handle)).bytes == b_state.bytes);
                           return {};
                       }
                   ));
        assert(ui.mount(root, scope, {}, {}, state_change));
        assert(!a->binding()->interaction->overlay() && a->binding()->session == key);
        assert(b->binding()->session == key && take(ui.captureState(root, b_handle)).bytes == b_state.bytes);
        assert(take(ui.captureState(root, a_handle)).bytes == changed_state.bytes);
        assert(model->describe().current == initial.current && model->describe().observed == observed_before_close);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        workspace::DockLayout flow_layout;
        flow_layout.id = {"1234567890abcdef1234567890abcdef"};
        flow_layout.label = "Flow factory layout";
        flow_layout.slots = {
            {{1}, views::ViewRestoreKey{"ec4-flow-a"}, views::ViewTypeId{"lux.editor.flowforge"}, true, changed_state},
            {{2}, views::ViewRestoreKey{"empty-flow"}, views::ViewTypeId{"lux.editor.flowforge"}, true, b_state}
        };
        flow_layout.dock.nodes = {{1, workspace::EDockSplit::LEAF, 0, 0, .5, {{1}, {2}}}};
        flow_layout.dock.roots = {{1}};
        assert(ui.applyLayout(root, scope, flow_layout));
        const auto windows = take(ui.describe(root));
        assert(windows.size() == 3 && activeWindows() == before + 3);
        const auto empty =
            std::ranges::find_if(windows, [](const auto& window) { return window.restore_key.name() == "empty-flow"; });
        assert(empty != windows.end() && empty->content.sessions.empty());
        assert(take(ui.captureState(root, empty->handle)).bytes == b_state.bytes);
        assert(a->binding()->session == key && b->binding()->session == key);
        assert(take(ui.captureState(root, b_handle)).bytes == b_state.bytes);
        const auto empty_handle = empty->handle;
        f.frame();
        assert(ui.applyLayout(root, scope, flow_layout));
        assert(take(ui.describe(root)).size() == 3 && root.findPane(empty_handle));
        const auto captured_layout = take(ui.captureLayout(root, flow_layout.id, flow_layout.label));
        assert(captured_layout.slots.size() == 3);
        const auto restored_layout = take(workspace::decodeLayout(take(workspace::encodeLayout(captured_layout))));
        assert(ui.applyLayout(root, scope, restored_layout));
        assert(take(ui.describe(root)).size() == 3 && root.findPane(empty_handle));
        assert(a->binding()->session == key && b->binding()->session == key);
        assert(take(ui.captureState(root, a_handle)).bytes == changed_state.bytes);
        assert(take(ui.captureState(root, b_handle)).bytes == b_state.bytes);
        assert(model->describe().current == initial.current && model->describe().observed == observed_before_close);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);

        auto empty_close = take(ui.prepareClose(root, std::span{&empty_handle, 1}));
        assert(root.commit(empty_close));
        (void)f.messages.collectRetired();
        assert(activeWindows() == before + 2);
        assert(model->describe().current == initial.current && model->describe().observed == observed_before_close);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        assert(a->cancelEdit());
        const auto operation = take(a->compile());
        auto compiler = take(services.get<ef::FlowCompilationService>(scope));
        const auto& compiled = take(compiler->operation(operation)).get();
        f.frame();
        assert(a->compilation() == operation && b->compilation() == operation);
        auto prepared_close = take(ui.prepareClose(root, closing));
        assert(root.findPane(a_handle) && root.findPane(b_handle));
        assert(root.commit(prepared_close));
        assert(!root.findPane(a_handle) && !root.findPane(b_handle));
        assert(activeWindows() == before);
        (void)f.messages.collectRetired();
        // No surviving Pane is responsible for this completion. The actual scoped service remains.
        compiler.reset();
        f.wait([&] { return compiled.ready(); });
        compiler = take(services.get<ef::FlowCompilationService>(scope));
        assert(compiled.retryable() && compiled.object());
        const auto object = compiled.object();
        assert(compiler->retryLink(operation, {linker, 2}));
        f.wait([&] { return compiled.ready(); });
        assert(compiled.result() && compiled.object() == object);

        input.instance = lux::ui::PaneId{"ec4-flow-reopened"};
        auto reopened = take(ui.create(factory, scope, input));
        auto* view = static_cast<ef::FlowView*>(reopened.get());
        assert(root.addSubPane(std::move(reopened)) && weak_model.lock().get() == model);
        assert(!root.findPane(a_handle) && take(root.findPane(take(root.identify(*view)))) == view);
        assert(view->binding()->session == key && !view->binding()->interaction->overlay());
        assert(view->compilation() == operation); // Reopening observes the service's actual retained result.
        f.frame();
        assert(view->status() && view->compilation() == operation);
        assert(model->describe().current == initial.current && model->describe().observed == initial.observed);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        const auto published = take(view->requestPublication());
        assert(publication.saving->update());
        assert(take(publication.saving->artifactReports()).front().admitted);
        auto permit = take(f.store.prepareClose(initial.current));
        assert(f.store.close(permit) && !f.store.access<ef::FlowSession>().share(key));
        assert(!weak_model.expired() && !view->beginEdit("closed identity"));
        assert(view->cancelEdit() && root.removeSubPane(*view));
        (void)f.messages.collectRetired();
        assert(weak_model.expired());
        (void)f.store_messages.collect();
        f.wait([&] { return publication.saving->settled(); });
        const auto report = take(publication.saving->artifactReports()).front();
        assert(
            report.id == published && report.terminal && std::holds_alternative<PublicationSucceeded>(report.status)
        );
        assert(std::filesystem::exists(f.files / report.path));
        assert(publication.saving->acknowledgeArtifact(published));
        auto result = take(compiled.result());
        assert(compiler->acknowledge(operation));
        compiler.reset();
        publication.saving.reset();
        f.publication = nullptr;
        assert(scope.release());
        (void)f.messages.collectRetired();
        assert(scope.drained() && services.drained() && !result->bytes().empty());
        std::printf("EC4 Flow: actual lazy UiRegistry factory, two local interactions/shared model, Root ownership, "
                    "no-view completion/retry and logical-close lifetime PASS\n");
    }

    void sceneComposition(Fixture& f)
    {
        services::ServiceRegistry services(f.messages.dispatcherRef());
        auto scope = take(services.createScope());
        auto module = take(extensions::EditorExtension::fromStatic(author::sceneModule()));
        auto declared = take(module.contributions());
        assert(declared.sessions.size() == 1 && declared.commands.empty());
        assert(declared.services.size() == 2 && declared.ui.size() == 7);
        assert(services.publish(std::move(declared.services)));
        assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
        assert(scope.provide(services::ServiceNameView{"lux.scene.runtime"}, *f.runtime));
        assert(scope.provide(services::ServiceNameView{"lux.process.execution"}, f.execution));
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.projection.environment"}, f.environment));
        desktop::UiRegistry windows(f.messages.dispatcherRef(), services);
        auto catalog = take(desktop::UiCatalog::prepare(std::move(declared.ui)));
        assert(windows.publish(catalog) && services.drained());
        auto factory = take(catalog.selectContent({"lux.editor.scene"}));
        const auto package = take(author::buildSceneSnapshotPackage(take(f.session->capture())));
        auto reserved = take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, object::CodeLease::builtin()));
        auto owner = take(author::SceneSession::create(
            reserved.id(),
            {},
            take(author::SceneSource::create(package, f.environment.components))
        ));
        auto* model = owner.get();
        assert(f.store.prepare(reserved, owner) && f.store.publish(reserved));
        const auto key = take(f.store.key<author::SceneSession>(reserved.id()));
        const auto initial = model->describe();
        const auto encode = [&]
        {
            return take(lux::scene::encodeScenePackage(
                take(author::buildSceneSnapshotPackage(take(model->capture()))),
                16 * 1024 * 1024
            ));
        };
        const auto bytes = encode();
        std::weak_ptr<author::SceneSession> weak_model = take(f.store.access<author::SceneSession>().share(key));
        desktop::UiCreateInfo input{f.messages.dispatcherRef(), ui::PaneId{"ec4-scene-a"}, {{key.id()}, key.id()}, {}};
        input.configuration.schema = 77;
        const auto invalid = windows.create(factory, scope, input);
        assert(!invalid && invalid.error().code == desktop::EUiError::INVALID_CONFIGURATION && services.drained());
        input.configuration.schema = 1;
        auto first = take(windows.create(factory, scope, input));
        input.instance = ui::PaneId{"ec4-scene-b"};
        auto second = take(windows.create(factory, scope, input));
        auto* a = static_cast<author::SceneView*>(first.get());
        auto* b = static_cast<author::SceneView*>(second.get());
        assert(!a->parent() && !b->parent() && !a->attachedRoot() && !b->attachedRoot());
        assert(a->interactionOwner() && b->interactionOwner() && a->interactionOwner() != b->interactionOwner());
        auto hub = take(services.get<author::ScenePresentationHub>(scope));
        assert(take(services.get<author::ScenePresentationHub>(scope)) == hub && hub->size() == 1);
        auto& root = f.desktop->root();
        std::array owners{std::move(first), std::move(second)};
        assert(root.addSubPanes(owners));
        const auto aid = take(root.identify(*a)), bid = take(root.identify(*b));
        const std::array handles{aid, bid};
        assert(take(author::shareSceneInteraction(root, aid)) == a->interactionOwner());
        const auto nested_read = [&](ui::Pane&)
        {
            auto busy = author::shareSceneInteraction(root, aid);
            assert(!busy && busy.error() == ui::EAttachmentError::BUSY);
        };
        assert(root.withPane(aid, nested_read));
        f.wait([&] { return a->image().isValid() && b->image().isValid(); });
        assert(a->presentedInstance() == b->presentedInstance() && a->viewport() != b->viewport());
        author::ResourceView diagnostics(f.messages.dispatcherRef(), ui::PaneId{"ec4-resources"}, *f.runtime);
        assert(root.addSubPane(diagnostics));
        f.wait([&] { return bool(diagnostics.followViewport(root, aid)); });
        const auto diagnosed_instance = diagnostics.snapshot().instance;
        assert(diagnosed_instance == a->presentedInstance() && diagnostics.content().primary == key.id());
        auto foreign_root = take(ui::Root::create(f.messages.dispatcherRef()));
        auto refused = diagnostics.followViewport(*foreign_root, aid);
        assert(!refused && refused.error().code == render::ERendererError::INVALID_ARGUMENT);
        assert(!diagnostics.followViewport(root, {}));
        auto foreign_group = author::shareSceneInteraction(*foreign_root, aid);
        assert(!foreign_group && foreign_group.error() == ui::EAttachmentError::NOT_ATTACHED);
        assert(diagnostics.snapshot().instance == diagnosed_instance && diagnostics.content().primary == key.id());
        foreign_root.reset();
        assert(a->beginEdit("scene factory temporary gesture"));
        std::vector<author::VSceneEdit> changes;
        changes.emplace_back(author::SceneSetField::make<ecs::Transform3D>(
            {{key.id(), initial.current.state.history, f.object},
             ecs::componentSchemaId("lux.ecs.Transform3D"),
             "translation"},
            Eigen::Vector3d{7, 0, 0}
        ));
        assert(a->previewEdit(changes) && !b->interactionOwner()->overlay() && encode() == bytes);
        assert(take(model->read())
                   .withRead(
                       [&](const author::SceneReadView&) -> author::SceneEditResult<void>
                       {
                           auto busy = windows.prepareClose(root, handles);
                           assert(!busy && busy.error().code == desktop::EUiError::BUSY);
                           assert(a->interactionOwner()->overlay() && root.findPane(aid) && root.findPane(bid));
                           return {};
                       }
                   ));
        const auto before_camera = a->state().camera.transform.translation;
        auto stale = key.id();
        ++stale.generation;
        assert(!windows.rebind(root, aid, {{stale}, stale}));
        assert(a->interactionOwner()->overlay() && a->state().camera.transform.translation == before_camera);
        assert(a->commitEdit());
        const auto committed = model->describe().current;
        f.wait([&] { return a->projectedContent() == committed && b->projectedContent() == committed; });
        assert(b->undo());
        f.wait([&] { return a->projectedContent() == initial.current && b->projectedContent() == initial.current; });
        assert(encode() == bytes && model->describe().dirty == initial.dirty);
        const auto other_camera = b->state().camera.transform.translation;
        views::CameraMotion motion;
        motion.local_translation.x() = 2;
        const auto original_pose = a->state().camera.transform.translation;
        // The shared author version precedes Runtime publication. A writable camera still obeys its gate.
        f.wait(
            [&]
            {
                auto navigation = a->navigate(motion);
                if (!navigation)
                {
                    const auto* renderer = std::get_if<render::RendererFailure>(&navigation.error().cause);
                    assert(renderer && renderer->code == render::ERendererError::BUSY);
                    const auto borrowed = f.runtime->borrowInstance(a->presentedInstance());
                    assert(!borrowed);
                    const auto* runtime_error = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
                    assert(runtime_error && *runtime_error == lux::scene::ESceneRuntimeError::BUSY);
                    assert(a->state().camera.transform.translation == original_pose);
                }
                assert(b->state().camera.transform.translation == other_camera);
                return bool(navigation);
            }
        );
        assert(a->state().camera.transform.translation != original_pose);
        const auto saved_state = take(windows.captureState(root, aid));
        assert(windows.mount(root, scope, {}, {}, {{bid, saved_state, {}}}));
        assert(take(windows.captureState(root, bid)).bytes == saved_state.bytes);
        f.wait(
            [&]
            {
                return a->viewport().isValid() && b->viewport().isValid() && a->image().isValid() &&
                       b->image().isValid();
            }
        );
        const auto receipt = take(f.resources->viewReceipt(a->viewport()));
        const auto instance = a->presentedInstance();
        auto permit = take(f.store.prepareClose(model->describe().current));
        assert(f.store.close(permit) && !weak_model.expired());
        assert(!a->beginEdit("logically closed") && !f.store.access<author::SceneSession>().share(key));
        auto close = take(windows.prepareClose(root, handles));
        assert(root.commit(close) && !root.findPane(aid) && !root.findPane(bid));
        // Reusing the visible name cannot redirect the diagnostic's captured attachment identity.
        ui::Pane reused(
            f.messages.dispatcherRef(),
            ui::PaneId{"ec4-scene-a"},
            ui::PaneTypeId{"ec4.unrelated"},
            "Replacement"
        );
        assert(root.addSubPane(reused));
        assert(!author::shareSceneInteraction(root, aid));
        f.frame(false);
        assert(
            diagnostics.status() && !diagnostics.snapshot().instance.valid() && diagnostics.content().sessions.empty()
        );
        assert(root.removeSubPane(reused) && root.removeSubPane(diagnostics));
        (void)f.messages.collectRetired();
        (void)f.store_messages.collect();
        assert(weak_model.expired());
        f.wait(
            [&]
            {
                hub->collectReleased();
                return hub->size() == 0;
            }
        );
        f.wait([&] { return receipt.status().status.state == lux::scene::EViewState::CLOSED; });
        assert(!f.runtime->borrowInstance(instance));
        hub.reset();
        assert(scope.release());
        (void)f.messages.collectRetired();
        assert(scope.drained() && services.drained());
        std::printf("EC4 Scene: declared factories, actual shared author/projection, independent GPU viewports, "
                    "BUSY close, failed rebind, undo, state, logical close and retirement PASS\n");
    }

    void materialComposition(Fixture& f)
    {
        namespace em = editor::material;
        ArtifactPublication publication(f);
        auto& dependencies = publication.dependencies;
        auto& scope = publication.scope;
        auto module = take(extensions::EditorExtension::fromStatic(em::materialModule()));
        auto declared = take(module.contributions());
        assert(declared.sessions.size() == 1 && declared.commands.size() == 1);
        assert(declared.services.size() == 1 && declared.ui.size() == 1);
        declared.services.push_back(
            services::ServiceEntry::bind<kProjectContentSavingService>(object::CodeLease::builtin())
        );
        assert(dependencies.publish(std::move(declared.services)));
        auto environment = f.environment;
        environment.assets = {{29, 1}, 1, take(process::asset_loading::makeAssetReadOverlay({}, {})), {}};
        std::vector<render::RenderFeatureRegistration> features;
        for (const auto& feature : render::builtinRenderFeatureRegistrations())
        {
            features.push_back(feature);
        }
        assert(scope.provide(services::ServiceNameView{"lux.scene.runtime"}, *f.runtime));
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.projection.environment"}, environment));
        assert(scope.provide(services::ServiceNameView{"lux.render.features"}, features));
        desktop::UiRegistry windows(f.messages.dispatcherRef(), dependencies);
        auto catalog = take(desktop::UiCatalog::prepare(std::move(declared.ui)));
        assert(windows.publish(catalog) && dependencies.drained());
        const auto factory = take(catalog.selectContent({"lux.editor.material"}));
        auto slot = take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, object::CodeLease::builtin()));
        lux::material::MaterialSource source{asset::AssetId{uuid("material")}, "EC4 material", {}};
        auto constant = std::make_unique<lux::material::ConstantNode>();
        constant->setType(lux::material::EValueType::VEC3);
        constant->value[0] = .7F;
        const auto node = source.graph.addNode(std::move(constant));
        const auto output = source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
        assert(source.graph.connect(node, 0, output, 0));
        auto model_owner = take(em::MaterialSession::create(slot.id(), {}, std::move(source)));
        auto* model = model_owner.get();
        assert(f.store.prepare(slot, model_owner) && f.store.publish(slot));
        const auto key = take(f.store.key<em::MaterialSession>(slot.id()));
        const auto initial = model->describe();
        const auto bytes = take(take(model->read()).encode());
        std::weak_ptr<em::MaterialSession> weak_model = take(f.store.access<em::MaterialSession>().share(key));
        desktop::UiCreateInfo
            input{f.messages.dispatcherRef(), ui::PaneId{"ec4-material-a"}, {{key.id()}, key.id()}, {}};
        input.configuration.schema = 99;
        auto invalid = windows.create(factory, scope, input);
        assert(!invalid && invalid.error().code == desktop::EUiError::INVALID_CONFIGURATION && dependencies.drained());
        input.configuration.schema = 1;
        auto first = take(windows.create(factory, scope, input));
        input.instance = ui::PaneId{"ec4-material-b"};
        auto second = take(windows.create(factory, scope, input));
        auto* a = static_cast<em::MaterialView*>(first.get());
        auto* b = static_cast<em::MaterialView*>(second.get());
        assert(!a->parent() && !b->parent() && !a->attachedRoot() && !b->attachedRoot());
        assert(a->binding()->session == key && b->binding()->session == key);
        assert(a->binding()->interaction != b->binding()->interaction);
        auto& root = f.desktop->root();
        std::array owners{std::move(first), std::move(second)};
        assert(root.addSubPanes(owners));
        const auto a_id = take(root.identify(*a));
        const auto b_id = take(root.identify(*b));
        const std::array closing{a_id, b_id};
        assert(a->beginEdit("local material gesture"));
        std::vector<em::VMaterialEdit> edits;
        edits.emplace_back(em::MaterialSetConstant{node, {.1F, .8F, .3F, 1.F}});
        assert(a->previewEdit(edits));
        assert(!b->binding()->interaction->overlay());
        assert(model->describe().current == initial.current && take(take(model->read()).encode()) == bytes);
        assert(take(model->read())
                   .withRead(
                       [&](const lux::material::MaterialSource&) -> em::MaterialEditResult<void>
                       {
                           auto busy = windows.prepareClose(root, closing);
                           assert(!busy && busy.error().code == desktop::EUiError::BUSY);
                           assert(a->binding()->interaction->overlay() && root.findPane(a_id) && root.findPane(b_id));
                           return {};
                       }
                   ));
        assert(a->cancelEdit());
        const auto a_compile = take(a->compile());
        const auto b_compile = take(b->compile());
        auto compiler = take(dependencies.get<em::MaterialCompilationService>(scope));
        assert((take(compiler->snapshotIds()) == std::vector{a_compile, b_compile}));
        f.wait([&] { return a->image().isValid() && b->image().isValid(); });
        assert(a->previewStatus().accepted && b->previewStatus().accepted);
        assert(a->previewStatus().accepted->target != b->previewStatus().accepted->target);
        const auto b_camera = b->state().camera.transform.translation;
        views::CameraMotion motion;
        motion.local_translation.x() = 1;
        assert(a->navigate(motion) && b->state().camera.transform.translation == b_camera);
        auto state = take(windows.captureState(root, a_id));
        const std::vector<desktop::UiStateRequest> state_change{{b_id, state, {}}};
        assert(windows.mount(root, scope, {}, {}, state_change));
        assert(take(windows.captureState(root, b_id)).bytes == state.bytes);
        auto result = take(take(compiler->operation(a_compile)).get().result());
        const auto pending = take(a->compile());
        auto close = take(windows.prepareClose(root, closing));
        assert(root.commit(close) && !root.findPane(a_id) && !root.findPane(b_id));
        (void)f.messages.collectRetired();
        compiler.reset();
        f.wait([&] { return take(scope.settled()); });
        compiler = take(dependencies.get<em::MaterialCompilationService>(scope));
        assert(take(compiler->operation(pending)).get().ready());
        assert(take(take(compiler->operation(pending)).get().result())->key().content == initial.current);
        assert((take(compiler->snapshotIds()) == std::vector{a_compile, b_compile, pending}));
        assert(take(compiler->latest(key.id())) == pending);
        const auto captured_assets = take(compiler->assets(pending));
        assert(
            captured_assets.source == environment.assets.source && captured_assets.version == environment.assets.version
        );
        ++environment.assets.version; // Reopening must not silently use the current project asset version.
        assert(model->describe().current == initial.current && model->describe().observed == initial.observed);
        assert(model->describe().dirty == initial.dirty && take(take(model->read()).encode()) == bytes);
        input.instance = ui::PaneId{"ec4-material-reopened"};
        auto reopened = take(windows.create(factory, scope, input));
        auto* view = static_cast<em::MaterialView*>(reopened.get());
        assert(root.addSubPane(std::move(reopened)) && weak_model.lock().get() == model);
        assert(view->compilation() == pending);
        f.wait([&] { return view->image().isValid(); });
        assert(view->previewStatus().accepted && view->previewStatus().accepted->input.content == initial.current);
        assert(take(compiler->assets(pending)).version == captured_assets.version);
        auto permit = take(f.store.prepareClose(initial.current));
        assert(f.store.close(permit) && !f.store.access<em::MaterialSession>().share(key));
        assert(!weak_model.expired() && !view->beginEdit("closed material identity"));
        const auto reopened_id = take(root.identify(*view));
        auto closed = take(windows.prepareClose(root, std::span{&reopened_id, 1}));
        assert(root.commit(closed));
        (void)f.messages.collectRetired();
        (void)f.store_messages.collect();
        assert(weak_model.expired());
        assert(compiler->acknowledge(a_compile) && compiler->acknowledge(b_compile) && compiler->acknowledge(pending));
        assert(compiler->empty());
        compiler.reset();
        assert(scope.release());
        (void)f.messages.collectRetired();
        assert(scope.drained() && dependencies.drained() && !result->bytes().empty());
        std::printf("EC4 Material: declared complete factories, shared model/compiler, local GPU targets, "
                    "BUSY-safe close, camera state, no-view completion and logical-close lifetime PASS\n");
    }

    void flowView(Fixture& f, const char* linker)
    {
        namespace ef = editor::flowforge;
        ArtifactPublication publication_owner(f);
        assert(publication_owner.dependencies.publish(
            {services::ServiceEntry::bind<kProjectContentSavingService>(object::CodeLease::builtin())}
        ));
        publication_owner.observe();
        const asset::AssetId asset{uuid("flow")};
        ef::FlowAuthoringSource source{asset, "P10 Flow", {}};
        const auto event = source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("event"));
        const auto event_id = source.graph.getNode(event).node->id();
        assert(source.graph.addExport(
            {lux::flowforge::FlowForgeExportNodeId{1}, source.graph.getNode(event).node->id(), 1234}
        ));
        auto reserved =
            take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, lux::object::CodeLease::builtin()));
        auto model =
            take(ef::FlowSession::create(reserved.id(), sessions::BoundSource{asset, "flow.lux"}, std::move(source)));
        auto* author = model.get();
        assert(f.store.prepare(reserved, model));
        const auto key = take(f.store.key<ef::FlowSession>(take(f.store.publish(reserved))));
        ef::FlowInteraction interaction(f.store.access<ef::FlowSession>(), key);
        auto compilation = std::make_shared<ef::FlowCompilationService>(f.execution);
        ef::FlowViewServices
            services{f.store.access<ef::FlowSession>(), compilation, ef::FlowEnvironment{}, publication_owner.saving};
        auto detached = take(registered_views::flow(
            f.messages.dispatcherRef(),
            ui::PaneId{"flow"},
            services,
            ef::FlowViewBinding{key, &interaction},
            ef::FlowViewState{{"P10-deliberately-missing-linker.exe"}}
        ));
        auto* view = static_cast<ef::FlowView*>(detached.pane());
        viewConfiguration(*view);
        const auto id = take((*f.legacy_host).adopt(detached, views::ViewRestoreKey{"flow"})).id;
        const auto initial = author->describe();
        const auto encoded = take(take(author->read()).encode());
        assert(view->beginEdit("variable"));
        std::vector<ef::VFlowEdit> edits;
        edits.emplace_back(ef::FlowAddVariable{
            "condition",
            std::string(meta::builtin_ref_type_ptr<bool>()->name),
            {lux::flowforge::EFlowLiteralKind::BOOLEAN, "true"}
        });
        assert(view->previewEdit(edits));
        assert(author->describe().current == initial.current && take(take(author->read()).encode()) == encoded);
        // A real Session gate is held. The host must retain its owned Pane and interaction until later.
        assert((*f.legacy_host).close(id));
        auto read = take(author->read());
        assert(read.withRead(
            [&]() -> ef::FlowEditResult<void>
            {
                const auto report = take((*f.legacy_host).drain());
                assert(report.completed == 0 && report.pending == 1);
                assert((*f.legacy_host).describe(id) && interaction.overlay());
                return {};
            }
        ));
        assert(take((*f.legacy_host).drain()).completed == 1);
        assert(!interaction.overlay() && !(*f.legacy_host).describe(id));
        assert(author->describe().current == initial.current && take(take(author->read()).encode()) == encoded);
        auto reopened = take(registered_views::flow(
            f.messages.dispatcherRef(),
            ui::PaneId{"flow"},
            services,
            ef::FlowViewBinding{key, &interaction},
            ef::FlowViewState{{"P10-deliberately-missing-linker.exe"}}
        ));
        view = static_cast<ef::FlowView*>(reopened.pane());
        const auto next = take((*f.legacy_host).adopt(reopened, views::ViewRestoreKey{"flow"})).id;
        // The public view commands used by the controls exercise the complete property matrix.
        auto apply_property = [&](std::vector<ef::VFlowEdit> changes)
        {
            const auto before = take(take(author->read()).encode());
            const auto stamp = author->describe();
            assert(view->beginEdit("Flow property") && view->previewEdit(changes));
            assert(take(take(author->read()).encode()) == before && author->describe().current == stamp.current);
            assert(view->commitEdit());
            f.frame();
        };
        std::vector<ef::VFlowEdit> properties;
        properties.emplace_back(ef::FlowInsertNode{
            lux::object::CodeLease::builtin(),
            std::make_unique<lux::flowforge::FuncDefNode>(
                "function",
                std::vector<lux::flowforge::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "condition"}},
                std::vector<lux::flowforge::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "result"}}
            ),
            {20, 30}
        });
        apply_property(std::move(properties));
        const auto definition = take(author->capture()).source().nodes.back();
        properties.emplace_back(ef::FlowInsertFunctionUse{definition.id, false, {50, 60}});
        properties.emplace_back(ef::FlowAddVariable{"flag", "bool", {lux::flowforge::EFlowLiteralKind::BOOLEAN, "true"}}
        );
        apply_property(std::move(properties));
        const auto call = take(author->capture()).source().nodes.back();
        const auto data_pin = [](const auto& pins)
        {
            for (const auto& pin : pins)
            {
                if (pin.kind == lux::flowforge::EPinKind::DATA_IN || pin.kind == lux::flowforge::EPinKind::DATA_OUT)
                {
                    return pin.id;
                }
            }
            std::abort();
        };
        properties.emplace_back(
            ef::FlowSetLiteral{data_pin(call.inputs), {lux::flowforge::EFlowLiteralKind::BOOLEAN, "false"}}
        );
        properties.emplace_back(ef::FlowConnect{data_pin(definition.outputs), data_pin(call.inputs)});
        properties.emplace_back(ef::FlowSetExports{{{lux::flowforge::FlowForgeExportNodeId{1}, event_id, 5678}}});
        apply_property(std::move(properties));
        const auto connected = take(author->capture());
        assert(!connected.source().links.empty() && connected.source().exports.front().symbol == 5678);
        properties.emplace_back(ef::FlowDisconnect{data_pin(definition.outputs), data_pin(call.inputs)});
        properties.emplace_back(ef::FlowSetSignature{
            definition.id,
            "renamed function",
            std::get<lux::flowforge::FlowSourceSignature>(definition.parameters)
        });
        apply_property(std::move(properties));
        for (int i{}; i < 4; ++i)
        {
            assert(view->undo());
        }
        assert(take(take(author->read()).encode()) == encoded && author->describe().current == initial.current);
        assert(view->redo() && view->undo());
        assert(view->beginEdit("rename"));
        edits.emplace_back(ef::FlowRename{"Compiled Flow"});
        assert(view->previewEdit(edits) && view->commitEdit());
        f.frame();
        const auto operation = take(view->compile());
        f.wait([&] { return take(compilation->operation(operation)).get().ready(); });
        const auto& completed = take(compilation->operation(operation)).get();
        assert(completed.object() && completed.retryable());
        const auto object = completed.object();
        assert(!view->retryLink({{}, 0}));
        assert(view->retryLink({linker, 2}));
        f.wait([&] { return completed.ready(); });
        assert(completed.result() && completed.attempts().size() == 2);
        assert(completed.object() == object); // Failed linker configuration cannot discard the compiled artifact.
        const auto published_author = author->describe();
        const auto artifact = take(ef::captureFlowArtifact(take(completed.result())));
        assert(artifact.valid() && artifact.info().content == published_author.current);
        assert(artifact.info().type() == script::ScriptArtifactAsset::asset_type);
        const auto encoded_source = take(artifact.encodeSource({}));
        const auto expected_source = take(lux::flowforge::encodeFlowSource(*take(completed.result())->source()));
        assert(std::ranges::equal(encoded_source.bytes.view(), std::as_bytes(std::span(expected_source))));
        const auto publication = take(view->requestPublication());
        assert(publication_owner.saving->update());
        assert(take(publication_owner.saving->artifactReports()).front().admitted);
        assert(view->undo() && author->describe().current == initial.current);
        assert((*f.legacy_host).close(next));
        f.wait([&] { return !(*f.legacy_host).describe(next); });
        assert(take(compilation->operation(operation)).get().object() == object);
        f.wait([&] { return publication_owner.saving->settled(); });
        const auto report = take(publication_owner.saving->artifactReports()).front();
        assert(
            report.id == publication && report.terminal && std::holds_alternative<PublicationSucceeded>(report.status)
        );
        const auto package = take(asset::inspectPak(f.files / report.path));
        assert(package.entries.size() == 1 && package.entries.front().size == take(completed.result())->bytes().size());
        std::ifstream file(f.files / report.path, std::ios::binary);
        file.seekg(static_cast<std::streamoff>(package.entries.front().offset));
        std::vector<std::byte> disk_bytes(package.entries.front().size);
        assert(file.read(reinterpret_cast<char*>(disk_bytes.data()), static_cast<std::streamsize>(disk_bytes.size())));
        assert(std::ranges::equal(disk_bytes, take(completed.result())->bytes().view()));
        assert(author->describe().current == initial.current && author->describe().dirty == initial.dirty);
        assert(publication_owner.saving->acknowledgeArtifact(publication));
        assert(compilation->acknowledge(operation));
        assert(take(take(author->read()).encode()) == encoded);
    }

    editor::material::MaterialPreviewRecipe quadPreviewRecipe()
    {
        const asset::AssetId mesh_id{uuid("dual-viewport-mesh")};
        auto mesh = std::make_shared<rdesc::Mesh>();
        for (const Eigen::Vector3f position :
             {Eigen::Vector3f{-1, -1, 0}, Eigen::Vector3f{1, -1, 0}, Eigen::Vector3f{1, 1, 0}, Eigen::Vector3f{-1, 1, 0}
             })
        {
            rdesc::Vertex vertex{};
            vertex.position = position;
            vertex.normal = {0, 0, 1};
            vertex.tangent = {1, 0, 0};
            vertex.bitangent = {0, 1, 0};
            vertex.uv = {0, 0};
            mesh->vertices.push_back(vertex);
        }
        mesh->indices = {0, 1, 2, 0, 2, 3};
        auto mesh_asset = take(asset::MeshAsset::create({mesh_id, asset::MeshAsset::asset_type}, mesh));
        auto bytes = std::make_shared<const std::vector<std::byte>>(
            take(asset::TAssetSerDeser<asset::MeshAsset>::encode(*mesh_asset, asset::AssetEncodeLimits{1024 * 1024}))
        );
        return {mesh_id, cxx::SharedBytes<>::fromOwner(bytes, *bytes)};
    }

    std::shared_ptr<const editor::material::CompiledMaterial> materialView(Fixture& f)
    {
        namespace em = editor::material;
        ArtifactPublication publication_owner(f);
        assert(publication_owner.dependencies.publish(
            {services::ServiceEntry::bind<kProjectContentSavingService>(object::CodeLease::builtin())}
        ));
        publication_owner.observe();
        const asset::AssetId asset{uuid("material")};
        lux::material::MaterialSource source{asset, "P10 material", {}};
        auto constant = std::make_unique<lux::material::ConstantNode>();
        constant->setType(lux::material::EValueType::VEC3);
        constant->value[0] = .7F;
        constant->value[1] = .1F;
        constant->value[2] = .2F;
        const auto constant_id = source.graph.addNode(std::move(constant));
        const auto output = source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
        assert(source.graph.connect(constant_id, 0, output, 0));
        auto reserved =
            take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
        auto model = take(
            em::MaterialSession::create(reserved.id(), sessions::BoundSource{asset, "material.lux"}, std::move(source))
        );
        auto* author = model.get();
        assert(f.store.prepare(reserved, model));
        const auto key = take(f.store.key<em::MaterialSession>(take(f.store.publish(reserved))));
        em::MaterialInteraction interaction(f.store.access<em::MaterialSession>(), key);
        em::MaterialPreviewEnvironment environment{f.environment, {}};
        for (const auto& feature : render::builtinRenderFeatureRegistrations())
        {
            environment.features.push_back(feature);
        }
        const auto shared_environment = environment;
        em::MaterialPreview preview{*f.runtime, std::move(environment)};
        f.material_preview = &preview;
        auto compilation = std::make_shared<em::MaterialCompilationService>(f.execution);
        em::MaterialSaveSource save_source(
            f.store.access<em::MaterialSession>(),
            key,
            take(f.disk.resolve("material.lux")),
            sessions::BindingRevision{1}
        );
        auto save_registration = take(f.saves.registerSource(save_source));
        em::MaterialViewState state;
        state.camera.transform.translation = {0, 0, 3.5};
        auto detached = take(registered_views::material(
            f.messages.dispatcherRef(),
            ui::PaneId{"material"},
            {f.store.access<em::MaterialSession>(),
             *f.runtime,
             *f.resources,
             *f.renderer,
             preview,
             compilation,
             f.environment,
             {},
             {2},
             publication_owner.saving},
            em::MaterialViewBinding{key, &interaction},
            state
        ));
        auto* view = static_cast<em::MaterialView*>(detached.pane());
        viewConfiguration(*view);
        const auto id = take((*f.legacy_host).adopt(detached, views::ViewRestoreKey{"material"})).id;
        const auto encoded = take(take(author->read()).encode());
        const auto initial = author->describe();
        {
            const auto snapshot = take(author->capture());
            const auto from = snapshot.source().graph.node(constant_id)->outputs().front().id;
            const auto to = snapshot.source().graph.node(output)->inputs().front().id;
            std::vector<em::VMaterialEdit> properties;
            properties.emplace_back(
                em::MaterialSetParameterSlots{{{"gain", lux::material::EValueType::FLOAT, {2, 0, 0, 0}}}}
            );
            properties.emplace_back(em::MaterialSetTextureSlots{{{"albedo", {}}}});
            properties.emplace_back(em::MaterialPlaceNode{constant_id, {20, 30}});
            properties.emplace_back(em::MaterialDisconnect{from, to});
            properties.emplace_back(em::MaterialConnect{from, to});
            assert(view->beginEdit("Slots, connections and placement") && view->previewEdit(properties));
            assert(author->describe().current == initial.current && take(take(author->read()).encode()) == encoded);
            assert(view->commitEdit());
            f.frame();
            const auto changed = take(author->capture());
            assert(changed.source().graph.param_slots.size() == 1 && changed.source().graph.texture_slots.size() == 1);
            assert(view->undo() && view->redo() && view->undo());
            assert(author->describe().current == initial.current && take(take(author->read()).encode()) == encoded);
        }
        assert(view->beginEdit("constant"));
        std::vector<em::VMaterialEdit> edits;
        edits.emplace_back(em::MaterialSetConstant{constant_id, {.2F, .8F, .1F, 1.F}});
        assert(view->previewEdit(edits));
        assert(take(take(author->read()).encode()) == encoded && author->describe().current == initial.current);
        assert(view->cancelEdit() && !interaction.overlay());
        assert(view->beginEdit("constant"));
        edits.emplace_back(em::MaterialSetConstant{constant_id, {.2F, .8F, .1F, 1.F}});
        assert(view->previewEdit(edits) && view->commitEdit());
        assert(author->describe().current != initial.current);
        const auto compile_id = take(view->compile());
        const auto* operation = &take(compilation->operation(compile_id)).get();
        f.wait([&] { return operation->ready(); });
        const auto compiled = take(operation->result());
        const auto unsaved = author->describe();
        const auto artifact = take(em::captureMaterialArtifact(compiled));
        assert(artifact.valid() && artifact.info().content == unsaved.current);
        assert(artifact.info().type() == asset::MaterialAsset::asset_type);
        const auto encoded_source = take(artifact.encodeSource({}));
        const auto expected_source = take(lux::material::encodeMaterialSource(*compiled->source()));
        assert(std::ranges::equal(encoded_source.bytes.view(), std::as_bytes(std::span(expected_source))));
        const auto publication = take(view->requestPublication());
        f.wait([&] { return publication_owner.saving->settled(); });
        const auto report = take(publication_owner.saving->artifactReports()).front();
        assert(
            report.id == publication && report.terminal && std::holds_alternative<PublicationSucceeded>(report.status)
        );
        const auto package = take(asset::inspectPak(f.files / report.path));
        assert(package.entries.size() == 1 && package.entries.front().size == compiled->bytes().size());
        {
            std::ifstream file(f.files / report.path, std::ios::binary);
            file.seekg(static_cast<std::streamoff>(package.entries.front().offset));
            std::vector<std::byte> disk_bytes(package.entries.front().size);
            assert(
                file.read(reinterpret_cast<char*>(disk_bytes.data()), static_cast<std::streamsize>(disk_bytes.size()))
            );
            assert(std::ranges::equal(disk_bytes, compiled->bytes().view()));
        }
        assert(author->describe().current == unsaved.current && author->describe().dirty == unsaved.dirty);
        assert(publication_owner.saving->acknowledgeArtifact(publication));
        auto reads = take(process::asset_loading::makeAssetReadOverlay({}, {}));
        const lux::scene::RenderAssetInput preview_assets{{9, 1}, 1, std::move(reads), {}};
        assert(preview.receive(preview.status().desired, operation->result(), preview_assets));
        f.wait([&] { return preview.status().accepted.has_value() && view->image().isValid(); });
        assert(preview.status().accepted->input == operation->key());
        assert(compilation->acknowledge(compile_id));
        operation = nullptr;
        assert(view->image().isValid());
        // Both real GPU targets consume one immutable compilation, after task acknowledgement.
        // Closing one target retires only its own scene/view; the first remains rendered.
        {
            em::MaterialPreview second{*f.runtime, shared_environment};
            const auto quad = quadPreviewRecipe();
            const auto sphere = take(em::makeSphereMaterialPreviewRecipe());
            const auto adoption = take(second.setDesired(compiled->key(), quad));
            assert(adoption.target != preview.status().desired.target);
            assert(second.receive(adoption, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    const auto status = second.status();
                    if (status.failure)
                    {
                        std::fprintf(stderr, "Second preview failed: %s\n", status.failure->domain.c_str());
                    }
                    assert(!status.failure);
                    return second.instance().valid();
                }
            );
            assert(second.instance() != preview.instance());
            auto presentation = take(views::ViewportPresentation::create(
                *f.runtime,
                second.instance(),
                *f.resources,
                {2},
                second.camera(),
                lux::scene::ViewConfig{.extent = {320, 240}}
            ));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().accepted == adoption && presentation->image().isValid();
                }
            );
            assert(view->image().isValid() && preview.status().accepted->input == compiled->key());
            const auto pixels = [&]
            {
                for (int i{}; i < 8; ++i)
                {
                    second.update();
                    presentation->update({320, 240});
                    f.frame();
                }
                const auto output = take(f.resources->outputInfo(take(f.resources->viewOutput(presentation->view()))));
                std::vector<std::byte> buffer(std::size_t(output.extent.width) * output.extent.height * 8);
                auto request =
                    f.renderer->control()->get().readbackTargetAsync(output.target, buffer.data(), buffer.size(), 4);
                f.wait([&] { return request.isReady(); });
                const auto result = request.tryResult();
                assert(result && result->get().status == 0 && result->get().bytes_written > 0);
                assert(result->get().bytes_written <= buffer.size());
                buffer.resize(static_cast<std::size_t>(result->get().bytes_written));
                return buffer;
            };
            const auto quad_pixels = pixels();
            const auto instance = second.instance();
            const auto sphere_adoption = take(second.setDesired(compiled->key(), sphere));
            assert(sphere_adoption.input == adoption.input && sphere_adoption.recipe > adoption.recipe);
            assert(second.receive(adoption, compiled, preview_assets)); // Late old recipe result must only settle.
            assert(second.status().accepted == adoption && !second.status().prepared && second.status().stale);
            assert(second.receive(sphere_adoption, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().accepted == sphere_adoption;
                }
            );
            const auto sphere_pixels = pixels();
            assert(sphere_pixels != quad_pixels && second.instance() == instance);
            // The same existing quad now replaces the sphere through the identical public entry.
            const auto again = take(second.setDesired(compiled->key(), quad));
            assert(second.receive(again, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().accepted == again;
                }
            );
            assert(pixels() == quad_pixels);
            // Replace a queued resource candidate before Runtime maintenance can consume it.
            const auto superseded = take(second.setDesired(compiled->key(), sphere));
            assert(second.receive(superseded, compiled, preview_assets));
            second.update();
            assert(second.status().prepared == superseded && second.status().accepted == again);
            const auto latest = take(second.setDesired(compiled->key(), quad));
            assert(second.receive(latest, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().accepted == latest;
                }
            );
            assert(second.receive(superseded, compiled, preview_assets));
            assert(second.status().accepted == latest && !second.status().prepared && pixels() == quad_pixels);
            // Same mesh ID, different owning bytes: a real decode failure, not a key-only change.
            auto corrupt = std::make_shared<const std::vector<std::byte>>(32, std::byte{0xff});
            const auto broken = take(second.setDesired(
                compiled->key(),
                em::MaterialPreviewRecipe{quad.mesh, cxx::SharedBytes<>::fromOwner(corrupt, *corrupt)}
            ));
            corrupt.reset();
            assert(broken.recipe > latest.recipe && broken.input == latest.input);
            assert(second.receive(broken, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().failure.has_value();
                }
            );
            assert(second.status().accepted == latest && second.status().stale && !second.status().prepared);
            assert(pixels() == quad_pixels); // Last successful geometry and material survived actual resource failure.
            assert(second.receive(sphere_adoption, compiled, preview_assets));
            assert(second.status().accepted == latest && !second.status().prepared);
            const auto recovered = take(second.setDesired(compiled->key(), sphere));
            assert(second.receive(recovered, compiled, preview_assets));
            f.wait(
                [&]
                {
                    second.update();
                    presentation->update({320, 240});
                    return second.status().accepted == recovered;
                }
            );
            assert(!second.status().failure && pixels() == sphere_pixels && second.instance() == instance);
            assert(author->describe().current == unsaved.current && author->describe().dirty == unsaved.dirty);
            std::printf("EC2-R1 XEC2-12 public sphere/quad recipe GPU readbacks differ; same-input adoption, "
                        "late completion, failed bytes, last-success pixels and retry PASS\n");
            f.wait([&] { return presentation->close() == render::ERenderClose::COMPLETE; });
            presentation.reset();
            const auto retired = second.close();
            f.wait([&] { return retired.complete(); });
            assert(view->image().isValid() && preview.status().accepted->input == compiled->key());
        }
        auto resource_candidate = take(editor::scene::makeResourceView(
            f.messages.dispatcherRef(),
            ui::PaneId{"resources"},
            *f.runtime,
            editor::scene::ResourceViewBinding{preview.instance(), {2}}
        ));
        auto* resources = static_cast<editor::scene::ResourceView*>(resource_candidate.pane());
        const auto resource_id =
            take((*f.legacy_host).adopt(resource_candidate, views::ViewRestoreKey{"resources"})).id;
        assert(!resources->snapshot().rows.empty());
        const auto previous_instance = resources->snapshot().instance;
        const auto previous_rows = resources->snapshot().rows.size();
        assert(!resources->rebind(editor::scene::ResourceViewBinding{{}, {2}}));
        assert(
            resources->snapshot().instance == previous_instance && resources->snapshot().rows.size() == previous_rows
        );
        assert(resources->refresh());
        assert((*f.legacy_host).close(resource_id));
        f.wait([&] { return !(*f.legacy_host).describe(resource_id); });
        assert(view->image().isValid());
        const auto before_navigation = author->describe();
        lux::editor::views::CameraMotion motion;
        motion.angular_delta.x() = .1;
        assert(view->navigate(motion) && author->describe().current == before_navigation.current);
        assert(view->undo() && author->describe().current == initial.current);
        assert(view->beginEdit("pending close"));
        edits.emplace_back(em::MaterialRename{"not committed"});
        assert(view->previewEdit(edits));
        assert((*f.legacy_host).close(id));
        auto read = take(author->read());
        assert(read.withRead(
            [&](const lux::material::MaterialSource&) -> em::MaterialEditResult<void>
            {
                const auto report = take((*f.legacy_host).drain());
                assert(report.completed == 0 && report.pending == 1 && interaction.overlay());
                assert((*f.legacy_host).describe(id));
                return {};
            }
        ));
        f.wait([&] { return !(*f.legacy_host).describe(id); });
        assert(!interaction.overlay() && take(take(author->read()).encode()) == encoded);
        // Accepted save work completes after its view is gone; the service alone adopts the baseline.
        const auto saved_content = author->describe().current;
        const auto save = take(f.saves.requestSave({key.id()}));
        f.wait([&] { return take(f.saves.status(save)).stage == persistence::ESaveStage::TERMINAL; });
        assert(take(f.saves.status(save)).outcome->adoption == persistence::EAdoption::APPLIED);
        assert(author->describe().current == saved_content && !author->describe().dirty);
        assert(std::filesystem::file_size(f.files / "material.lux") > 0);
        assert(f.saves.acknowledge(save));
        const auto retiring = preview.close();
        f.wait([&] { return retiring.complete(); });
        f.material_preview = nullptr;
        return compiled;
    }
} // namespace
namespace
{
    void meshViews(Fixture& f, const editor::material::CompiledMaterial& material)
    {
        const auto recipe = quadPreviewRecipe();
        const auto mesh_id = recipe.mesh;
        auto assets = take(process::asset_loading::makeAssetReadOverlay(
            {{material.artifact()->id(), {material.bytes()}}, {mesh_id, {recipe.mesh_image}}},
            {}
        ));
        struct FailingRead final : process::asset_loading::AssetReadPort::Endpoint
        {
            process::asset_loading::AssetReadPort source;
            asset::AssetId material;
            std::atomic_bool available{};
            std::atomic_uint failures{};
            async::SubmitResult submit(
                process::asset_loading::ReadAssetImage request,
                void* owner,
                void (*complete)(void*, Outcome&&) noexcept,
                async::SubmitOptions options
            ) noexcept override
            {
                if (request.id == material && !available.load())
                {
                    ++failures;
                    complete(
                        owner,
                        cxx::unexpected(async::TOperationFailure<asset::EAssetStorageError>::domain(
                            asset::EAssetStorageError::NOT_FOUND
                        ))
                    );
                    return {};
                }
                return source.submit(request, owner, complete, options);
            }
        };
        auto failing = std::make_shared<FailingRead>();
        failing->source = std::move(assets);
        failing->material = material.artifact()->id();
        auto environment = f.environment;
        environment.version = 2;
        environment.assets = {{30, 1}, 1, process::asset_loading::AssetReadPort{failing}, {}};
        std::vector<ecs::ComponentSchema> schemas(
            environment.components.all().begin(),
            environment.components.all().end()
        );
        for (const auto& schema : ecs::visualComponentSchemas())
        {
            if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
            {
                schemas.push_back(schema);
            }
        }
        environment.components = take(ecs::ComponentSchemaSet::build(std::move(schemas)));
        environment.scene_systems.push_back(lux::scene::builtinMeshQuerySystemRegistration());
        lux::scene::RenderSystemConfiguration config;
        for (auto name :
             {"lux.render.view_camera.v1",
              "lux.render.material.v1",
              "lux.render.mesh_stack.v1",
              "lux.render.light.v1",
              "lux.render.forward_mesh.v1",
              "lux.render.shadow_map.v1",
              "lux.render.highlight.v1"})
        {
            const auto registrations = render::builtinRenderFeatureRegistrations();
            auto found = std::ranges::find(
                registrations,
                std::string_view{name},
                [](const auto& value) { return value.factory.descriptor.canonical_name; }
            );
            assert(found != registrations.end());
            std::vector<std::byte> payload;
            assert(found->configuration.portable.encode_default(payload));
            config.features.push_back(
                {found->factory.descriptor.type,
                 std::move(payload),
                 std::string(found->configuration.schema),
                 found->configuration.schema_version}
            );
        }
        lux::scene::SceneDescriptionBuilder builder;
        for (std::size_t i{}; i < environment.scene_systems.size(); ++i)
        {
            const auto& registration = environment.scene_systems[i];
            std::vector<std::byte> payload;
            if (i == 0)
            {
                lux::scene::WorldLoadingConfiguration loading{{{0}}};
                assert(registration.configuration.encode(&loading, payload));
            }
            if (i == 1)
            {
                payload = take(lux::scene::makeTransformSystemConfiguration(64, {1024, 65536}));
            }
            if (i == 2)
            {
                assert(registration.configuration.encode(&config, payload));
            }
            assert(builder.addSystem(
                {i + 1},
                std::to_string(i),
                registration.type,
                registration.description->version,
                registration.description->configuration_schema_name,
                registration.description->configuration_schema_version,
                payload
            ));
        }
        assert(builder.addDependency({2}, {4})); // Mesh query observes the updated transforms.
        assert(builder.addDependency({4}, {3})); // RenderSystem binds the query installed before it.
        std::vector<world::WorldDataSchemaId> schema_ids;
        for (const auto& schema : environment.components.all())
        {
            schema_ids.push_back(world::worldDataSchemaId(schema.id.name));
        }
        auto package = take(lux::scene::createScenePackage(
            asset::AssetId{uuid("mesh-scene")},
            "Dual mesh viewport",
            schema_ids,
            std::make_shared<const simulation::SimulationDescription>(
                take(std::move(simulation::SimulationDescriptionBuilder{}).build())
            ),
            take(std::move(builder).buildResolved())
        ));
        auto reservation =
            take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
        auto model = take(author::SceneSession::create(
            reservation.id(),
            {},
            take(author::SceneSource::create(package, environment.components))
        ));
        auto* session = model.get();
        assert(f.store.prepare(reservation, model));
        const auto key = take(f.store.key<author::SceneSession>(take(f.store.publish(reservation))));
        const world::WorldObjectId object{uuid("quad")};
        ecs::WorldEntityMap identities;
        author::SceneEditBatch batch{session->describe().current, "quad and light", {}};
        batch.edits.emplace_back(author::SceneCreateObject{
            {object,
             {0},
             {take(author::encodeSceneValue(ecs::Transform3D{}, environment.components, identities, 4096)),
              take(author::encodeSceneValue(
                  ecs::Mesh3D{rdesc::MeshVisualDescription{mesh_id, material.artifact()->id(), true, false, false}},
                  environment.components,
                  identities,
                  4096
              ))}}
        });
        rdesc::LightDescription light;
        light.type = rdesc::ELightType::DIRECTIONAL;
        light.intensity = 3;
        light.cast_shadow = false;
        batch.edits.emplace_back(author::SceneCreateObject{
            {world::WorldObjectId{uuid("lamp")},
             {0},
             {take(author::encodeSceneValue(ecs::Transform3D{}, environment.components, identities, 4096)),
              take(author::encodeSceneValue(ecs::Light3D{light}, environment.components, identities, 4096))}}
        });
        assert(session->apply(std::move(batch)));
        const auto before = session->describe();
        author::SceneInteractionGroup left(f.store.access<author::SceneSession>(), key, {81});
        author::SceneInteractionGroup right(f.store.access<author::SceneSession>(), key, {82});
        author::SceneViewServices services{
            f.store.access<author::SceneSession>(),
            f.hub,
            *f.runtime,
            *f.resources,
            *f.renderer,
            environment,
            {}
        };
        auto info = f.info("mesh-left", left);
        info.binding = author::EditedSceneBinding{key, &left};
        info.state.camera.transform.translation = {0, 0, 4};
        auto first = take(registered_views::scene(f.messages.dispatcherRef(), services, info));
        info.id = ui::PaneId{"mesh-right"};
        info.title = "Mesh right";
        info.binding = author::EditedSceneBinding{key, &right};
        auto second = take(registered_views::scene(f.messages.dispatcherRef(), services, info));
        auto* a = static_cast<author::SceneView*>(first.pane());
        auto* b = static_cast<author::SceneView*>(second.pane());
        const auto aid = take((*f.legacy_host).adopt(first, views::ViewRestoreKey{"mesh-left"})).id;
        const auto bid = take((*f.legacy_host).adopt(second, views::ViewRestoreKey{"mesh-right"})).id;
        f.wait(
            [&]
            {
                auto resources = author::captureResourceStatus(*f.runtime, a->presentedInstance(), {3});
                return resources &&
                       std::ranges::any_of(
                           resources->rows,
                           [](const auto& row) { return row.state == lux::scene::ERenderAssetState::FAILED; }
                       );
            }
        );
        auto recovery = take(author::makeResourceView(
            f.messages.dispatcherRef(),
            ui::PaneId{"resource-recovery"},
            *f.runtime,
            author::ResourceViewBinding{a->presentedInstance(), {3}}
        ));
        auto* resource_view = static_cast<author::ResourceView*>(recovery.pane());
        const auto recovery_id = take((*f.legacy_host).adopt(recovery, views::ViewRestoreKey{"resource-recovery"})).id;
        const auto failed = std::ranges::find_if(
            resource_view->snapshot().rows,
            [](const auto& row) { return row.state == lux::scene::ERenderAssetState::FAILED; }
        );
        assert(failed != resource_view->snapshot().rows.end() && failing->failures > 0);
        failing->available = true;
        assert(resource_view->retry(failed->key));
        assert((*f.legacy_host).close(recovery_id));
        f.wait([&] { return !(*f.legacy_host).describe(recovery_id); });
        f.wait(
            [&]
            {
                if (!a->image().isValid() || !b->image().isValid())
                {
                    return false;
                }
                auto resources = author::captureResourceStatus(*f.runtime, a->presentedInstance(), {3});
                if (!resources || resources->rows.empty())
                {
                    return false;
                }
                return std::ranges::all_of(
                    resources->rows,
                    [](const auto& row) { return row.state == lux::scene::ERenderAssetState::READY; }
                );
            }
        );
        assert(a->presentedInstance() == b->presentedInstance() && a->viewport() != b->viewport());
        const auto pixels = [&](author::SceneView& view)
        {
            for (int i{}; i < 8; ++i)
            {
                f.frame();
            }
            auto output = take(f.resources->outputInfo(take(f.resources->viewOutput(view.viewport()))));
            std::vector<std::byte> buffer(std::size_t(output.extent.width) * output.extent.height * 8);
            auto request =
                f.renderer->control()->get().readbackTargetAsync(output.target, buffer.data(), buffer.size(), 4);
            f.wait([&] { return request.isReady(); });
            const auto result = request.tryResult();
            assert(result && result->get().status == 0);
            assert(result->get().bytes_written > 0 && result->get().bytes_written <= buffer.size());
            buffer.resize(static_cast<std::size_t>(result->get().bytes_written));
            return buffer;
        };
        const auto left_plain = pixels(*a), right_plain = pixels(*b);
        const auto picked_left = a->pick({160, 120}, {320, 240});
        if (!picked_left)
        {
            printFailure(picked_left.error());
        }
        assert(picked_left);
        assert(left.selection().objects.size() == 1 && right.selection().objects.empty());
        const auto left_selected = pixels(*a), right_unselected = pixels(*b);
        assert(left_selected != left_plain && right_unselected == right_plain);
        assert(left.select({}));
        assert(b->pick({160, 120}, {320, 240}));
        const auto left_cleared = pixels(*a), right_selected = pixels(*b);
        assert(left_cleared == left_plain && right_selected != right_plain);
        // Deliberately do not drive the runtime/render thread between navigation and input.
        // The retained image still describes the earlier camera, not the newly patched ECS pose.
        const auto displayed = a->image();
        lux::editor::views::CameraMotion delayed_motion;
        delayed_motion.local_translation.x() = 1;
        assert(a->navigate(delayed_motion));
        assert(a->image() == displayed);
        auto premature_pick = a->pick({160, 120}, {320, 240});
        std::fprintf(
            stderr,
            "EC3 delayed camera output: retained image=%d premature pick accepted=%d\n",
            a->image() == displayed,
            bool(premature_pick)
        );
        assert(!premature_pick && left.selection().objects.empty());
        delayed_motion.local_translation.x() = -1;
        assert(a->navigate(delayed_motion));
        assert(pixels(*a) == left_plain);
        assert(a->pick({160, 120}, {320, 240}));
        assert(left.selection().objects.size() == 1);
        assert(left.select({}));
        assert(pixels(*b) == right_selected);
        assert(session->describe().current == before.current);
        const auto command_results = f.desktop->commands()->takeCompletions();
        assert(command_results.size() == 1 && command_results.front().result);
        assert(f.commands.publish({}));
        const auto receipt = take(f.resources->viewReceipt(a->viewport()));
        assert((*f.legacy_host).close(aid));
        f.wait(
            [&]
            {
                return !(*f.legacy_host).describe(aid) &&
                       receipt.status().status.state == lux::scene::EViewState::CLOSED;
            }
        );
        assert(pixels(*b) == right_selected);
        assert((*f.legacy_host).close(bid));
        f.wait([&] { return !(*f.legacy_host).describe(bid); });
        auto permit = take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        std::printf("P10 dual SceneView GPU readback: failed material read -> ResourceView retry -> ready after close; "
                    "actual mesh pick, left/right highlight isolation and retirement verified.\n");
    }
    void creationView(Fixture& f, const std::filesystem::path& installation)
    {
        // This form offers the full 3D preset; its registry must include the advertised author codecs.
        std::vector<ecs::ComponentSchema> types(
            f.environment.components.all().begin(),
            f.environment.components.all().end()
        );
        for (const auto& schema : ecs::visualComponentSchemas())
        {
            if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
            {
                types.push_back(schema);
            }
        }
        const auto metadata = take(ecs::ComponentSchemaSet::build(std::move(types)));
        lux::project::PluginCatalog catalog;
        assert(catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation));
        constexpr author::SceneProviderOption providers[]{
            {"lux.render.runtime", "main-window"},
            {"lux.render.scene_bindings", "render-bindings"},
            {"lux.render.resources", "resources"},
            {"lux.render.assets", "assets"},
            {"lux.world.loading", "world-storage"}
        };
        author::SceneConfigurationInputs inputs{
            catalog,
            metadata,
            *f.environment.simulation_systems,
            f.environment.scene_systems,
            render::builtinRenderFeatureRegistrations(),
            providers,
            {},
            lux::scene::builtinRenderFeatureSceneBindings()
        };
        services::ServiceRegistry services(f.messages.dispatcherRef());
        auto scope = take(services.createScope());
        assert(scope.provide(services::ServiceNameView{"lux.editor.sessions"}, f.store));
        assert(scope.provide(services::ServiceNameView{"lux.editor.scene.configuration"}, inputs));
        auto module = take(extensions::EditorExtension::fromStatic(author::sceneModule()));
        auto declared = take(module.contributions());
        desktop::UiRegistry windows(f.messages.dispatcherRef(), services);
        auto ui_catalog = take(desktop::UiCatalog::prepare(std::move(declared.ui)));
        assert(windows.publish(ui_catalog));
        auto& root = f.desktop->root();
        auto configuration_view = take(windows.create(
            take(ui_catalog.find(author::kSceneConfigurationView.type)), scope,
            {f.messages.dispatcherRef(), ui::PaneId{"configuration"}, {{f.key->id()}, f.key->id()}}
        ));
        auto* configuration = static_cast<author::SceneConfigurationView*>(configuration_view.get());
        assert(root.addSubPane(std::move(configuration_view)));
        const auto config_id = take(root.identify(*configuration));
        const auto original = f.session->describe();
        const auto find_page = [&](auto&& self, object::LuxObject& owner) -> lux::ui::NumericEdit*
        {
            if (auto* number = dynamic_cast<lux::ui::NumericEdit*>(&owner);
                number && number->id() == lux::ui::ElementId{"Coordinate page size"})
            {
                return number;
            }
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
            {
                if (auto* number = self(self, *child))
                {
                    return number;
                }
            }
            return nullptr;
        };
        auto* page = find_page(find_page, *configuration);
        assert(page && std::get<double>(page->value()) == 1024.0);
        page->setValue(256.0);
        assert(f.session->describe().current == original.current);
        configuration->requestApply();
        f.frame(false);
        if (!configuration->status())
        {
            std::fprintf(stderr, "%s\n", configuration->status().error().domain.c_str());
            printFailure(configuration->status().error());
        }
        assert(configuration->status());
        assert(f.session->describe().current != original.current);
        auto current_read = take(f.session->read());
        lux::scene::RenderSystemConfiguration actual;
        assert(f.environment.scene_systems[2].configuration.decode(
            current_read.configuration().scene->data().findSystem({3}).configurationPayload(),
            &actual
        ));
        assert(actual.coordinate_page_size == 256.0);
        assert(f.session->undo());
        configuration->requestRevert();
        f.frame(false);
        assert(configuration->status() && f.session->describe().current == original.current);
        page = find_page(find_page, *configuration);
        assert(page && std::get<double>(page->value()) == 1024.0);
        assert(take(f.session->read()).withRead([&](const author::SceneReadView&) -> author::SceneEditResult<void>
        {
            auto denied = windows.prepareClose(root, std::span{&config_id, 1});
            assert(!denied && denied.error().code == desktop::EUiError::BUSY);
            assert(root.findPane(config_id) && configuration->form());
            return {};
        }));
        auto config_close = take(windows.prepareClose(root, std::span{&config_id, 1}));
        assert(root.commit(config_close) && !root.findPane(config_id));
        f.frame(false);
        std::optional<sessions::SessionId> created;
        const auto before = f.session->describe();
        std::size_t requests{};
        std::optional<sessions::InstalledSession> installed;
        sessions::SessionCreation sink =
            [&](sessions::SessionPreparation input) -> commands::CommandResult<commands::DispatchReceipt>
        {
            ++requests;
            if (requests == 1)
            {
                return cxx::unexpected(commands::CommandFailure{
                    commands::ECommandError::BUSY, "scene.create.admission"
                });
            }
            auto prepared = take(std::move(input).prepare(f.store, f.saves));
            installed.emplace(take(prepared.publish()));
            created = installed->id();
            return commands::DispatchReceipt{commands::ImmediateCompletion{}};
        };
        assert(scope.provide(sessions::kSessionCreation, sink));
        auto view = take(windows.create(
            take(ui_catalog.find(author::kSceneCreationView.type)), scope,
            {f.messages.dispatcherRef(), ui::PaneId{"creation"}}
        ));
        auto* pane = static_cast<author::SceneCreationView*>(view.get());
        assert(!pane->attachedRoot());
        for (auto preset :
             {author::ESceneContentPreset::TWO_DIMENSIONAL, author::ESceneContentPreset::THREE_DIMENSIONAL})
        {
            assert(pane->configuration().applyPreset(preset));
            auto prepared = pane->configuration().build();
            if (!prepared)
            {
                std::fprintf(
                    stderr,
                    "Preset %u: %s: %s\n",
                    static_cast<unsigned>(preset),
                    prepared.error().domain.c_str(),
                    prepared.error().message.c_str()
                );
            }
            auto config = take(std::move(prepared));
            assert(config.scene.systemCount() == 3 && config.simulation->systemCount() == 0);
        }
        assert(root.addSubPane(std::move(view)));
        const auto mounted = take(root.identify(*pane));
        pane->requestCreate();
        f.frame(false);
        assert(!created && requests == 1);
        f.frame(false);
        assert(created && requests == 2);
        assert(f.session->describe().current == before.current);
        auto close = take(windows.prepareClose(root, std::span{&mounted, 1}));
        assert(root.commit(close) && !root.findPane(mounted));
        f.frame(false);
        const auto current = take(f.store.describe(*created)).current;
        assert(installed->close(current));
        installed.reset();
        assert(scope.release() && scope.drained());
        std::puts("EC4 scene configuration/creation: declared factories, BUSY retention, actual session roles and close PASS");
    }

#include "AuxiliaryViews.hpp"
#include "DraftSources.hpp"
#include "NativeDesktop.hpp"
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 3 || argc == 4);
    if (argc == 4 && std::string_view{argv[3]}.starts_with("r1-"))
    {
        Fixture fixture;
        return (std::string_view{argv[3]}.starts_with("r1-material-") ? draft_test::material(fixture, argv[3])
                                                                      : draft_test::flow(fixture, argv[3]))
                   ? 0
                   : 1;
    }
#if defined(_WIN32)
    if (argc == 4)
    {
        window::GlfwRuntime glfw;
        assert(glfw.valid());
        window::LuxWindow window(1000, 750, "Lux P10 native desktop verification");
        assert(window.isInitialized());
        Fixture native(&window);
        nativeDesktop(native, window);
        return 0;
    }
#endif
    Fixture f;
    author::SceneInteractionGroup first_group(f.store.access<author::SceneSession>(), *f.key, {1});
    author::SceneInteractionGroup second_group(f.store.access<author::SceneSession>(), *f.key, {2});
    auto first = take(registered_views::scene(f.messages.dispatcherRef(), f.services(), f.info("one", first_group)));
    auto second = take(registered_views::scene(f.messages.dispatcherRef(), f.services(), f.info("two", second_group)));
    auto* a = static_cast<author::SceneView*>(first.pane());
    auto* b = static_cast<author::SceneView*>(second.pane());
    assert(!a->attachedRoot() && !b->attachedRoot());
    const auto id_a = take((*f.legacy_host).adopt(first, views::ViewRestoreKey{"one"})).id;
    const auto id_b = take((*f.legacy_host).adopt(second, views::ViewRestoreKey{"two"})).id;
    f.wait([&] { return a->image().isValid() && b->image().isValid(); });
    assert(a->presentedInstance() == b->presentedInstance() && a->viewport() != b->viewport());
    const auto before = f.session->describe();
    const auto second_camera = b->state().camera.transform.translation;
    lux::editor::views::CameraMotion motion;
    motion.local_translation.x() = 2;
    assert(a->navigate(motion));
    assert(b->state().camera.transform.translation == second_camera);
    assert(f.session->describe().current == before.current);
    assert(a->beginEdit("move"));
    std::vector<author::VSceneEdit> edits;
    edits.push_back(author::SceneSetField::make<ecs::Transform3D>(
        {{f.key->id(), before.current.state.history, f.object},
         ecs::componentSchemaId("lux.ecs.Transform3D"),
         "translation"},
        Eigen::Vector3d{3, 0, 0}
    ));
    assert(a->previewEdit(edits));
    assert(f.session->describe().current == before.current);
    const auto camera_before_failure = a->state().camera.transform.translation;
    auto rejected = a->rebind(author::EditedSceneBinding{*f.key, nullptr});
    assert(!rejected && first_group.overlay() && a->state().camera.transform.translation == camera_before_failure);
    assert(a->commitEdit());
    const auto committed = f.session->describe().current;
    f.wait(
        [&]
        {
            return a->projectedContent() == committed && b->projectedContent() == committed && a->image().isValid() &&
                   b->image().isValid();
        }
    );
    // The actual DesktopShell menu submits a pinned view command; execution still enters the
    // same SceneView/domain undo path that this dual-viewport regression has always observed.
    using namespace editor::commands;
    auto undo = CommandEntry::create(
        lux::object::CodeLease::builtin(),
        CommandDescriptor{
            .id = CommandIdView{"p11.undo"},
            .label = "Undo",
            .group = "Edit",
            .shortcut = "Ctrl+Z",
            .scope = ECommandScope::VIEW,
            .target_type = cxx::typeToken<views::ViewId>()
        },
        [&](const CommandQuery& input) -> CommandResult<CommandState>
        {
            if (*input.view<views::ViewId>() != id_b || !(*f.legacy_host).describe(id_b))
            {
                return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "scene.view"});
            }
            return CommandState{take(f.session->historyView()).can_undo};
        },
        [&](const CommandInvocation&) -> CommandResult<DispatchReceipt>
        {
            assert(b->undo());
            return DispatchReceipt{ImmediateCompletion{}};
        }
    );
    assert(f.commands.publish(take(CommandRegistrySnapshot::create({undo}))));
    assert(f.desktop->installCommands(
        f.commands,
        f.dispatcher,
        [&](const CommandDescriptor&, const ui::Pane*, const ui::Element*) -> CommandResult<CommandInvocation>
        { return CommandInvocation::forView(id_b, lux::object::CodeLease::builtin()); }
    ));
    f.frame();
    ui::MenuRequest open_menu;
    assert(object::sendEvent(f.desktop->root(), open_menu));
    ui::MenuRequest invoke{
        ui::EMenuAction::COMMAND,
        {},
        {},
        {ui::CommandIdView{"p11.undo"}, ui::ECommandPhase::EXECUTE},
        open_menu.source,
        0
    };
    assert(object::sendEvent(f.desktop->root(), invoke));
    assert(f.dispatcher.pending() == 1);
    f.wait(
        [&]
        {
            return a->projectedContent() == before.current && b->projectedContent() == before.current &&
                   a->image().isValid() && b->image().isValid();
        }
    );
    const auto receipt = take(f.resources->viewReceipt(a->viewport()));
    assert((*f.legacy_host).close(id_a));
    f.wait([&] { return !(*f.legacy_host).describe(id_a); });
    assert(f.session->describe().current == before.current && b->image().isValid());
    f.wait([&] { return receipt.status().status.state == lux::scene::EViewState::CLOSED; });
    auto third = take(registered_views::scene(f.messages.dispatcherRef(), f.services(), f.info("three", first_group)));
    const auto id_c = take((*f.legacy_host).adopt(third, views::ViewRestoreKey{"three"})).id;
    assert(id_c != id_a && !(*f.legacy_host).describe(id_a));
    assert((*f.legacy_host).close(id_b) && (*f.legacy_host).close(id_c));
    f.wait([&] { return take((*f.legacy_host).describeAll()).empty(); });
    inspectorView(f);
    ownedSceneTools(f);
    declaredSceneTools(f);
    closeInspectorContent(f);
    creationView(f, argv[2]);
    runningView(f);
    const auto compiled = materialView(f);
    sceneComposition(f);
    materialComposition(f);
    flowView(f, argv[1]);
    flowComposition(f, argv[1]);
    auxiliaryViews(f);
    meshViews(f, *compiled);
    std::printf(
        "P10 DEV SceneView GPU: shared author edit/undo, independent cameras, failed rebind gesture, close/reopen; "
        "frames=%llu validation_errors=%llu\n",
        static_cast<unsigned long long>(f.frames),
        static_cast<unsigned long long>(f.renderer->statistics().validation_errors)
    );
}
