#include <lux/engine/render/runtime/RenderClient.hpp>
#include "ModelSceneSupport.hpp"
#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/WindowSurface.hpp>
#include <lux/engine/systems/render/mesh/MeshInstanceReaction.hpp>
#include <lux/engine/systems/render/mesh/MeshVisibilitySystem.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp>
#include <lux/engine/systems/render/mesh/MeshTransformCollector.hpp>
#include <lux/engine/systems/render/model/ModelInstantiation.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <lux/engine/systems/render/view/ViewCollector.hpp>
#include <lux/engine/systems/render/view/ViewReaction.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/world/World.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <semaphore>

namespace
{
    using namespace lux;
    namespace rendering = engine::render;
    using Result = render::RenderResult<void>;
    using Clock = std::chrono::steady_clock;

    Result failure(const char* message) noexcept
    {
        std::fprintf(stderr, "MODEL FAIL: %s\n", message);
        return cxx::unexpected(error::Error{render::kRuntimeState});
    }

    using engine::test::createViewComponents;
    using engine::test::LightCollector;
    using engine::test::runTask;

    class ModelScene final
    {
    public:
        ModelScene(
            engine::EngineContext& context,
            window::LuxWindow& window,
            window::LuxWindow& second_window,
            bool hold,
            bool two_scenes,
            bool remove_loading,
            bool offscreen,
            std::optional<rendering::MeshUpload>& upload,
            std::optional<rendering::TextureUpload>& textures,
            std::optional<rendering::MaterialUpload>& materials
        ) noexcept
            : window_(window), second_window_(second_window), hold_(hold), two_scenes_(two_scenes),
              remove_loading_(remove_loading), offscreen_(offscreen), vfs_(context.assetVfs()), upload_(upload),
              texture_upload_(textures), material_upload_(materials), assets_(context.assets()), context_(context),
              engine_(context)
        {
        }

        engine::Engine& engine() noexcept
        {
            return engine_;
        }

        exec::task<Result> prepare(const std::filesystem::path& source, const std::filesystem::path& cooked) noexcept
        {
            auto& execution = execution::ExecutionRuntime::instance();
            auto io = execution.blockingIoScheduler();
            if (!io)
            {
                co_return cxx::unexpected(execution::toError(io.error()));
            }
            auto cooked_model = co_await engine::test::cookModelAssets(source, cooked);
            if (!cooked_model)
            {
                co_return cxx::unexpected(cooked_model.error());
            }
            product_ = std::move(*cooked_model);
            if (product_.meshes.size() != 1 || product_.model->data().primitives.size() != 1)
            {
                co_return failure("this lifecycle fixture requires one mesh primitive");
            }
            mesh_ = product_.meshes.front()->id();
            auto provider = co_await engine::test::openModelAssets(cooked);
            if (!provider)
            {
                co_return cxx::unexpected(provider.error());
            }
            auto mounted = vfs_.mount({"/Model", *provider});
            if (!mounted)
            {
                co_return failure("mount cooked mesh");
            }
            mount_.emplace(std::move(*mounted));
            std::printf(
                "COOK mesh vertices=%zu indices=%zu materials=%zu textures=%zu\n",
                product_.meshes.front()->data().vertices.size(),
                product_.meshes.front()->data().indices.size(),
                product_.materials.size(),
                product_.textures.size()
            );
            auto output = co_await createOutput();
            if (!output)
            {
                co_return output;
            }
            auto assembled = co_await assemble();
            if (!assembled || !remove_loading_)
            {
                co_return assembled;
            }
            // Test-owned IO gate: existing worker jobs precede repository reads. No production seam or new scope.
            io_blocked_ = true;
            for (unsigned index = 0; index < 2; ++index)
            {
                auto operation = stdexec::continues_on(
                    stdexec::then(
                        stdexec::schedule(*io),
                        [this]() noexcept
                        {
                            io_entered_.fetch_add(1, std::memory_order_release);
                            io_release_.acquire();
                        }
                    ),
                    execution.mainScheduler()
                );
                auto accepted = execution.spawn(stdexec::then(
                    execution::asResult(std::move(operation)),
                    [this](auto result) noexcept
                    {
                        if (!result)
                        {
                            std::abort(); // The fixture's IO gate must complete before its borrowed semaphore dies.
                        }
                        ++io_completed_;
                    }
                ));
                if (!accepted)
                {
                    co_return cxx::unexpected(execution::toError(accepted.error()));
                }
                ++io_accepted_;
            }
            co_return Result{};
        }

        Result run() noexcept
        {
            started_ = Clock::now();
            return engine_.run(
                [this]() noexcept
                {
                    window::LuxWindow::pollEvents();
                    if (window_.shouldClose() || second_window_.shouldClose())
                    {
                        engine_.requestStop();
                    }
                },
                [](Clock::time_point deadline) noexcept
                {
                    const auto seconds =
                        deadline == Clock::time_point::max()
                            ? 0.05
                            : std::max(0.0, std::chrono::duration<double>(deadline - Clock::now()).count());
                    window::LuxWindow::waitEvents(std::min(seconds, 0.05));
                }
            );
        }

        // Host orchestration: the actual roots enter the Engine queue, children never re-enqueue themselves.
        exec::task<Result> retire() noexcept
        {
            if (io_blocked_)
            {
                io_release_.release(2);
                io_blocked_ = false;
            }
            while (io_completed_ != io_accepted_)
            {
                co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
            }
            auto consumed = co_await rendering::RenderFrameSubmission::instance().whenSubmittedFramesConsumed();
            if (!consumed)
            {
                co_return consumed;
            }
            if (completed_frames_ != frames_)
            {
                co_return failure("frame replies did not settle before Scene retirement");
            }
            std::printf("FRAMES submitted=%llu completed=%llu\n", frames_, completed_frames_);
            if (scene_id_.valid() && !first_scene_removed_ && !loading_removed_)
            {
                auto removed = co_await engine_.removeScene(scene_id_);
                if (!removed)
                {
                    co_return removed;
                }
            }
            if (second_scene_id_.valid())
            {
                auto removed = co_await engine_.removeScene(second_scene_id_);
                if (!removed)
                {
                    co_return removed;
                }
            }
            registry_ = second_registry_ = nullptr;
            candidate_.reset();
            auto usage = assets_.usage(mesh_);
            if (usage && (usage->references || usage->data_readers || usage->use_readers))
            {
                co_return failure("asset borrows survived Scene removal");
            }
            std::puts("REMOVE Scene users released; Context still owns the shared asset cache");
            co_return Result{};
        }

        Result verifyRepositoryDestruction() noexcept
        {
            if (!upload_)
            {
                return {};
            }
            auto repository = engine::assets::AssetRepository::create(vfs_.view(), {64U << 20U, 128U << 20U, 16});
            if (!repository)
            {
                return cxx::unexpected(repository.error());
            }
            auto registered = (*repository)->registerUse<rendering::MeshUploadDefinition>(*upload_);
            if (!registered)
            {
                return registered;
            }
            render::MeshId resource;
            const auto load = [&]() noexcept -> exec::task<Result>
            {
                {
                    auto identity = (*repository)->acquire<asset::MeshAsset>(product_.meshes.front()->id());
                    if (!identity)
                    {
                        co_return cxx::unexpected(identity.error());
                    }
                    auto [first, second, cpu] = co_await stdexec::when_all(
                        (*repository)->use<rendering::MeshUploadDefinition>(*identity, *upload_),
                        (*repository)->use<rendering::MeshUploadDefinition>(*identity, *upload_),
                        (*repository)->read(*identity)
                    );
                    if (!first)
                    {
                        co_return cxx::unexpected(first.error());
                    }
                    if (!second)
                    {
                        co_return cxx::unexpected(second.error());
                    }
                    if (!cpu)
                    {
                        co_return cxx::unexpected(cpu.error());
                    }
                    if (**first != **second)
                    {
                        co_return failure("shared asset use did not reuse the same MeshId");
                    }
                    std::size_t uses = 0;
                    (*repository)
                        ->forEachUse(
                            identity->id(),
                            [&](auto, const void*, std::size_t count, bool, bool) noexcept { uses += count; }
                        );
                    if (uses != 2)
                    {
                        co_return failure("asset use query did not report both borrowers");
                    }
                    const auto usage = (*repository)->usage(identity->id());
                    if (!usage || usage->data_readers != 1 || usage->use_readers != 2 || usage->references == 0)
                    {
                        co_return failure("CPU and Render usage are not visible through the same repository");
                    }
                    std::puts("ASSET USE concurrent requests share MeshId; repository reports CPU=1, Render=2");
                    // Persist only the generated fixture, keeping old CPU/GPU borrows alive across publication.
                    auto written = co_await (*repository)->write(*identity, **cpu);
                    if (!written)
                    {
                        co_return cxx::unexpected(written.error());
                    }
                    auto refreshed = co_await (*repository)->use<rendering::MeshUploadDefinition>(*identity, *upload_);
                    if (!refreshed || **refreshed == **first || **first != **second)
                    {
                        co_return failure("source revision reused or invalidated an older borrowed result");
                    }
                    std::puts("REVISION new source produces a new MeshId while prior borrows retain the old result");
                    resource = **refreshed;
                }
                co_return Result{};
            };
            auto loaded = runTask(load());
            if (!loaded)
            {
                return loaded;
            }
            repository->reset(); // Outer Main boundary: RAII waits and releases the GPU use.
            const auto check = [&]() noexcept -> exec::task<Result>
            {
                auto stale = co_await upload_->release(resource);
                if (stale || stale.error().type != render::kStorageIdentity)
                {
                    co_return failure("Mesh survived repository RAII destruction");
                }
                co_return Result{};
            };
            return runTask(check());
        }

        bool verified() const noexcept
        {
            if (remove_loading_)
            {
                return loading_removed_ && !frame_error_.type;
            }
            return removed_ && recreated_ && resized_ && two_views_ && resumed_verified_ && last_good_verified_ &&
                   target_recreated_ && content_phase_ == 6 && completed_frames_ > 300 && !frame_error_.type &&
                   (!two_scenes_ || (first_scene_removed_ && second_view_rendered_));
        }

    private:
        exec::task<Result> createOutput() noexcept
        {
            auto& runtime = context_.renderClient();
            window_.framebufferSize(description_.width, description_.height);
            description_.kind = offscreen_ ? render::ETargetKind::OFFSCREEN : render::ETargetKind::SURFACE;
            description_.format = rdesc::ETextureFormat::RGBA8_UNORM;
            const render::EffectOutputDesc effect_output{
                description_.format, description_.samples, description_.kind, description_.sampled
            };
            auto scene = co_await engine::test::resolveComposition(context_.renderClient(), effect_, effect_output);
            if (!scene)
            {
                co_return scene;
            }
            for (const auto& contribution : effect_.contributions)
            {
                implementations_.push_back(contribution.implementation_id);
            }
            const std::array command_names{
                render::CommandName{render::Mesh3dCommands::UploadMesh::name},
                render::CommandName{render::Mesh3dCommands::ReleaseMesh::name},
                render::CommandName{render::ImageCommands::Upload::name},
                render::CommandName{render::ImageCommands::Release::name},
                render::CommandName{render::MaterialCommands::Create::name},
                render::CommandName{render::MaterialCommands::Release::name},
                render::CommandName{render::Mesh3dCommands::CreateInstances::name},
                render::CommandName{render::Mesh3dCommands::ReleaseInstance::name},
                render::CommandName{render::Lighting3dCommands::CreateLights::name}
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
            const auto create_lights = resolved_commands.ids[8];
            auto upload = rendering::MeshUpload::create(
                runtime, implementations_[0], upload_mesh, release_mesh
            );
            if (!upload)
            {
                co_return cxx::unexpected(upload.error());
            }
            upload_.emplace(std::move(*upload));
            auto registered = assets_.registerUse<rendering::MeshUploadDefinition>(*upload_);
            if (!registered)
            {
                co_return cxx::unexpected(registered.error());
            }
            auto textures = rendering::TextureUpload::create(
                runtime, implementations_[0], upload_texture, release_texture
            );
            auto materials = rendering::MaterialUpload::create(
                runtime, implementations_[0], create_material, release_material
            );
            if (!textures || !materials)
            {
                co_return failure("material/texture command lookup");
            }
            texture_upload_.emplace(std::move(*textures));
            material_upload_.emplace(std::move(*materials));
            release_instance_id_ = release_instance;
            create_lights_id_ = create_lights;
            const auto factory = [this, create = create_instances, release = release_instance](
                rendering::RenderSystem& system, render::CapabilityImplementationId selected
            ) noexcept
            {
                return rendering::MeshInstanceReaction::create(
                    system, selected, *upload_, *material_upload_, create, release
                );
            };
            auto mesh_factory = metadata_.addReaction<rendering::MeshInstanceReaction>(factory);
            if (!mesh_factory) co_return mesh_factory;
            if (two_scenes_)
            {
                auto second_factory = second_metadata_.addReaction<rendering::MeshInstanceReaction>(factory);
                if (!second_factory) co_return second_factory;
            }

            for (auto registered :
                 {assets_.registerUse<rendering::TextureUploadDefinition>(*texture_upload_),
                  assets_.registerUse<rendering::MaterialUploadDefinition>(*material_upload_, *texture_upload_)})
            {
                if (!registered)
                {
                    co_return cxx::unexpected(registered.error());
                }
            }
            co_return Result{};
        }

        exec::task<Result> assemble() noexcept
        {
            const render::EffectOutputDesc effect_output{
                description_.format, description_.samples, description_.kind, description_.sampled
            };
            auto world = std::make_unique<world::World>(world::WorldDescription{"Model", {{"mesh3d", {}}}});
            registry_ = &world->registry();
            auto& metadata = metadata_;
            for (auto registered :
                 {metadata.addSceneReaction<rendering::ViewReaction>(),
                  metadata.addCollector<rendering::MeshTransformCollector>(),
                  metadata.addCollector<rendering::ViewCollector<render::Mesh3dCapability>>(),
                  metadata.addCollector<LightCollector>(lights_),
                  metadata.addCollector<rendering::ViewCollector<render::Lighting3dCapability>>()})
            {
                if (!registered)
                {
                    co_return registered;
                }
            }
            // Production static-model expansion folds node transforms before Reaction subscriptions exist.
            auto entities = rendering::createModelEntities(*registry_, product_.model->data());
            if (!entities || entities->size() != 1)
            {
                co_return failure("static model expansion");
            }
            entity_ = entities->front();
            model_transform_ = registry_->get<ecs::Transform3D>(entity_);
            auto shared = rendering::createModelEntities(*registry_, product_.model->data());
            if (!shared || shared->size() != 1)
            {
                co_return failure("shared static model expansion");
            }
            shared_entity_ = shared->front();
            const Eigen::Affine3d placement = Eigen::Translation3d(model_transform_.translation) *
                                              model_transform_.rotation * Eigen::Scaling(model_transform_.scale);
            auto minimum = Eigen::Vector3d::Constant(std::numeric_limits<double>::max()).eval();
            Eigen::Vector3d maximum = -minimum;
            for (const auto& vertex : product_.meshes.front()->data().vertices)
            {
                const Eigen::Vector3d position = placement * vertex.position.cast<double>();
                minimum = minimum.cwiseMin(position);
                maximum = maximum.cwiseMax(position);
            }
            center_ = (minimum + maximum) * 0.5;
            scale_ = 2.0 / (maximum - minimum).maxCoeff();
            updateTransform(0);
            auto first_view = createViewComponents(*registry_, window_, description_, false);
            auto second_view = createViewComponents(*registry_, second_window_, description_, true);
            if (!first_view || !second_view)
            {
                co_return failure("View components");
            }
            view_entity_ = *first_view;
            second_view_entity_ = *second_view;
            auto system = rendering::RenderSystem::create(
                std::move(effect_),
                effect_output,
                *world,
                context_,
                metadata
            );
            if (!system)
            {
                co_return cxx::unexpected(system.error());
            }
            auto visibility = rendering::MeshVisibilitySystem::create(*world, context_, 0, 100, 120, 10, 1);
            if (!visibility) co_return cxx::unexpected(visibility.error());
            candidate_ =
                std::make_unique<scene::SceneInstance>(std::move(world), std::make_unique<simulation::Simulation>());
            rendering_system_ = system->get();
            candidate_->add(std::move(*visibility));
            candidate_->add(std::move(*system));
            auto id = engine_.addScene(candidate_);
            if (!id)
            {
                co_return cxx::unexpected(id.error());
            }
            scene_id_ = *id;
            if (auto initialized = co_await engine_.whenSceneInitialized(scene_id_); !initialized)
            {
                co_return initialized;
            }
            if (two_scenes_)
            {
                auto prepared = co_await engine::test::resolveComposition(
                    context_.renderClient(), second_effect_, effect_output
                );
                if (!prepared)
                {
                    co_return prepared;
                }
                auto second_world = std::make_unique<world::World>(world::WorldDescription{"Shared", {{"mesh3d", {}}}});
                second_registry_ = &second_world->registry();
                auto model = rendering::createModelEntities(*second_registry_, product_.model->data());
                if (!model || model->size() != 1)
                {
                    co_return failure("second Scene model expansion");
                }
                second_scene_entity_ = model->front();
                auto offscreen = description_;
                offscreen.kind = render::ETargetKind::OFFSCREEN;
                auto preparation_view = createViewComponents(*second_registry_, second_window_, offscreen, false);
                if (!preparation_view) co_return cxx::unexpected(preparation_view.error());
                auto& second_metadata = second_metadata_;
                for (auto registered :
                     {second_metadata.addSceneReaction<rendering::ViewReaction>(),
                      second_metadata.addCollector<rendering::MeshTransformCollector>(),
                      second_metadata.addCollector<rendering::ViewCollector<render::Mesh3dCapability>>(),
                      second_metadata.addCollector<LightCollector>(second_lights_),
                      second_metadata.addCollector<rendering::ViewCollector<render::Lighting3dCapability>>()})
                {
                    if (!registered)
                    {
                        co_return registered;
                    }
                }
                auto second_system = rendering::RenderSystem::create(
                    std::move(second_effect_),
                effect_output,
                    *second_world,
                    context_,
                        second_metadata
                );
                if (!second_system)
                {
                    co_return cxx::unexpected(second_system.error());
                }
                auto second_visibility = rendering::MeshVisibilitySystem::create(
                    *second_world, context_, 0, 100, 120, 10, 1
                );
                if (!second_visibility) co_return cxx::unexpected(second_visibility.error());
                candidate_ = std::make_unique<scene::SceneInstance>(
                    std::move(second_world),
                    std::make_unique<simulation::Simulation>()
                );
                second_rendering_system_ = second_system->get();
                candidate_->add(std::move(*second_visibility));
                candidate_->add(std::move(*second_system));
                auto second_id = engine_.addScene(candidate_);
                if (!second_id)
                {
                    co_return cxx::unexpected(second_id.error());
                }
                second_scene_id_ = *second_id;
                if (auto initialized = co_await engine_.whenSceneInitialized(second_scene_id_); !initialized)
                {
                    co_return initialized;
                }
            }
            engine_.addEngineStageCallback(
                [this]() noexcept { return beforeFrame(); },
                engine::EEngineStage::BEGIN_UPDATE
            );
            engine_.addEngineStageCallback(
                [this]() noexcept -> Result
                {
                    auto writer = rendering::RenderFrameSubmission::instance().writer();
                    writer.onCompletion(this, &frameCompleted);
                    ++frames_;
                    return {};
                },
                engine::EEngineStage::END_UPDATE
            );
            std::puts("ASSEMBLE production Reaction/Collector and Engine Scene complete");
            co_return Result{};
        }

        void updateTransform(double angle) noexcept
        {
            const Eigen::Quaterniond rotation{Eigen::AngleAxisd(angle, Eigen::Vector3d::UnitY())};
            if (second_registry_)
            {
                second_registry_->patch<ecs::Transform3D>(
                    second_scene_entity_,
                    [&](auto& value) noexcept
                    {
                        value.rotation = rotation * model_transform_.rotation;
                        value.scale = model_transform_.scale * scale_;
                        value.translation = rotation * ((model_transform_.translation - center_) * scale_);
                    }
                );
            }
            if (!registry_)
            {
                return;
            }
            for (auto entity : {entity_, shared_entity_})
            {
                registry_->patch<ecs::Transform3D>(
                    entity,
                    [&](auto& value) noexcept
                    {
                        value.rotation = rotation * model_transform_.rotation;
                        value.scale = model_transform_.scale * scale_ * 0.55;
                        value.translation = rotation * ((model_transform_.translation - center_) * scale_ * 0.55);
                        value.translation.x() += entity == entity_ ? -0.7 : 0.7;
                    }
                );
            }
        }

        static void frameCompleted(void* context, const render::FrameCompletion& completion) noexcept
        {
            auto& self = *static_cast<ModelScene*>(context);
            // A structural request may end an output-blocked, unsubmitted frame. The production
            // frame stream recollects it; partially accepted work and ordinary failures remain errors.
            const bool retry_unsubmitted = completion.outcome == render::EFrameOutcome::NOT_SUBMITTED &&
                completion.error.type == render::kDeviceBusy;
            const bool reject_stale = completion.outcome == render::EFrameOutcome::REJECTED &&
                completion.error.type == render::kFrameResynchronization;
            const bool retryable = (retry_unsubmitted || reject_stale) && completion.submitted_batches == 0 &&
                completion.requires_resynchronization && completion.failed_entry == UINT32_MAX;
            const bool failed = completion.error.type && !self.frame_error_.type && !retryable;
            if (failed)
            {
                self.frame_error_ = completion.error;
                std::fprintf(
                    stderr,
                    "FRAME %llu failed entry=%u outcome=%u: %s\n",
                    completion.sequence,
                    completion.failed_entry,
                    unsigned(completion.outcome),
                    error::format(completion.error).c_str()
                );
            }
            ++self.completed_frames_;
        }

        exec::task<Result> beforeFrame() noexcept
        {
            if (frame_error_.type)
            {
                co_return cxx::unexpected(frame_error_);
            }
            if (Clock::now() - started_ > std::chrono::seconds(60) && !hold_)
            {
                co_return failure("scenario timeout");
            }
            if (!first_scene_removed_ && !loading_removed_)
            {
                const bool ready = !rendering_system_->sceneId().isNull() &&
                                   (!two_scenes_ || !second_rendering_system_->sceneId().isNull());
                if (!ready)
                {
                    co_return Result{};
                }
                destination_ = {rendering_system_->effectId(), 0, 0, rendering_system_->sceneId()};
                if (lights_.empty() && !remove_loading_)
                {
                    auto& runtime = context_.renderClient();
                    std::array<render::VDirectLight, 1> lights{
                        render::DirectionalLightGpu{{3, 3, 3, 0}, {-0.4F, -0.8F, -0.4472136F, 0}}
                    };
                    render::Lighting3dCommands::CreateLights::Request light_request{
                        implementations_[1],
                        std::get<render::SceneId>(destination_.scope),
                        {0, lights, {}}
                    };
                    auto lit = co_await runtime.request<render::Lighting3dCommands::CreateLights>(
                        create_lights_id_, light_request
                    );
                    if (lit.error.type)
                    {
                        co_return cxx::unexpected(lit.error);
                    }
                    std::puts("CREATE light accepted before first frame");
                    lights_.push_back({lit.lights.front(), lights.front(), {}});
                    if (two_scenes_)
                    {
                        std::array<render::VDirectLight, 1> descriptions{
                            render::DirectionalLightGpu{{3, 3, 3, 0}, {-0.4F, -0.8F, -0.4472136F, 0}}
                        };
                        render::Lighting3dCommands::CreateLights::Request request{
                            implementations_[1],
                            second_rendering_system_->sceneId(),
                            {0, descriptions, {}}
                        };
                        auto lights =
                            co_await context_.renderClient().request<render::Lighting3dCommands::CreateLights>(
                                create_lights_id_, request
                            );
                        if (lights.error.type)
                        {
                            co_return cxx::unexpected(lights.error);
                        }
                        second_lights_.push_back({lights.lights.front(), descriptions.front(), {}});
                    }
                }
            }
            if (remove_loading_)
            {
                while (io_entered_.load(std::memory_order_acquire) != 2)
                {
                    co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
                }
                const auto usage = assets_.usage(mesh_);
                // Visibility starts CPU reads before Render resource preparation. Both IO workers
                // remain blocked; waiting for a GPU-use record would prevent this scenario from progressing.
                if (usage && usage->references != 0 && !usage->data_resident)
                {
                    const auto scheduler = execution::ExecutionRuntime::instance().mainScheduler();
                    // Engine removal first crosses one Main safe point. Two FIFO scheduler hops keep
                    // IO blocked until that retirement request has run, without a timing-based sleep.
                    auto release_io = stdexec::let_value(
                        stdexec::schedule(scheduler),
                        [this, scheduler]() noexcept
                        {
                            return stdexec::then(
                                stdexec::schedule(scheduler),
                                [this]() noexcept
                                {
                                    io_release_.release(2);
                                    io_blocked_ = false;
                                }
                            );
                        }
                    );
                    auto removed = co_await stdexec::then(
                        stdexec::when_all(engine_.removeScene(scene_id_), std::move(release_io)),
                        [](Result result) noexcept { return result; }
                    );
                    if (!removed)
                    {
                        co_return removed;
                    }
                    registry_ = nullptr;
                    loading_removed_ = true;
                    std::puts("LOADING Scene removed with CPU read roots accepted, data absent and IO workers blocked");
                    engine_.requestStop();
                }
                co_return Result{};
            }
            if (first_scene_removed_)
            {
                updateTransform(double(frames_) * 0.012);
                if (!rendering::findRenderTarget(*second_registry_, second_view_entity_).isNull() &&
                    frames_ > second_visible_frame_ + 60)
                {
                    second_view_rendered_ = true;
                    if (!hold_)
                    {
                        engine_.requestStop();
                    }
                }
                co_return Result{};
            }
            if (two_scenes_ && resized_ && content_phase_ == 6 && frames_ > 350)
            {
                auto removed = co_await engine_.removeScene(scene_id_);
                if (!removed)
                {
                    co_return removed;
                }
                registry_ = nullptr;
                first_scene_removed_ = true;
                const auto* retained =
                    second_registry_->try_get<rendering::MeshRenderResourceComponent>(second_scene_entity_);
                if (!retained || *retained->mesh != old_mesh_)
                {
                    co_return failure("shared Mesh lost after original Scene/Effect retirement");
                }
                auto view = createViewComponents(*second_registry_, second_window_, description_, false);
                if (!view)
                {
                    co_return cxx::unexpected(view.error());
                }
                second_view_entity_ = *view;
                second_visible_frame_ = frames_;
                std::puts("SCENE original Scene/Effect removed; surviving Scene retains Mesh and creates its View");
                co_return Result{};
            }
            const auto update_extent = [&](ecs::Entity entity, window::LuxWindow& window) noexcept
            {
                std::uint32_t width, height;
                window.framebufferSize(width, height);
                const auto& desired = registry_->get<rendering::RenderTargetComponent>(entity).description;
                if (desired.width != width || desired.height != height)
                {
                    registry_->patch<rendering::RenderTargetComponent>(
                        entity,
                        [&](auto& target) noexcept
                        {
                            target.description.width = width;
                            target.description.height = height;
                        }
                    );
                }
            };
            if (two_views_ && first_visible_ && frames_ >= first_visible_ + 30 && !pause_started_)
            {
                registry_->patch<rendering::RenderTargetComponent>(
                    view_entity_,
                    [](auto& target) noexcept
                    {
                        target.description.width = 0;
                        target.description.height = 0;
                    }
                );
                pause_started_ = true;
            }
            if (!pause_started_ || pause_verified_)
            {
                update_extent(view_entity_, window_);
            }
            if (registry_->all_of<rendering::RenderTargetComponent>(second_view_entity_))
            {
                update_extent(second_view_entity_, second_window_);
            }
            if (pause_started_ && !resumed_verified_)
            {
                const auto& first = registry_->get<rendering::RenderViewComponent>(view_entity_);
                const auto& second = registry_->get<rendering::RenderViewComponent>(second_view_entity_);
                auto a = co_await context_.renderClient().request<render::QueryExecutionProgress>(
                    render::AttachmentId{first.attachment}
                );
                auto b = co_await context_.renderClient().request<render::QueryExecutionProgress>(
                    render::AttachmentId{second.attachment}
                );
                if (a.error.type || b.error.type)
                {
                    co_return failure("View execution progress");
                }
                const auto first_count = a.progress.count(render::EExecutionOutcome::EXECUTED);
                const auto second_count = b.progress.count(render::EExecutionOutcome::EXECUTED);
                if (!pause_verified_)
                {
                    pause_verified_ =
                        first_count > 0 && first_count == pause_counts_[0] && second_count > pause_counts_[1];
                    pause_counts_ = {first_count, second_count};
                }
                else if (first_count > pause_counts_[0])
                {
                    resumed_verified_ = true;
                    std::puts("PAUSE zero extent stopped one View while the other advanced; positive extent resumed");
                }
            }
            const auto first_target = rendering::findRenderTarget(*registry_, view_entity_);
            const auto second_target = rendering::findRenderTarget(*registry_, second_view_entity_);
            const auto* first_description = rendering::renderTargetDescription(*registry_, view_entity_);
            const auto* second_description = rendering::renderTargetDescription(*registry_, second_view_entity_);
            if (!first_target.isNull() && !second_target.isNull())
            {
                if (first_target == second_target)
                {
                    co_return failure("two Views share a Target identity");
                }
                if (!two_views_)
                {
                    second_description_ = *second_description;
                    two_views_ = true;
                    std::puts("VIEWS perspective/orthographic outputs independently adopted");
                }
                else if (*second_description != second_description_)
                {
                    co_return failure("first window resize changed the second View");
                }
            }
            auto& runtime = context_.renderClient();
            if (recreated_ && first_description && first_description->width != description_.width)
            {
                description_ = *first_description;
                auto stats = co_await runtime.request<lux::render::QueryRenderStats>({});
                if (stats.error.type || stats.compiled_graphs != (two_scenes_ ? 2 : 1))
                {
                    co_return failure("Resize recompiled Effect");
                }
                resized_ = true;
                std::printf(
                    "RESIZE ECS adopted %ux%u; second View unchanged; compiled_graphs=%llu\n",
                    description_.width,
                    description_.height,
                    stats.compiled_graphs
                );
            }
            if (registry_->valid(entity_))
            {
                updateTransform(double(frames_) * 0.012);
            }
            const auto* shadow = registry_->try_get<rendering::MeshRenderResourceComponent>(entity_);
            const auto* shared = registry_->try_get<rendering::MeshRenderResourceComponent>(shared_entity_);
            const auto* other_scene =
                second_registry_
                    ? second_registry_->try_get<rendering::MeshRenderResourceComponent>(second_scene_entity_)
                    : nullptr;
            if (shadow && shared && (!two_scenes_ || other_scene) && first_visible_ == 0)
            {
                if (*shadow->mesh != *shared->mesh || *shadow->materials.front() != *shared->materials.front() ||
                    shadow->instances.front() == shared->instances.front())
                {
                    co_return failure("two entities did not share assets with independent instances");
                }
                if (other_scene && (*other_scene->mesh != *shadow->mesh ||
                                    *other_scene->materials.front() != *shadow->materials.front()))
                {
                    co_return failure("two Scenes did not share asset use results");
                }
                std::puts("SHARE entities/Scenes use the same Mesh/Material and independent Instances");
                first_visible_ = frames_ + 1;
                old_mesh_ = *shadow->mesh;
                old_instance_ = shadow->instances.front();
                std::puts("ADOPT pre-existing Mesh3D -> production MeshInstanceReaction -> private shadow");
            }
            if (first_visible_ && frames_ == first_visible_ + 10)
            {
                registry_->patch<ecs::RenderableMeshComponent>(entity_, [](auto& value) noexcept
                { value.keep_resources_when_hidden = true; });
                registry_->emplace<ecs::VisibilityComponent>(entity_, false);
            }
            if (first_visible_ && frames_ == first_visible_ + 20)
            {
                if (!shadow || shadow->instances.front() != old_instance_)
                    co_return failure("hidden retained representation recreated or lost its instance");
                std::puts("HIDDEN retained instance identity while Transform continues changing");
            }
            if (first_visible_ && frames_ == first_visible_ + 25)
            {
                registry_->patch<ecs::VisibilityComponent>(entity_, [](auto& value) noexcept { value.is_visible = true; });
            }
            if (first_visible_ && frames_ == first_visible_ + 35)
            {
                if (!shadow || shadow->instances.front() != old_instance_)
                    co_return failure("restoring visibility recreated the retained instance");
                registry_->patch<ecs::RenderableMeshComponent>(entity_, [](auto& value) noexcept
                { value.keep_resources_when_hidden = false; });
                std::puts("VISIBLE restored the same instance with current Transform");
            }
            if (first_visible_ && frames_ == first_visible_ + 60)
            {
                last_good_instance_ = shared->instances.front();
                registry_->patch<ecs::MaterialComponent>(
                    shared_entity_,
                    [](auto& value) noexcept
                    {
                        value.materials.clear(); // Invalid replacement must not discard the adopted Instance.
                    }
                );
            }
            if (first_visible_ && frames_ == first_visible_ + 90)
            {
                if (!shared || shared->instances.front() != last_good_instance_ || *shared->mesh != old_mesh_)
                {
                    co_return failure("invalid material replacement discarded last-good resources");
                }
                last_good_verified_ = true;
                std::puts("LAST GOOD invalid material replacement retained the adopted Mesh/Material/Instance");
            }
            if (first_visible_ && frames_ == first_visible_ + 95)
            {
                detached_target_ = registry_->get<rendering::RenderTargetComponent>(second_view_entity_);
                previous_target_ = rendering::findRenderTarget(*registry_, second_view_entity_);
                registry_->remove<rendering::RenderTargetComponent>(second_view_entity_);
            }
            if (detached_target_ && rendering::findRenderTarget(*registry_, second_view_entity_).isNull())
            {
                registry_->emplace<rendering::RenderTargetComponent>(second_view_entity_, *detached_target_);
                detached_target_.reset();
            }
            const auto replacement = rendering::findRenderTarget(*registry_, second_view_entity_);
            if (!previous_target_.isNull() && !replacement.isNull() && replacement != previous_target_ &&
                !target_recreated_)
            {
                target_recreated_ = true;
                std::puts("TARGET component removed/re-added; old output released before replacement adoption");
            }
            if (first_visible_ && frames_ == first_visible_ + 120)
            {
                registry_->remove<ecs::Mesh3D>(entity_);
                std::puts("REMOVE Mesh3D: no explicit release from test");
            }
            if (first_visible_ && frames_ > first_visible_ + 120 && !removed_)
            {
                auto usage = assets_.usage(mesh_);
                if (usage && usage->use_readers == (two_scenes_ ? 2 : 1) && !shadow && shared)
                {
                    render::Mesh3dCommands::ReleaseInstance::Request instance{old_instance_};
                    auto stale_instance = co_await runtime.request<render::Mesh3dCommands::ReleaseInstance>(
                        release_instance_id_, instance
                    );
                    if (stale_instance.error.type != render::kStorageIdentity || *shared->mesh != old_mesh_)
                    {
                        co_return failure("remaining shared instance lost its resource");
                    }
                    removed_ = true;
                    std::puts("RELEASE first Instance rejected; remaining entity retains shared Mesh/Material");
                    registry_->emplace<ecs::Mesh3D>(entity_, rdesc::MeshVisualDescription{mesh_});
                }
            }
            if (removed_ && shadow && !recreated_)
            {
                if (*shadow->mesh != old_mesh_ || shadow->instances.front() == old_instance_)
                {
                    co_return failure("shared asset or new instance generation mismatch");
                }
                recreated_ = true;
                std::puts("RECREATE shared Mesh reused; new Instance generation; Transform Collector continues");
                auto state = window_.state();
                if (!state)
                {
                    co_return failure("window placement query");
                }
                auto placement = state->placement;
                placement.normal.width = 1100;
                placement.normal.height = 700;
                if (!window_.applyPlacement(placement))
                {
                    co_return failure("window resize request");
                }
            }
            if (recreated_ && content_phase_ == 0 && frames_ >= first_visible_ + 180)
            {
                registry_->patch<ecs::MaterialComponent>(shared_entity_, [&](auto& value) noexcept
                { value.materials = registry_->get<ecs::MaterialComponent>(entity_).materials; });
                old_instance_ = shadow->instances.front();
                auto asset = assets_.acquire<asset::MeshAsset>(mesh_);
                if (!asset) co_return cxx::unexpected(asset.error());
                auto updated = co_await assets_.write(*asset, product_.meshes.front()->data());
                if (!updated) co_return cxx::unexpected(updated.error());
                content_phase_ = 1;
                co_return Result{}; // Registry storage may have changed while this test root suspended.
            }
            if (content_phase_ == 1 && shadow && shared && *shadow->mesh != old_mesh_ &&
                *shadow->mesh == *shared->mesh && (!two_scenes_ || (other_scene && *other_scene->mesh == *shadow->mesh)))
            {
                if (shadow->instances.front() == old_instance_)
                    co_return failure("same-id Mesh content change did not replace the old representation");
                old_mesh_ = *shadow->mesh;
                old_instance_ = shadow->instances.front();
                old_material_ = *shadow->materials.front();
                std::puts("CONTENT Mesh same-id change rebuilt all active representations through Reaction");
                if (product_.textures.empty())
                {
                    content_phase_ = 3;
                }
                else
                {
                    auto texture = assets_.acquire<asset::TextureAsset>(product_.textures.front()->id());
                    if (!texture) co_return cxx::unexpected(texture.error());
                    auto updated = co_await assets_.write(*texture, product_.textures.front()->data());
                    if (!updated) co_return cxx::unexpected(updated.error());
                    content_phase_ = 2;
                    co_return Result{};
                }
            }
            if (content_phase_ == 2 && shadow && shared && *shadow->materials.front() != old_material_ &&
                *shadow->materials.front() == *shared->materials.front() &&
                (!two_scenes_ || (other_scene && *other_scene->materials.front() == *shadow->materials.front())))
            {
                if (*shadow->mesh != old_mesh_ || shadow->instances.front() == old_instance_)
                    co_return failure("Texture dependency refresh lost Mesh reuse or retained the stale instance");
                content_phase_ = 3;
                std::puts("CONTENT Texture change invalidated Material uses and all active Reactions refreshed");
            }
            if (content_phase_ == 3 && shadow)
            {
                old_instance_ = shadow->instances.front();
                registry_->patch<ecs::VisibilityComponent>(entity_, [](auto& value) noexcept { value.is_visible = false; });
                content_phase_ = 4;
                co_return Result{};
            }
            if (content_phase_ == 4 && !shadow && shared)
            {
                const auto usage = assets_.usage(mesh_);
                if (usage && usage->use_readers == (two_scenes_ ? 2 : 1))
                {
                    render::Mesh3dCommands::ReleaseInstance::Request request{old_instance_};
                    const auto released = co_await runtime.request<render::Mesh3dCommands::ReleaseInstance>(
                        release_instance_id_, request
                    );
                    if (released.error.type != render::kStorageIdentity)
                        co_return failure("hidden non-retained instance was not actually released");
                    registry_->patch<ecs::VisibilityComponent>(entity_, [](auto& value) noexcept { value.is_visible = true; });
                    content_phase_ = 5;
                    co_return Result{};
                }
            }
            if (content_phase_ == 5 && shadow)
            {
                if (shadow->instances.front() == old_instance_ || *shadow->mesh != old_mesh_)
                    co_return failure("hidden non-retained restore lost shared Mesh reuse or reused stale instance");
                content_phase_ = 6;
                std::puts("HIDDEN RELEASE no-retention removed Instance; restore reused shared Mesh with a new Instance");
            }
            if (!hold_ && verified())
            {
                // Keep the second entity alive: final Engine retirement must release its shadow automatically.
                engine_.requestStop();
            }
            co_return Result{};
        }

        window::LuxWindow& window_;
        window::LuxWindow& second_window_;
        bool hold_;
        bool two_scenes_;
        bool remove_loading_;
        bool offscreen_;
        std::counting_semaphore<2> io_release_{0};
        std::atomic_uint io_entered_{};
        unsigned io_completed_ = 0, io_accepted_ = 0;
        bool io_blocked_ = false, loading_removed_ = false;
        toolchain::ModelCookProduct product_;
        asset::AssetVfs& vfs_;
        std::optional<asset::MountLease> mount_;
        std::optional<rendering::MeshUpload>& upload_;
        std::optional<rendering::TextureUpload>& texture_upload_;
        std::optional<rendering::MaterialUpload>& material_upload_;
        engine::assets::AssetRepository& assets_;
        std::vector<render::DirectLightUpdate> lights_, second_lights_;
        render::CapabilityComposition effect_, second_effect_;
        rendering::RenderCapabilityMetaRegistry metadata_, second_metadata_;
        rendering::RenderSystem* rendering_system_ = nullptr;
        rendering::RenderSystem* second_rendering_system_ = nullptr;
        scene::SceneInstanceId scene_id_, second_scene_id_;
        render::FrameDataDestination destination_;
        std::vector<render::CapabilityImplementationId> implementations_;
        render::TargetDesc description_;
        render::TargetDesc second_description_;
        std::unique_ptr<scene::SceneInstance> candidate_;
        render::CommandId create_lights_id_;
        render::CommandId release_instance_id_;
        engine::EngineContext& context_;
        engine::Engine engine_;
        ecs::Registry* registry_ = nullptr;
        ecs::Registry* second_registry_ = nullptr;
        ecs::Entity second_scene_entity_ = ecs::NullEntity;
        ecs::Entity entity_ = ecs::NullEntity;
        ecs::Entity shared_entity_ = ecs::NullEntity;
        ecs::Entity view_entity_ = ecs::NullEntity;
        ecs::Entity second_view_entity_ = ecs::NullEntity;
        asset::AssetId mesh_;
        render::MeshId old_mesh_;
        render::MaterialId old_material_;
        unsigned content_phase_ = 0;
        render::MeshInstanceId old_instance_, last_good_instance_;
        ecs::Transform3D model_transform_;
        Eigen::Vector3d center_;
        double scale_ = 1;
        Clock::time_point started_;
        render::RenderError frame_error_;
        std::uint64_t frames_ = 0, first_visible_ = 0, completed_frames_ = 0;
        bool removed_ = false, recreated_ = false, resized_ = false, two_views_ = false;
        bool first_scene_removed_ = false, second_view_rendered_ = false;
        bool pause_started_ = false, pause_verified_ = false, resumed_verified_ = false;
        bool last_good_verified_ = false;
        bool target_recreated_ = false;
        std::optional<rendering::RenderTargetComponent> detached_target_;
        render::TargetId previous_target_;
        std::array<std::uint64_t, 2> pause_counts_{UINT64_MAX, 0};
        std::uint64_t second_visible_frame_ = 0;
    };
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: engine_model_scene_test <model.gltf> <cooked-directory> [--hold]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool hold = false, two_scenes = false, remove_loading = false, offscreen = false, flat = false, fif2 = false;
    for (int argument = 3; argument < argc; ++argument)
    {
        hold = hold || std::string_view(argv[argument]) == "--hold";
        two_scenes = two_scenes || std::string_view(argv[argument]) == "--two-scenes";
        remove_loading = remove_loading || std::string_view(argv[argument]) == "--remove-loading";
        offscreen = offscreen || std::string_view(argv[argument]) == "--offscreen";
        flat = flat || std::string_view(argv[argument]) == "--flat";
        fif2 = fif2 || std::string_view(argv[argument]) == "--fif2";
    }
    auto platform = window::GlfwRuntime::create();
    auto window = window::LuxWindow::create({960, 640, "LuxWindow + Engine / Shared Model"});
    auto second_window = window::LuxWindow::create({720, 540, "LuxWindow + Engine / Orthographic"});
    if (!platform || !window || !second_window)
    {
        return 1;
    }
    std::atomic_uint validation_errors{};
    render::RenderServerConfig configuration;
    configuration.channel_name = "lux.engine.model_scene";
    configuration.validation = true;
    configuration.uploads.emplace();
    configuration.frames_in_flight = fif2 ? 2 : 3;
    configuration.max_frames_ahead = 2;
    configuration.diagnostics = {
        &validation_errors,
        [](void* state, const render::RenderDiagnostic& diagnostic) noexcept
        {
            if (diagnostic.severity == render::ERenderDiagnosticSeverity::ERROR_MESSAGE)
            {
                ++*static_cast<std::atomic_uint*>(state);
            }
            const auto sink = rendering::renderDiagnostics();
            sink.report(sink.context, diagnostic);
        }
    };
    render::DeviceOptions device;
    device.texture_descriptors = true;
    device.buffer_device_address = !flat;
    device.multiple_queues = true;
    bool passed = false;
    std::optional<rendering::MeshUpload> upload;
    std::optional<rendering::TextureUpload> textures;
    std::optional<rendering::MaterialUpload> materials;
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
        auto& execution = execution::ExecutionRuntime::instance();
        execution.setWake(&window::LuxWindow::wakeEvents);
        {
            ModelScene scene{
                **initialization,
                **window,
                **second_window,
                hold,
                two_scenes,
                remove_loading,
                offscreen,
                upload,
                textures,
                materials
            };
            auto prepared = runTask(scene.prepare(argv[1], argv[2]));
            // Resize completes in BEGIN_UPDATE before System/Collector writes into the admitted frame.
            auto ran = prepared ? scene.run() : prepared;
            if (!ran)
            {
                std::fprintf(stderr, "Run: %s\n", error::format(ran.error()).c_str());
                std::fprintf(
                    stderr,
                    "Error details: %llu/%llu/%llu\n",
                    ran.error().args[0],
                    ran.error().args[1],
                    ran.error().args[2]
                );
            }
            passed = bool(ran) && scene.verified();
            auto retired = runTask(scene.retire());
            if (!retired)
            {
                std::fprintf(stderr, "Retirement: %s\n", error::format(retired.error()).c_str());
                std::abort();
            }
            passed = passed && scene.verified();
            auto repository_released = scene.verifyRepositoryDestruction();
            passed = passed && bool(repository_released);
        }
        execution.setWake(nullptr);
    }
    std::printf("MODEL SCENE %s validation_errors=%u\n", passed ? "PASS" : "FAIL", validation_errors.load());
    return passed && validation_errors == 0 ? 0 : 1;
}
