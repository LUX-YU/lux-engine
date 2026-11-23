#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include "ModelSceneSupport.hpp"

#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/systems/render/mesh/MeshInstanceReaction.hpp>
#include <lux/engine/systems/render/mesh/MeshVisibilitySystem.hpp>
#include <lux/engine/systems/render/mesh/MeshTransformCollector.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp>
#include <lux/engine/systems/render/model/ModelInstantiation.hpp>
#include <lux/engine/systems/render/view/ViewCollector.hpp>
#include <lux/engine/systems/render/view/ViewReaction.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/world/World.hpp>

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace lux;
namespace rendering = lux::engine::render;
using engine::test::Result;

// Normal model use. Failure injection, shared-Scene assertions and IO gates are in ModelSceneLifecycle.cpp.
int main(int argc, char** argv)
{
    const auto startup_begin = std::chrono::steady_clock::now();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string_view{argv[1]} == "--cook")
    {
        if (argc != 4)
        {
            std::fprintf(stderr, "usage: engine_model_example --cook <model.gltf> <asset-directory>\n");
            return 2;
        }
        auto execution = execution::ExecutionRuntime::create(
            {.cpu_concurrency = 2, .timer = {32}, .blocking = execution::BlockingSchedulerConfig{2, 32}}
        );
        if (!execution)
        {
            std::fprintf(stderr, "Execution initialization failed: %u\n", static_cast<unsigned>(execution.error()));
            return 1;
        }
        const std::filesystem::path source{argv[2]}, directory{argv[3]};
        const auto cook = [&]() noexcept -> exec::task<Result>
        {
            auto product = co_await engine::test::cookModelAssets(source, directory);
            if (!product) co_return cxx::unexpected(product.error());
            co_return Result{};
        };
        auto cooked = engine::test::runTask(cook(), false);
        if (!cooked)
        {
            std::fprintf(stderr, "Cooking failed: %s\n", error::format(cooked.error()).c_str());
            return 1;
        }
        return 0;
    }
    std::uint32_t frame_limit = 0;
    std::uint32_t instance_count = 1, dirty_percent = 100, warmup = 0, width = 960, height = 640;
    std::uint32_t shadow_resolution = 1024, gpu_wait_ns = 1'000'000;
    bool validation = true, vertical_sync = true, offscreen = false, full_scene = false, single_queue = false;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path floor_source;
    if (argc < 2)
    {
        std::fprintf(
            stderr,
            "usage: engine_model_example <asset-directory> [--frames N] [--instances N] "
            "[--dirty-percent N] [--warmup N] [--width N] [--height N] "
            "[--no-validation] [--no-vsync] [--offscreen] [--full-scene] "
            "[--model asset-directory] [--floor asset-directory] "
            "[--shadow-resolution N] [--single-queue] [--gpu-wait-ns N]\n"
        );
        return 2;
    }
    sources.emplace_back(argv[1]);
    for (int argument = 2; argument < argc; ++argument)
    {
        const std::string_view option{argv[argument]};
        if (option == "--single-queue") { single_queue = true; continue; }
        if (option == "--no-validation") { validation = false; continue; }
        if (option == "--no-vsync") { vertical_sync = false; continue; }
        if (option == "--offscreen") { offscreen = true; continue; }
        if (option == "--full-scene") { full_scene = true; continue; }
        if (option == "--model" || option == "--floor")
        {
            if (++argument == argc) return 2;
            if (option == "--model") sources.emplace_back(argv[argument]);
            else floor_source = argv[argument];
            continue;
        }
        auto* output = option == "--frames" ? &frame_limit : option == "--instances" ? &instance_count :
            option == "--dirty-percent" ? &dirty_percent : option == "--warmup" ? &warmup :
            option == "--width" ? &width : option == "--height" ? &height :
            option == "--shadow-resolution" ? &shadow_resolution : option == "--gpu-wait-ns" ? &gpu_wait_ns : nullptr;
        if (!output || ++argument == argc) return 2;
        const std::string_view value{argv[argument]};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), *output);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        {
            return 2;
        }
    }
    const bool invalid_extent = !width || !height || width > INT_MAX || height > INT_MAX;
    const bool invalid_workload = !instance_count || instance_count > 100000 || dirty_percent > 100;
    if (invalid_workload || invalid_extent || gpu_wait_ns > 1'000'000) return 2;
    auto platform = window::GlfwRuntime::create();
    auto window = window::LuxWindow::create({int(width), int(height), "LuxWindow + Engine / Model"});
    if (!platform || !window)
    {
        return 1;
    }
    std::printf("STARTUP window_ms=%.3f\n", std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startup_begin
    ).count());
    std::atomic_uint validation_errors{};
    render::RenderServerConfig configuration;
    configuration.channel_name = "lux.engine.model_example";
    configuration.validation = validation;
    configuration.uploads.emplace();
    configuration.frames_in_flight = 3;
    configuration.gpu_wait_ns = gpu_wait_ns;
    configuration.max_frames_ahead = 2;
    configuration.diagnostics = {
        &validation_errors,
        [](void* context, const render::RenderDiagnostic& diagnostic) noexcept
        {
            if (diagnostic.severity == render::ERenderDiagnosticSeverity::ERROR_MESSAGE)
            {
                ++*static_cast<std::atomic_uint*>(context);
            }
            const auto sink = rendering::renderDiagnostics();
            sink.report(sink.context, diagnostic);
        }
    };
    render::DeviceOptions device;
    device.texture_descriptors = true;
    device.buffer_device_address = true;
    device.multiple_queues = !single_queue;
    // Stable professional providers outlive Context's repository settlement. They are filled only after init;
    // their inert destructors never access the Runtime after Context has stopped it.
    std::vector<asset::MountLease> mounts;
    std::optional<rendering::MeshUpload> meshes;
    std::optional<rendering::TextureUpload> textures;
    std::optional<rendering::MaterialUpload> materials;
    // Submission replies are not GPU completion. The result below is printed only after the
    // Context has joined RenderServer and settled its GPU work. No per-frame CPU/GPU wait is added.
    render::RenderError frame_error;
    Result prepared;
    std::uint32_t frames = 0, ready_frames = 0;
    std::uint64_t execution_begin = 0, execution_end = 0;
    std::chrono::steady_clock::time_point measured_begin{}, previous_frame{}, scene_initialized{}, resources_ready{};
    std::pair<std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point>
        first_full_submission{startup_begin, {}};
    std::vector<double> frame_times;
    std::vector<render::FrameCompletion> completions;
    std::pair<render::RenderError*, std::vector<render::FrameCompletion>*> completion_context{
        &frame_error, &completions
    };
    if (frame_limit)
    {
        frame_times.reserve(frame_limit);
        completions.reserve(frame_limit);
    }
    {
        auto initialization = engine::init(
            {.cpu_concurrency = 2, .timer = {32}, .blocking = execution::BlockingSchedulerConfig{2, 32}},
            {64U << 20U, 128U << 20U, 16},
            std::move(configuration),
            device
        );
        if (!initialization)
        {
            std::fprintf(stderr, "Init: %s\n", error::format(initialization.error()).c_str());
            return 1;
        }
        std::printf("STARTUP context_ms=%.3f\n", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startup_begin
        ).count());
        auto& execution = execution::ExecutionRuntime::instance();
        execution.setWake(&window::LuxWindow::wakeEvents);

        // Providers outlive the repository. The Engine retires its Scenes before either service is destroyed.
        auto& context = **initialization;
        auto& vfs = context.assetVfs();
        auto& assets = context.assets();
        render::CapabilityComposition effect;
        render::CapabilityImplementationId lighting_implementation;
        rendering::RenderCapabilityMetaRegistry metadata;
        rendering::RenderSystem* rendering_system = nullptr;
        std::size_t light_count = 0;
        std::unique_ptr<scene::SceneInstance> candidate;
        {
            ecs::Registry* registry = nullptr;
            std::vector<ecs::Entity> entities;
            ecs::Entity view = ecs::NullEntity;
            std::vector<Eigen::Vector3d> rotation_origins;
            std::size_t animated_entities = 0;
            auto animation_time = std::chrono::steady_clock::now();
            engine::Engine engine{context};

            const auto prepare = [&]() noexcept -> exec::task<Result>
            {
                render::TargetDesc target;
                (*window)->framebufferSize(target.width, target.height);
                target.kind = offscreen ? render::ETargetKind::OFFSCREEN : render::ETargetKind::SURFACE;
                target.format = rdesc::ETextureFormat::RGBA8_UNORM;
                render::DeferredLightingConfiguration lighting;
                lighting.shadows = lighting.clustered = full_scene;
                lighting.environment =
                    full_scene ? std::array<float, 4>{0.06F, 0.06F, 0.06F, 0} : std::array<float, 4>{};
                lighting.shadow = {shadow_resolution, 1, 60};
                const render::EffectOutputDesc effect_output{
                    target.format, target.samples, target.kind, target.sampled
                };
                auto created = co_await engine::test::resolveComposition(
                    context.renderClient(), effect, effect_output, lighting
                );
                if (!created)
                {
                    co_return created;
                }
                auto& runtime = context.renderClient();
                const auto implementation = effect.contributions[0].implementation_id;
                const std::array command_names{
                    render::CommandName{render::Mesh3dCommands::UploadMesh::name},
                    render::CommandName{render::Mesh3dCommands::ReleaseMesh::name},
                    render::CommandName{render::ImageCommands::Upload::name},
                    render::CommandName{render::ImageCommands::Release::name},
                    render::CommandName{render::MaterialCommands::Create::name},
                    render::CommandName{render::MaterialCommands::Release::name},
                    render::CommandName{render::Mesh3dCommands::CreateInstances::name},
                    render::CommandName{render::Mesh3dCommands::ReleaseInstance::name}
                };
                auto resolved_commands = co_await runtime.request<render::QueryCommandIds>(command_names);
                if (resolved_commands.error.type) co_return cxx::unexpected(resolved_commands.error);
                const auto upload_mesh = resolved_commands.ids[0];
                const auto release_mesh = resolved_commands.ids[1];
                const auto upload_texture = resolved_commands.ids[2];
                const auto release_texture = resolved_commands.ids[3];
                const auto create_material = resolved_commands.ids[4];
                const auto release_material = resolved_commands.ids[5];
                const auto create_instances = resolved_commands.ids[6];
                const auto release_instance = resolved_commands.ids[7];
                auto mesh_upload = rendering::MeshUpload::create(
                    runtime, implementation, upload_mesh, release_mesh
                );
                auto texture_upload = rendering::TextureUpload::create(
                    runtime, implementation, upload_texture, release_texture
                );
                auto material_upload = rendering::MaterialUpload::create(
                    runtime, implementation, create_material, release_material
                );
                if (!mesh_upload || !texture_upload || !material_upload)
                {
                    co_return cxx::unexpected(render::RenderError{render::kCapabilityDeclaration});
                }
                meshes.emplace(std::move(*mesh_upload));
                textures.emplace(std::move(*texture_upload));
                materials.emplace(std::move(*material_upload));
                auto mesh_factory = metadata.addReaction<rendering::MeshInstanceReaction>(
                    [&meshes, &materials, create = create_instances, release = release_instance](
                        rendering::RenderSystem& system, render::CapabilityImplementationId selected
                    ) noexcept
                    {
                        return rendering::MeshInstanceReaction::create(
                            system, selected, *meshes, *materials, create, release
                        );
                    }
                );
                if (!mesh_factory) co_return mesh_factory;

                for (auto registered :
                     {assets.registerUse<rendering::MeshUploadDefinition>(*meshes),
                      assets.registerUse<rendering::TextureUploadDefinition>(*textures),
                      assets.registerUse<rendering::MaterialUploadDefinition>(*materials, *textures)})
                {
                    if (!registered)
                    {
                        co_return registered;
                    }
                }

                auto world = std::make_unique<world::World>(world::WorldDescription{"Model", {{"mesh3d", {}}}});
                registry = &world->registry();
                const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(double(instance_count) * 4 / 3)));
                const auto rows = (instance_count + columns - 1) / columns;
                const auto source_count = sources.size();
                if (!floor_source.empty()) sources.push_back(floor_source);
                double mount_ms = 0, model_read_ms = 0, mesh_read_ms = 0;
                std::uint64_t triangle_count = 0;
                std::size_t material_count = 0, texture_count = 0;
                for (std::size_t source_index = 0; source_index < sources.size(); ++source_index)
                {
                    const bool is_floor = source_index == source_count;
                    const auto mount_begin = std::chrono::steady_clock::now();
                    auto provider = co_await engine::test::openModelAssets(sources[source_index]);
                    if (!provider) co_return cxx::unexpected(provider.error());
                    const auto prefix = "/Model" + std::to_string(source_index);
                    auto mounted = vfs.mount({prefix, *provider});
                    if (!mounted) co_return cxx::unexpected(render::RenderError{render::kRuntimeState});
                    mounts.push_back(std::move(*mounted));
                    mount_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - mount_begin
                    ).count();
                    const auto read_begin = std::chrono::steady_clock::now();
                    auto model_asset = assets.acquire<asset::ModelAsset>(prefix + "/model");
                    if (!model_asset) co_return cxx::unexpected(model_asset.error());
                    auto model_data = co_await assets.read(*model_asset);
                    if (!model_data) co_return cxx::unexpected(model_data.error());
                    model_read_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - read_begin
                    ).count();
                    const auto& description = **model_data;
                    (*provider)->enumerate([&](const asset::ProviderEntry& entry) noexcept
                    {
                        material_count += entry.magic_number == asset::MaterialAsset::primary_magic;
                        texture_count += entry.magic_number == asset::TextureAsset::primary_magic;
                    });
                    std::vector<ecs::Transform3D> normalized;
                    std::uint64_t model_triangles = 0;
                    for (std::size_t instance = source_index;
                         is_floor || instance < instance_count;
                         instance += source_count)
                    {
                        auto model = rendering::createModelEntities(*registry, description);
                        if (!model) co_return cxx::unexpected(model.error());
                        if (normalized.empty())
                        {
                            auto minimum = Eigen::Vector3d::Constant(std::numeric_limits<double>::max()).eval();
                            Eigen::Vector3d maximum = -minimum;
                            for (const auto entity : *model)
                            {
                                const auto& transform = registry->get<ecs::Transform3D>(entity);
                                const Eigen::Affine3d placement = Eigen::Translation3d(transform.translation) *
                                    transform.rotation * Eigen::Scaling(transform.scale);
                                const auto mesh_id = registry->get<ecs::Mesh3D>(entity).value.mesh;
                                const auto mesh_begin = std::chrono::steady_clock::now();
                                auto mesh_asset = assets.acquire<asset::MeshAsset>(mesh_id);
                                if (!mesh_asset) co_return cxx::unexpected(mesh_asset.error());
                                auto mesh = co_await assets.read(*mesh_asset);
                                if (!mesh) co_return cxx::unexpected(mesh.error());
                                mesh_read_ms += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - mesh_begin
                                ).count();
                                model_triangles += (*mesh)->indices.size() / 3;
                                for (const auto& vertex : (*mesh)->vertices)
                                {
                                    const Eigen::Vector3d position = placement * vertex.position.cast<double>();
                                    minimum = minimum.cwiseMin(position);
                                    maximum = maximum.cwiseMax(position);
                                }
                            }
                            const double scale = 2.0 / (maximum - minimum).maxCoeff();
                            const Eigen::Vector3d origin{(minimum.x() + maximum.x()) / 2, minimum.y(),
                                                         (minimum.z() + maximum.z()) / 2};
                            for (const auto entity : *model)
                            {
                                auto value = registry->get<ecs::Transform3D>(entity);
                                value.translation = (value.translation - origin) * scale;
                                value.scale *= scale;
                                normalized.push_back(value);
                            }
                        }
                        const Eigen::Vector3d offset = is_floor ? Eigen::Vector3d::Zero().eval() : Eigen::Vector3d{
                            (double(instance % columns) - double(columns - 1) / 2) * 2.6, 0,
                            (double(instance / columns) - double(rows - 1) / 2) * 2.6
                        };
                        for (std::size_t part = 0; part < model->size(); ++part)
                        {
                            const auto entity = (*model)[part];
                            registry->patch<ecs::Transform3D>(entity, [&](auto& value) noexcept
                            {
                                value = normalized[part];
                                if (is_floor)
                                {
                                    value.rotation = Eigen::Quaterniond::Identity();
                                    value.translation = {0, -0.1, 0};
                                    value.scale = {double(columns) * 2.8, 0.2, double(rows) * 2.8};
                                }
                                else value.translation += offset;
                            });
                            entities.push_back(entity);
                            rotation_origins.push_back(offset);
                        }
                        triangle_count += model_triangles;
                        if (is_floor) break;
                        animated_entities = entities.size();
                    }
                    std::printf("MODEL cooked=%s primitives=%zu triangles=%llu\n",
                                sources[source_index].filename().string().c_str(),
                                description.primitives.size(), model_triangles);
                }
                std::printf("SCENE models=%u entities=%zu triangles=%llu materials=%zu textures=%zu\n",
                            instance_count, entities.size(), triangle_count, material_count, texture_count);
                std::printf("STARTUP_ASSETS mount_ms=%.3f model_read_ms=%.3f mesh_read_ms=%.3f assembly_ms=%.3f\n",
                            mount_ms, model_read_ms, mesh_read_ms,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - startup_begin
                            ).count());
                auto camera = engine::test::createViewComponents(*registry, **window, target, false);
                if (!camera)
                {
                    co_return cxx::unexpected(camera.error());
                }
                view = *camera;
                if (full_scene || instance_count > 1)
                {
                    registry->patch<ecs::Transform3D>(view, [&](auto& value) noexcept
                    {
                        value.translation = {double(columns) * 1.5, double(rows) * 2.0, double(rows) * 2.5};
                        const Eigen::Vector3d forward = (Eigen::Vector3d{0, 0.6, 0} - value.translation).normalized();
                        const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
                        Eigen::Matrix3d basis;
                        basis.col(0) = right;
                        basis.col(1) = right.cross(forward);
                        basis.col(2) = -forward;
                        value.rotation = Eigen::Quaterniond(basis);
                    });
                    registry->patch<scene::ViewComponent>(view, [](auto& value) noexcept
                    {
                        value.projection = scene::PerspectiveProjection{std::numbers::pi / 3, 0.1, 80};
                    });
                }
                registry->patch<rendering::RenderTargetComponent>(view, [&](auto& value) noexcept
                {
                    if (value.surface) value.surface->vertical_sync = vertical_sync;
                });

                for (auto registered :
                     {metadata.addSceneReaction<rendering::ViewReaction>(),
                      metadata.addCollector<rendering::MeshTransformCollector>(),
                      metadata.addCollector<rendering::ViewCollector<render::Mesh3dCapability>>(),
                      metadata.addCollector<rendering::ViewCollector<render::Lighting3dCapability>>()})
                {
                    if (!registered)
                    {
                        co_return registered;
                    }
                }
                lighting_implementation = effect.contributions[1].implementation_id;
                auto render_system = rendering::RenderSystem::create(
                    std::move(effect),
                    effect_output,
                    *world,
                    context,
                    metadata
                );
                if (!render_system)
                {
                    co_return cxx::unexpected(render_system.error());
                }
                auto visibility = rendering::MeshVisibilitySystem::create(*world, context, 0, 100, 120, 10, 1);
                if (!visibility) co_return cxx::unexpected(visibility.error());
                candidate =
                    std::make_unique<scene::SceneInstance>(
                        std::move(world),
                        std::make_unique<simulation::Simulation>()
                    );
                rendering_system = render_system->get();
                candidate->add(std::move(*visibility));
                candidate->add(std::move(*render_system));
                auto scene = engine.addScene(candidate);
                if (!scene)
                {
                    co_return cxx::unexpected(scene.error());
                }
                auto initialized = co_await engine.whenSceneInitialized(*scene);
                if (!initialized) co_return initialized;
                scene_initialized = std::chrono::steady_clock::now();
                std::printf("STARTUP scene_initialized_ms=%.3f\n",
                            std::chrono::duration<double, std::milli>(scene_initialized - startup_begin).count());
                co_return Result{};
            };
            prepared = engine::test::runTask(prepare());
            if (prepared)
            {
                engine.addEngineStageCallback(
                    [&]() noexcept -> exec::task<Result>
                    {
                        if (light_count != 0 || rendering_system->sceneId().isNull())
                        {
                            co_return Result{};
                        }
                        // Reuse the existing test light Collector.
                        std::vector<render::VDirectLight> descriptions{
                            render::DirectionalLightGpu{{3, 3, 3, 0}, {-0.6666667F, -0.6666667F, 0.3333333F, 0}}
                        };
                        std::vector<render::LightShadowSettings> shadows(full_scene ? 10 : 1);
                        if (full_scene)
                        {
                            shadows[0] = {true, 0.0005F, 0.01F, 1, 1, 4, {0.1F, 0.25F, 0.5F, 1}};
                            for (int index = 0; index < 6; ++index)
                            {
                                const float x = float(index % 3 - 1) * 6;
                                const float z = index < 3 ? -4.0F : 4.0F;
                                descriptions.push_back(render::PointLightGpu{
                                    {x, 4, z, 14}, {9, index % 2 ? 6.0F : 8.4F, index % 2 ? 4.4F : 10.0F, 0}
                                });
                                if (index < 2) shadows[index + 1] = {true, 0.0005F, 0.01F, 1, 1};
                            }
                            for (int index = 0; index < 3; ++index)
                            {
                                descriptions.push_back(render::SpotLightGpu{
                                    {float(index - 1) * 6, 7, 0, 18}, {18, 17, 14, 0},
                                    {0, -1, 0, 0.7071068F}, {1, 0, 1, 0.8660254F}
                                });
                                if (index < 2) shadows[index + 7] = {true, 0.0005F, 0.01F, 1, 1};
                            }
                        }
                        render::Lighting3dCommands::CreateLights::Request light_request{
                            lighting_implementation, rendering_system->sceneId(), {0, descriptions, shadows}
                        };
                        auto& runtime = context.renderClient();
                        const std::array light_commands{
                            render::CommandName{render::Lighting3dCommands::CreateLights::name}
                        };
                        auto light_ids = co_await runtime.request<render::QueryCommandIds>(light_commands);
                        if (light_ids.error.type) co_return cxx::unexpected(light_ids.error);
                        auto illuminated = co_await runtime.request<render::Lighting3dCommands::CreateLights>(
                            light_ids.ids.front(), light_request
                        );
                        if (illuminated.error.type)
                        {
                            co_return cxx::unexpected(illuminated.error);
                        }
                        light_count = illuminated.lights.size();
                        std::printf("LIGHTS count=%zu clustered=%d shadow_maps=%u resolution=%u cascades=4 "
                                    "splits=0.1,0.25,0.5,1 shadow_distance=60 camera_near=0.1 camera_far=80\n",
                                    light_count, full_scene, full_scene ? 18 : 0, shadow_resolution);

                        co_return Result{};
                    },
                    engine::EEngineStage::BEGIN_UPDATE
                );
                engine.addEngineStageCallback(
                    [&]() noexcept -> exec::task<Result>
                    {
                        const bool ready =
                            registry->view<rendering::MeshRenderResourceComponent>().size() == entities.size() &&
                            registry->all_of<rendering::RenderViewComponent>(view);
                        if (ready && ready_frames >= warmup &&
                            measured_begin == std::chrono::steady_clock::time_point{})
                        {
                            auto consumed =
                                co_await rendering::RenderFrameSubmission::instance().whenSubmittedFramesConsumed();
                            if (!consumed) co_return consumed;
                            const auto attachment = registry->get<rendering::RenderViewComponent>(view).attachment;
                            auto progress =
                                co_await context.renderClient().request<render::QueryExecutionProgress>(
                                    render::AttachmentId{attachment}
                                );
                            if (progress.error.type) co_return cxx::unexpected(progress.error);
                            execution_begin = progress.progress.count(render::EExecutionOutcome::EXECUTED);
                            measured_begin = previous_frame = std::chrono::steady_clock::now();
                            std::uint32_t actual_width, actual_height;
                            (*window)->framebufferSize(actual_width, actual_height);
                            std::printf("PROFILE_READY instances=%u dirty_percent=%u extent=%ux%u "
                                        "validation=%d vsync=%d offscreen=%d\n",
                                        instance_count, dirty_percent, actual_width, actual_height,
                                        validation, vertical_sync, offscreen);
                        }
                        co_return Result{};
                    },
                    engine::EEngineStage::BEGIN_UPDATE
                );
                // Component updates drive the existing upload and Resize paths.
                engine.addEngineStageCallback(
                    [&]() noexcept -> Result
                    {
                        const auto now = std::chrono::steady_clock::now();
                        const double delta = std::chrono::duration<double>(now - animation_time).count();
                        animation_time = now;
                        const Eigen::Quaterniond rotation{Eigen::AngleAxisd(delta * 0.2, Eigen::Vector3d::UnitY())};
                        const auto moving = animated_entities * dirty_percent / 100;
                        for (std::size_t index = 0; index < moving; ++index)
                        {
                            const auto ordinal = index * animated_entities / moving;
                            const auto entity = entities[ordinal];
                            registry->patch<ecs::Transform3D>(
                                entity,
                                [&](auto& value) noexcept
                                {
                                    value.rotation = (rotation * value.rotation).normalized();
                                    value.translation = rotation_origins[ordinal] +
                                        rotation * (value.translation - rotation_origins[ordinal]);
                                }
                            );
                        }
                        std::uint32_t width, height;
                        (*window)->framebufferSize(width, height);
                        const auto& target = registry->get<rendering::RenderTargetComponent>(view).description;
                        if (target.width != width || target.height != height)
                        {
                            registry->patch<rendering::RenderTargetComponent>(
                                view,
                                [&](auto& value) noexcept
                                {
                                    value.description.width = width;
                                    value.description.height = height;
                                }
                            );
                        }
                        return {};
                    },
                    engine::EEngineStage::SIMULATION_FINISHED
                );
                engine.addEngineStageCallback(
                    [&]() noexcept -> Result
                    {
                        const bool ready =
                            registry->view<rendering::MeshRenderResourceComponent>().size() == entities.size() &&
                                           registry->all_of<rendering::RenderViewComponent>(view);
                        if (ready)
                        {
                            ++ready_frames;
                            if (resources_ready == std::chrono::steady_clock::time_point{})
                            {
                                resources_ready = std::chrono::steady_clock::now();
                                std::printf("STARTUP resources_adopted_ms=%.3f read_upload_adoption_ms=%.3f\n",
                                    std::chrono::duration<double, std::milli>(resources_ready - startup_begin).count(),
                                    std::chrono::duration<double, std::milli>(
                                        resources_ready - scene_initialized
                                    ).count());
                            }
                        }
                        const bool measured = ready && measured_begin != std::chrono::steady_clock::time_point{};
                        auto writer = rendering::RenderFrameSubmission::instance().writer();
                        if (ready && first_full_submission.second == std::chrono::steady_clock::time_point{})
                        {
                            writer.onCompletion(
                                &first_full_submission,
                                [](void* context, const auto& completion) noexcept
                                {
                                    auto& timing = *static_cast<decltype(first_full_submission)*>(context);
                                    if (timing.second == std::chrono::steady_clock::time_point{} &&
                                        completion.outcome == render::EFrameOutcome::SUBMITTED)
                                    {
                                        timing.second = std::chrono::steady_clock::now();
                                        const std::chrono::duration<double, std::milli>
                                            elapsed{timing.second - timing.first};
                                        std::printf("STARTUP full_scene_submit_reply_ms=%.3f\n", elapsed.count());
                                    }
                                }
                            );
                        }
                        if (measured && frame_limit)
                        {
                            writer.onCompletion(&completion_context, [](void* context, const auto& completion) noexcept
                            {
                                auto& output = *static_cast<decltype(completion_context)*>(context);
                                output.second->push_back(completion);
                                const bool retryable = completion.submitted_batches == 0 &&
                                    completion.requires_resynchronization &&
                                    (completion.error.type == render::kDeviceBusy ||
                                     completion.error.type == render::kFrameResynchronization);
                                if (completion.error.type && !retryable) *output.first = completion.error;
                            });
                        }
                        if (measured)
                        {
                            const auto now = std::chrono::steady_clock::now();
                            if (frames && frame_limit) frame_times.push_back(
                                std::chrono::duration<double, std::milli>(now - previous_frame).count()
                            );
                            previous_frame = now;
                            ++frames;
                        }
                        return {};
                    },
                    engine::EEngineStage::END_UPDATE
                );
                prepared = engine.run(
                    [&]() noexcept
                    {
                        window::LuxWindow::pollEvents();
                        if ((*window)->shouldClose() || frame_error.type || (frame_limit && frames >= frame_limit))
                        {
                            engine.requestStop();
                        }
                    },
                    [](engine::Engine::TimePoint deadline) noexcept
                    {
                        const auto seconds =
                            deadline == engine::Engine::TimePoint::max()
                                ? 0.05
                                : std::max(
                                      0.0,
                                      std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count()
                                  );
                        window::LuxWindow::waitEvents(std::min(seconds, 0.05));
                    }
                );
            }

            if (prepared && measured_begin != std::chrono::steady_clock::time_point{})
            {
                const auto finish = [&]() noexcept -> exec::task<Result>
                {
                    auto consumed =
                                co_await rendering::RenderFrameSubmission::instance().whenSubmittedFramesConsumed();
                    if (!consumed) co_return consumed;
                    const auto attachment = registry->get<rendering::RenderViewComponent>(view).attachment;
                    auto progress =
                                co_await context.renderClient().request<render::QueryExecutionProgress>(
                                    render::AttachmentId{attachment}
                                );
                    if (progress.error.type) co_return cxx::unexpected(progress.error);
                    execution_end = progress.progress.count(render::EExecutionOutcome::EXECUTED);
                    co_return Result{};
                };
                prepared = engine::test::runTask(finish());
            }
        } // Engine retires Scene and its accepted tasks while Context and provider dependencies remain alive.
        execution.setWake(nullptr);
    } // Context joins RenderServer and completes all accepted GPU work before the timestamp below.
    const auto gpu_drained = std::chrono::steady_clock::now();
    std::printf("STARTUP gpu_drained_including_shutdown_ms=%.3f\n",
                std::chrono::duration<double, std::milli>(gpu_drained - startup_begin).count());
    if (!frame_times.empty())
    {
        std::ranges::sort(frame_times);
        const auto elapsed = std::chrono::duration<double>(previous_frame - measured_begin).count();
        std::printf(
            "PROFILE_RESULT intervals=%zu seconds=%.6f main_fps=%.3f p50_ms=%.6f p95_ms=%.6f p99_ms=%.6f\n",
            frame_times.size(), elapsed, double(frame_times.size()) / elapsed, frame_times[frame_times.size() / 2],
            frame_times[frame_times.size() * 95 / 100], frame_times[frame_times.size() * 99 / 100]
        );
    }
    if (!prepared || frame_error.type || validation_errors.load())
    {
        const auto failure = !prepared ? prepared.error() : frame_error;
        std::fprintf(stderr, "Model example failed: %s\n", error::format(failure).c_str());
        return 1;
    }
    if (measured_begin != std::chrono::steady_clock::time_point{})
    {
        const auto submitted = std::ranges::count_if(completions, [](const auto& completion) noexcept
        {
            return completion.outcome == render::EFrameOutcome::SUBMITTED;
        });
        const double seconds = std::chrono::duration<double>(gpu_drained - measured_begin).count();
        std::printf("GPU_DRAINED executed_frames=%llu submission_replies=%zu submitted=%lld "
                    "seconds_including_shutdown=%.6f completed_fps_lower_bound=%.3f\n",
                    execution_end - execution_begin, completions.size(), submitted, seconds,
                    double(execution_end - execution_begin) / seconds);
    }
    std::printf("MODEL EXAMPLE completed frames=%u validation_errors=0\n", frames);
    return 0;
}
