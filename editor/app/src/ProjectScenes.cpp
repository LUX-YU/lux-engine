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
#include <lux/engine/scene/WorldLoadingSystem.hpp>
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
        struct Play final
        {
            scene::SceneInstanceId instance;
            scene::SceneRenderInputs inputs;
            scene::RenderSystem* rendering{};
            std::optional<scene::InstanceRetirement> retirement;
            bool stopping{}, query_assets_changed{};
        };

        struct Entry final
        {
            std::unique_ptr<SceneDocument> document;
            std::filesystem::path path;
            scene::SceneRenderInputs inputs;
            scene::RenderSystem* rendering{};
            std::optional<scene::InstanceRetirement> retirement;
            std::optional<Play> play;
            bool closing{}, saving{};
            bool query_assets_changed{};
        };

        struct Viewport final
        {
            SceneViewportId id;
            asset::AssetId document;
            scene::VSceneCameraSource camera;
            render::TargetDesc requested;
            render::TargetDesc applied;
            render::TargetId target;
            scene::SceneViewId view;
            rdesc::UiImageToken image;
            render::RenderError error;
            bool closing{}, pending{};
            bool play{};
        };

        EditorContext& context;
        ProjectAssets& project_assets;
        object::Connection assets_changed;
        rendering::RenderExecution* rendering;
        rendering::RenderAssets* assets;
        EditorUiScene* ui;
        std::map<asset::AssetId, std::unique_ptr<Entry>> documents;
        cxx::StableSlotMap<Viewport, SceneViewportTag> viewports;
        std::optional<VProjectSceneFailure> failure;
        std::size_t operations{};
        std::uint64_t asset_revision{}, asset_source{};
        asset::AssetId active_document;
        bool closing{}, catalog_write_pending{};

        Impl(EditorContext& context, ProjectAssets& project_assets) noexcept
            : context(context), project_assets(project_assets),
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
                stdexec::then([path, stop]() noexcept { return readScenePackageBytes(path, scene_byte_limit, stop); })
            );
            if (!bytes)
            {
                co_return cxx::unexpected(taskFailure(bytes.error()));
            }
            reporter.setPhase("Decode Scene");
            auto decoded = co_await execution::taskResult(
                stdexec::schedule(execution.cpu()) | stdexec::then([bytes = std::move(*bytes), stop]() noexcept
                                                                   { return scene::decodeScenePackage(bytes, stop); })
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
            scene::SceneRenderInputs& inputs,
            scene::RenderSystem*& projection
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
            entry->inputs.asset_revision = asset_revision;
            entry->inputs.asset_source = asset_source;
            entry->inputs.asset_snapshot = context.assets().view().capture();
            auto instance = buildInstance(
                package,
                std::shared_ptr<const scene::SceneDescription>{package.scene, &package.scene->data()},
                entry->inputs,
                entry->rendering
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
                stdexec::then([&]() noexcept
                              { return writeScenePackageBytesAtomic(path, *bytes, ESceneWrite::REPLACE, stop); })
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
                                SceneFileFailure{ESceneFileError::IO, directory_error},
                                false
                            });
                        }
                        auto written = writeScenePackageBytesAtomic(path, *encoded, ESceneWrite::CREATE, stop);
                        if (!written)
                        {
                            const bool published = written.error().code == ESceneFileError::PUBLICATION_UNKNOWN;
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
            viewport.applied = candidate;
            if (!viewport.closing)
            {
                scene::SceneViewRequest request;
                request.camera = viewport.camera;
                request.target = viewport.target;
                request.description = candidate;
                viewport.view = inputs(viewport).addView(std::move(request));
            }
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
            if (!viewport.closing && !viewport.view.isNull())
            {
                auto& source = inputs(viewport);
                auto request = source.findView(viewport.view)->request;
                request.description = viewport.applied;
                static_cast<void>(source.updateView(viewport.view, std::move(request)));
            }
            co_return render::RenderResult<void>{};
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

        void startViewportChange(Viewport& viewport, exec::task<render::RenderResult<void>> work) noexcept
        {
            viewport.pending = true;
            auto result =
                execution::taskResult(stdexec::starts_on(context.engine().execution().mainScheduler(), std::move(work))
                );
            auto admitted = context.engine().execution().spawn(stdexec::then(
                std::move(result),
                [&viewport](auto result) noexcept
                {
                    viewport.error = result                           ? render::RenderError{}
                                     : result.error().domainFailure() ? *result.error().domainFailure()
                                                                      : render::RenderError{render::kRuntimeState};
                    const bool transient =
                        viewport.error.type == render::kDeviceBusy || viewport.error.type == render::kRuntimeCapacity;
                    if (transient)
                    {
                        viewport.error = {};
                    }
                    viewport.pending = false;
                }
            ));
            if (!admitted)
            {
                viewport.pending = false;
                viewport.error = execution::toError(admitted.error());
            }
        }

        scene::SceneRenderInputs& inputs(const Viewport& viewport) noexcept
        {
            auto& entry = *documents.find(viewport.document)->second;
            return viewport.play ? entry.play->inputs : entry.inputs;
        }

        scene::RenderSystem& projection(const Viewport& viewport) noexcept
        {
            auto& entry = *documents.find(viewport.document)->second;
            return *(viewport.play ? entry.play->rendering : entry.rendering);
        }

        void maintainViews() noexcept
        {
            std::vector<SceneViewportId> removed;
            const auto maintain = [&](Viewport& viewport)
            {
                const auto id = viewport.id;
                if (viewport.pending || (viewport.error.type != 0 && !viewport.closing))
                {
                    return;
                }
                if (!viewport.closing && viewport.target.isNull())
                {
                    startViewportChange(viewport, createTarget(viewport));
                    return;
                }
                if (!viewport.closing && viewport.applied != viewport.requested)
                {
                    startViewportChange(viewport, resizeTarget(viewport));
                    return;
                }
                if (viewport.closing)
                {
                    if (viewport.image.value != 0)
                    {
                        ui->removeTargetImage(viewport.image);
                        viewport.image = {};
                    }
                    if (!ui->imagesApplied())
                    {
                        return;
                    }
                    if (!viewport.view.isNull())
                    {
                        static_cast<void>(inputs(viewport).removeView(viewport.view));
                        if (!projection(viewport).viewRetired(viewport.view))
                        {
                            return;
                        }
                        viewport.view = {};
                    }
                    if (!viewport.target.isNull())
                    {
                        startViewportChange(viewport, removeTarget(viewport));
                        return;
                    }
                    removed.push_back(id);
                    return;
                }
                if (!viewport.view.isNull())
                {
                    const auto error = projection(viewport).viewError(viewport.view);
                    if (error.type != 0)
                    {
                        if (viewport.image.value != 0)
                        {
                            ui->removeTargetImage(viewport.image);
                            viewport.image = {};
                        }
                        return;
                    }
                    if (viewport.image.value == 0 && !projection(viewport).attachment(viewport.view).isNull())
                    {
                        auto image = ui->addTargetImage(viewport.target, viewport.applied);
                        if (!image)
                        {
                            viewport.error = image.error();
                        }
                        else
                        {
                            viewport.image = *image;
                        }
                    }
                }
            };
            for (auto& viewport : viewports)
            {
                maintain(viewport);
            }
            for (auto id : removed)
            {
                viewports.erase(id);
            }
        }

        void maintainQueries(scene::SceneInstanceId instance, bool& invalidated) noexcept
        {
            auto& runtime = context.engine().sceneRuntime();
            const auto borrowed = std::as_const(runtime).borrowInstance(instance);
            if (!borrowed)
            {
                return;
            }
            const auto* query = borrowed->get().ctx().find<scene::MeshQuery>();
            if (!query || (!invalidated && query->missingGeometry().empty()))
            {
                return;
            }
            auto adopt = [&](simulation::ecs::Registry& registry) noexcept
            {
                auto& query = registry.ctx().get<scene::MeshQuery>();
                if (std::exchange(invalidated, false))
                {
                    query.clearGeometry();
                }
                for (const auto id : query.missingGeometry())
                {
                    if (auto ready = project_assets.meshGeometry(id))
                    {
                        query.setGeometry(id, std::move(ready));
                    }
                    else if (auto started = project_assets.prepareMeshGeometry(id); !started)
                    {
                        query.setGeometryFailure(id, {scene::EMeshQueryError::ASSET_FAILURE, {}, id});
                    }
                }
            };
            static_cast<void>(runtime.editInstance(instance, adopt)
            ); // Retry only if the structural safe point is busy.
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
        : impl_(std::make_unique<Impl>(context, assets))
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
                    [this](auto result) noexcept
                    {
                        --impl_->operations;
                        if (!result)
                        {
                            impl_->failure = taskFailure(result.error());
                            const auto state = result.error().isCancelled() ? execution::ETaskState::CANCELLED
                                                                            : execution::ETaskState::FAILED;
                            static_cast<void>(emit(changed));
                            return state;
                        }
                        impl_->failure.reset();
                        impl_->active_document = *result;
                        static_cast<void>(emit(opened, *result));
                        return execution::ETaskState::SUCCEEDED;
                    }
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
                    [this](auto result) noexcept
                    {
                        --impl_->operations;
                        impl_->catalog_write_pending = false;
                        if (!result)
                        {
                            impl_->failure = taskFailure(result.error());
                            static_cast<void>(emit(changed));
                            return execution::ETaskState::FAILED;
                        }
                        impl_->failure.reset();
                        if (find(*result))
                        {
                            impl_->active_document = *result;
                            static_cast<void>(emit(opened, *result));
                        }
                        else
                        {
                            static_cast<void>(emit(changed));
                        }
                        return execution::ETaskState::SUCCEEDED;
                    }
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
                    [this, id](auto result) noexcept
                    {
                        --impl_->operations;
                        impl_->documents.find(id)->second->saving = false; // close retains a saving document.
                        if (!result)
                        {
                            impl_->failure = taskFailure(result.error());
                        }
                        else
                        {
                            impl_->failure.reset();
                        }
                        static_cast<void>(emit(changed));
                        return result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                    }
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
        play.inputs.asset_source = entry.inputs.asset_source;
        play.inputs.asset_revision = entry.inputs.asset_revision;
        play.inputs.asset_snapshot = entry.inputs.asset_snapshot;
        auto instance = impl_->buildInstance(*package, std::move(*description), play.inputs, play.rendering);
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
        if (found == impl_->documents.end() || !found->second->play)
        {
            return;
        }
        found->second->play->stopping = true;
        static_cast<void>(impl_->context.engine().sceneRuntime().pauseSimulation(found->second->play->instance));
        for (auto& viewport : impl_->viewports)
        {
            viewport.closing |= viewport.document == id && viewport.play;
        }
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
        entry.closing = true;
        entry.document->setReadOnly(true);
        stopPlay(id);
        static_cast<void>(impl_->context.engine().sceneRuntime().pauseSimulation(entry.document->instance()));
        for (auto& viewport : impl_->viewports)
        {
            viewport.closing |= viewport.document == id;
        }
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

    void ProjectScenes::maintain() noexcept
    {
        if (impl_->ui)
        {
            impl_->maintainViews();
        }
        bool removed = false;
        for (auto entry = impl_->documents.begin(); entry != impl_->documents.end();)
        {
            auto& value = *entry->second;
            if (!value.closing)
            {
                impl_->maintainQueries(value.document->instance(), value.query_assets_changed);
            }
            if (value.play && !value.play->stopping)
            {
                impl_->maintainQueries(value.play->instance, value.play->query_assets_changed);
            }
            if (value.play && value.play->stopping)
            {
                const bool has_play_view = std::ranges::any_of(
                    impl_->viewports,
                    [&](const auto& viewport) noexcept { return viewport.document == entry->first && viewport.play; }
                );
                auto& play = *value.play;
                if (!has_play_view && !play.retirement)
                {
                    auto retired = impl_->context.retireScene(play.instance);
                    if (retired)
                    {
                        play.retirement = *retired;
                    }
                    else
                    {
                        impl_->failure = retired.error();
                    }
                }
                if (play.retirement && play.retirement->complete())
                {
                    value.play.reset();
                    value.document->setReadOnly(value.closing);
                    removed = true;
                }
            }
            if (!value.closing)
            {
                ++entry;
                continue;
            }
            bool has_view = false;
            for (const auto& viewport : impl_->viewports)
            {
                has_view |= viewport.document == entry->first;
            }
            if (has_view || value.saving || value.play)
            {
                ++entry;
                continue;
            }
            if (!value.retirement)
            {
                auto retired = impl_->context.retireScene(value.document->instance());
                if (!retired)
                {
                    impl_->failure = retired.error();
                    ++entry;
                    continue;
                }
                value.retirement = *retired;
            }
            if (!value.retirement->complete())
            {
                ++entry;
                continue;
            }
            entry = impl_->documents.erase(entry);
            removed = true;
        }
        if (removed)
        {
            if (!impl_->documents.contains(impl_->active_document))
            {
                impl_->active_document = impl_->documents.empty() ? asset::AssetId{} : impl_->documents.begin()->first;
            }
            static_cast<void>(emit(changed));
        }
    }

    void ProjectScenes::setAssetRevision(std::uint64_t revision) noexcept
    {
        if (impl_->asset_revision == revision)
        {
            return;
        }
        impl_->asset_revision = revision;
        auto snapshot = impl_->context.assets().view().capture();
        for (auto& [id, entry] : impl_->documents)
        {
            entry->inputs.asset_revision = revision;
            entry->inputs.asset_snapshot = snapshot;
            entry->query_assets_changed = true;
            if (entry->play)
            {
                entry->play->inputs.asset_revision = revision;
                entry->play->inputs.asset_snapshot = snapshot;
                entry->play->query_assets_changed = true;
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
        scene::VSceneCameraSource camera,
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
        if (unavailable || !width || !height)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::RENDER_UNAVAILABLE});
        }
        Impl::Viewport viewport;
        viewport.document = document;
        viewport.play = play;
        viewport.camera = std::move(camera);
        viewport
            .requested = {width, height, rdesc::ETextureFormat::RGBA8_UNORM, 1, render::ETargetKind::OFFSCREEN, true};
        const auto id = impl_->viewports.emplace(std::move(viewport));
        impl_->viewports.find(id)->id = id;
        return id;
    }

    bool ProjectScenes::updateViewport(
        SceneViewportId id,
        scene::VSceneCameraSource camera,
        std::uint32_t width,
        std::uint32_t height
    ) noexcept
    {
        auto* viewport = impl_->viewports.find(id);
        if (!viewport || viewport->closing || !width || !height)
        {
            return false;
        }
        viewport->camera = std::move(camera);
        viewport->requested.width = width;
        viewport->requested.height = height;
        if (!viewport->view.isNull())
        {
            auto& inputs = impl_->inputs(*viewport);
            auto request = inputs.findView(viewport->view)->request;
            request.camera = viewport->camera;
            static_cast<void>(inputs.updateView(viewport->view, std::move(request)));
        }
        return true;
    }

    void ProjectScenes::closeViewport(SceneViewportId id) noexcept
    {
        if (auto* viewport = impl_->viewports.find(id))
        {
            viewport->closing = true;
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
        auto error = viewport->error;
        if (!viewport->view.isNull() && error.type == 0)
        {
            error = impl_->projection(*viewport).viewError(viewport->view);
        }
        return {
            viewport->image,
            viewport->target,
            error,
            !viewport->closing && !viewport->pending && error.type == 0 && viewport->applied == viewport->requested &&
                viewport->image.value != 0 && impl_->ui->imagesApplied()
        };
    }
} // namespace lux::editor
