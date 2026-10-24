#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/editor/detail/RenderCommandRequest.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/project/SceneRegistrations.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/ui/Command.hpp>

#include <algorithm>
#include <map>

namespace lux::editor
{
    using detail::requestRenderCommand;

    namespace
    {
        constexpr std::size_t scene_byte_limit = 256U << 20U;

        template <class E> VProjectSceneFailure taskFailure(const execution::TTaskError<E>& failure) noexcept
        {
            if (auto* domain = failure.domainFailure())
            {
                return VProjectSceneFailure{*domain};
            }
            if (auto* scheduler_error = failure.executionFailure())
            {
                return execution::toError(*scheduler_error);
            }
            return EProjectSceneError::CLOSED;
        }

        ProjectSceneResult<std::shared_ptr<const scene::SceneDescription>> previewSystems(
            const scene::ScenePackage& package,
            ESceneRunMode mode
        ) noexcept
        {
            const auto& source = package.scene->data();
            if (mode == ESceneRunMode::GRAPHICAL)
            {
                return std::shared_ptr<const scene::SceneDescription>{package.scene, &source};
            }
            scene::SceneDescriptionBuilder builder;
            builder.setWorld(source.world());
            builder.setSimulation(source.simulation());
            const auto is_presentation = [](scene::SceneSystemDescription system) noexcept
            { return system.type().name == scene::RenderSystem::Description.canonical_name; };
            for (std::size_t i = 0; i < source.systemCount(); ++i)
            {
                const auto system = source.systemAt(i);
                if (is_presentation(system))
                {
                    continue; // Explicit optional presentation selection in this application composition.
                }
                if (!builder.addSystem(
                        system.instanceId(),
                        system.instanceName(),
                        system.type(),
                        system.version(),
                        system.configurationSchemaName(),
                        system.configurationSchemaVersion(),
                        system.configurationPayload()
                    ))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
                for (std::size_t j = 0; j < system.requirementBindingCount(); ++j)
                {
                    const auto binding = system.requirementBindingAt(j);
                    if (!builder.bindRequirement(system.instanceId(), binding.requirement(), binding.provider()))
                    {
                        return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                    }
                }
            }
            for (std::size_t i = 0; i < source.dependencyCount(); ++i)
            {
                const auto dependency = source.dependencyAt(i);
                // Do not erase a dependency on an omitted system and pretend the remaining program is valid.
                if (is_presentation(source.findSystem(dependency.after())))
                {
                    continue;
                }
                const bool omitted = is_presentation(source.findSystem(dependency.before()));
                if (omitted || !builder.addDependency(dependency.before(), dependency.after()))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            auto resolved = std::move(builder).buildResolved();
            if (!resolved)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            // SceneRuntime Builder still checks all data/service requirements for the selected program.
            return std::make_shared<const scene::SceneDescription>(std::move(*resolved));
        }
    } // namespace

    struct ProjectScenes::Impl final
    {
        enum class ECloseCondition
        {
            VIEWPORT,
            PLAY,
            SAVE,
            ALL
        };

        struct CloseWaiter
        {
            CloseWaiter* next{};
            ECloseCondition kind;
            SceneViewportId viewport;
            asset::AssetId document;
            void (*complete)(CloseWaiter&) noexcept {};
            std::shared_ptr<const void> code_lifetime;
        };

        struct CloseCompletionSender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
            Impl& collection;
            ECloseCondition kind;
            SceneViewportId viewport{};
            asset::AssetId document{};

            template <class Receiver> struct Operation final : CloseWaiter
            {
                using operation_state_concept = stdexec::operation_state_t;
                Impl& collection;
                Receiver receiver;

                Operation(const CloseCompletionSender& sender, Receiver receiver) noexcept
                    : collection(sender.collection), receiver(std::move(receiver))
                {
                    this->kind = sender.kind;
                    this->viewport = sender.viewport;
                    this->document = sender.document;
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](CloseWaiter& waiter) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver));
                    };
                }

                void start() & noexcept
                {
                    collection.awaitClose(*this);
                }
            };

            template <class Receiver> auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{*this, std::move(receiver)};
            }
        };

        bool closeConditionSatisfied(const CloseWaiter& waiter) const noexcept
        {
            if (waiter.kind == ECloseCondition::VIEWPORT)
            {
                return !viewports.find(waiter.viewport);
            }
            if (waiter.kind == ECloseCondition::ALL)
            {
                return operations == 0 && documents.empty() && viewports.empty();
            }
            const auto entry = documents.find(waiter.document);
            return entry == documents.end() ||
                   (waiter.kind == ECloseCondition::PLAY ? !entry->second->play : !entry->second->saving);
        }

        static void deliverClose(CloseWaiter& waiter) noexcept
        {
            auto code_lifetime = std::move(waiter.code_lifetime);
            waiter.complete(waiter);
        }

        void awaitClose(CloseWaiter& waiter) noexcept
        {
            if (closeConditionSatisfied(waiter))
            {
                deliverClose(waiter);
                return;
            }
            waiter.next = std::exchange(close_waiters, &waiter);
        }

        void completeClosures() noexcept
        {
            auto** link = &close_waiters;
            while (*link)
            {
                auto* waiter = *link;
                if (!closeConditionSatisfied(*waiter))
                {
                    link = &waiter->next;
                    continue;
                }
                *link = waiter->next;
                deliverClose(*waiter);
            }
        }

        struct GeometryRequests final
        {
            Impl* collection{};
            scene::SceneInstanceId instance;
        };

        struct Play final
        {
            scene::SceneInstanceId instance;
            scene::RenderAssetInput inputs;
            scene::RenderSystem* rendering{};
            std::array<entt::scoped_connection, 3> camera_connections;
            bool stopping{};
            GeometryRequests geometry;
        };

        struct Entry final
        {
            std::unique_ptr<SceneDocument> document;
            std::filesystem::path path;
            scene::RenderAssetInput inputs;
            scene::RenderSystem* rendering{};
            std::array<entt::scoped_connection, 3> camera_connections;
            std::optional<Play> play;
            bool closing{}, saving{};
            GeometryRequests geometry;
        };

        struct Viewport final
        {
            SceneViewportId id;
            asset::AssetId document;
            ViewportCameraSelection selection;
            simulation::ecs::Entity camera{simulation::ecs::NullEntity};
            simulation::ecs::Entity free_camera{simulation::ecs::NullEntity};
            scene::CameraTargetId camera_target;
            bool camera_dirty = true;
            render::TargetDesc requested;
            render::TargetDesc applied;
            render::TargetId target;
            std::uint64_t target_epoch = 0;
            rdesc::UiImageToken image;
            render::RenderError error, camera_error;
            bool closing{}, pending{};
            std::uint64_t failed_revision{}, requested_revision = 1;
            const simulation::ecs::Registry* registry{};
            bool play{};
        };

        ProjectScenes& collection;
        EditorContext& context;
        ProjectAssets& project_assets;
        object::Connection assets_changed, geometry_changed;
        std::map<asset::AssetId, std::vector<scene::SceneInstanceId>> geometry_consumers;
        rendering::RenderExecution* rendering;
        rendering::RenderAssets* assets;
        EditorUiScene* ui;
        std::map<asset::AssetId, std::unique_ptr<Entry>> documents;
        cxx::StableSlotMap<Viewport, SceneViewportTag> viewports;
        std::map<const simulation::ecs::Registry*, std::vector<SceneViewportId>> camera_viewports;
        std::optional<VProjectSceneFailure> failure;
        std::size_t operations{};
        CloseWaiter* close_waiters{};
        std::uint64_t asset_revision{}, asset_source{};
        asset::AssetId active_document;
        bool closing{}, catalog_write_pending{};

        Impl(ProjectScenes& collection, EditorContext& context, ProjectAssets& project_assets) noexcept
            : collection(collection), context(context), project_assets(project_assets),
              rendering(context.extensions().find<rendering::RenderExecution>()),
              assets(context.extensions().find<rendering::RenderAssets>()),
              ui(context.extensions().find<EditorUiScene>())
        {
        }

        exec::task<ProjectSceneResult<asset::AssetId>> load(
            std::filesystem::path path,
            execution::TaskReporter reporter
        ) noexcept
        {
            auto& execution = context.engine().execution();
            const auto blocking = execution.blocking();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            reporter.setPhase("Read Scene");
            auto bytes = co_await execution::taskResult(
                stdexec::schedule(*blocking) |
                stdexec::then([path, stop]() noexcept
                              { return platform::readFileBounded(path, scene_byte_limit, stop); })
            );
            if (!bytes)
            {
                co_return cxx::unexpected(taskFailure(bytes.error()));
            }
            reporter.setPhase("Decode Scene");
            auto decoded = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) |
                stdexec::then(
                    [bytes = std::move(*bytes), stop]() noexcept
                    {
                        auto backing = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
                        return scene::decodeScenePackage(cxx::SharedBytes<>::fromOwner(backing, *backing), stop);
                    }
                )
            );
            if (!decoded)
            {
                co_return cxx::unexpected(taskFailure(decoded.error()));
            }
            if (closing || stop.stop_requested())
            {
                co_return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
            }
            reporter.setPhase("Create Scene instance");
            co_return adoptPackage(std::move(*decoded), std::move(path));
        }

        ProjectSceneResult<scene::SceneInstanceId> buildInstance(
            const scene::ScenePackage& package,
            std::shared_ptr<const scene::SceneDescription> description,
            scene::RenderAssetInput& inputs,
            scene::RenderSystem*& projection,
            std::array<entt::scoped_connection, 3>& camera_connections,
            GeometryRequests& requests
        ) noexcept
        {
            const auto world = std::shared_ptr<const world::WorldDescription>{package.world, &package.world->data()};
            const auto simulation = std::shared_ptr<const simulation::SimulationDescription>{
                package.simulation,
                &package.simulation->data()
            };
            auto source = process::world_loading::makeWorldMemoryStorageSource(world, package.volumes);
            if (!source)
            {
                return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            scene::WorldLoadingServices loading{*source, context.tasks()};
            loading.initial_partitions = package.partitions;
            std::vector<scene::SceneCapabilityProvider> providers;
            providers.push_back(
                scene::makeSceneCapabilityProvider<scene::WorldLoadingServices>("loading", "lux.world.loading", loading)
            );
            bool renders = false;
            for (std::size_t i = 0; i < description->systemCount(); ++i)
            {
                renders |= description->systemAt(i).type().name == scene::RenderSystem::Description.canonical_name;
            }
            std::optional<scene::RenderSystemDependencies> render_dependencies;
            if (renders)
            {
                if (!rendering || !assets || !ui)
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::RENDER_UNAVAILABLE});
                }
                auto reads = context.prepareAssetReads();
                if (!reads)
                {
                    return cxx::unexpected(VProjectSceneFailure{reads.error()});
                }
                render_dependencies
                    .emplace(rendering->mainClient(), context.engine().execution(), *assets, inputs, *reads);
                providers.push_back(scene::makeSceneCapabilityProvider<scene::RenderSystemDependencies>(
                    "rendering",
                    "lux.scene.rendering",
                    *render_dependencies
                ));
            }
            const auto& registrations = context.sceneRegistrations();
            auto& runtime = context.engine().sceneRuntime();
            auto lease = runtime.builder()
                             .setDescription(description)
                             .setWorld(world)
                             .setSimulation(simulation)
                             .setRegistrations(
                                 registrations.components,
                                 *registrations.simulation_systems,
                                 registrations.scene_systems
                             )
                             .setProviders(providers)
                             .setFrameAdmissionRequired(renders)
                             .build();
            if (!lease)
            {
                return cxx::unexpected(VProjectSceneFailure{lease.error()});
            }
            const auto instance = lease->id();
            if (renders)
            {
                auto system = runtime.borrowSystem<scene::RenderSystem>(instance);
                if (!system)
                {
                    return cxx::unexpected(VProjectSceneFailure{system.error()});
                }
                projection = *system;
            }
            auto adopted = context.adoptScene(std::move(*lease));
            if (!adopted)
            {
                std::terminate(); // Same uninterrupted open-Context construction segment.
            }
            requests = {this, instance};
            auto connect = [&](simulation::ecs::Registry& registry) noexcept
            {
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    query->setGeometryRequests(
                        &requests,
                        [](void* data, std::span<const asset::AssetId> assets) noexcept
                        {
                            const auto& request = *static_cast<GeometryRequests*>(data);
                            request.collection->requestGeometry(request.instance, assets);
                        }
                    );
                }
                camera_connections = {
                    registry.on_construct<scene::Camera>().connect<&Impl::camerasChanged>(*this),
                    registry.on_update<scene::Camera>().connect<&Impl::camerasChanged>(*this),
                    registry.on_destroy<scene::Camera>().connect<&Impl::cameraRemoved>(*this)
                };
            };
            if (auto connected = runtime.editInstance(instance, connect); !connected)
            {
                std::terminate(); // Newly constructed, unpublished instance has no running simulation.
            }
            return instance;
        }

        ProjectSceneResult<asset::AssetId> adoptPackage(
            scene::ScenePackage package,
            std::filesystem::path path
        ) noexcept
        {
            const auto id = package.scene->id();
            if (const auto found = documents.find(id); found != documents.end())
            {
                return cxx::unexpected(VProjectSceneFailure{
                    found->second->closing ? EProjectSceneError::BUSY : EProjectSceneError::DUPLICATE_DOCUMENT
                });
            }
            auto entry = std::make_unique<Entry>();
            entry->path = std::move(path);
            entry->inputs.revision = asset_revision;
            entry->inputs.source = asset_source;
            entry->inputs.snapshot = context.assets().view().capture();
            auto instance = buildInstance(
                package,
                std::shared_ptr<const scene::SceneDescription>{package.scene, &package.scene->data()},
                entry->inputs,
                entry->rendering,
                entry->camera_connections,
                entry->geometry
            );
            if (!instance)
            {
                return cxx::unexpected(instance.error());
            }
            static_cast<void>(context.engine().sceneRuntime().pauseSimulation(*instance));
            entry->document = std::make_unique<SceneDocument>(
                context.engine().sceneRuntime(),
                *instance,
                context.sceneRegistrations().components,
                std::move(package)
            );
            documents.emplace(id, std::move(entry));
            return id;
        }

        exec::task<ProjectSceneResult<void>> saveScene(
            asset::AssetId id,
            object::ObjectId document_id,
            scene::ScenePackage package,
            std::filesystem::path path,
            std::uint64_t revision,
            execution::TaskReporter reporter
        ) noexcept
        {
            auto& execution = context.engine().execution();
            const auto blocking = execution.blocking();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            reporter.setPhase("Encode Scene snapshot");
            auto bytes = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) |
                stdexec::then([&]() noexcept { return scene::encodeScenePackage(package, scene_byte_limit, stop); })
            );
            if (!bytes)
            {
                co_return cxx::unexpected(taskFailure(bytes.error()));
            }
            reporter.setPhase("Publish Scene file");
            auto written = co_await execution::taskResult(
                stdexec::schedule(*blocking) |
                stdexec::then(
                    [&]() noexcept
                    { return platform::writeFileAtomic(path, *bytes, platform::EFileWriteMode::REPLACE, stop); }
                )
            );
            if (!written)
            {
                co_return cxx::unexpected(taskFailure(written.error()));
            }
            // The file is published. Later stop never turns that fact into a guaranteed cancellation.
            const auto found = documents.find(id);
            if (found != documents.end() && found->second->document->objectId() == document_id)
            {
                auto saved = found->second->document->saved(std::move(package), revision);
                if (!saved)
                {
                    co_return cxx::unexpected(VProjectSceneFailure{saved.error()});
                }
            }
            reporter.setProgress(1, 1);
            co_return ProjectSceneResult<void>{};
        }

        exec::task<ProjectSceneResult<asset::AssetId>> createScene(
            ProjectManifest::SceneRecord record,
            SceneCreateSelection selection,
            SceneProfileRegistration profile,
            ProjectManifest expected,
            ProjectManifest candidate,
            execution::TaskReporter reporter
        ) noexcept
        {
            auto& execution = context.engine().execution();
            const auto blocking = execution.blocking();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            reporter.setPhase("Prepare Scene");
            auto prepared = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) |
                stdexec::then(
                    [&]() noexcept
                    { return profile.create({record.id, record.name, context.sceneRegistrations(), stop, &selection}); }
                )
            );
            if (!prepared)
            {
                co_return cxx::unexpected(taskFailure(prepared.error()));
            }
            auto encoded = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) |
                stdexec::then([&]() noexcept { return scene::encodeScenePackage(*prepared, scene_byte_limit, stop); })
            );
            if (!encoded)
            {
                co_return cxx::unexpected(taskFailure(encoded.error()));
            }
            if (closing || context.manifest() != expected)
            {
                co_return cxx::unexpected(VProjectSceneFailure{ProjectFailure{EProjectError::STALE_REVISION}});
            }
            const auto path = context.project().root / std::filesystem::u8path(record.path);
            const auto manifest_path = context.project().manifest_file;
            reporter.setPhase("Publish Scene and project catalog");
            // One writer for this project's catalog. Cancellation is observed by IO before the first
            // publication. After the Scene file exists, complete/report the transaction truthfully.
            auto published = co_await execution::taskResult(
                stdexec::schedule(*blocking) |
                stdexec::then(
                    [&]() noexcept -> cxx::expected<void, SceneCreationFailure>
                    {
                        auto current = readProjectManifest(manifest_path, stop);
                        if (!current)
                        {
                            return cxx::unexpected(SceneCreationFailure{path, current.error(), false});
                        }
                        if (*current != expected)
                        {
                            return cxx::unexpected(
                                SceneCreationFailure{path, ProjectFailure{EProjectError::STALE_REVISION}, false}
                            );
                        }
                        std::error_code directory_error;
                        std::filesystem::create_directories(path.parent_path(), directory_error);
                        if (directory_error)
                        {
                            return cxx::unexpected(SceneCreationFailure{
                                path,
                                platform::FileFailure{platform::EFileIoError::IO, directory_error},
                                false
                            });
                        }
                        auto written =
                            platform::writeFileAtomic(path, *encoded, platform::EFileWriteMode::CREATE, stop);
                        if (!written)
                        {
                            const bool published = written.error().code == platform::EFileIoError::PUBLICATION_UNKNOWN;
                            return cxx::unexpected(SceneCreationFailure{path, written.error(), published});
                        }
                        auto catalog = writeProjectManifestAtomic(manifest_path, candidate, EProjectWrite::REPLACE);
                        if (!catalog)
                        {
                            return cxx::unexpected(SceneCreationFailure{path, catalog.error(), true});
                        }
                        return {};
                    }
                )
            );
            if (!published)
            {
                co_return cxx::unexpected(taskFailure(published.error()));
            }
            auto adopted = context.adoptSceneCatalog(expected, std::move(candidate.scenes), candidate.startup_scene);
            if (!adopted)
            {
                co_return cxx::unexpected(VProjectSceneFailure{adopted.error()});
            }
            reporter.setProgress(1, 1);
            if (closing)
            {
                co_return record.id; // Persistent creation succeeded; closing suppresses only runtime opening.
            }
            co_return adoptPackage(std::move(*prepared), path);
        }

        exec::task<render::RenderResult<void>> createTarget(Viewport& viewport) noexcept
        {
            const auto candidate = viewport.requested;
            auto created = co_await requestRenderCommand<render::CreateTarget>(rendering->mainClient(), {candidate});
            if (created.error.type != 0)
            {
                co_return cxx::unexpected(created.error);
            }
            viewport.target = created.target;
            viewport.applied = created.description;
            viewport.target_epoch = created.epoch;
            viewport.camera_dirty = true;
            co_return render::RenderResult<void>{};
        }

        exec::task<render::RenderResult<void>> resizeTarget(Viewport& viewport) noexcept
        {
            const auto candidate = viewport.requested;
            const auto resized = co_await requestRenderCommand<render::ResizeTarget>(
                rendering->mainClient(),
                {viewport.target, candidate}
            );
            if (resized.error.type != 0)
            {
                co_return cxx::unexpected(resized.error);
            }
            viewport.applied = resized.description;
            viewport.target_epoch = resized.epoch;
            // Publish the replied extent through CameraTargets at the next Scene edit safe point.
            viewport.camera_dirty = true;
            co_return render::RenderResult<void>{};
        }

        exec::task<render::RenderResult<void>> removeImage(Viewport& viewport) noexcept
        {
            const auto token = std::exchange(viewport.image, rdesc::UiImageToken{});
            auto result = co_await ui->removeTargetImage(token);
            if (!result)
            {
                viewport.image = token; // No release proof: preserve the Target and the current sampling identity.
            }
            co_return result;
        }

        exec::task<render::RenderResult<void>> removeTarget(Viewport& viewport) noexcept
        {
            auto result = co_await requestRenderCommand<render::RemoveTarget>(rendering->mainClient(), viewport.target);
            if (result.error.type != 0)
            {
                co_return cxx::unexpected(result.error);
            }
            viewport.target = {};
            co_return render::RenderResult<void>{};
        }

        void requestViewportChange(Viewport& viewport) noexcept
        {
            const bool failed_intent =
                viewport.error.type != 0 && viewport.failed_revision == viewport.requested_revision;
            if (viewport.pending || failed_intent)
            {
                return;
            }
            viewport.pending = true;
            auto result = execution::taskResult(
                stdexec::starts_on(context.engine().execution().mainScheduler(), synchronizeViewport(viewport))
            );
            auto admitted = context.engine().execution().spawn(stdexec::then(
                std::move(result),
                [this, &viewport](auto result) noexcept
                {
                    viewport.pending = false;
                    viewport.failed_revision = viewport.requested_revision;
                    viewport.error = result                           ? render::RenderError{}
                                     : result.error().domainFailure() ? *result.error().domainFailure()
                                                                      : render::RenderError{render::kRuntimeState};
                    const auto id = viewport.id;
                    if (result && viewport.closing)
                    {
                        auto& related = camera_viewports.find(viewport.registry)->second;
                        std::erase(related, id);
                        if (related.empty())
                        {
                            camera_viewports.erase(viewport.registry);
                        }
                        viewports.erase(id);
                    }
                    static_cast<void>(collection.emit(collection.viewportChanged, id));
                    completeClosures();
                }
            ));
            if (!admitted)
            {
                viewport.pending = false;
                viewport.error = execution::toError(admitted.error());
                viewport.failed_revision = viewport.requested_revision;
            }
        }

        scene::SceneInstanceId instance(const Viewport& viewport) const noexcept
        {
            const auto& entry = *documents.find(viewport.document)->second;
            return viewport.play ? entry.play->instance : entry.document->instance();
        }

        scene::RenderSystem& projection(const Viewport& viewport) noexcept
        {
            auto& entry = *documents.find(viewport.document)->second;
            return *(viewport.play ? entry.play->rendering : entry.rendering);
        }

        void invalidateCameraSelection(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity camera,
            bool removed
        ) noexcept
        {
            const auto found = camera_viewports.find(&registry);
            if (found == camera_viewports.end())
            {
                return;
            }
            const auto* residency = registry.ctx().find<scene::WorldResidency>();
            const auto* description = registry.try_get<scene::Camera>(camera);
            const bool primary = !removed && description && description->primary;
            for (auto id : found->second)
            {
                auto& viewport = *viewports.find(id);
                const bool selected = viewport.camera == camera;
                const bool primary_changed = viewport.selection.kind == EViewportCamera::PRIMARY &&
                                             (selected ? !primary : primary || viewport.camera_error.type != 0);
                const bool fixed_changed =
                    viewport.selection.kind != EViewportCamera::PRIMARY &&
                    ((selected && removed) ||
                     (!selected && !removed && viewport.selection.kind == EViewportCamera::SCENE && residency &&
                      residency->identities().entity(viewport.selection.object) == camera));
                const bool affected = primary_changed || fixed_changed;
                if (affected && !viewport.closing)
                {
                    viewport.camera_dirty = true;
                    ++viewport.requested_revision;
                    requestViewportChange(viewport); // starts_on(Main); never mutates ECS in its observer.
                }
            }
        }

        void camerasChanged(simulation::ecs::Registry& registry, simulation::ecs::Entity camera) noexcept
        {
            invalidateCameraSelection(registry, camera, false);
        }

        void cameraRemoved(simulation::ecs::Registry& registry, simulation::ecs::Entity camera) noexcept
        {
            invalidateCameraSelection(registry, camera, true);
        }

        simulation::ecs::Entity selectCamera(simulation::ecs::Registry& registry, Viewport& viewport) noexcept
        {
            using namespace simulation::ecs;
            viewport.camera_error = {};
            if (viewport.selection.kind == EViewportCamera::EDITOR)
            {
                if (!registry.valid(viewport.free_camera))
                {
                    viewport.free_camera = registry.create();
                    registry.emplace<scene::Camera>(viewport.free_camera);
                    Transform3D pose;
                    pose.translation = Eigen::Vector3d{0, 2, 8};
                    registry.emplace<Transform3D>(viewport.free_camera, std::move(pose));
                }
                return viewport.free_camera;
            }
            auto selected = NullEntity;
            const auto* residency = registry.ctx().find<scene::WorldResidency>();
            if (viewport.selection.kind == EViewportCamera::SCENE)
            {
                selected = residency ? residency->identities().entity(viewport.selection.object) : NullEntity;
            }
            else
            {
                for (const auto camera : registry.view<const scene::Camera>())
                {
                    if (!registry.get<const scene::Camera>(camera).primary)
                    {
                        continue;
                    }
                    if (selected != NullEntity)
                    {
                        viewport.camera_error = render::RenderError{
                            render::kCommandContract,
                            {static_cast<std::uint64_t>(scene::ECameraError::MULTIPLE_PRIMARY_CAMERAS)}
                        };
                        return NullEntity;
                    }
                    selected = camera;
                }
            }
            const bool available = registry.valid(selected) && registry.all_of<scene::Camera>(selected);
            if (!available)
            {
                viewport.camera_error = render::RenderError{
                    render::kCommandContract,
                    {static_cast<std::uint64_t>(scene::ECameraError::NO_PRIMARY_CAMERA)}
                };
                return NullEntity;
            }
            return selected;
        }

        exec::task<render::RenderResult<void>> synchronizeCamera(Viewport& viewport) noexcept
        {
            using namespace simulation::ecs;
            auto& system = projection(viewport);
            auto& runtime = context.engine().sceneRuntime();
            for (;;)
            {
                bool remove_image = false;
                std::optional<std::pair<Entity, scene::CameraTargetId>> removed_target;
                render::RenderError error;
                auto apply = [&](Registry& registry) noexcept
                {
                    const auto selected = viewport.closing ? NullEntity : selectCamera(registry, viewport);
                    if (viewport.camera_target.valid() && viewport.camera != selected)
                    {
                        if (viewport.image.value != 0)
                        {
                            remove_image = true;
                        }
                        else
                        {
                            removed_target = std::pair{viewport.camera, viewport.camera_target};
                            static_cast<void>(system.removeCameraTarget(viewport.camera, viewport.camera_target));
                        }
                        return;
                    }
                    viewport.camera = selected;
                    if (selected != NullEntity && !viewport.target.isNull())
                    {
                        scene::CameraTarget desired;
                        desired.target = viewport.target;
                        desired.description = viewport.applied;
                        desired.active = viewport.requested.width != 0 && viewport.requested.height != 0;
                        desired.epoch = viewport.target_epoch;
                        if (viewport.camera_target.valid())
                        {
                            const auto& targets = registry.get<scene::CameraTargetsComponent>(selected).targets;
                            const auto found =
                                std::ranges::find(targets, viewport.camera_target, &scene::CameraTarget::id);
                            if (found->description != desired.description || found->active != desired.active ||
                                found->epoch != desired.epoch)
                            {
                                static_cast<void>(system.updateCameraTarget(selected, viewport.camera_target, desired));
                            }
                        }
                        else
                        {
                            auto added = system.addCameraTarget(selected, desired);
                            if (added)
                            {
                                viewport.camera_target = *added;
                            }
                            else
                            {
                                error = added.error();
                            }
                        }
                    }
                    if (viewport.closing && registry.valid(viewport.free_camera))
                    {
                        registry.destroy(viewport.free_camera);
                        viewport.free_camera = NullEntity;
                    }
                    viewport.camera_dirty = false;
                };
                auto edited = co_await runtime.editInstanceAsync(instance(viewport), apply);
                if (!edited)
                {
                    const bool failed_scene = std::holds_alternative<scene::SceneDriveFailure>(edited.error().cause);
                    if (viewport.closing && failed_scene)
                    {
                        if (viewport.camera_target.valid())
                        {
                            auto retired =
                                co_await system.whenCameraTargetRetired(viewport.camera, viewport.camera_target);
                            if (!retired)
                            {
                                co_return retired;
                            }
                            viewport.camera_target = {};
                        }
                        // Failed Scene maintenance owns withdrawal; its Registry retains the temporary camera
                        // until retirement. No mutation of a stopped Scene is needed to release the output.
                        co_return render::RenderResult<void>{};
                    }
                    co_return cxx::unexpected(scene::toError(edited.error()));
                }
                if (error.type != 0)
                {
                    co_return cxx::unexpected(error);
                }
                if (remove_image)
                {
                    auto removed = co_await removeImage(viewport);
                    if (!removed)
                    {
                        co_return removed;
                    }
                    continue;
                }
                if (removed_target)
                {
                    auto retired =
                        co_await system.whenCameraTargetRetired(removed_target->first, removed_target->second);
                    if (!retired)
                    {
                        co_return retired;
                    }
                    viewport.camera_target = {};
                    continue;
                }
                if (viewport.camera_target.valid())
                {
                    co_return co_await system.whenCameraTargetReady(viewport.camera, viewport.camera_target);
                }
                co_return render::RenderResult<void>{};
            }
        }

        exec::task<render::RenderResult<void>> configureViewport(Viewport& viewport) noexcept
        {
            if (viewport.closing)
            {
                if (viewport.image.value != 0)
                {
                    auto removed = co_await removeImage(viewport);
                    if (!removed)
                    {
                        co_return removed;
                    }
                }
                auto detached = co_await synchronizeCamera(viewport);
                if (!detached)
                {
                    co_return detached;
                }
                co_return viewport.target.isNull() ? render::RenderResult<void>{} : co_await removeTarget(viewport);
            }
            const bool visible = viewport.requested.width != 0 && viewport.requested.height != 0;
            if (visible && viewport.target.isNull())
            {
                auto created = co_await createTarget(viewport);
                if (!created)
                {
                    co_return created;
                }
            }
            else if (visible && viewport.applied != viewport.requested)
            {
                auto resized = co_await resizeTarget(viewport);
                if (!resized)
                {
                    co_return resized;
                }
            }
            if (viewport.camera_dirty)
            {
                auto camera = co_await synchronizeCamera(viewport);
                if (!camera)
                {
                    co_return camera;
                }
            }
            if (viewport.camera_target.valid() && viewport.image.value == 0 && !viewport.closing)
            {
                auto image = ui->addTargetImage(viewport.target, viewport.applied);
                if (!image)
                {
                    co_return cxx::unexpected(image.error());
                }
                viewport.image = *image;
                co_return co_await ui->whenImageReady(*image);
            }
            co_return render::RenderResult<void>{};
        }

        exec::task<render::RenderResult<void>> synchronizeViewport(Viewport& viewport) noexcept
        {
            for (;;)
            {
                const auto revision = viewport.requested_revision;
                auto result = co_await configureViewport(viewport);
                if (!result && result.error().type == render::kDeviceBusy)
                {
                    co_await rendering->mainClient().whenSubmittedFramesComplete();
                    co_await stdexec::schedule(context.engine().execution().mainScheduler());
                    continue;
                }
                if (revision == viewport.requested_revision)
                {
                    co_return result;
                }
            }
        }

        exec::task<FrameworkResult<void>> closePlay(asset::AssetId id) noexcept
        {
            auto& entry = *documents.find(id)->second;
            std::vector<SceneViewportId> outputs;
            for (const auto& viewport : viewports)
            {
                if (viewport.document == id && viewport.play)
                {
                    outputs.push_back(viewport.id);
                }
            }
            for (auto viewport : outputs)
            {
                co_await stdexec::continues_on(
                    CloseCompletionSender{*this, ECloseCondition::VIEWPORT, viewport},
                    context.engine().execution().mainScheduler()
                );
            }
            auto& play = *entry.play;
            for (auto& connection : play.camera_connections)
            {
                connection.release();
            }
            auto retired = context.retireScene(play.instance);
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            co_await stdexec::continues_on(retired->whenComplete(), context.engine().execution().mainScheduler());
            entry.play.reset();
            entry.document->setReadOnly(entry.closing);
            static_cast<void>(collection.emit(collection.changed));
            completeClosures();
            co_return FrameworkResult<void>{};
        }

        exec::task<FrameworkResult<void>> closeDocument(asset::AssetId id) noexcept
        {
            auto& entry = *documents.find(id)->second;
            std::vector<SceneViewportId> outputs;
            for (const auto& viewport : viewports)
            {
                if (viewport.document == id)
                {
                    outputs.push_back(viewport.id);
                }
            }
            for (auto viewport : outputs)
            {
                co_await stdexec::continues_on(
                    CloseCompletionSender{*this, ECloseCondition::VIEWPORT, viewport},
                    context.engine().execution().mainScheduler()
                );
            }
            co_await stdexec::continues_on(
                CloseCompletionSender{*this, ECloseCondition::PLAY, {}, id},
                context.engine().execution().mainScheduler()
            );
            co_await stdexec::continues_on(
                CloseCompletionSender{*this, ECloseCondition::SAVE, {}, id},
                context.engine().execution().mainScheduler()
            );
            for (auto& connection : entry.camera_connections)
            {
                connection.release();
            }
            auto retired = context.retireScene(entry.document->instance());
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            co_await stdexec::continues_on(retired->whenComplete(), context.engine().execution().mainScheduler());
            documents.erase(id);
            if (active_document == id)
            {
                active_document = documents.empty() ? asset::AssetId{} : documents.begin()->first;
            }
            static_cast<void>(collection.emit(collection.changed));
            completeClosures();
            co_return FrameworkResult<void>{};
        }

        void startClose(exec::task<FrameworkResult<void>> work) noexcept
        {
            // These ordinary close operations borrow the collection; its enclosing close waits on this count.
            ++operations;
            auto result =
                execution::taskResult(stdexec::starts_on(context.engine().execution().mainScheduler(), std::move(work))
                );
            auto accepted = context.engine().execution().spawn(stdexec::then(
                std::move(result),
                [this](auto result) noexcept
                {
                    --operations;
                    if (!result)
                    {
                        failure = result.error().domainFailure() ? *result.error().domainFailure()
                                                                 : render::RenderError{render::kRuntimeState};
                    }
                    completeClosures();
                }
            ));
            if (!accepted)
            {
                std::terminate();
            }
        }

        using DocumentResult = execution::TTaskResult<asset::AssetId, VProjectSceneFailure>;
        using SaveResult = execution::TTaskResult<void, VProjectSceneFailure>;

        execution::ETaskState finishOpen(ProjectScenes& collection, DocumentResult result) noexcept
        {
            --operations;
            completeClosures();
            if (!result)
            {
                failure = taskFailure(result.error());
                const auto outcome =
                    result.error().isCancelled() ? execution::ETaskState::CANCELLED : execution::ETaskState::FAILED;
                static_cast<void>(collection.emit(collection.changed));
                return outcome;
            }
            failure.reset();
            active_document = *result;
            static_cast<void>(collection.emit(collection.opened, *result));
            return execution::ETaskState::SUCCEEDED;
        }

        execution::ETaskState finishCreate(ProjectScenes& collection, DocumentResult result) noexcept
        {
            --operations;
            completeClosures();
            catalog_write_pending = false;
            if (!result)
            {
                failure = taskFailure(result.error());
                static_cast<void>(collection.emit(collection.changed));
                return execution::ETaskState::FAILED;
            }
            failure.reset();
            if (collection.find(*result))
            {
                active_document = *result;
                static_cast<void>(collection.emit(collection.opened, *result));
            }
            else
            {
                static_cast<void>(collection.emit(collection.changed));
            }
            return execution::ETaskState::SUCCEEDED;
        }

        execution::ETaskState finishSave(ProjectScenes& collection, asset::AssetId id, SaveResult result) noexcept
        {
            --operations;
            documents.find(id)->second->saving = false;
            completeClosures(); // The accepted save no longer borrows its document.
            if (!result)
            {
                failure = taskFailure(result.error());
            }
            else
            {
                failure.reset();
            }
            static_cast<void>(collection.emit(collection.changed));
            return result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
        }

        exec::task<void> adoptGeometry(
            scene::SceneInstanceId instance,
            asset::AssetId mesh,
            std::uint64_t revision,
            std::shared_ptr<const scene::MeshQueryGeometry> geometry,
            scene::EMeshQueryError error
        ) noexcept
        {
            auto& runtime = context.engine().sceneRuntime();
            auto adopt = [&](simulation::ecs::Registry& registry) noexcept
            {
                if (closing || asset_revision != revision)
                {
                    return;
                }
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    if (geometry)
                    {
                        query->setGeometry(mesh, std::move(geometry));
                    }
                    else
                    {
                        query->setGeometryFailure(mesh, {error, {}, mesh});
                    }
                }
            };
            static_cast<void>(co_await runtime.editInstanceAsync(instance, adopt));
        }

        exec::task<void> invalidateGeometry(scene::SceneInstanceId instance) noexcept
        {
            auto& runtime = context.engine().sceneRuntime();
            auto invalidate = [this](simulation::ecs::Registry& registry) noexcept
            {
                if (closing)
                {
                    return;
                }
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    query->clearGeometry();
                }
            };
            static_cast<void>(co_await runtime.editInstanceAsync(instance, invalidate));
        }

        void startQueryWork(exec::task<void> work) noexcept
        {
            ++operations;
            auto result = execution::taskResult(stdexec::starts_on(
                context.engine().execution().mainScheduler(),
                stdexec::then(std::move(work), []() noexcept { return FrameworkResult<void>{}; })
            ));
            auto accepted = context.engine().execution().spawn(stdexec::then(
                std::move(result),
                [this](auto result) noexcept
                {
                    --operations;
                    if (!result)
                    {
                        failure = render::RenderError{render::kRuntimeState};
                    }
                    completeClosures();
                }
            ));
            if (!accepted)
            {
                std::terminate();
            }
        }

        void requestGeometry(scene::SceneInstanceId instance, std::span<const asset::AssetId> meshes) noexcept
        {
            if (closing)
            {
                return;
            }
            for (auto mesh : meshes)
            {
                if (auto ready = project_assets.meshGeometry(mesh))
                {
                    startQueryWork(adoptGeometry(instance, mesh, asset_revision, std::move(ready), {}));
                    continue;
                }
                auto& consumers = geometry_consumers[mesh];
                if (std::ranges::find(consumers, instance) != consumers.end())
                {
                    continue;
                }
                consumers.push_back(instance);
                if (auto prepared = project_assets.prepareMeshGeometry(mesh); !prepared)
                {
                    const auto error = prepared.error().code == EAssetImportError::BUSY
                                           ? scene::EMeshQueryError::CAPACITY
                                           : scene::EMeshQueryError::ASSET_FAILURE;
                    startQueryWork(adoptGeometry(instance, mesh, asset_revision, {}, error));
                    std::erase(consumers, instance);
                    if (consumers.empty())
                    {
                        geometry_consumers.erase(mesh);
                    }
                }
            }
        }

        void geometryReady(asset::AssetId mesh, std::uint64_t revision) noexcept
        {
            if (closing || revision != asset_revision)
            {
                return;
            }
            const auto found = geometry_consumers.find(mesh);
            if (found == geometry_consumers.end())
            {
                return;
            }
            auto consumers = std::move(found->second);
            geometry_consumers.erase(found);
            const auto geometry = project_assets.meshGeometry(mesh);
            for (auto instance : consumers)
            {
                startQueryWork(adoptGeometry(instance, mesh, revision, geometry, scene::EMeshQueryError::ASSET_FAILURE)
                );
            }
        }
    };

    std::string describeProjectSceneFailure(const VProjectSceneFailure& failure)
    {
        return std::visit(
            [](const auto& value) -> std::string
            {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, error::Error>)
                {
                    return error::format(value);
                }
                else if constexpr (std::same_as<T, scene::ScenePackageFailure>)
                {
                    const auto* subject = std::get_if<std::string>(&value.cause);
                    return value.stage + (subject ? ": " + *subject : "") + " (Scene package error " +
                           std::to_string(static_cast<unsigned>(value.code)) + ", detail " +
                           std::to_string(value.ordinal) + ")";
                }
                else if constexpr (std::same_as<T, scene::SceneRuntimeFailure>)
                {
                    return "Scene construction/operation failed, category " + std::to_string(value.cause.index());
                }
                else if constexpr (std::same_as<T, SceneCreationFailure>)
                {
                    return "Scene file " + value.scene_path.string() +
                           (value.scene_published ? " was published; " : " was not published; ") +
                           std::visit(
                               [](const auto& cause)
                               { return describeProjectSceneFailure(VProjectSceneFailure{cause}); },
                               value.cause
                           );
                }
                else if constexpr (std::same_as<T, VSceneEditFailure>)
                {
                    return "Scene author operation rejected, category " + std::to_string(value.index());
                }
                else if constexpr (std::same_as<T, EProjectSceneError>)
                {
                    switch (value)
                    {
                    case EProjectSceneError::INVALID_DOCUMENT:
                        return "Invalid Scene document";
                    case EProjectSceneError::DUPLICATE_DOCUMENT:
                        return "Scene is already open";
                    case EProjectSceneError::NOT_OPEN:
                        return "Scene is not open";
                    case EProjectSceneError::CLOSED:
                        return "Project/operation is closing";
                    case EProjectSceneError::RENDER_UNAVAILABLE:
                        return "Selected Scene has no available rendering service";
                    case EProjectSceneError::INVALID_VIEW:
                        return "Invalid viewport description";
                    case EProjectSceneError::BUSY:
                        return "Project catalog is being published";
                    }
                    std::terminate();
                }
                else
                {
                    return "File/catalog error " + std::to_string(static_cast<unsigned>(value.code)) + ": " +
                           value.system.message();
                }
            },
            failure
        );
    }

    ProjectScenes::ProjectScenes(EditorContext& context, ProjectAssets& assets) noexcept
        : impl_(std::make_unique<Impl>(*this, context, assets))
    {
        const auto id = objectId();
        impl_->asset_source = ((std::uint64_t{id.gen} << 32) | id.index) + 1;
        auto connection = connect(
            &assets,
            &ProjectAssets::changed,
            this,
            [this]() noexcept
            {
                if (impl_->project_assets.ready())
                {
                    setAssetRevision(impl_->project_assets.revision());
                }
            }
        );
        if (!connection)
        {
            std::terminate();
        }
        impl_->assets_changed = std::move(*connection);
        auto geometry = connect(
            &assets,
            &ProjectAssets::geometryChanged,
            this,
            [this](const MeshGeometryChanged& result) noexcept { impl_->geometryReady(result.mesh, result.revision); },
            object::EDelivery::DIRECT
        );
        if (!geometry)
        {
            std::terminate();
        }
        impl_->geometry_changed = std::move(*geometry);
        setAssetRevision(assets.revision());
    }

    ProjectScenes::~ProjectScenes() noexcept
    {
        if (!settled())
        {
            std::terminate(); // Application maintenance settles accepted producers and Scene borrows first.
        }
    }

    ProjectSceneResult<execution::TaskId> ProjectScenes::open(std::filesystem::path path) noexcept
    {
        auto& state = *impl_;
        if (!state.project_assets.ready())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        if (state.closing || path.empty())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
        }
        ++state.operations;
        auto admitted = state.context.tasks().submit(
            {"Open Scene", "Scene"},
            [this, path = std::move(path)](execution::TaskReporter reporter) mutable noexcept
            {
                auto result = execution::taskResult(stdexec::starts_on(
                    impl_->context.engine().execution().mainScheduler(),
                    impl_->load(std::move(path), reporter)
                ));
                return stdexec::then(
                    std::move(result),
                    [this](auto result) noexcept { return impl_->finishOpen(*this, std::move(result)); }
                );
            }
        );
        if (!admitted)
        {
            --state.operations;
            return cxx::unexpected(VProjectSceneFailure{execution::toError(admitted.error())});
        }
        return *admitted;
    }

    ProjectSceneResult<execution::TaskId> ProjectScenes::create(
        ProjectManifest::SceneRecord record,
        SceneCreateSelection selection
    ) noexcept
    {
        auto& state = *impl_;
        if (!state.project_assets.ready())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        if (state.closing)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
        }
        if (state.catalog_write_pending)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        auto profile = state.context.sceneProfiles().find(record.profile);
        if (!profile)
        {
            return cxx::unexpected(VProjectSceneFailure{profile.error()});
        }
        auto expected = state.context.manifest();
        auto candidate = expected;
        candidate.scenes.push_back(record);
        if (!candidate.startup_scene)
        {
            candidate.startup_scene = record.id;
        }
        if (auto valid = validateProjectManifest(candidate); !valid)
        {
            return cxx::unexpected(VProjectSceneFailure{valid.error()});
        }
        state.catalog_write_pending = true;
        ++state.operations;
        auto admitted = state.context.tasks().submit(
            {"Create Scene", "Scene"},
            [this,
             record = std::move(record),
             selection = std::move(selection),
             profile = profile->get(),
             expected = std::move(expected),
             candidate = std::move(candidate)](execution::TaskReporter reporter) mutable noexcept
            {
                auto work = impl_->createScene(
                    std::move(record),
                    std::move(selection),
                    std::move(profile),
                    std::move(expected),
                    std::move(candidate),
                    reporter
                );
                // IO observes the reporter token explicitly. Scheduler cancellation must not erase a
                // published file result or prevent Main from adopting the new catalog.
                auto result = execution::taskResult(stdexec::write_env(
                    stdexec::starts_on(impl_->context.engine().execution().mainScheduler(), std::move(work)),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                ));
                return stdexec::then(
                    std::move(result),
                    [this](auto result) noexcept { return impl_->finishCreate(*this, std::move(result)); }
                );
            }
        );
        if (!admitted)
        {
            --state.operations;
            state.catalog_write_pending = false;
            return cxx::unexpected(VProjectSceneFailure{execution::toError(admitted.error())});
        }
        return *admitted;
    }

    SceneDocument* ProjectScenes::find(asset::AssetId id) noexcept
    {
        const auto found = impl_->documents.find(id);
        return found == impl_->documents.end() || found->second->closing ? nullptr : found->second->document.get();
    }

    const std::filesystem::path* ProjectScenes::path(asset::AssetId id) const noexcept
    {
        const auto found = impl_->documents.find(id);
        return found == impl_->documents.end() ? nullptr : &found->second->path;
    }

    std::vector<asset::AssetId> ProjectScenes::documents() const
    {
        std::vector<asset::AssetId> result;
        for (const auto& [id, entry] : impl_->documents)
        {
            if (!entry->closing)
            {
                result.push_back(id);
            }
        }
        return result;
    }

    asset::AssetId ProjectScenes::activeDocument() const noexcept
    {
        return impl_->active_document;
    }

    bool ProjectScenes::selectDocument(asset::AssetId id) noexcept
    {
        if (!find(id))
        {
            return false;
        }
        if (impl_->active_document != id)
        {
            impl_->active_document = id;
            static_cast<void>(emit(changed));
        }
        return true;
    }

    bool ProjectScenes::saving(asset::AssetId id) const noexcept
    {
        const auto found = impl_->documents.find(id);
        return found != impl_->documents.end() && found->second->saving;
    }

    ProjectSceneResult<execution::TaskId> ProjectScenes::save(asset::AssetId id) noexcept
    {
        auto* document = find(id);
        if (!document || impl_->closing)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::NOT_OPEN});
        }
        auto& entry = *impl_->documents.find(id)->second;
        if (entry.saving)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        auto package = document->capture();
        if (!package)
        {
            return cxx::unexpected(VProjectSceneFailure{package.error()});
        }
        entry.saving = true;
        ++impl_->operations;
        auto admitted = impl_->context.tasks().submit(
            {"Save Scene", "Scene"},
            [this,
             id,
             document_id = document->objectId(),
             revision = document->revision(),
             path = entry.path,
             package = std::move(*package)](execution::TaskReporter reporter) mutable noexcept
            {
                auto work = impl_->saveScene(id, document_id, std::move(package), std::move(path), revision, reporter);
                auto result = execution::taskResult(stdexec::write_env(
                    stdexec::starts_on(impl_->context.engine().execution().mainScheduler(), std::move(work)),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                ));
                return stdexec::then(
                    std::move(result),
                    [this, id](auto result) noexcept { return impl_->finishSave(*this, id, std::move(result)); }
                );
            }
        );
        if (!admitted)
        {
            entry.saving = false;
            --impl_->operations;
            return cxx::unexpected(VProjectSceneFailure{execution::toError(admitted.error())});
        }
        return *admitted;
    }

    ProjectSceneResult<scene::SceneInstanceId> ProjectScenes::startPlay(asset::AssetId id, ESceneRunMode mode) noexcept
    {
        auto* document = find(id);
        if (!document || impl_->closing)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::NOT_OPEN});
        }
        auto& entry = *impl_->documents.find(id)->second;
        if (entry.play)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        auto package = document->capture();
        if (!package)
        {
            return cxx::unexpected(VProjectSceneFailure{package.error()});
        }
        auto description = previewSystems(*package, mode);
        if (!description)
        {
            return cxx::unexpected(description.error());
        }
        auto& play = entry.play.emplace();
        play.inputs.source = entry.inputs.source;
        play.inputs.revision = entry.inputs.revision;
        play.inputs.snapshot = entry.inputs.snapshot;
        auto instance = impl_->buildInstance(
            *package,
            std::move(*description),
            play.inputs,
            play.rendering,
            play.camera_connections,
            play.geometry
        );
        if (!instance)
        {
            entry.play.reset();
            return cxx::unexpected(instance.error());
        }
        play.instance = *instance;
        document->setReadOnly(true);
        static_cast<void>(emit(changed));
        return *instance;
    }

    void ProjectScenes::stopPlay(asset::AssetId id) noexcept
    {
        const auto found = impl_->documents.find(id);
        if (found == impl_->documents.end() || !found->second->play || found->second->play->stopping)
        {
            return;
        }
        found->second->play->stopping = true;
        static_cast<void>(impl_->context.engine().sceneRuntime().pauseSimulation(found->second->play->instance));
        for (auto& viewport : impl_->viewports)
        {
            if (viewport.document == id && viewport.play)
            {
                viewport.closing = true;
                ++viewport.requested_revision;
                impl_->requestViewportChange(viewport);
            }
        }
        impl_->startClose(impl_->closePlay(id));
        static_cast<void>(emit(changed));
    }

    scene::SceneInstanceId ProjectScenes::playInstance(asset::AssetId id) const noexcept
    {
        const auto found = impl_->documents.find(id);
        return found != impl_->documents.end() && found->second->play ? found->second->play->instance
                                                                      : scene::SceneInstanceId{};
    }

    bool ProjectScenes::stoppingPlay(asset::AssetId id) const noexcept
    {
        const auto found = impl_->documents.find(id);
        return found != impl_->documents.end() && found->second->play && found->second->play->stopping;
    }

    ProjectSceneResult<void> ProjectScenes::close(asset::AssetId id) noexcept
    {
        const auto found = impl_->documents.find(id);
        if (found == impl_->documents.end())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::NOT_OPEN});
        }
        auto& entry = *found->second;
        if (entry.closing)
        {
            return {};
        }
        entry.closing = true;
        entry.document->setReadOnly(true);
        stopPlay(id);
        static_cast<void>(impl_->context.engine().sceneRuntime().pauseSimulation(entry.document->instance()));
        for (auto& viewport : impl_->viewports)
        {
            if (viewport.document == id)
            {
                viewport.closing = true;
                ++viewport.requested_revision;
                impl_->requestViewportChange(viewport);
            }
        }
        impl_->startClose(impl_->closeDocument(id));
        static_cast<void>(emit(changed));
        return {};
    }

    void ProjectScenes::closeAll() noexcept
    {
        impl_->closing = true;
        for (const auto& [id, entry] : impl_->documents)
        {
            static_cast<void>(close(id));
        }
    }

    exec::task<void> ProjectScenes::closeAsync() noexcept
    {
        closeAll();
        if (settled())
        {
            co_return;
        }
        co_await stdexec::continues_on(
            Impl::CloseCompletionSender{*impl_, Impl::ECloseCondition::ALL},
            impl_->context.engine().execution().mainScheduler()
        );
    }

    void ProjectScenes::setAssetRevision(std::uint64_t revision) noexcept
    {
        if (impl_->asset_revision == revision)
        {
            return;
        }
        impl_->asset_revision = revision;
        impl_->geometry_consumers.clear();
        auto snapshot = impl_->context.assets().view().capture();
        for (auto& [id, entry] : impl_->documents)
        {
            entry->inputs.revision = revision;
            entry->inputs.snapshot = snapshot;
            impl_->startQueryWork(impl_->invalidateGeometry(entry->document->instance()));
            if (entry->play)
            {
                entry->play->inputs.revision = revision;
                entry->play->inputs.snapshot = snapshot;
                impl_->startQueryWork(impl_->invalidateGeometry(entry->play->instance));
            }
        }
    }

    bool ProjectScenes::settled() const noexcept
    {
        return impl_->operations == 0 && impl_->documents.empty() && impl_->viewports.empty();
    }

    const std::optional<VProjectSceneFailure>& ProjectScenes::failure() const noexcept
    {
        return impl_->failure;
    }

    ProjectSceneResult<SceneViewportId> ProjectScenes::addViewport(
        asset::AssetId document,
        std::uint32_t width,
        std::uint32_t height,
        bool play
    ) noexcept
    {
        const auto found = impl_->documents.find(document);
        const bool missing = impl_->closing || found == impl_->documents.end() || found->second->closing;
        const bool unavailable =
            missing || (play ? !found->second->play || found->second->play->stopping || !found->second->play->rendering
                             : !found->second->rendering);
        if (unavailable)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::RENDER_UNAVAILABLE});
        }
        Impl::Viewport viewport;
        viewport.document = document;
        viewport.play = play;
        viewport
            .requested = {width, height, rdesc::ETextureFormat::RGBA8_UNORM, 1, render::ETargetKind::OFFSCREEN, true};
        const auto id = impl_->viewports.emplace(std::move(viewport));
        auto& installed = *impl_->viewports.find(id);
        installed.id = id;
        const auto registry =
            std::as_const(impl_->context.engine().sceneRuntime()).borrowInstance(impl_->instance(installed));
        installed.registry = &registry->get();
        impl_->camera_viewports[installed.registry].push_back(id);
        impl_->requestViewportChange(installed);
        return id;
    }

    bool ProjectScenes::updateViewport(SceneViewportId id, std::uint32_t width, std::uint32_t height) noexcept
    {
        auto* viewport = impl_->viewports.find(id);
        if (!viewport || viewport->closing)
        {
            return false;
        }
        const bool changed = viewport->requested.width != width || viewport->requested.height != height;
        const bool retry = viewport->error.type != 0;
        if (changed || retry)
        {
            viewport->camera_dirty = true;
            ++viewport->requested_revision;
            viewport->error = {}; // A new size is a new attempt; the failed size is not retried every tick.
            viewport->requested.width = width;
            viewport->requested.height = height;
            impl_->requestViewportChange(*viewport);
        }
        return true;
    }

    bool ProjectScenes::selectViewportCamera(SceneViewportId id, ViewportCameraSelection selection) noexcept
    {
        auto* viewport = impl_->viewports.find(id);
        if (!viewport || viewport->closing)
        {
            return false;
        }
        if (viewport->selection != selection)
        {
            viewport->selection = selection;
            viewport->camera_dirty = true;
            ++viewport->requested_revision;
            impl_->requestViewportChange(*viewport);
        }
        return true;
    }

    bool ProjectScenes::setViewportCameraTransform(
        SceneViewportId id,
        const simulation::ecs::Transform3D& pose
    ) noexcept
    {
        auto* viewport = impl_->viewports.find(id);
        if (!viewport || viewport->closing || viewport->selection.kind != EViewportCamera::EDITOR)
        {
            return false;
        }
        auto apply = [&](simulation::ecs::Registry& registry) noexcept
        {
            const auto camera = impl_->selectCamera(registry, *viewport);
            registry.patch<simulation::ecs::Transform3D>(camera, [&](auto& value) noexcept { value = pose; });
        };
        return impl_->context.engine().sceneRuntime().editInstance(impl_->instance(*viewport), apply).has_value();
    }

    void ProjectScenes::closeViewport(SceneViewportId id) noexcept
    {
        if (auto* viewport = impl_->viewports.find(id))
        {
            viewport->closing = true;
            ++viewport->requested_revision;
            impl_->requestViewportChange(*viewport);
        }
    }

    void ProjectScenes::event(object::EventView& event) noexcept
    {
        auto* command = event.getIf<ui::Command>();
        if (!command)
        {
            return;
        }
        const bool undo = command->id == ui::CommandIdView{"lux.edit.undo"};
        const bool redo = command->id == ui::CommandIdView{"lux.edit.redo"};
        const bool save_command = command->id == ui::CommandIdView{"lux.scene.save"};
        const bool play = command->id == ui::CommandIdView{"lux.scene.play"};
        const bool headless = command->id == ui::CommandIdView{"lux.scene.play_headless"};
        const bool stop = command->id == ui::CommandIdView{"lux.scene.stop"};
        if (!(undo || redo || save_command || play || headless || stop))
        {
            return;
        }
        event.accept();
        auto* document = find(activeDocument());
        command->enabled = document && (undo           ? document->canUndo()
                                        : redo         ? document->canRedo()
                                        : save_command ? !saving(document->id()) && !document->previewing()
                                        : stop         ? playInstance(document->id()).valid()
                                               : !playInstance(document->id()).valid() && !document->previewing());
        command->result =
            command->enabled ? ui::ECommandDispatchResult::NOT_FOUND : ui::ECommandDispatchResult::DISABLED;
        if (!command->enabled || command->phase != ui::ECommandPhase::EXECUTE)
        {
            return;
        }
        bool succeeded = true;
        if (undo || redo)
        {
            auto result = undo ? document->undo() : document->redo();
            succeeded = result.has_value();
            if (!result)
            {
                impl_->failure = result.error();
            }
        }
        else if (save_command)
        {
            auto result = save(document->id());
            succeeded = result.has_value();
            if (!result)
            {
                impl_->failure = result.error();
            }
        }
        else if (stop)
        {
            stopPlay(document->id());
        }
        else
        {
            auto result = startPlay(document->id(), headless ? ESceneRunMode::HEADLESS : ESceneRunMode::GRAPHICAL);
            succeeded = result.has_value();
            if (!result)
            {
                impl_->failure = result.error();
            }
        }
        command->result = succeeded ? ui::ECommandDispatchResult::EXECUTED : ui::ECommandDispatchResult::FAILED;
    }

    SceneViewportStatus ProjectScenes::viewport(SceneViewportId id) const noexcept
    {
        const auto* viewport = impl_->viewports.find(id);
        if (!viewport)
        {
            return {};
        }
        auto error = viewport->error.type != 0 ? viewport->error : viewport->camera_error;
        if (viewport->camera_target.valid() && error.type == 0)
        {
            error = impl_->projection(*viewport).cameraTargetError(viewport->camera, viewport->camera_target);
        }
        const bool ready = !viewport->closing && !viewport->pending && error.type == 0 &&
                           viewport->applied == viewport->requested && viewport->image.value != 0 &&
                           impl_->ui->imageReady(viewport->image);
        return {
            viewport->image,
            viewport->target,
            viewport->applied,
            impl_->instance(*viewport),
            viewport->camera,
            error,
            ready,
            viewport->pending || viewport->applied != viewport->requested
        };
    }
} // namespace lux::editor
