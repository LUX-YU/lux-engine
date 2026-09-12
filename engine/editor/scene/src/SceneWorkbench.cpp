#include <lux/engine/editor/scene/SceneWorkbench.hpp>
#include <lux/engine/editor/scene/SceneWorkbenchMeasurement.hpp>
#include <lux/engine/editor/scene/SceneCamera.hpp>
#include <lux/engine/editor/scene/SceneResources.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>

#include <lux/engine/editor/inspector/EntityInspector.hpp>
#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <memory>
#include <utility>
#include <vector>
#include <atomic>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>
#if LUX_SV1_DIAGNOSTICS
#include <lux/engine/editor/scene/SceneWorkbenchDiagnostics.hpp>
#include <fstream>
#endif

namespace lux::editor::workbench
{
    namespace
    {
        std::atomic<std::uint64_t> next_texture{1};

        lux::ui::TextureHandle issueTexture() noexcept
        {
            auto value = next_texture.load(std::memory_order_relaxed);
            while (value != std::numeric_limits<std::uint64_t>::max())
                if (next_texture.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                    return lux::ui::TextureHandle{value};
            return {};
        }
        template<class Type>
        [[nodiscard]] Type uuidId(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16U> bytes{};
            bytes.back() = tail;
            return Type{uuids::uuid(bytes)};
        }

        [[nodiscard]] asset::AssetId assetId(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16U> bytes{};
            bytes.back() = tail;
            return asset::AssetId{bytes};
        }

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<scene::Scene>, EWorkbenchError>
        buildDevelopmentScene(const scene::SceneMetaManager& meta, scene::RenderRuntime& runtime)
        {
            world::WorldDescriptionBuilder world_builder;
            if (!world_builder.setIdentity(
                    uuidId<world::WorldBundleId>(1U),
                    uuidId<world::WorldBundleGeneration>(1U),
                    "lux-editor-development-scene"
                ) ||
                !world_builder.setPartitioner({world::worldPartitionerId("lux.editor.none"), 1U}, 0U))
            {
                return lux::cxx::unexpected(EWorkbenchError::WORLD_BUILD_FAILURE);
            }
            auto world = std::move(world_builder).build();
            if (!world)
                return lux::cxx::unexpected(EWorkbenchError::WORLD_BUILD_FAILURE);

            simulation::SimulationDescriptionBuilder simulation_builder;
            auto transform_config = simulation::makeTransformSystemConfiguration(1024, {2048, 1024 * 1024});
            if (!transform_config || !simulation_builder.addSystem(
                system::SystemInstanceId{1}, "transform", simulation::transformSystemDescription(),
                *transform_config))
                return lux::cxx::unexpected(EWorkbenchError::SIMULATION_BUILD_FAILURE);
            auto simulation = std::move(simulation_builder).build();
            if (!simulation)
                return lux::cxx::unexpected(EWorkbenchError::SIMULATION_BUILD_FAILURE);

            scene::SceneDescriptionBuilder scene_builder;
            scene_builder.setWorld(assetId(1U));
            scene_builder.setSimulation(assetId(2U));
            scene::RenderSystemConfiguration render_config;
            for (const auto name : {"lux.render.view_camera.v1", "lux.render.material.v1",
                "lux.render.mesh_stack.v1", "lux.render.light.v1", "lux.render.forward_mesh.v1",
                "lux.render.shadow_map.v1"})
            {
                const auto* feature = meta.getRenderFeatureMeta(render::featureId(name));
                if (!feature)
                    return lux::cxx::unexpected(EWorkbenchError::META_BUILD_FAILURE);
                scene::RenderFeatureInstanceDescription selected;
                selected.type = feature->type;
                selected.configuration.assign(feature->default_configuration.begin(),
                    feature->default_configuration.end());
                render_config.features.push_back(std::move(selected));
            }
            const auto render_registration = scene::builtinRenderSystemRegistration();
            std::vector<std::byte> render_bytes;
            if (!render_registration.configuration.encode(&render_config, render_bytes))
                return lux::cxx::unexpected(EWorkbenchError::SCENE_DESCRIPTION_BUILD_FAILURE);
            const auto render_id = system::SystemInstanceId{2};
            const auto& type = scene::RenderSystem::Description;
            if (!scene_builder.addSystem(render_id, "render", render_registration.type, type.version,
                type.configuration_schema_name, type.configuration_schema_version, render_bytes) ||
                !scene_builder.bindRequirement(render_id, "render_runtime", "main-window"))
                return lux::cxx::unexpected(EWorkbenchError::SCENE_DESCRIPTION_BUILD_FAILURE);
            auto description = std::move(scene_builder).build();
            if (!description)
                return lux::cxx::unexpected(EWorkbenchError::SCENE_DESCRIPTION_BUILD_FAILURE);

            const std::array providers{scene::makeSceneCapabilityProvider<scene::RenderRuntime>(
                "main-window", "lux.render.runtime", runtime)};
            auto result = scene::Scene::create({
                std::make_shared<scene::SceneDescription>(std::move(*description)),
                std::make_shared<world::WorldDescription>(std::move(*world)),
                std::make_shared<simulation::SimulationDescription>(std::move(*simulation)),
                meta,
                providers
            });
            if (!result)
            {
                std::fprintf(stderr, "Scene build failed: %u / %u / %llu\n",
                    unsigned(result.error().code), unsigned(result.error().scene_system.code),
                    static_cast<unsigned long long>(result.error().scene_system.subject_hash));
                return lux::cxx::unexpected(EWorkbenchError::SCENE_BUILD_FAILURE);
            }
            if (!(*result)->simulation().seal())
                return lux::cxx::unexpected(EWorkbenchError::SIMULATION_BUILD_FAILURE);
            return std::move(*result);
        }
    } // namespace

    class WorkbenchPane final : public object::Object<WorkbenchPane, lux::ui::Pane>
    {
    public:
      WorkbenchPane(lux::ui::UISession &ui, std::string id, std::string title,
                    std::function<void(lux::ui::Frame &)> draw)
          : Object(ui.dispatcherRef(), lux::ui::PaneId{id}, lux::ui::PaneTypeId{id}, std::move(title)),
            draw_(std::move(draw))
      {
      }

    private:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext&) override { draw_(frame); }
        std::function<void(lux::ui::Frame&)> draw_;
    };

    struct SceneWorkbench::Impl final
    {
        ~Impl() noexcept
        {
            pane_registration.reset();
            registrations.clear();
            panes.clear();
            inspector.reset();
            if (context && scene_handle.valid())
                static_cast<void>(context->selection().deactivate(scene_handle));
            target = {};
            view = {};
            scene.reset();
            if (runtime)
                static_cast<void>(runtime.control().flushDeferredReleases());
        }

        EditorContext* context{};
        SceneViewRenderPort* port{};
        scene::RenderRuntimeLease runtime;
        std::unique_ptr<scene::Scene> scene;
        EditorSceneHandle scene_handle{};
        std::unique_ptr<inspector::EntityInspector> inspector;
        lux::ui::PaneRegistration pane_registration;
        std::vector<std::unique_ptr<WorkbenchPane>> panes;
        std::vector<lux::ui::PaneRegistration> registrations;
        std::optional<task::TaskExecutor> executor;
        SceneCamera camera;
        lux::ui::ViewportElement viewport;
        lux::ui::ViewportResult viewport_result;
        render::RenderRequest<render::ViewCreatedReply> pending_view;
        render::RenderRequest<render::TargetReadyReply> pending_target;
        render::RenderRequest<render::TargetResizedReply> pending_resize;
        render::RenderRequest<render::TargetReleasedReply> target_close;
        render::RenderViewLease view;
        render::RenderTargetLease target;
        render::RenderSceneId render_scene;
        std::shared_ptr<const void> cpu_lease{std::make_shared<int>(0)};
        std::shared_ptr<bool> view_closed{std::make_shared<bool>(false)};
        lux::ui::TextureHandle texture{issueTexture()};
        math::Extent2u extent{1024, 640};
        bool linked{}, view_created{}, suspended{}, closing{}, closed{};
        std::string status{"Creating scene view"};
        std::string filter;
        std::vector<simulation::ecs::Entity> entities;
        std::vector<std::unique_ptr<detail::ResourceJob>> resources;
        std::array<std::uint64_t, 3> resource_serials{1, 1, 1};
        bool retry_requested{};
#if LUX_SV1_DIAGNOSTICS
        std::filesystem::path diagnostic_directory;
        render::RenderRequest<render::ReadbackTargetReply> diagnostic_readback;
        std::vector<std::uint8_t> diagnostic_pixels;
        std::array<std::uint64_t, 5> diagnostic_checksums{};
        unsigned diagnostic_phase{};
        std::uint64_t diagnostic_next_frame{30};
        bool diagnostic_passed{};
        std::array<char, 262144> diagnostic_timing_text{};
        render::RenderRequest<render::GpuTimingReply> diagnostic_timing;

        void verifyScene()
        {
            if (diagnostic_directory.empty() || closing)
                return;
            if (diagnostic_timing.valid())
            {
                if (!diagnostic_timing.isReady())
                    return;
                const auto result = diagnostic_timing.tryResult();
                if (result && result->get().status == 0 && result->get().written == result->get().needed &&
                    result->get().written <= diagnostic_timing_text.size())
                {
                    std::ofstream output{diagnostic_directory /
                        (diagnostic_phase == 2 ? "gpu-scene-timing.txt" : "gpu-timing.txt")};
                    output.write(diagnostic_timing_text.data(), result->get().written);
                }
                diagnostic_timing = {};
                if (diagnostic_phase == 2)
                    return;
                closing = true;
                camera.releaseCapture();
                port->setViewFrame({});
                port->deferNewFrames(false);
                return;
            }
            if (diagnostic_readback.valid())
            {
                if (!diagnostic_readback.isReady())
                    return;
                const auto result = diagnostic_readback.tryResult();
                if (!result || result->get().status != 0 || result->get().bytes_per_pixel != 4)
                {
                    closing = true;
                    return;
                }
                const auto& reply = result->get();
                if (reply.width == 0 || reply.height == 0 || reply.bytes_written > diagnostic_pixels.size() ||
                    reply.bytes_written != std::uint64_t(reply.width) * reply.height * 4)
                {
                    closing = true;
                    return;
                }
                std::ofstream image{diagnostic_directory / ("scene-" + std::to_string(diagnostic_phase) + ".ppm"),
                    std::ios::binary};
                image << "P6\n" << reply.width << " " << reply.height << "\n255\n";
                std::uint64_t checksum = 1469598103934665603ULL;
                for (std::size_t offset = 0; offset < reply.bytes_written; offset += 4)
                {
                    // The actual offscreen target is BGRA8_SRGB; retain its encoded RGB bytes.
                    const char rgb[]{char(diagnostic_pixels[offset + 2]), char(diagnostic_pixels[offset + 1]),
                        char(diagnostic_pixels[offset])};
                    image.write(rgb, 3);
                    for (const auto channel : rgb)
                        checksum = (checksum ^ static_cast<unsigned char>(channel)) * 1099511628211ULL;
                }
                diagnostic_checksums[diagnostic_phase] = checksum;
                std::fprintf(stderr, "SV1 capture phase=%u extent=%ux%u checksum=%llu bytes=%llu\n",
                    diagnostic_phase, reply.width, reply.height, static_cast<unsigned long long>(checksum),
                    static_cast<unsigned long long>(reply.bytes_written));
                diagnostic_readback = {};
                if (!image)
                {
                    closing = true;
                    return;
                }
                if (diagnostic_phase == 0)
                {
                    // Deliberate diagnostic mutation at the owner safe point, outside production Pane callbacks.
                    const auto entity = resources[1]->entity;
                    scene->registry().patch<simulation::ecs::Transform3D>(entity, [](auto& transform) {
                        transform.translation.x() -= 2.0;
                    });
                    for (const auto light : scene->registry().view<simulation::ecs::Light3D>())
                        scene->registry().patch<simulation::ecs::Light3D>(light, [](auto& value) {
                            value.value.intensity *= 0.5F;
                        });
                }
                else if (diagnostic_phase == 1)
                    camera.focus({0, 1, 0}, 2.0);
                else if (diagnostic_phase == 2)
                {
                    for (const auto& job : resources)
                        scene->registry().remove<simulation::ecs::Mesh3D>(job->entity);
                }
                else if (diagnostic_phase == 3)
                    scene->registry().clear();
                ++diagnostic_phase;
                diagnostic_next_frame = port->diagnostics().frames + 40;
                if (diagnostic_phase == 2)
                    diagnostic_timing = runtime.control().queryGpuTiming(render_scene,
                        diagnostic_timing_text.data(), diagnostic_timing_text.size());
                if (diagnostic_phase == 5)
                {
                    diagnostic_passed = diagnostic_checksums[0] != diagnostic_checksums[1] &&
                        diagnostic_checksums[1] != diagnostic_checksums[2] &&
                        diagnostic_checksums[2] != diagnostic_checksums[3] &&
                        diagnostic_checksums[3] == diagnostic_checksums[4];
                    diagnostic_timing = runtime.control().queryGpuTiming(render_scene,
                        diagnostic_timing_text.data(), diagnostic_timing_text.size());
                    if (!diagnostic_timing.valid())
                    {
                        diagnostic_passed = false;
                        closing = true;
                    }
                }
                return;
            }
            const bool ready = std::all_of(resources.begin(), resources.end(), [](const auto& job) {
                return job->adopted;
            });
            if (!ready || !linked || pending_resize.valid() || port->diagnostics().frames < diagnostic_next_frame)
                return;
            diagnostic_pixels.resize(std::size_t(extent.width) * extent.height * 4);
            diagnostic_readback = runtime.control().readbackTargetAsync(target.id(), diagnostic_pixels.data(),
                diagnostic_pixels.size());
        }
#endif

        void layout()
        {
            context->ui().setSplitLayout({"lux.scene.outline", "lux.scene.viewport",
                "lux-editor.entity-inspector", "lux.scene.resources", 260, 350, 200, "lux.scene.toolbar"});
        }

        void outline(lux::ui::Frame& frame)
        {
            static_cast<void>(frame.inputText("Filter", filter));
            const auto& registry = std::as_const(scene->registry());
            std::vector<simulation::ecs::Entity> visited;
            visited.reserve(entities.size());
            const auto row = [&](auto&& self, simulation::ecs::Entity entity, std::size_t depth) -> void {
                if (!registry.valid(entity) || depth > entities.size() ||
                    std::find(visited.begin(), visited.end(), entity) != visited.end())
                    return;
                visited.push_back(entity);
                constexpr std::array names{"Scene", "Orange cube", "Blue block", "Ground", "Key light"};
                const auto index = static_cast<std::size_t>(entt::to_integral(entity));
                const auto label = (index < names.size() ? std::string{names[index]} : "Entity") +
                    " #" + std::to_string(index);
                if (!filter.empty() && label.find(filter) == std::string::npos)
                    return;
                bool leaf = true;
                for (const auto child : entities)
                    if (const auto* parent = registry.try_get<simulation::ecs::Parent>(child);
                        parent && parent->entity == entity)
                        leaf = false;
                auto group = frame.treeRow({lux::ui::WidgetIdView{label}, label,
                    context->selection().current().entity == entity, leaf, true});
                if (group.activated())
                    static_cast<void>(context->selection().select(scene_handle, entity));
                if (group.open())
                    for (const auto child : entities)
                        if (const auto* parent = registry.try_get<simulation::ecs::Parent>(child);
                            parent && parent->entity == entity)
                            self(self, child, depth + 1);
            };
            for (const auto entity : entities)
            {
                const auto* parent = registry.try_get<simulation::ecs::Parent>(entity);
                if (!filter.empty() || !parent || !registry.valid(parent->entity))
                    row(row, entity, 0);
            }
            // Only disconnected cycles need fallback roots. Collapsed descendants stay hidden.
            for (const auto entity : entities)
                if (std::find(visited.begin(), visited.end(), entity) == visited.end())
                {
                    auto ancestor = entity;
                    std::size_t hops{};
                    while (const auto* parent = registry.try_get<simulation::ecs::Parent>(ancestor))
                    {
                        if (!registry.valid(parent->entity))
                            break;
                        ancestor = parent->entity;
                        if (++hops > entities.size())
                        {
                            row(row, entity, 0);
                            break;
                        }
                    }
                }
        }

        bool addPane(std::string id, std::string title, std::function<void(lux::ui::Frame&)> draw)
        {
            auto pane = std::make_unique<WorkbenchPane>(
                context->ui(), std::move(id), std::move(title), std::move(draw));
            auto registration = context->ui().registerPane(*pane);
            if (!registration)
                return false;
            panes.push_back(std::move(pane));
            registrations.push_back(std::move(*registration));
            return true;
        }

        void acceptView()
        {
            if (pending_view.valid() && pending_view.isReady())
            {
                const auto result = pending_view.tryResult();
                if (result && result->get().error.ok() && result->get().view.isValid())
                {
                    const auto flag = view_closed;
                    view = runtime.control().adoptView(render_scene, result->get().view,
                        [flag](const render::GenericOkReply&) { *flag = true; });
                    view_created = true;
                }
                else
                    status = "View creation failed";
                pending_view = {};
            }
            if (pending_target.valid() && pending_target.isReady())
            {
                const auto result = pending_target.tryResult();
                if (result && result->get().status == 0 && result->get().target.isValid())
                    target = runtime.control().adoptTarget(result->get().target);
                else
                    status = "Offscreen target creation failed";
                pending_target = {};
            }
            if (!closing && view && target && !linked)
            {
                runtime.control().setLayer(target.id(), 0, render_scene, view.id());
                linked = true;
                status = "Scene view ready; waiting for assets";
            }
            if (pending_resize.valid() && pending_resize.isReady())
            {
                const auto result = pending_resize.tryResult();
                if (result && result->get().target == target.id() && result->get().status == 0)
                    extent = result->get().extent;
                else
                    status = "Resize failed; previous target retained";
                pending_resize = {};
            }
        }
    };

    lux::cxx::expected<scene::SceneMetaManager, EWorkbenchError> buildDevelopmentSceneMeta() noexcept
    {
        try
        {
            scene::initializeBuiltinRenderSystemMeta();
            render::initializeBuiltinRenderFeatureMeta();
            std::vector<simulation::ecs::ComponentSchema> schemas;
            const auto append = [&schemas](std::span<const simulation::ecs::ComponentSchema> values) {
                schemas.insert(schemas.end(), values.begin(), values.end());
            };
            append(simulation::ecs::transformComponentSchemas());
            append(simulation::ecs::hierarchyComponentSchemas());
            append(simulation::ecs::visualComponentSchemas());
            append(scene::sceneRenderComponentSchemas());
            auto schema_set = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
            if (!schema_set)
                return lux::cxx::unexpected(EWorkbenchError::SCHEMA_BUILD_FAILURE);

            simulation::SimulationSystemRegistry simulation_systems;
            if (!simulation_systems.add(simulation::transformSystemRegistrations()))
                return lux::cxx::unexpected(EWorkbenchError::META_BUILD_FAILURE);
            const auto render_systems = scene::builtinRenderSystemRegistrations();
            const auto features = render::builtinRenderFeatureRegistrations();
            const auto bindings = scene::builtinRenderFeatureSceneBindings();
            auto meta = scene::SceneMetaManager::build({
                std::move(*schema_set),
                std::move(simulation_systems),
                {render_systems.begin(), render_systems.end()},
                {features.begin(), features.end()},
                {bindings.begin(), bindings.end()}
            });
            if (!meta)
            {
                std::fprintf(stderr, "Scene meta failed: %u / %llu\n", unsigned(meta.error().code),
                    static_cast<unsigned long long>(meta.error().subject_hash));
                return lux::cxx::unexpected(EWorkbenchError::META_BUILD_FAILURE);
            }
            return std::move(*meta);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EWorkbenchError::ALLOCATION_FAILURE);
        }
    }

    SceneWorkbench::SceneWorkbench(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    SceneWorkbench::CreateResult SceneWorkbench::create(EditorContext& context, SceneViewRenderPort& render) noexcept
    {
        try
        {
            auto scene_result = buildDevelopmentScene(context.sceneMeta(), render.runtime());
            if (!scene_result)
                return lux::cxx::unexpected(scene_result.error());

            auto bindings = inspector::buildFirstPartyComponentEditorBindings();
            if (!bindings)
                return lux::cxx::unexpected(EWorkbenchError::BINDING_BUILD_FAILURE);

            auto impl = std::make_unique<Impl>();
            if (!impl->texture.valid())
                return lux::cxx::unexpected(EWorkbenchError::ALLOCATION_FAILURE);
            impl->context = &context;
            impl->port = &render;
            impl->scene = std::move(*scene_result);
            auto lease = render.runtime().acquire();
            auto executor = task::TaskExecutor::create({0, 32});
            if (!lease || !executor)
                return lux::cxx::unexpected(EWorkbenchError::SCENE_BUILD_FAILURE);
            impl->runtime = std::move(*lease);
            impl->executor.emplace(std::move(*executor));
            impl->render_scene = impl->scene->findSceneSystem<scene::RenderSystem>()->renderSceneId();

            auto& registry = impl->scene->registry();
            const auto parent = registry.create();
            registry.emplace<simulation::ecs::Transform3D>(parent);
            registry.emplace<simulation::ecs::WorldTransform3D>(parent);

            const auto selected = registry.create();
            auto& transform = registry.emplace<simulation::ecs::Transform3D>(selected);
            transform.translation = Eigen::Vector3d{0.0, 1.0, 0.0};
            registry.emplace<simulation::ecs::WorldTransform3D>(selected);
            auto& visual = registry.emplace<simulation::ecs::Mesh3D>(selected);
            visual.value.mesh = detail::seedAssetId(10);
            visual.value.material = detail::seedAssetId(20);
            if (!simulation::ecs::reparent(registry, selected, parent))
                return lux::cxx::unexpected(EWorkbenchError::SELECTION_FAILURE);

            impl->inspector = std::make_unique<inspector::EntityInspector>(
                context.ui().dispatcherRef(),
                lux::ui::PaneId{"lux-editor.entity-inspector"},
                context,
                std::move(*bindings),
                inspector::EInspectorMode::READ_ONLY
            );
            auto registration = context.ui().registerPane(*impl->inspector);
            if (!registration)
                return lux::cxx::unexpected(EWorkbenchError::PANE_REGISTRATION_FAILURE);
            impl->pane_registration = std::move(*registration);

            impl->scene_handle = context.selection().activate(*impl->scene);
            if (!impl->scene_handle.valid() || !context.selection().select(impl->scene_handle, selected))
                return lux::cxx::unexpected(EWorkbenchError::SELECTION_FAILURE);
            impl->resources.reserve(3);
            impl->resources.push_back(std::make_unique<detail::ResourceJob>(context, impl->scene_handle, selected,
                visual.value.mesh, visual.value.material));
            for (int index = 0; index < 2; ++index)
            {
                const auto entity = registry.create();
                simulation::ecs::Transform3D transform;
                transform.translation = index == 0 ? Eigen::Vector3d{-2.0, 0.75, -1.0} : Eigen::Vector3d::Zero();
                transform.scale = index == 0 ? Eigen::Vector3d{1.5, 1.5, 0.7} : Eigen::Vector3d::Ones();
                registry.emplace<simulation::ecs::Transform3D>(entity, transform);
                registry.emplace<simulation::ecs::WorldTransform3D>(entity);
                simulation::ecs::Mesh3D mesh;
                mesh.value.mesh = detail::seedAssetId(index == 0 ? 10 : 11);
                mesh.value.material = detail::seedAssetId(index == 0 ? 21 : 22);
                registry.emplace<simulation::ecs::Mesh3D>(entity, mesh);
                if (!simulation::ecs::reparent(registry, entity, parent))
                    return lux::cxx::unexpected(EWorkbenchError::SCENE_BUILD_FAILURE);
                impl->resources.push_back(std::make_unique<detail::ResourceJob>(context, impl->scene_handle, entity,
                    mesh.value.mesh, mesh.value.material));
            }
            const auto light_entity = registry.create();
            simulation::ecs::Transform3D light_transform;
            light_transform.translation = {2.0, 5.0, 3.0};
            registry.emplace<simulation::ecs::Transform3D>(light_entity, light_transform);
            registry.emplace<simulation::ecs::WorldTransform3D>(light_entity);
            simulation::ecs::Light3D light;
            light.value.type = rdesc::ELightType::POINT;
            light.value.intensity = 3.0F;
            light.value.range = 30.0F;
            registry.emplace<simulation::ecs::Light3D>(light_entity, light);
            auto* state = impl.get();
            impl->addPane("lux.scene.toolbar", "Workbench", [state](lux::ui::Frame& frame) {
                auto table = frame.table({lux::ui::WidgetIdView{"workbench-toolbar"}, 3});
                table.nextColumn();
                frame.text("LUX / Scene Workbench");
                table.nextColumn();
                frame.textMuted("SV-1  |  Read-only scene");
                table.nextColumn();
                if (frame.smallButton("Restore panels / layout"))
                {
                    for (auto& pane : state->panes)
                        pane->setVisible(true);
                    state->inspector->setVisible(true);
                    state->layout();
                }
            });
            impl->addPane("lux.scene.viewport", "Scene View", [state](lux::ui::Frame& frame) {
                frame.textMuted("RMB + WASD/QE: fly  |  MMB: pan  |  Wheel: move  |  F: focus  |  Home: reset");
                frame.textMuted(state->status);
                state->viewport_result = state->viewport.draw(
                    frame, lux::ui::ViewportSpec{state->linked && !state->closing ? state->texture
                                                                                  : lux::ui::TextureHandle{}});
            });
            impl->addPane("lux.scene.outline", "Scene Outline",
                          [state](lux::ui::Frame &frame) { state->outline(frame); });
            impl->addPane("lux.scene.resources", "Resources / Diagnostics / History", [state](lux::ui::Frame& frame) {
                if (frame.smallButton("Retry failed resources"))
                    state->retry_requested = true;
                frame.text(state->status);
                frame.text("History: not connected (SV-1 read-only)");
                frame.text("Render extent: " + std::to_string(state->extent.width) + " x " +
                    std::to_string(state->extent.height));
                const auto diagnostics = state->port->diagnostics();
                frame.text("GPU frames: " + std::to_string(diagnostics.frames) + " | FIF mask: " +
                    std::to_string(diagnostics.slot_mask) + " | Render events: " +
                    std::to_string(diagnostics.render_events));
                for (const auto& job : state->resources)
                {
                    frame.text(uuids::to_string(job->mesh_source.uuid()) + " / " + job->status);
                    frame.textMuted(uuids::to_string(job->material_source.uuid()));
                }
            });
            if (impl->registrations.size() != 4)
                return lux::cxx::unexpected(EWorkbenchError::PANE_REGISTRATION_FAILURE);
            impl->layout();
            auto result = std::unique_ptr<SceneWorkbench>{new SceneWorkbench(std::move(impl))};
            auto& ready = *result->impl_;
            ready.pending_view = ready.runtime.control().addView(ready.render_scene, ready.extent, "Scene View");
            ready.pending_target = ready.runtime.control().createOffscreenRenderTarget(
                ready.extent, render::kTargetFlagSampled);
            for (auto& resource : result->impl_->resources)
                resource->start();
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(EWorkbenchError::ALLOCATION_FAILURE);
        }
    }

    SceneWorkbench::~SceneWorkbench() noexcept
    {
        if (!impl_->closed)
        {
            requestClose();
            while (!advanceClose())
            {
                static_cast<void>(impl_->context->execution().drainMain(64));
                std::this_thread::yield();
            }
        }
    }

    void SceneWorkbench::beforeUiFrame()
    {
        impl_->port->pump();
        impl_->acceptView();
        // Retained updates get a forwarding opportunity even while a UI Frame is backpressured.
        if (impl_->scene && !impl_->closing)
        {
            static_cast<void>(impl_->scene->executePresentation());
            impl_->port->deferNewFrames(impl_->scene->findSceneSystem<scene::RenderSystem>()->hasPendingUpdate());
        }
        std::size_t ready{};
        for (std::size_t index = 0; index < impl_->resources.size(); ++index)
        {
            auto& resource = impl_->resources[index];
            resource->poll(impl_->runtime, *impl_->context, impl_->closing, impl_->resource_serials[index]);
            if (resource->failed && resource->settled())
            {
                resource->release(impl_->runtime);
                if (impl_->retry_requested && !impl_->closing &&
                    impl_->resource_serials[index] != std::numeric_limits<std::uint64_t>::max())
                {
                    auto replacement = std::make_unique<detail::ResourceJob>(*impl_->context, impl_->scene_handle,
                        resource->entity, resource->mesh_source, resource->material_source,
                        impl_->resource_serials[index] + 1);
                    ++impl_->resource_serials[index];
                    resource = std::move(replacement);
                    resource->start();
                }
            }
            ready += resource->adopted;
        }
        impl_->retry_requested = false;
#if LUX_SV1_DIAGNOSTICS
        impl_->verifyScene();
#endif
        if (impl_->linked && !impl_->closing)
        {
            const bool failed = std::any_of(impl_->resources.begin(), impl_->resources.end(),
                [](const auto& resource) { return resource->failed; });
            if (failed)
                impl_->status = "Some scene resources failed; see Resources";
            else if (ready == impl_->resources.size() && !impl_->resources.empty())
                impl_->status = "Scene resources ready";
            else
                impl_->status = "Scene view ready; waiting for assets";
        }
        impl_->viewport_result = {};
        static_cast<void>(impl_->context->selection().validate());
        impl_->entities.clear();
        if (impl_->scene)
            for (const auto [entity] : impl_->scene->registry().storage<simulation::ecs::Entity>().each())
                impl_->entities.push_back(entity);
    }

    void SceneWorkbench::afterUiFrame(double seconds, lux::ui::Vec2 scale)
    {
        if (impl_->closing || impl_->closed)
            return;
        auto& state = *impl_;
        const auto input = state.context->ui().inputSnapshot();
        state.camera.update(input, state.viewport_result, seconds);
        if (state.viewport_result.size.width <= 0 || state.viewport_result.size.height <= 0)
        {
            state.camera.releaseCapture();
            state.port->setViewFrame({});
            if (state.linked && !state.suspended && !state.port->framePending())
            {
                state.runtime.control().removeLayer(state.target.id(), 0);
                state.suspended = true;
            }
            return;
        }
        if (input.pressed[static_cast<std::size_t>(lux::ui::EKey::F)] && state.viewport_result.window_focused &&
            !input.keyboard_blocked)
        {
            const auto selection = state.context->selection().current();
            if (selection.scene == state.scene_handle)
                if (const auto* transform = state.scene->registry().try_get<simulation::ecs::WorldTransform3D>(
                    selection.entity))
                {
                    Eigen::Vector3d center = transform->value.translation();
                    double radius = 1.0;
                    for (const auto& resource : state.resources)
                        if (resource->entity == selection.entity && resource->adopted && resource->mesh_read->asset)
                        {
                            const auto& bounds = resource->mesh_read->asset->data().bounds;
                            if (bounds && bounds->isValid())
                            {
                                center = transform->value * bounds->center().cast<double>();
                                radius = (transform->value.linear().cwiseAbs() *
                                    bounds->halfExtents().cast<double>()).norm();
                            }
                        }
                    state.camera.focus(center, radius);
                }
        }
        if (!state.linked || state.port->framePending())
            return;
        if (state.suspended)
        {
            state.runtime.control().setLayer(state.target.id(), 0, state.render_scene, state.view.id());
            state.suspended = false;
        }
        const math::Extent2u desired{
            static_cast<std::uint32_t>(std::clamp(state.viewport_result.size.width * scale.x, 1.0F, 8192.0F)),
            static_cast<std::uint32_t>(std::clamp(state.viewport_result.size.height * scale.y, 1.0F, 8192.0F))};
        if (!state.pending_resize.valid() &&
            (desired.width != state.extent.width || desired.height != state.extent.height))
            state.pending_resize = state.runtime.control().requestResizeTarget(state.target.id(), desired);
        const auto step = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>{std::clamp(seconds, 0.001, 0.1)});
        if (!state.scene->simulation().execute(*state.executor, step) ||
            !state.scene->executeStablePoint() || !state.scene->executePresentation())
        {
            state.status = "Scene update failed";
            static bool reported{};
            if (!std::exchange(reported, true))
                std::fprintf(stderr, "Scene simulation/stable point/presentation update failed\n");
            return;
        }
        SceneViewFrame frame;
        state.port->deferNewFrames(state.scene->findSceneSystem<scene::RenderSystem>()->hasPendingUpdate());
        frame.target = state.target.id();
        frame.texture = state.texture;
        frame.cpu_lease = state.cpu_lease;
        frame.camera.scene_id = state.render_scene;
        frame.camera.view = state.view.id();
        Eigen::Vector3d origin;
        for (int axis = 0; axis < 3; ++axis)
        {
            const auto page = std::floor(state.camera.position()[axis] / 1024.0);
            if (!std::isfinite(page) || page < std::numeric_limits<std::int32_t>::min() ||
                page > std::numeric_limits<std::int32_t>::max())
            {
                state.status = "Camera outside supported render coordinates";
                return;
            }
            frame.camera.render_origin.page_delta[axis] = static_cast<std::int32_t>(page);
            frame.camera.render_origin.local[axis] = static_cast<float>(state.camera.position()[axis] - page * 1024.0);
            origin[axis] = state.camera.position()[axis];
        }
        const Eigen::Matrix4f view = state.camera.view(origin).cast<float>();
        const Eigen::Matrix4f projection = state.camera.projection(double(state.extent.width) / state.extent.height)
            .cast<float>();
        std::copy_n(view.data(), 16, frame.camera.view_matrix);
        std::copy_n(projection.data(), 16, frame.camera.proj_matrix);
        state.port->setViewFrame(frame);
    }

    void SceneWorkbench::requestClose() noexcept
    {
        impl_->closing = true;
        impl_->camera.releaseCapture();
        impl_->port->setViewFrame({});
        impl_->port->deferNewFrames(false);
    }

    bool SceneWorkbench::closeRequested() const noexcept { return impl_->closing; }

    bool SceneWorkbench::advanceClose()
    {
        auto& state = *impl_;
        if (state.closed)
            return true;
        if (!state.closing)
            return false;
        state.port->pump();
        state.port->drainViewFrames();
        state.acceptView();
#if LUX_SV1_DIAGNOSTICS
        if (state.diagnostic_readback.valid() && !state.diagnostic_readback.isReady())
            return false;
        if (state.diagnostic_timing.valid() && !state.diagnostic_timing.isReady())
            return false;
#endif
        for (std::size_t index = 0; index < state.resources.size(); ++index)
        {
            auto& resource = state.resources[index];
            resource->poll(state.runtime, *state.context, true, state.resource_serials[index]);
            if (!resource->settled())
                return false;
        }
        if (state.pending_view.valid() || state.pending_target.valid() || state.pending_resize.valid() ||
            state.cpu_lease.use_count() > 1)
            return false;
        if (state.port->stopping())
        {
            state.target = {};
            state.view = {};
        }
        else if (state.target)
        {
            state.runtime.control().removeLayer(state.target.id(), 0);
            auto result = state.target.close();
            if (!result)
                return false;
            state.target_close = std::move(*result);
        }
        if (state.view)
            static_cast<void>(state.view.close());
        if (state.target_close.valid() && !state.target_close.isReady())
            return false;
        if (!state.port->stopping() && !*state.view_closed && state.view_created)
            return false;
        for (auto& resource : state.resources)
            resource->release(state.runtime);
        if (state.scene)
        {
            state.scene->requestStop();
            static_cast<void>(state.context->selection().deactivate(state.scene_handle));
            state.scene_handle = {};
            state.scene.reset();
        }
        static_cast<void>(state.runtime.control().flushDeferredReleases());
        if (!state.port->stopping() && (state.runtime.control().pendingSceneReleases() != 0 ||
            state.runtime.control().pendingViewReleases() != 0 || state.runtime.control().pendingTargetReleases() != 0))
            return false;
        state.closed = true;
        return true;
    }
#if LUX_SV1_DIAGNOSTICS
    void detail::SceneWorkbenchDiagnostics::enable(SceneWorkbench& workbench, const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        workbench.impl_->diagnostic_directory = directory;
    }

    bool detail::SceneWorkbenchDiagnostics::passed(const SceneWorkbench& workbench) noexcept
    {
        return workbench.impl_->diagnostic_passed;
    }
#endif
    detail::WorkbenchMeasurement detail::SceneWorkbenchMeasurement::read(const SceneWorkbench& workbench) noexcept
    {
        const auto& state = *workbench.impl_;
        detail::WorkbenchMeasurement result{state.target.id(), state.cpu_lease, state.extent.width, state.extent.height,
                state.resources.size(), static_cast<std::size_t>(std::count_if(state.resources.begin(),
                    state.resources.end(), [](const auto& job) { return job->adopted; })),
                state.pending_resize.valid()};
        result.presentation_pending = state.scene &&
            state.scene->findSceneSystem<scene::RenderSystem>()->hasPendingUpdate();
        for (const auto& job : state.resources)
        {
            result.failed += job->failed;
            result.settled += job->settled();
            result.serial_sum += job->serial;
            result.pending_requests += !job->mesh_read->done() + !job->material_read->done() +
                job->mesh_upload.valid() + job->material_upload.valid() + job->forward_compile.valid() +
                job->gbuffer_compile.valid();
            if (!job->released)
                result.live_handles += job->mesh.isValid() + job->material.isValid() +
                    job->forward.isValid() + job->gbuffer.isValid();
            if (job->failed && job->mesh_read->done())
            {
                result.asset_error = static_cast<std::uint32_t>(job->mesh_read->failure.code);
                result.storage_error = static_cast<std::uint32_t>(job->mesh_read->failure.storage_error);
            }
        }
        return result;
    }
} // namespace lux::editor::workbench
