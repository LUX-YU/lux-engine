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
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/project/SceneRegistrations.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>

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
    } // namespace

    struct ProjectScenes::Impl final
    {
        struct Entry final
        {
            std::unique_ptr<SceneDocument> document;
            std::filesystem::path path;
            scene::SceneRenderInputs inputs;
            scene::RenderSystem* rendering{};
            std::optional<scene::InstanceRetirement> retirement;
            bool closing{};
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
            const auto world = std::shared_ptr<const world::WorldDescription>{package.world, &package.world->data()};
            const auto description =
                std::shared_ptr<const scene::SceneDescription>{package.scene, &package.scene->data()};
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
                    .emplace(rendering->mainClient(), context.engine().execution(), *assets, entry->inputs, *reads);
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
            auto adopted = context.adoptScene(std::move(*lease));
            if (!adopted)
            {
                // This uninterrupted Main segment entered with an open Context and built a valid lease.
                std::terminate();
            }
            static_cast<void>(runtime.pauseSimulation(instance));
            entry->document =
                std::make_unique<SceneDocument>(runtime, instance, registrations.components, std::move(package));
            auto& published = *documents.emplace(id, std::move(entry)).first->second;
            if (renders)
            {
                auto system = runtime.borrowSystem<scene::RenderSystem>(instance);
                if (!system)
                {
                    // Keep all constructor borrows until the ordinary Scene retirement has finished.
                    published.closing = true;
                    published.document->setReadOnly(true);
                    return cxx::unexpected(VProjectSceneFailure{system.error()});
                }
                published.rendering = *system;
            }
            return id;
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
                viewport.view = documents.find(viewport.document)->second->inputs.addView(std::move(request));
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
                auto& inputs = documents.find(viewport.document)->second->inputs;
                auto request = inputs.findView(viewport.view)->request;
                request.description = viewport.applied;
                static_cast<void>(inputs.updateView(viewport.view, std::move(request)));
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
                auto& entry = *documents.find(viewport.document)->second;
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
                        static_cast<void>(entry.inputs.removeView(viewport.view));
                        if (!entry.rendering->viewRetired(viewport.view))
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
                    const auto error = entry.rendering->viewError(viewport.view);
                    if (error.type != 0)
                    {
                        if (viewport.image.value != 0)
                        {
                            ui->removeTargetImage(viewport.image);
                            viewport.image = {};
                        }
                        return;
                    }
                    if (viewport.image.value == 0 && !entry.rendering->attachment(viewport.view).isNull())
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
                ++entry;
                continue;
            }
            bool has_view = false;
            for (const auto& viewport : impl_->viewports)
            {
                has_view |= viewport.document == entry->first;
            }
            if (has_view)
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
        std::uint32_t height
    ) noexcept
    {
        const auto found = impl_->documents.find(document);
        const bool unavailable =
            impl_->closing || found == impl_->documents.end() || found->second->closing || !found->second->rendering;
        if (unavailable || !width || !height)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::RENDER_UNAVAILABLE});
        }
        Impl::Viewport viewport;
        viewport.document = document;
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
            auto& inputs = impl_->documents.find(viewport->document)->second->inputs;
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
            error = impl_->documents.find(viewport->document)->second->rendering->viewError(viewport->view);
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
