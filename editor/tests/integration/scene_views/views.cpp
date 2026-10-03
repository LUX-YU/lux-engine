#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/material/MaterialSaveSource.hpp>
#include <lux/engine/editor/material/MaterialCodec.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <fstream>
#include <source_location>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include "../../../../../cmake/installed-consumers/common/ControlsTestAccess.hpp"
#include <lux/engine/editor/flowforge/FlowView.hpp>
#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <imgui_internal.h>
#ifdef LUX_P10_R1_NATIVE
#include "../../../authoring/flow/src/FlowSessionData.hpp"
#include <lux/engine/editor/flowforge/PreparedFlowReload.hpp>
#include "../../../authoring/material/src/MaterialSessionData.hpp"
#endif
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <imgui.h>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/Input.hpp>
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
            return lux::cxx::unexpected(snapshot.error());
        const views::ViewFactoryInput input{
            dispatcher,
            std::move(id),
            contracts::CodeLease::builtin(),
            lux::cxx::typeToken<Value>(),
            std::make_shared<const Value>(std::move(value))
        };
        return snapshot->prepare(views::ViewTypeId{entry->descriptor().type.name()}, input);
    }
    // These fixtures deliberately borrow an explicit interaction to inspect its gesture lifetime.
    // Production content factories instead construct the complete owner from ContentViewInput.
    template <class Create>
    auto fixtureFactory(views::ViewTypeId type, Create create)
    {
        return views::ViewFactoryEntry::create(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{type.view(), "Borrowed fixture", lux::cxx::typeToken<std::monostate>()},
            [create = std::move(create)](const views::ViewFactoryInput& input) mutable
                -> views::ViewFactoryResult<views::DetachedView> {
                auto view = create(input);
                if (!view)
                    return lux::cxx::unexpected(views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "fixture"});
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
        return prepare(dispatcher, id, fixtureFactory(
            views::ViewTypeId{"lux.editor.scene.view"},
            [services, info = std::move(info)](const views::ViewFactoryInput& input) mutable {
                return scene::makeSceneView(input.dispatcher(), services, std::move(info));
            }
        ), std::monostate{});
    }
    auto material(
        lux::object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        material::MaterialViewServices services,
        std::optional<material::MaterialViewBinding> binding,
        material::MaterialViewState state
    )
    {
        return prepare(dispatcher, id, fixtureFactory(
            views::ViewTypeId{"lux.editor.material"},
            [services, binding, state](const views::ViewFactoryInput& input) {
                return material::makeMaterialView(input.dispatcher(), input.paneId(), services, binding, state);
            }
        ), std::monostate{});
    }
    auto flow(
        lux::object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        flowforge::FlowViewServices services,
        std::optional<flowforge::FlowViewBinding> binding,
        flowforge::FlowViewState state
    )
    {
        return prepare(dispatcher, id, fixtureFactory(
            views::ViewTypeId{"lux.editor.flowforge"},
            [services, binding, state](const views::ViewFactoryInput& input) {
                return flowforge::makeFlowView(input.dispatcher(), input.paneId(), services, binding, state);
            }
        ), std::monostate{});
    }

}
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
            printFailure(failure.cause);
        else if constexpr (requires { failure.code; })
            printFailure(failure.code);
        else if constexpr (std::is_enum_v<T>)
            std::fprintf(stderr, "%s=%u\n", typeid(T).name(), static_cast<unsigned>(failure));
        else if constexpr (requires { std::variant_size<T>::value; })
            std::visit([](const auto& value) { printFailure(value); }, failure);
        else
            std::fprintf(stderr, "Failure type: %s\n", typeid(T).name());
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
    void viewConfiguration(views::DetachedView& view)
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
        prepared.apply();
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
        std::unique_ptr<author::ScenePresentationHub> hub;
        sessions::SessionStore store{4};
        object::ObjectMessageQueue messages{take(object::ObjectMessageQueue::create(256))};
        editor::commands::CommandRegistry commands;
        editor::commands::CommandDispatcher dispatcher{commands};
        std::unique_ptr<desktop::DesktopShell> desktop;
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

        window::LuxWindow* window_{};
        input::Input input_;

        explicit Fixture(window::LuxWindow* native = nullptr) : window_(native)
        {
            std::filesystem::create_directories(files);
            render::RendererConfig config;
            config.validation = true;
            if (window_)
                for (auto extension : window::LuxWindow::requiredVulkanInstanceExtensions())
                    config.instance_extensions.emplace_back(extension);
            renderer = take(render::RenderRuntime::create(config, [](auto severity, auto message) {
                if (severity == 2)
                    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
            }));
            std::vector<render::RenderFeatureRegistration> features;
            for (const auto& entry : render::builtinRenderFeatureRegistrations())
                features.push_back(entry);
            features.push_back(render::kUiRenderRenderFeatureRegistration);
            assert(renderer->beginFeatureRegistration(std::move(features)));
            wait(
                [&] {
                    return renderer->featureRegistrationStatus().state !=
                           render::EFeatureRegistrationState::REGISTERING;
                },
                false
            );
            assert(renderer->featureRegistrationStatus().state == render::EFeatureRegistrationState::READY);
            assert(renderer->commitFeatureRegistration());
            resources = take(lux::scene::RenderResources::create(*renderer, tasks, execution.cpu()));
            runtime = take(lux::scene::SceneRuntime::create(execution, {0, 2048}));
            hub = std::make_unique<author::ScenePresentationHub>(*runtime, execution);
            environment.renderer = renderer.get();
            environment.resources = resources.get();
            environment.simulation_systems = std::make_shared<simulation::SimulationSystemRegistry>();
            for (const auto& binding : lux::scene::builtinRenderFeatureSceneBindings())
                environment.render_bindings.push_back(binding);
            std::vector<ecs::ComponentSchema> types;
            for (auto group : {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas()})
                for (auto schema : group)
                    if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                        types.push_back(std::move(schema));
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
            const auto add = [&](std::size_t index, std::span<const std::byte> bytes) {
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
                    continue;
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
                schemas.push_back(world::worldDataSchemaId(schema.id.name));
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
                take(store.reserve<author::SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
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
            if (material_preview)
                material_preview->update();
            if (desktop)
            {
                if (window_)
                {
                    window::LuxWindow::pollEvents();
                    input_.sample(*window_);
                    assert(desktop->feedInput(input_.snapshot()));
                }
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
                    assert(runs->update());
            }
            if (hub)
                hub->collectReleased();
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
            return {store.access<author::SceneSession>(), *hub, *runtime, *resources, *renderer, environment, {}};
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
            desktop.reset();
            hub.reset();
            wait([&] { return resources->empty(); }, false);
            assert(tasks.join());
            assert(renderer->statistics().validation_errors == 0);
            assert(renderer->beginClose());
            wait(
                [&] {
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
        const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"inspector"})).id;
        f.frame(false);
        const auto find = [&](auto&& self, object::LuxObject& object) -> lux::ui::NumericEdit* {
            if (auto* control = dynamic_cast<lux::ui::NumericEdit*>(&object))
                return control;
            for (auto* child = object.firstChild(); child; child = child->nextSibling())
                if (auto* result = self(self, *child))
                    return result;
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
        auto busy = read.withRead([&](const author::SceneReadView&) -> author::SceneEditResult<void> {
            assert(!inspector->prepareClose());
            assert(inspector->target() == target && group.overlay());
            assert(f.desktop->views().close(id));
            auto drain = take(f.desktop->views().drain());
            assert(drain.pending == 1 && f.desktop->views().describe(id));
            return {};
        });
        assert(busy);
        f.wait([&] { return !f.desktop->views().describe(id); }, false);
        assert(!group.overlay() && f.session->describe().current == before.current);
    }
    void ownedSceneTools(Fixture& f)
    {
        const auto before = f.session->describe();
        const author::SceneObjectRef target{f.key->id(), before.current.state.history, f.object};
        author::SceneViewCreateInfo input;
        input.id = ui::PaneId{"owned-primary"};
        auto detached = take(author::makeSceneView(f.messages.dispatcherRef(), f.services(), std::move(input)));
        auto* primary = static_cast<author::SceneView*>(detached.pane());
        assert(primary->rebindContent({{f.key->id()}, f.key->id()}));
        auto group = primary->interactionOwner();
        assert(group && group->select({{target}}));
        const auto primary_id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"owned-primary"})).id;
        f.wait([&] { return primary->image().isValid(); });
        author::RunStore runs(*f.runtime, f.execution);
        lux::project::PluginCatalog catalog;
        author::SceneToolInputs inputs{
            f.services(), runs, f.environment.components, author::sceneInspectorComponents(), nullptr,
            {catalog, f.environment.components, *f.environment.simulation_systems,
             f.environment.scene_systems, render::builtinRenderFeatureRegistrations(), {}}
        };
        const auto make = [&](const char* name, author::ESceneTool tool) {
            auto candidate = take(author::makeSceneToolView(
                f.messages.dispatcherRef(), ui::PaneId{name}, f.desktop->views(), primary_id, tool, inputs
            ));
            return take(f.desktop->views().adopt(candidate, views::ViewRestoreKey{name})).id;
        };
        const auto outline = make("owned-outline", author::ESceneTool::OUTLINER);
        const auto inspector = make("owned-inspector", author::ESceneTool::INSPECTOR);
        const auto resources = make("owned-resources", author::ESceneTool::RESOURCES);
        std::weak_ptr<author::SceneInteractionGroup> lifetime = group;
        group.reset();
        assert(f.desktop->views().close(primary_id));
        f.wait([&] { return !f.desktop->views().describe(primary_id); });
        assert(!lifetime.expired());
        assert(take(f.desktop->views().describe(outline)).content.primary == f.key->id());
        assert(take(f.desktop->views().describe(inspector)).content.primary == f.key->id());
        f.frame(false);
        assert(take(f.desktop->views().describe(resources)).content.sessions.empty());
        assert(lifetime.lock()->select({}));
        f.frame(false);
        const auto no_target = [&](ui::Pane& pane) {
            assert(!static_cast<author::InspectorView&>(pane).target());
        };
        assert(f.desktop->views().withView(inspector, no_target));
        // An empty selection does not detach the still-live author's content association.
        assert(take(f.desktop->views().describe(inspector)).content.primary == f.key->id());
        assert(lifetime.lock()->select({{target}}));
        auto read = take(f.session->read());
        assert(read.withRead([&](const author::SceneReadView&) -> author::SceneEditResult<void> {
            f.frame(false);
            const auto unchanged = [&](ui::Pane& pane) {
                auto& view = static_cast<author::InspectorView&>(pane);
                assert(!view.target() && !view.status());
            };
            assert(f.desktop->views().withView(inspector, unchanged));
            assert(lifetime.lock()->selection().objects == std::vector<author::VSceneSelectionTarget>{target});
            return {};
        }));
        f.frame(false);
        const auto restored = [&](ui::Pane& pane) {
            auto& view = static_cast<author::InspectorView&>(pane);
            assert(view.status() && view.target() == target);
        };
        assert(f.desktop->views().withView(inspector, restored));
        assert(f.desktop->views().close(outline));
        f.wait([&] { return !f.desktop->views().describe(outline); }, false);
        assert(!lifetime.expired());
        assert(f.desktop->views().close(inspector) && f.desktop->views().close(resources));
        f.wait([&] { return !f.desktop->views().describe(inspector) && !f.desktop->views().describe(resources); }, false);
        assert(lifetime.expired());
        assert(f.session->describe().current == before.current && f.session->describe().dirty == before.dirty);
        std::puts("EC1 complete Scene tools: owner lifetime, no-target association, BUSY retry and release");
    }
    void closeInspectorContent(Fixture& f)
    {
        auto snapshot = take(f.session->capture());
        auto package = take(author::buildSceneSnapshotPackage(snapshot));
        auto reservation =
            take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
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
        auto& host = f.desktop->views();
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
            contracts::CodeLease::builtin(),
            std::move(blocker),
            +[](ui::Pane& pane) -> views::ViewCloseResult {
                if (static_cast<BlockingPane&>(pane).refuse)
                    return cxx::unexpected(views::ViewPreparationFailure{"test.close", 7, "Not ready", true});
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
            [&](const ui::AttachmentChanged& change) noexcept {
                if (change.mounted)
                    return;
                notified = true;
                assert(take(f.store.describe(key.id())).admission == sessions::EEditAdmission::CLOSING);
                auto refused = source->apply({before.current, "reentrant close edit", {}});
                assert(!refused);
                assert(inspector->target() == target); // Still owned until the handoff completes.
            }
        ));
        const auto commit_content = [&]() noexcept {
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
            take(registered_views::scene(f.messages.dispatcherRef(), services, f.info("author-run-pair", author_group))
            );
        auto info = f.info("running", run_group);
        info.binding = author::RunningSceneBinding{run, &run_group};
        auto running = take(registered_views::scene(f.messages.dispatcherRef(), services, info));
        viewConfiguration(author_candidate);
        viewConfiguration(running);
        auto* a = static_cast<author::SceneView*>(author_candidate.pane());
        auto* b = static_cast<author::SceneView*>(running.pane());
        const auto aid = take(f.desktop->views().adopt(author_candidate, views::ViewRestoreKey{"author-run-pair"})).id;
        const auto bid = take(f.desktop->views().adopt(running, views::ViewRestoreKey{"running"})).id;
        auto command_candidate = take(author::makeRunSceneView(
            f.messages.dispatcherRef(), services, ui::PaneId{"run-commands"}, run, {3}
        ));
        const auto command_view = take(
            f.desktop->views().adopt(command_candidate, views::ViewRestoreKey{"run-commands"})
        ).id;
        // The installed tool provider binds real run/view identities without an Application owner.
        const auto available = [](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
            return commands::CommandState{true};
        };
        std::optional<author::StepTicket> command_step;
        std::optional<author::StopTicket> command_stop;
        auto run_commands = author::makeRunViewCommands(
            available, f.desktop->views(), runs,
            [&](author::RunId id) -> commands::CommandResult<void> {
                assert(id == run && !command_step);
                command_step = take(runs.step(id));
                return {};
            },
            [&](author::RunId id) -> commands::CommandResult<void> {
                assert(id == run && !command_stop);
                command_stop = take(runs.stop(id));
                return {};
            }
        );
        std::optional<author::ESceneTool> requested_tool;
        auto tool_commands = author::makeSceneToolCommands(
            available, [&](views::ViewId id, author::ESceneTool tool) -> commands::CommandResult<void> {
                assert(id == bid);
                requested_tool = tool;
                return {};
            }
        );
        auto other_tools = author::makeSceneToolCommands(
            available, [](views::ViewId, author::ESceneTool) -> commands::CommandResult<void> { return {}; }
        );
        for (std::size_t i{}; i < tool_commands.size(); ++i)
            assert(&tool_commands[i]->descriptor() == &other_tools[i]->descriptor());
        run_commands.insert(run_commands.end(), tool_commands.begin(), tool_commands.end());
        commands::CommandRegistry controls;
        assert(controls.publish(take(commands::CommandRegistrySnapshot::create(std::move(run_commands)))));
        const auto invoke = [&](const char* name, views::ViewId target) {
            return controls.execute(
                take(controls.snapshot().find(commands::CommandIdView{name})), commands::CommandInvocation{target}
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
            assert(invoke(name, bid));
            assert(requested_tool == tool);
        }
        const auto refused_author = invoke("lux.editor.scene.pause", aid);
        // This original fixture borrows an external group; tool sharing correctly refuses it.
        assert(!refused_author && refused_author.error().code == commands::ECommandError::DOMAIN_FAILURE);
        f.wait([&] { return a->image().isValid() && b->image().isValid(); });
        auto outline = take(author::makeOutlinerView(
            f.messages.dispatcherRef(),
            ui::PaneId{"outline"},
            f.store.access<author::SceneSession>(),
            author::EditedSceneBinding{*f.key, &author_group},
            {},
            f.environment.components
        ));
        auto* tree = static_cast<author::OutlinerView*>(outline.pane());
        const auto tree_id = take(f.desktop->views().adopt(outline, views::ViewRestoreKey{"outline"})).id;
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
        assert(f.desktop->views().close(tree_id));
        f.wait([&] { return !f.desktop->views().describe(tree_id); });
        assert(a->presentedInstance() != b->presentedInstance());
        assert(b->presentedInstance() == take(runs.info(run)).instance);
        const auto stamp = f.session->describe();
        const auto camera = a->state().camera.transform.translation;
        lux::editor::views::CameraMotion motion;
        motion.local_translation.x() = 1;
        const auto run_camera = b->state().camera.transform.translation;
        f.wait([&] {
            auto result = b->navigate(motion);
            if (!result)
            {
                const auto* failure = std::get_if<render::RendererFailure>(&result.error().cause);
                assert(failure && failure->code == render::ERendererError::BUSY);
                assert(b->state().camera.transform.translation == run_camera);
            }
            return result.has_value();
        });
        assert(a->state().camera.transform.translation == camera && f.session->describe().current == stamp.current);
        assert(!b->beginEdit("must not edit author") && !run_group.overlay());
        assert(invoke("lux.editor.scene.pause", command_view));
        f.wait([&] { return take(runs.info(run)).state == author::ERunState::PAUSED; });
        const auto registry = take(runs.inspect().borrow(run));
        const auto entity = registry.get().view<const simulation::ecs::Transform3D>().front();
        const auto target = take(runs.inspect().reference(run, entity));
        auto run_inspector = take(author::makeRunInspectorView(
            f.messages.dispatcherRef(),
            ui::PaneId{"run-fields"},
            runs,
            target,
            f.environment.components,
            author::runInspectorComponents()
        ));
        auto* run_fields_view = static_cast<author::RunInspectorView*>(run_inspector.pane());
        const auto inspector_id = take(f.desktop->views().adopt(run_inspector, views::ViewRestoreKey{"run-fields"})).id;
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
                                     const std::optional<editing::HistorySnapshot>&) -> author::RunResult<void> {
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
            assert(invoke("lux.editor.scene.resume", command_view));
            assert(!fields.finish() && fields.active());
            assert(fields.cancel());
            f.wait([&] { return fields.refresh().has_value(); });
            assert(!fields.writeRestriction().empty());
            assert(invoke("lux.editor.scene.pause", command_view));
            f.wait([&] { return take(runs.info(run)).state == author::ERunState::PAUSED; });
            assert(fields.refresh());
            assert(take(take(runs.debugHistory(run)).get().view()).snapshot.history != paused.history);
            assert(f.session->describe().current == stamp.current);
        }
        assert(f.desktop->views().close(inspector_id));
        f.wait([&] { return !f.desktop->views().describe(inspector_id); });
        assert(invoke("lux.editor.scene.step", command_view) && command_step);
        f.wait([&] {
            return take(runs.stepStatus(*command_step)).state == lux::scene::ESceneStepState::COMPLETED;
        });
        assert(runs.acknowledgeStep(*command_step));
        const auto clock = take(runs.info(run)).progress.time.elapsed;
        for (int i{}; i < 3; ++i)
            f.frame();
        assert(take(runs.info(run)).progress.time.elapsed == clock);
        assert(f.desktop->views().close(bid));
        f.wait([&] { return !f.desktop->views().describe(bid); });
        assert(!invoke("lux.editor.scene.resume", bid));
        assert(take(runs.info(run)).state == author::ERunState::PAUSED && a->image().isValid());
        assert(invoke("lux.editor.scene.stop", command_view) && command_stop);
        assert(f.desktop->views().close(command_view));
        f.wait([&] { return !f.desktop->views().describe(command_view); });
        const auto stopped = *command_stop;
        f.wait([&] { return stopped.complete(); });
        assert(runs.acknowledgeStop(run));
        assert(f.desktop->views().close(aid));
        f.wait([&] { return !f.desktop->views().describe(aid); });
        assert(f.session->describe().current == stamp.current);
        f.runs = nullptr;
    }

    void flowView(Fixture& f, const char* linker)
    {
        namespace ef = editor::flowforge;
        const asset::AssetId asset{uuid("flow")};
        ef::FlowAuthoringSource source{asset, "P10 Flow", {}};
        const auto event = source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("event"));
        const auto event_id = source.graph.getNode(event).node->id();
        assert(source.graph.addExport(
            {lux::flowforge::FlowForgeExportNodeId{1}, source.graph.getNode(event).node->id(), 1234}
        ));
        auto reserved =
            take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
        auto model =
            take(ef::FlowSession::create(reserved.id(), sessions::BoundSource{asset, "flow.lux"}, std::move(source)));
        auto* author = model.get();
        assert(f.store.prepare(reserved, model));
        const auto key = take(f.store.key<ef::FlowSession>(take(f.store.publish(reserved))));
        ef::FlowInteraction interaction(f.store.access<ef::FlowSession>(), key);
        ef::FlowCompilationService compilation(f.execution);
        ef::FlowViewServices services{f.store.access<ef::FlowSession>(), compilation, {}};
        auto detached = take(registered_views::flow(
            f.messages.dispatcherRef(),
            ui::PaneId{"flow"},
            services,
            ef::FlowViewBinding{key, &interaction},
            ef::FlowViewState{{"P10-deliberately-missing-linker.exe"}}
        ));
        auto* view = static_cast<ef::FlowView*>(detached.pane());
        viewConfiguration(detached);
        const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"flow"})).id;
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
        assert(f.desktop->views().close(id));
        auto read = take(author->read());
        assert(read.withRead([&]() -> ef::FlowEditResult<void> {
            const auto report = take(f.desktop->views().drain());
            assert(report.completed == 0 && report.pending == 1);
            assert(f.desktop->views().describe(id) && interaction.overlay());
            return {};
        }));
        assert(take(f.desktop->views().drain()).completed == 1);
        assert(!interaction.overlay() && !f.desktop->views().describe(id));
        assert(author->describe().current == initial.current && take(take(author->read()).encode()) == encoded);
        auto reopened = take(registered_views::flow(
            f.messages.dispatcherRef(),
            ui::PaneId{"flow"},
            services,
            ef::FlowViewBinding{key, &interaction},
            ef::FlowViewState{{"P10-deliberately-missing-linker.exe"}}
        ));
        view = static_cast<ef::FlowView*>(reopened.pane());
        const auto next = take(f.desktop->views().adopt(reopened, views::ViewRestoreKey{"flow"})).id;
        // The public view commands used by the controls exercise the complete property matrix.
        auto apply_property = [&](std::vector<ef::VFlowEdit> changes) {
            const auto before = take(take(author->read()).encode());
            const auto stamp = author->describe();
            assert(view->beginEdit("Flow property") && view->previewEdit(changes));
            assert(take(take(author->read()).encode()) == before && author->describe().current == stamp.current);
            assert(view->commitEdit());
            f.frame();
        };
        std::vector<ef::VFlowEdit> properties;
        properties.emplace_back(ef::FlowInsertNode{
            contracts::CodeLease::builtin(),
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
        const auto data_pin = [](const auto& pins) {
            for (const auto& pin : pins)
                if (pin.kind == lux::flowforge::EPinKind::DATA_IN || pin.kind == lux::flowforge::EPinKind::DATA_OUT)
                    return pin.id;
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
            assert(view->undo());
        assert(take(take(author->read()).encode()) == encoded && author->describe().current == initial.current);
        assert(view->redo() && view->undo());
        assert(view->beginEdit("rename"));
        edits.emplace_back(ef::FlowRename{"Compiled Flow"});
        assert(view->previewEdit(edits) && view->commitEdit());
        f.frame();
        const auto operation = take(view->compile());
        f.wait([&] { return take(compilation.operation(operation)).get().ready(); });
        const auto& completed = take(compilation.operation(operation)).get();
        assert(completed.object() && completed.retryable());
        const auto object = completed.object();
        assert(!view->retryLink({{}, 0}));
        assert(view->retryLink({linker, 2}));
        f.wait([&] { return completed.ready(); });
        assert(completed.result() && completed.attempts().size() == 2);
        assert(completed.object() == object); // Failed linker configuration cannot discard the compiled artifact.
        const auto published_author = author->describe();
        std::optional<persistence::WriteTicket> published_ticket;
        auto publish_connection = take(object::LuxObject::connect(
            view,
            &ef::FlowView::publishRequested,
            [&](const persistence::DerivedArtifact& result) noexcept {
                assert(result.valid() && result.info().content == published_author.current);
                assert(result.info().type() == script::ScriptArtifactAsset::asset_type);
                const auto encoded_source = take(result.encodeSource({}));
                const auto expected_source = take(lux::flowforge::encodeFlowSource(*take(completed.result())->source()));
                assert(std::ranges::equal(encoded_source.bytes.view(), std::as_bytes(std::span(expected_source))));
                published_ticket = take(persistence::publishEncodedArtifact(
                    f.writes, take(f.disk.resolve("derived.flow")), {result.bytes()}
                ));
            }
        ));
        assert(view->requestPublication() && published_ticket);
        const auto publication = *published_ticket;
        assert(view->undo() && author->describe().current == initial.current);
        assert(f.desktop->views().close(next));
        f.wait([&] { return !f.desktop->views().describe(next); });
        assert(take(compilation.operation(operation)).get().object() == object);
        f.wait([&] { return take(f.writes.status(publication)).stage == persistence::EWriteStage::TERMINAL; });
        assert(std::holds_alternative<persistence::CommitReceipt>(*take(f.writes.status(publication)).outcome));
        assert(std::filesystem::file_size(f.files / "derived.flow") == take(completed.result())->bytes().size());
        assert(author->describe().current == initial.current && author->describe().dirty == initial.dirty);
        assert(f.writes.acknowledge(publication));
        assert(compilation.acknowledge(operation));
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
            take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
        auto model = take(
            em::MaterialSession::create(reserved.id(), sessions::BoundSource{asset, "material.lux"}, std::move(source))
        );
        auto* author = model.get();
        assert(f.store.prepare(reserved, model));
        const auto key = take(f.store.key<em::MaterialSession>(take(f.store.publish(reserved))));
        em::MaterialInteraction interaction(f.store.access<em::MaterialSession>(), key);
        em::MaterialPreviewEnvironment environment{f.environment, {}};
        for (const auto& feature : render::builtinRenderFeatureRegistrations())
            environment.features.push_back(feature);
        const auto shared_environment = environment;
        em::MaterialPreview preview{*f.runtime, std::move(environment)};
        f.material_preview = &preview;
        em::MaterialCompilationService compilation(f.execution);
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
             {2}},
            em::MaterialViewBinding{key, &interaction},
            state
        ));
        auto* view = static_cast<em::MaterialView*>(detached.pane());
        viewConfiguration(detached);
        const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"material"})).id;
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
        const auto* operation = &take(compilation.operation(compile_id)).get();
        f.wait([&] { return operation->ready(); });
        const auto compiled = take(operation->result());
        const auto unsaved = author->describe();
        std::optional<persistence::WriteTicket> published_ticket;
        auto publish_connection = take(object::LuxObject::connect(
            view,
            &em::MaterialView::publishRequested,
            [&](const persistence::DerivedArtifact& result) noexcept {
                assert(result.valid() && result.info().content == unsaved.current);
                assert(result.info().type() == asset::MaterialAsset::asset_type);
                const auto encoded_source = take(result.encodeSource({}));
                const auto expected_source = take(lux::material::encodeMaterialSource(*compiled->source()));
                assert(std::ranges::equal(encoded_source.bytes.view(), std::as_bytes(std::span(expected_source))));
                published_ticket = take(persistence::publishEncodedArtifact(
                    f.writes, take(f.disk.resolve("derived.material")), {result.bytes()}
                ));
            }
        ));
        assert(view->requestPublication() && published_ticket);
        const auto publication = *published_ticket;
        f.wait([&] { return take(f.writes.status(publication)).stage == persistence::EWriteStage::TERMINAL; });
        assert(std::holds_alternative<persistence::CommitReceipt>(*take(f.writes.status(publication)).outcome));
        assert(std::filesystem::file_size(f.files / "derived.material") == compiled->bytes().size());
        assert(author->describe().current == unsaved.current && author->describe().dirty == unsaved.dirty);
        assert(f.writes.acknowledge(publication));
        auto reads = take(process::asset_loading::makeAssetReadOverlay({}, {}));
        const lux::scene::RenderAssetInput preview_assets{{9, 1}, 1, std::move(reads), {}};
        assert(preview.receive(preview.status().desired, operation->result(), preview_assets));
        f.wait([&] { return preview.status().accepted.has_value() && view->image().isValid(); });
        assert(preview.status().accepted->input == operation->key());
        assert(compilation.acknowledge(compile_id));
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
            f.wait([&] {
                second.update();
                const auto status = second.status();
                if (status.failure)
                    std::fprintf(stderr, "Second preview failed: %s\n", status.failure->domain.c_str());
                assert(!status.failure);
                return second.instance().valid();
            });
            assert(second.instance() != preview.instance());
            auto presentation = take(views::ViewportPresentation::create(
                *f.runtime, second.instance(), *f.resources, {2}, second.camera(),
                lux::scene::ViewConfig{.extent = {320, 240}}
            ));
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().accepted == adoption && presentation->image().isValid();
            });
            assert(view->image().isValid() && preview.status().accepted->input == compiled->key());
            const auto pixels = [&] {
                for (int i{}; i < 8; ++i)
                {
                    second.update();
                    presentation->update({320, 240});
                    f.frame();
                }
                const auto output = take(f.resources->outputInfo(take(f.resources->viewOutput(presentation->view()))));
                std::vector<std::byte> buffer(std::size_t(output.extent.width) * output.extent.height * 8);
                auto request = f.renderer->control()->get().readbackTargetAsync(
                    output.target, buffer.data(), buffer.size(), 4
                );
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
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().accepted == sphere_adoption;
            });
            const auto sphere_pixels = pixels();
            assert(sphere_pixels != quad_pixels && second.instance() == instance);
            // The same existing quad now replaces the sphere through the identical public entry.
            const auto again = take(second.setDesired(compiled->key(), quad));
            assert(second.receive(again, compiled, preview_assets));
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().accepted == again;
            });
            assert(pixels() == quad_pixels);
            // Replace a queued resource candidate before Runtime maintenance can consume it.
            const auto superseded = take(second.setDesired(compiled->key(), sphere));
            assert(second.receive(superseded, compiled, preview_assets));
            second.update();
            assert(second.status().prepared == superseded && second.status().accepted == again);
            const auto latest = take(second.setDesired(compiled->key(), quad));
            assert(second.receive(latest, compiled, preview_assets));
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().accepted == latest;
            });
            assert(second.receive(superseded, compiled, preview_assets));
            assert(second.status().accepted == latest && !second.status().prepared && pixels() == quad_pixels);
            // Same mesh ID, different owning bytes: a real decode failure, not a key-only change.
            auto corrupt = std::make_shared<const std::vector<std::byte>>(32, std::byte{0xff});
            const auto broken = take(second.setDesired(
                compiled->key(), em::MaterialPreviewRecipe{quad.mesh, cxx::SharedBytes<>::fromOwner(corrupt, *corrupt)}
            ));
            corrupt.reset();
            assert(broken.recipe > latest.recipe && broken.input == latest.input);
            assert(second.receive(broken, compiled, preview_assets));
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().failure.has_value();
            });
            assert(second.status().accepted == latest && second.status().stale && !second.status().prepared);
            assert(pixels() == quad_pixels); // Last successful geometry and material survived actual resource failure.
            assert(second.receive(sphere_adoption, compiled, preview_assets));
            assert(second.status().accepted == latest && !second.status().prepared);
            const auto recovered = take(second.setDesired(compiled->key(), sphere));
            assert(second.receive(recovered, compiled, preview_assets));
            f.wait([&] {
                second.update();
                presentation->update({320, 240});
                return second.status().accepted == recovered;
            });
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
            take(f.desktop->views().adopt(resource_candidate, views::ViewRestoreKey{"resources"})).id;
        assert(!resources->snapshot().rows.empty());
        const auto previous_instance = resources->snapshot().instance;
        const auto previous_rows = resources->snapshot().rows.size();
        assert(!resources->rebind(editor::scene::ResourceViewBinding{{}, {2}}));
        assert(
            resources->snapshot().instance == previous_instance && resources->snapshot().rows.size() == previous_rows
        );
        assert(resources->refresh());
        assert(f.desktop->views().close(resource_id));
        f.wait([&] { return !f.desktop->views().describe(resource_id); });
        assert(view->image().isValid());
        const auto before_navigation = author->describe();
        lux::editor::views::CameraMotion motion;
        motion.angular_delta.x() = .1;
        assert(view->navigate(motion) && author->describe().current == before_navigation.current);
        assert(view->undo() && author->describe().current == initial.current);
        assert(view->beginEdit("pending close"));
        edits.emplace_back(em::MaterialRename{"not committed"});
        assert(view->previewEdit(edits));
        assert(f.desktop->views().close(id));
        auto read = take(author->read());
        assert(read.withRead([&](const lux::material::MaterialSource&) -> em::MaterialEditResult<void> {
            const auto report = take(f.desktop->views().drain());
            assert(report.completed == 0 && report.pending == 1 && interaction.overlay());
            assert(f.desktop->views().describe(id));
            return {};
        }));
        f.wait([&] { return !f.desktop->views().describe(id); });
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
}
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
            if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                schemas.push_back(schema);
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
            auto found = std::ranges::find(registrations, std::string_view{name}, [](const auto& value) {
                return value.factory.descriptor.canonical_name;
            });
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
                payload = take(lux::scene::makeTransformSystemConfiguration(64, {1024, 65536}));
            if (i == 2)
                assert(registration.configuration.encode(&config, payload));
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
            schema_ids.push_back(world::worldDataSchemaId(schema.id.name));
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
            take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
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
            *f.hub,
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
        const auto aid = take(f.desktop->views().adopt(first, views::ViewRestoreKey{"mesh-left"})).id;
        const auto bid = take(f.desktop->views().adopt(second, views::ViewRestoreKey{"mesh-right"})).id;
        f.wait([&] {
            auto resources = author::captureResourceStatus(*f.runtime, a->presentedInstance(), {3});
            return resources && std::ranges::any_of(resources->rows, [](const auto& row) {
                       return row.state == lux::scene::ERenderAssetState::FAILED;
                   });
        });
        auto recovery = take(author::makeResourceView(
            f.messages.dispatcherRef(),
            ui::PaneId{"resource-recovery"},
            *f.runtime,
            author::ResourceViewBinding{a->presentedInstance(), {3}}
        ));
        auto* resource_view = static_cast<author::ResourceView*>(recovery.pane());
        const auto recovery_id =
            take(f.desktop->views().adopt(recovery, views::ViewRestoreKey{"resource-recovery"})).id;
        const auto failed = std::ranges::find_if(resource_view->snapshot().rows, [](const auto& row) {
            return row.state == lux::scene::ERenderAssetState::FAILED;
        });
        assert(failed != resource_view->snapshot().rows.end() && failing->failures > 0);
        failing->available = true;
        assert(resource_view->retry(failed->key));
        assert(f.desktop->views().close(recovery_id));
        f.wait([&] { return !f.desktop->views().describe(recovery_id); });
        f.wait([&] {
            if (!a->image().isValid() || !b->image().isValid())
                return false;
            auto resources = author::captureResourceStatus(*f.runtime, a->presentedInstance(), {3});
            if (!resources || resources->rows.empty())
                return false;
            return std::ranges::all_of(resources->rows, [](const auto& row) {
                return row.state == lux::scene::ERenderAssetState::READY;
            });
        });
        assert(a->presentedInstance() == b->presentedInstance() && a->viewport() != b->viewport());
        const auto pixels = [&](author::SceneView& view) {
            for (int i{}; i < 8; ++i)
                f.frame();
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
            printFailure(picked_left.error());
        assert(picked_left);
        assert(left.selection().objects.size() == 1 && right.selection().objects.empty());
        const auto left_selected = pixels(*a), right_unselected = pixels(*b);
        assert(left_selected != left_plain && right_unselected == right_plain);
        assert(left.select({}));
        assert(b->pick({160, 120}, {320, 240}));
        const auto left_cleared = pixels(*a), right_selected = pixels(*b);
        assert(left_cleared == left_plain && right_selected != right_plain);
        assert(session->describe().current == before.current);
        const auto command_results = f.desktop->commands()->takeCompletions();
        assert(command_results.size() == 1 && command_results.front().result);
        assert(f.commands.publish({}));
        const auto receipt = take(f.resources->viewReceipt(a->viewport()));
        assert(f.desktop->views().close(aid));
        f.wait([&] {
            return !f.desktop->views().describe(aid) && receipt.status().status.state == lux::scene::EViewState::CLOSED;
        });
        assert(pixels(*b) == right_selected);
        assert(f.desktop->views().close(bid));
        f.wait([&] { return !f.desktop->views().describe(bid); });
        auto permit = take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        std::printf("P10 dual SceneView GPU readback: failed material read -> ResourceView retry -> ready after close; "
                    "actual mesh pick, left/right highlight isolation and retirement verified.\n");
    }
    void creationView(Fixture& f, const std::filesystem::path& installation)
    {
        // This form offers the full 3D preset; its registry must include the advertised author codecs.
        std::vector<ecs::ComponentSchema> types(f.environment.components.all().begin(), f.environment.components.all().end());
        for (const auto& schema : ecs::visualComponentSchemas())
            if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
                types.push_back(schema);
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
            providers, {}, lux::scene::builtinRenderFeatureSceneBindings()
        };
        auto configuration_view = take(author::makeSceneConfigurationView(
            f.messages.dispatcherRef(),
            lux::ui::PaneId{"configuration"},
            f.store.access<author::SceneSession>(),
            inputs,
            *f.key
        ));
        auto* configuration = static_cast<author::SceneConfigurationView*>(configuration_view.pane());
        const auto config_id =
            take(f.desktop->views().adopt(configuration_view, views::ViewRestoreKey{"configuration"})).id;
        const auto original = f.session->describe();
        const auto find_page = [&](auto&& self, object::LuxObject& owner) -> lux::ui::NumericEdit* {
            if (auto* number = dynamic_cast<lux::ui::NumericEdit*>(&owner);
                number && number->id() == lux::ui::ElementId{"Coordinate page size"})
                return number;
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                if (auto* number = self(self, *child))
                    return number;
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
        assert(f.desktop->views().close(config_id));
        f.wait([&] { return !f.desktop->views().describe(config_id); }, false);
        std::optional<sessions::SessionId> created;
        const auto before = f.session->describe();
        std::size_t requests{};
        author::SceneCreationRequests sink{
            [&](const author::SceneCreationConfiguration& config) -> author::SceneConfigurationResult<void> {
                ++requests;
                if (requests == 1)
                    return cxx::unexpected(author::SceneConfigurationFailure{
                        author::ESceneConfigurationError::BUSY,
                        "scene.create.admission"
                    });
                auto package = take(lux::scene::createScenePackage(
                    asset::AssetId{uuid("created-by-form")},
                    config.name,
                    config.schemas,
                    config.simulation,
                    config.scene
                ));
                auto reservation =
                    take(f.store.reserve<author::SceneSession>({"lux.editor.scene"}, contracts::CodeLease::builtin()));
                auto model = take(author::SceneSession::create(
                    reservation.id(),
                    {},
                    take(author::SceneSource::create(package, metadata))
                ));
                assert(f.store.prepare(reservation, model));
                created = take(f.store.publish(reservation));
                return {};
            }
        };
        auto view =
            take(author::makeSceneCreationView(f.messages.dispatcherRef(), lux::ui::PaneId{"creation"}, inputs, sink));
        auto* pane = static_cast<author::SceneCreationView*>(view.pane());
        assert(!pane->attachedRoot());
        for (auto preset :
             {author::ESceneContentPreset::TWO_DIMENSIONAL, author::ESceneContentPreset::THREE_DIMENSIONAL})
        {
            assert(pane->configuration().applyPreset(preset));
            auto prepared = pane->configuration().build();
            if (!prepared)
                std::fprintf(stderr, "Preset %u: %s: %s\n", static_cast<unsigned>(preset),
                    prepared.error().domain.c_str(), prepared.error().message.c_str());
            auto config = take(std::move(prepared));
            assert(config.scene.systemCount() == 3 && config.simulation->systemCount() == 0);
        }
        const auto mounted = take(f.desktop->views().adopt(view, views::ViewRestoreKey{"creation"})).id;
        pane->requestCreate();
        f.frame(false);
        assert(!created && requests == 1);
        f.frame(false);
        assert(created && requests == 2);
        assert(f.session->describe().current == before.current);
        assert(f.desktop->views().close(mounted));
        f.wait([&] { return !f.desktop->views().describe(mounted); });
        const auto current = take(f.store.describe(*created)).current;
        auto permit = take(f.store.prepareClose(current));
        assert(f.store.close(permit));
    }

#include "NativeDesktop.hpp"
#include "AuxiliaryViews.hpp"
#include "DraftSources.hpp"
}
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
    const auto id_a = take(f.desktop->views().adopt(first, views::ViewRestoreKey{"one"})).id;
    const auto id_b = take(f.desktop->views().adopt(second, views::ViewRestoreKey{"two"})).id;
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
    f.wait([&] {
        return a->projectedContent() == committed && b->projectedContent() == committed && a->image().isValid() &&
               b->image().isValid();
    });
    // The actual DesktopShell menu submits a pinned view command; execution still enters the
    // same SceneView/domain undo path that this dual-viewport regression has always observed.
    using namespace editor::commands;
    auto undo = CommandEntry::create(
        contracts::CodeLease::builtin(),
        CommandDescriptor{CommandIdView{"p11.undo"}, "Undo", "Edit", "Ctrl+Z", ECommandScope::VIEW},
        [&](const CommandQuery& input) -> CommandResult<CommandState> {
            if (std::get<views::ViewId>(input.target) != id_b || !f.desktop->views().describe(id_b))
                return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "scene.view"});
            return CommandState{take(f.session->historyView()).can_undo};
        },
        [&](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
            assert(b->undo());
            return DispatchReceipt{ImmediateCompletion{}};
        }
    );
    assert(f.commands.publish(take(CommandRegistrySnapshot::create({undo}))));
    assert(f.desktop->installCommands(
        f.commands,
        f.dispatcher,
        [&](const CommandDescriptor&, const ui::Pane*, const ui::Element*) -> CommandResult<CommandInvocation> {
            return CommandInvocation{id_b};
        }
    ));
    f.frame();
    ui::MenuRequest open_menu;
    assert(object::sendEvent(f.desktop->root(), open_menu));
    ui::MenuRequest
        invoke{ui::EMenuAction::COMMAND, {}, {}, {ui::CommandIdView{"p11.undo"}, ui::ECommandPhase::EXECUTE}};
    assert(object::sendEvent(f.desktop->root(), invoke));
    assert(f.dispatcher.pending() == 1);
    f.wait([&] {
        return a->projectedContent() == before.current && b->projectedContent() == before.current &&
               a->image().isValid() && b->image().isValid();
    });
    const auto receipt = take(f.resources->viewReceipt(a->viewport()));
    assert(f.desktop->views().close(id_a));
    f.wait([&] { return !f.desktop->views().describe(id_a); });
    assert(f.session->describe().current == before.current && b->image().isValid());
    f.wait([&] { return receipt.status().status.state == lux::scene::EViewState::CLOSED; });
    auto third = take(registered_views::scene(f.messages.dispatcherRef(), f.services(), f.info("three", first_group)));
    const auto id_c = take(f.desktop->views().adopt(third, views::ViewRestoreKey{"three"})).id;
    assert(id_c != id_a && !f.desktop->views().describe(id_a));
    assert(f.desktop->views().close(id_b) && f.desktop->views().close(id_c));
    f.wait([&] { return take(f.desktop->views().describeAll()).empty(); });
    inspectorView(f);
    ownedSceneTools(f);
    closeInspectorContent(f);
    creationView(f, argv[2]);
    runningView(f);
    const auto compiled = materialView(f);
    flowView(f, argv[1]);
    auxiliaryViews(f);
    meshViews(f, *compiled);
    std::printf(
        "P10 DEV SceneView GPU: shared author edit/undo, independent cameras, failed rebind gesture, close/reopen; "
        "frames=%llu validation_errors=%llu\n",
        static_cast<unsigned long long>(f.frames),
        static_cast<unsigned long long>(f.renderer->statistics().validation_errors)
    );
}
