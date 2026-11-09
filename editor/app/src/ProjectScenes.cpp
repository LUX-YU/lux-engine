#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <exec/task.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/Engine.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/scene/ScenePreparation.hpp>
#include <lux/engine/ui/Command.hpp>

#include <algorithm>
#include <map>

namespace lux::editor
{

    namespace
    {
        constexpr std::size_t scene_byte_limit = 256U << 20U;

        template <class E> VProjectSceneFailure taskFailure(const execution::ExecutionFailure<E>& failure) noexcept
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
            ESceneRunMode mode,
            const plugins::SceneRegistrations& registrations
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
            for (const auto& geometry : source.geometries())
            {
                if (!builder.selectGeometry(geometry))
                {
                    return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
                }
            }
            const auto is_presentation = [&](scene::SceneSystemDescription system) noexcept
            {
                const auto registration = std::ranges::find(
                    registrations.scene_systems,
                    system.type(),
                    &scene::SceneSystemRegistration::type
                );
                return registration != registrations.scene_systems.end() &&
                       std::ranges::find(registration->description->capabilities, "lux.scene.optional_presentation") !=
                           registration->description->capabilities.end();
            };
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

    struct SceneCreationCandidate::Impl final
    {
        object::ObjectId project;
        ProjectManifest::SceneRecord record;
        ProjectManifest expected, manifest;
        scene::ScenePackage package;
        std::unique_ptr<scene::SceneRuntime::Builder> builder;
    };

    SceneCreationCandidate::SceneCreationCandidate(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    SceneCreationCandidate::~SceneCreationCandidate() noexcept = default;

    struct ProjectScenes::Impl final
    {
        enum class ECloseCondition
        {
            PLAY,
            SAVE,
            ALL,
            OPERATIONS
        };

        struct CloseWaiter
        {
            CloseWaiter* next{};
            ECloseCondition kind;
            asset::AssetId document;
            void (*complete)(CloseWaiter&, FrameworkResult<void>) noexcept {};
            std::shared_ptr<const void> code_lifetime;
        };

        struct CloseCompletionSender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(FrameworkResult<void>)>;
            Impl& collection;
            ECloseCondition kind;
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
                    this->document = sender.document;
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](CloseWaiter& waiter, FrameworkResult<void> result) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver), std::move(result));
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

        std::optional<FrameworkResult<void>> closeResult(const CloseWaiter& waiter) const noexcept
        {
            if (waiter.kind == ECloseCondition::ALL && close_failure.type != 0)
            {
                return FrameworkResult<void>{cxx::unexpected(close_failure)};
            }
            if (waiter.kind == ECloseCondition::ALL || waiter.kind == ECloseCondition::OPERATIONS)
            {
                const bool operations_done = operations == 0;
                const bool objects_done = waiter.kind == ECloseCondition::OPERATIONS || documents.empty();
                return operations_done && objects_done ? std::optional{FrameworkResult<void>{}} : std::nullopt;
            }
            const auto entry = documents.find(waiter.document);
            if (entry == documents.end())
            {
                return FrameworkResult<void>{};
            }
            if (waiter.kind == ECloseCondition::PLAY)
            {
                if (!entry->second->play)
                {
                    return FrameworkResult<void>{};
                }
                if (entry->second->play->close_failure.type != 0)
                {
                    return FrameworkResult<void>{cxx::unexpected(entry->second->play->close_failure)};
                }
                return {};
            }
            return !entry->second->saving ? std::optional{FrameworkResult<void>{}} : std::nullopt;
        }

        static void deliverClose(CloseWaiter& waiter, FrameworkResult<void> result) noexcept
        {
            auto code_lifetime = std::move(waiter.code_lifetime);
            waiter.complete(waiter, std::move(result));
        }

        void awaitClose(CloseWaiter& waiter) noexcept
        {
            if (auto ready = closeResult(waiter))
            {
                deliverClose(waiter, std::move(*ready));
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
                auto ready = closeResult(*waiter);
                if (!ready)
                {
                    link = &waiter->next;
                    continue;
                }
                *link = waiter->next;
                deliverClose(*waiter, std::move(*ready));
            }
        }

        struct Play final
        {
            scene::SceneInstanceId instance;
            bool stopping{}, preparing{true};
            error::Error close_failure;
        };

        struct Entry final
        {
            std::unique_ptr<SceneDocument> document;
            std::filesystem::path path;
            std::optional<Play> play;
            bool closing{}, saving{};
        };

        ProjectScenes& collection;
        EditorContext& context;
        std::vector<scene::ResolvedSceneDependency> services;
        std::map<asset::AssetId, std::unique_ptr<Entry>> documents;
        std::optional<VProjectSceneFailure> failure;
        error::Error close_failure;
        std::size_t operations{};
        CloseWaiter* close_waiters{};
        asset::AssetId active_document;
        bool closing{}, catalog_write_pending{};

        Impl(
            ProjectScenes& collection,
            EditorContext& context,
            std::vector<scene::ResolvedSceneDependency> services
        ) noexcept
            : collection(collection), context(context), services(std::move(services))
        {
        }

        template <class Work> ProjectSceneResult<execution::TaskId> submitCreation(Work work) noexcept
        {
            if (closing || catalog_write_pending)
            {
                return cxx::unexpected(
                    VProjectSceneFailure{closing ? EProjectSceneError::CLOSED : EProjectSceneError::BUSY}
                );
            }
            catalog_write_pending = true;
            ++operations;
            auto accepted = context.tasks().submit(
                {"Create Scene", "Scene"},
                [this, work = std::move(work)](execution::TaskReporter reporter) mutable noexcept
                {
                    // File publication observes reporter cancellation explicitly; Main must adopt its actual result.
                    auto result = execution::asResult(stdexec::write_env(
                        stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), work(reporter)),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    ));
                    return stdexec::then(
                        std::move(result),
                        [this](auto result) noexcept { return finishCreate(collection, std::move(result)); }
                    );
                }
            );
            if (!accepted)
            {
                --operations;
                catalog_write_pending = false;
                return cxx::unexpected(VProjectSceneFailure{execution::toError(accepted.error())});
            }
            return *accepted;
        }

        ProjectSceneResult<ProjectManifest> creationManifest(const ProjectManifest::SceneRecord& record) const noexcept
        {
            auto manifest = context.manifest();
            manifest.scenes.push_back(record);
            if (!manifest.startup_scene)
            {
                manifest.startup_scene = record.id;
            }
            if (auto valid = validateProjectManifest(manifest); !valid)
            {
                return cxx::unexpected(VProjectSceneFailure{valid.error()});
            }
            return manifest;
        }

        exec::task<ProjectSceneResult<asset::AssetId>>
        load(std::filesystem::path path, execution::TaskReporter reporter) noexcept
        {
            auto& execution = execution::ExecutionRuntime::instance();
            const auto blocking = execution.blockingIoScheduler();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            reporter.setPhase("Read Scene");
            auto bytes = co_await execution::asResult(
                stdexec::schedule(*blocking) |
                stdexec::then([path, stop]() noexcept
                              { return platform::readFileBounded(path, scene_byte_limit, stop); })
            );
            if (!bytes)
            {
                co_return cxx::unexpected(taskFailure(bytes.error()));
            }
            reporter.setPhase("Decode Scene");
            auto decoded = co_await execution::asResult(
                stdexec::schedule(execution.cpuScheduler()) |
                stdexec::then(
                    [bytes = std::move(*bytes), stop, schemas = context.sceneRegistrations().components]() noexcept
                    {
                        auto backing = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
                        auto package =
                            scene::decodeScenePackage(cxx::SharedBytes<>::fromOwner(backing, *backing), stop);
                        if (!package)
                        {
                            return package;
                        }
                        if (auto migrated = scene::migrateScenePackageComponents(*package, schemas, stop); !migrated)
                        {
                            return decltype(package){cxx::unexpected(migrated.error())};
                        }
                        return package;
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
            auto entry = makeEntry(std::move(path));
            auto builder = co_await prepareInstance(
                *decoded,
                std::shared_ptr<const scene::SceneDescription>{decoded->scene, &decoded->scene->data()}
            );
            if (!builder)
            {
                co_return cxx::unexpected(builder.error());
            }
            reporter.setPhase("Create Scene instance");
            co_return co_await stdexec::write_env(
                adoptPackage(std::move(*decoded), std::move(entry), std::move(*builder)),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            );
        }

        exec::task<FrameworkResult<void>> discardInstance(scene::SceneInstanceId instance) noexcept
        {
            auto retired = context.retireScene(instance);
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            auto retirement = std::move(*retired);
            auto closed =
                co_await stdexec::continues_on(
                    retirement.whenComplete(),
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
            if (!closed)
            {
                co_return cxx::unexpected(closed.error());
            }
            co_return FrameworkResult<void>{};
        }

        std::unique_ptr<Entry> makeEntry(std::filesystem::path path)
        {
            auto entry = std::make_unique<Entry>();
            entry->path = std::move(path);
            return entry;
        }

        exec::task<ProjectSceneResult<std::unique_ptr<scene::SceneRuntime::Builder>>> prepareInstance(
            scene::ScenePackage& package,
            std::shared_ptr<const scene::SceneDescription> description
        ) noexcept
        {
            auto inputs = services;
            inputs.push_back(
                scene::makeSceneDependency<scene::ScenePackage>("package", "lux.scene.package", package)
            );
            inputs.push_back(scene::makeSceneDependency<execution::TaskScope>(
                "tasks",
                "lux.execution.tasks",
                context.tasks()
            ));
            const auto& registrations = context.sceneRegistrations();
            auto builder = std::make_unique<scene::SceneRuntime::Builder>(context.engine().sceneRuntime().builder());
            builder->setDescription(std::move(description))
                .setWorld(std::shared_ptr<const world::WorldDescription>{package.world, &package.world->data()})
                .setSimulation(std::shared_ptr<const simulation::SimulationDescription>{
                    package.simulation,
                    &package.simulation->data()
                })
                .setRegistrations(
                    registrations.components,
                    *registrations.simulation_systems,
                    registrations.scene_systems
                )
                .setContentDeclarations(
                    registrations.allowed_content,
                    registrations.geometries,
                    registrations.partitions
                );
            auto preparations = co_await context.engine().prepareScene(*builder, inputs, services);
            if (!preparations)
            {
                co_return cxx::unexpected(VProjectSceneFailure{preparations.error()});
            }
            auto qualified = builder->validate();
            if (!qualified)
            {
                co_return cxx::unexpected(VProjectSceneFailure{qualified.error()});
            }
            co_return std::move(builder);
        }

        exec::task<ProjectSceneResult<scene::SceneInstanceId>>
        buildInstance(scene::SceneRuntime::Builder& candidate) noexcept
        {
            auto created = co_await context.createSceneAsync(candidate);
            if (!created)
            {
                co_return cxx::unexpected(VProjectSceneFailure{created.error()});
            }
            static_cast<void>(context.engine().sceneRuntime().pauseSimulation(*created));
            if (closing)
            {
                auto discarded = co_await discardInstance(*created);
                if (!discarded)
                {
                    co_return cxx::unexpected(VProjectSceneFailure{discarded.error()});
                }
                co_return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
            }
            co_return *created;
        }

        exec::task<ProjectSceneResult<asset::AssetId>> adoptPackage(
            scene::ScenePackage package,
            std::unique_ptr<Entry> entry,
            std::unique_ptr<scene::SceneRuntime::Builder> builder
        ) noexcept
        {
            const auto id = package.scene->id();
            if (const auto found = documents.find(id); found != documents.end())
            {
                co_return cxx::unexpected(VProjectSceneFailure{
                    found->second->closing ? EProjectSceneError::BUSY : EProjectSceneError::DUPLICATE_DOCUMENT
                });
            }
            const auto& registrations = context.sceneRegistrations();
            auto content = scene::resolveGeometryDomains(
                package.scene->data().geometries(),
                package.world->data(),
                registrations.geometries
            );
            if (!content)
            {
                co_return cxx::unexpected(VProjectSceneFailure{VSceneEditFailure{content.error()}});
            }
            if (auto qualified = scene::validateWorldObjectDomains(
                    *content,
                    package.world->data(),
                    registrations.geometries,
                    package.partitions
                );
                !qualified)
            {
                co_return cxx::unexpected(VProjectSceneFailure{VSceneEditFailure{qualified.error()}});
            }
            auto candidate = co_await buildInstance(*builder);
            if (!candidate)
            {
                co_return cxx::unexpected(candidate.error());
            }
            const auto instance = *candidate;
            static_cast<void>(context.engine().sceneRuntime().pauseSimulation(instance));
            auto document = SceneDocument::create(
                context.engine().sceneRuntime(),
                instance,
                registrations.components,
                std::move(package),
                registrations.geometries
            );
            if (!document)
            {
                auto retired = context.retireScene(instance);
                if (!retired)
                {
                    co_return cxx::unexpected(VProjectSceneFailure{retired.error()});
                }
                auto retired_result = co_await stdexec::continues_on(
                    retired->whenComplete(),
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
                if (!retired_result)
                {
                    co_return cxx::unexpected(retired_result.error());
                }
                co_return cxx::unexpected(VProjectSceneFailure{document.error()});
            }
            entry->document = std::move(*document);
            documents.emplace(id, std::move(entry));
            // Publish the document, selection and notification in one Main-thread segment.
            // The enclosing Task's terminal delivery may resume on a later scheduler turn.
            failure.reset();
            active_document = id;
            static_cast<void>(collection.emit(collection.opened, id));
            co_return id;
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
            auto& execution = execution::ExecutionRuntime::instance();
            const auto blocking = execution.blockingIoScheduler();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            reporter.setPhase("Encode Scene snapshot");
            auto bytes = co_await execution::asResult(
                stdexec::schedule(execution.cpuScheduler()) |
                stdexec::then([&]() noexcept { return scene::encodeScenePackage(package, scene_byte_limit, stop); })
            );
            if (!bytes)
            {
                co_return cxx::unexpected(taskFailure(bytes.error()));
            }
            reporter.setPhase("Publish Scene file");
            auto written = co_await execution::asResult(
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

        exec::task<SceneCreationResult> prepareCreation(
            ProjectManifest::SceneRecord record,
            SceneCreateSelection selection,
            SceneProfileRegistration profile,
            ProjectManifest expected,
            ProjectManifest manifest,
            execution::TaskReporter reporter
        ) noexcept
        {
            const auto stop = reporter.stopToken();
            reporter.setPhase("Prepare Scene");
            auto package = co_await execution::asResult(
                stdexec::schedule(execution::ExecutionRuntime::instance().cpuScheduler()) |
                stdexec::then(
                    [&]() noexcept
                    { return profile.create({record.id, record.name, context.sceneRegistrations(), stop, &selection}); }
                )
            );
            if (!package)
            {
                co_return cxx::unexpected(taskFailure(package.error()));
            }
            if (closing || stop.stop_requested())
            {
                co_return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
            }
            const bool is_invalid_package =
                !package->scene || !package->world || !package->simulation || package->scene->id() != record.id;
            if (is_invalid_package)
            {
                co_return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
            }
            auto value = std::make_unique<SceneCreationCandidate::Impl>();
            value->project = collection.objectId();
            value->record = std::move(record);
            value->expected = std::move(expected);
            value->manifest = std::move(manifest);
            value->package = std::move(*package);
            auto builder = co_await prepareInstance(
                value->package,
                std::shared_ptr<const scene::SceneDescription>{value->package.scene, &value->package.scene->data()}
            );
            if (!builder)
            {
                co_return cxx::unexpected(builder.error());
            }
            value->builder = std::move(*builder);
            co_return std::unique_ptr<SceneCreationCandidate>{new SceneCreationCandidate(std::move(value))};
        }

        exec::task<ProjectSceneResult<asset::AssetId>>
        publishCreation(std::unique_ptr<SceneCreationCandidate> prepared, execution::TaskReporter reporter) noexcept
        {
            auto& value = *prepared->impl_;
            auto& record = value.record;
            auto& expected = value.expected;
            auto& candidate = value.manifest;
            auto& execution = execution::ExecutionRuntime::instance();
            const auto blocking = execution.blockingIoScheduler();
            if (!blocking)
            {
                co_return cxx::unexpected(VProjectSceneFailure{execution::toError(blocking.error())});
            }
            const auto stop = reporter.stopToken();
            auto encoded = co_await execution::asResult(
                stdexec::schedule(execution.cpuScheduler()) |
                stdexec::then([&]() noexcept
                              { return scene::encodeScenePackage(value.package, scene_byte_limit, stop); })
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
            auto published = co_await execution::asResult(
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
            co_return co_await adoptPackage(
                std::move(value.package),
                makeEntry(context.project().root / std::filesystem::u8path(record.path)),
                std::move(value.builder)
            );
        }

        exec::task<ProjectSceneResult<asset::AssetId>> createScene(
            ProjectManifest::SceneRecord record,
            SceneCreateSelection selection,
            SceneProfileRegistration profile,
            ProjectManifest expected,
            ProjectManifest manifest,
            execution::TaskReporter reporter
        ) noexcept
        {
            auto prepared = co_await prepareCreation(
                std::move(record),
                std::move(selection),
                std::move(profile),
                std::move(expected),
                std::move(manifest),
                reporter
            );
            if (!prepared)
            {
                co_return cxx::unexpected(prepared.error());
            }
            co_return co_await publishCreation(std::move(*prepared), reporter);
        }

        exec::task<ProjectSceneResult<scene::SceneInstanceId>> preparePlay(
            asset::AssetId id,
            scene::ScenePackage package,
            std::shared_ptr<const scene::SceneDescription> description
        ) noexcept
        {
            auto& entry = *documents.find(id)->second;
            auto& play = *entry.play;
            auto builder = co_await prepareInstance(package, std::move(description));
            if (!builder)
            {
                co_return cxx::unexpected(builder.error());
            }
            auto candidate = co_await buildInstance(**builder);
            if (!candidate)
            {
                co_return cxx::unexpected(candidate.error());
            }
            if (closing || entry.closing || play.stopping)
            {
                auto discarded = co_await discardInstance(*candidate);
                if (!discarded)
                {
                    co_return cxx::unexpected(VProjectSceneFailure{discarded.error()});
                }
                co_return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
            }
            const auto instance = *candidate;
            play.instance = instance;
            play.preparing = false;
            static_cast<void>(context.engine().sceneRuntime().resumeSimulation(play.instance));
            co_return instance;
        }

        exec::task<FrameworkResult<void>> closePlay(asset::AssetId id) noexcept
        {
            auto& entry = *documents.find(id)->second;
            auto& play = *entry.play;
            auto retired = context.retireScene(play.instance);
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            auto retired_result =
                co_await stdexec::continues_on(
                    retired->whenComplete(),
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
            if (!retired_result)
            {
                co_return cxx::unexpected(retired_result.error());
            }
            entry.play.reset();
            entry.document->setReadOnly(entry.closing);
            static_cast<void>(collection.emit(collection.changed));
            completeClosures();
            co_return FrameworkResult<void>{};
        }

        exec::task<FrameworkResult<void>> closeDocument(asset::AssetId id) noexcept
        {
            auto& entry = *documents.find(id)->second;
            if (auto closed = co_await stdexec::continues_on(
                    CloseCompletionSender{*this, ECloseCondition::PLAY, id},
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
                !closed)
            {
                co_return cxx::unexpected(closed.error());
            }
            if (auto closed = co_await stdexec::continues_on(
                    CloseCompletionSender{*this, ECloseCondition::SAVE, id},
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
                !closed)
            {
                co_return cxx::unexpected(closed.error());
            }
            auto retired = context.retireScene(entry.document->instance());
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            auto retired_result =
                co_await stdexec::continues_on(
                    retired->whenComplete(),
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
            if (!retired_result)
            {
                co_return cxx::unexpected(retired_result.error());
            }
            documents.erase(id);
            if (active_document == id)
            {
                active_document = documents.empty() ? asset::AssetId{} : documents.begin()->first;
            }
            static_cast<void>(collection.emit(collection.changed));
            completeClosures();
            co_return FrameworkResult<void>{};
        }

        void startClose(exec::task<FrameworkResult<void>> work, asset::AssetId document, bool play) noexcept
        {
            // These ordinary close operations borrow the collection; its enclosing close waits on this count.
            ++operations;
            auto result =
                execution::asResult(stdexec::starts_on(
                    execution::ExecutionRuntime::instance().mainScheduler(),
                    std::move(work)
                ));
            auto accepted = execution::ExecutionRuntime::instance().spawn(stdexec::then(
                std::move(result),
                [this, document, play](auto result) noexcept
                {
                    --operations;
                    if (!result)
                    {
                        const auto cause = result.error().domainFailure()
                                               ? *result.error().domainFailure()
                                               : execution::toError(execution::EExecutionError::INVALID_STATE);
                        failure = cause;
                        if (close_failure.type == 0)
                        {
                            close_failure = cause;
                        }
                        const auto found = documents.find(document);
                        if (play && found != documents.end() && found->second->play)
                        {
                            found->second->play->close_failure = cause;
                        }
                    }
                    completeClosures();
                }
            ));
            if (!accepted)
            {
                --operations;
                close_failure = execution::toError(accepted.error());
                failure = close_failure;
                completeClosures();
            }
        }

        using DocumentResult = execution::ExecutionResult<asset::AssetId, VProjectSceneFailure>;
        using SaveResult = execution::ExecutionResult<void, VProjectSceneFailure>;

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
            if (!collection.find(*result))
            {
                failure.reset();
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
                    auto text = error::format(scene::toError(value));
                    if (const auto* build = std::get_if<scene::SceneBuildFailure>(&value.cause))
                    {
                        if (build->compatibility)
                        {
                            text += ": " + build->compatibility->data;
                        }
                        if (build->content)
                        {
                            text += ": " + build->content->subject;
                        }
                    }
                    return text;
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

    ProjectScenes::ProjectScenes(EditorContext& context, std::vector<scene::ResolvedSceneDependency> services) noexcept
        : impl_(std::make_unique<Impl>(*this, context, std::move(services)))
    {
    }

    ProjectScenes::~ProjectScenes() noexcept
    {
        if (!settled())
        {
            std::terminate(); // Application maintenance settles accepted producers and Scene borrows first.
        }
    }

    exec::task<FrameworkResult<void>> ProjectScenes::initializeStartupScene(asset::AssetId startup) noexcept
    {
        auto& state = *impl_;
        const auto& manifest = state.context.manifest();
        const auto found = std::ranges::find(manifest.scenes, startup, &ProjectManifest::SceneRecord::id);
        if (found == manifest.scenes.end())
        {
            co_return cxx::unexpected(error::Error{
                Errors::EditorStartupSceneFailure,
                {0, static_cast<std::uint64_t>(EProjectError::INVALID_STARTUP_SCENE)}
            });
        }
        if (state.documents.contains(found->id))
        {
            co_return FrameworkResult<void>{};
        }
        ++state.operations;
        cxx::scope_exit finish(
            [&state]() noexcept
            {
                --state.operations;
                state.completeClosures();
            }
        );
        auto loaded = co_await execution::asResult(stdexec::starts_on(
            execution::ExecutionRuntime::instance().mainScheduler(),
            state.load(state.context.project().root / found->path, {})
        ));
        if (!loaded)
        {
            if (loaded.error().isCancelled())
            {
                co_return cxx::unexpected(error::Error{Errors::ProjectCancelled});
            }
            state.failure = taskFailure(loaded.error());
            const auto failure = std::visit(
                [category = state.failure->index()](const auto& value) noexcept -> error::Error
                {
                    using T = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<T, error::Error>)
                    {
                        return value;
                    }
                    else if constexpr (std::same_as<T, scene::SceneRuntimeFailure>)
                    {
                        return scene::toError(value);
                    }
                    else
                    {
                        std::uint64_t code{}, ordinal{};
                        if constexpr (std::is_enum_v<T>)
                        {
                            code = static_cast<std::uint64_t>(value);
                        }
                        else if constexpr (requires { value.code; })
                        {
                            code = static_cast<std::uint64_t>(value.code);
                        }
                        if constexpr (requires { value.ordinal; })
                        {
                            ordinal = value.ordinal;
                        }
                        return {Errors::EditorStartupSceneFailure, {category, code, ordinal}};
                    }
                },
                *state.failure
            );
            co_return cxx::unexpected(failure);
        }
        static_cast<void>(selectDocument(*loaded));
        co_return FrameworkResult<void>{};
    }

    ProjectSceneResult<execution::TaskId> ProjectScenes::open(std::filesystem::path path) noexcept
    {
        auto& state = *impl_;
        if (state.closing || path.empty())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
        }
        ++state.operations;
        auto admitted = state.context.tasks().submit(
            {"Open Scene", "Scene"},
            [this, path = std::move(path)](execution::TaskReporter reporter) mutable noexcept
            {
                auto result = execution::asResult(stdexec::starts_on(
                    execution::ExecutionRuntime::instance().mainScheduler(),
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

    ProjectSceneResult<execution::TaskId>
    ProjectScenes::create(ProjectManifest::SceneRecord record, SceneCreateSelection selection) noexcept
    {
        auto& state = *impl_;
        auto profile = state.context.sceneProfiles().find(record.profile);
        if (!profile)
        {
            return cxx::unexpected(VProjectSceneFailure{profile.error()});
        }
        auto manifest = state.creationManifest(record);
        if (!manifest)
        {
            return cxx::unexpected(manifest.error());
        }
        return state.submitCreation(
            [&state,
             record = std::move(record),
             selection = std::move(selection),
             profile = profile->get(),
             expected = state.context.manifest(),
             manifest = std::move(*manifest)](execution::TaskReporter reporter) mutable noexcept
            {
                return state.createScene(
                    std::move(record),
                    std::move(selection),
                    std::move(profile),
                    std::move(expected),
                    std::move(manifest),
                    reporter
                );
            }
        );
    }

    ProjectSceneResult<execution::TaskId> ProjectScenes::prepare(
        ProjectManifest::SceneRecord record,
        SceneCreateSelection selection,
        SceneCreationCompletion completion
    ) noexcept
    {
        auto& state = *impl_;
        if (state.closing)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
        }
        auto profile = state.context.sceneProfiles().find(record.profile);
        if (!profile)
        {
            return cxx::unexpected(VProjectSceneFailure{profile.error()});
        }
        auto expected = state.context.manifest();
        auto proposed = state.creationManifest(record);
        if (!proposed)
        {
            return cxx::unexpected(proposed.error());
        }
        auto manifest = std::move(*proposed);
        ++state.operations;
        auto admitted = state.context.tasks().submit(
            {"Validate Scene selection", "Scene"},
            [this,
             record = std::move(record),
             selection = std::move(selection),
             profile = profile->get(),
             expected = std::move(expected),
             manifest = std::move(manifest),
             completion = std::move(completion)](execution::TaskReporter reporter) mutable noexcept
            {
                auto work = impl_->prepareCreation(
                    std::move(record),
                    std::move(selection),
                    std::move(profile),
                    std::move(expected),
                    std::move(manifest),
                    reporter
                );
                return execution::asResult(
                           stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), std::move(work))
                       ) |
                       stdexec::then(
                           [this, completion = std::move(completion)](auto result) mutable noexcept
                           {
                               const auto status =
                                   result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                               // Deliver while this accepted operation still keeps the collection alive.
                               completion(
                                   result ? SceneCreationResult{std::move(*result)}
                                          : SceneCreationResult{cxx::unexpected(taskFailure(result.error()))}
                               );
                               --impl_->operations;
                               impl_->completeClosures();
                               return status;
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

    ProjectSceneResult<execution::TaskId>
    ProjectScenes::create(std::unique_ptr<SceneCreationCandidate> candidate) noexcept
    {
        auto& state = *impl_;
        if (state.closing)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::CLOSED});
        }
        if (state.catalog_write_pending)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::BUSY});
        }
        const bool is_stale = !candidate || candidate->impl_->project != objectId() ||
                              candidate->impl_->expected != state.context.manifest();
        if (is_stale)
        {
            return cxx::unexpected(VProjectSceneFailure{ProjectFailure{EProjectError::STALE_REVISION}});
        }
        return state.submitCreation([&state, candidate = std::move(candidate)](execution::TaskReporter reporter
                                    ) mutable noexcept { return state.publishCreation(std::move(candidate), reporter); }
        );
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
                auto result = execution::asResult(stdexec::write_env(
                    stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), std::move(work)),
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

    ProjectSceneResult<execution::TaskId> ProjectScenes::startPlay(asset::AssetId id, ESceneRunMode mode) noexcept
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
        auto description = previewSystems(*package, mode, impl_->context.sceneRegistrations());
        if (!description)
        {
            return cxx::unexpected(description.error());
        }
        entry.play.emplace();
        document->setReadOnly(true);
        ++impl_->operations;
        auto admitted = impl_->context.tasks().submit(
            {"Prepare preview", "Scene"},
            [this, id, package = std::move(*package), description = std::move(*description)](execution::TaskReporter
            ) mutable noexcept
            {
                auto work = stdexec::write_env(
                    stdexec::starts_on(
                        execution::ExecutionRuntime::instance().mainScheduler(),
                        impl_->preparePlay(id, std::move(package), std::move(description))
                    ),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                );
                return stdexec::then(
                    execution::asResult(std::move(work)),
                    [this, id](auto result) noexcept
                    {
                        --impl_->operations;
                        auto& entry = *impl_->documents.find(id)->second;
                        if (!result)
                        {
                            impl_->failure = taskFailure(result.error());
                            entry.play.reset();
                            entry.document->setReadOnly(entry.closing);
                        }
                        impl_->completeClosures();
                        static_cast<void>(emit(changed));
                        return result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                    }
                );
            }
        );
        if (!admitted)
        {
            --impl_->operations;
            entry.play.reset();
            document->setReadOnly(false);
            return cxx::unexpected(VProjectSceneFailure{execution::toError(admitted.error())});
        }
        static_cast<void>(emit(changed));
        return *admitted;
    }

    void ProjectScenes::stopPlay(asset::AssetId id) noexcept
    {
        const auto found = impl_->documents.find(id);
        if (found == impl_->documents.end() || !found->second->play || found->second->play->stopping)
        {
            return;
        }
        found->second->play->stopping = true;
        if (found->second->play->preparing)
        {
            // The accepted startup owns its candidate and will retire it before publishing completion.
            return;
        }
        static_cast<void>(impl_->context.engine().sceneRuntime().pauseSimulation(found->second->play->instance));
        impl_->startClose(impl_->closePlay(id), id, true);
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
        impl_->startClose(impl_->closeDocument(id), id, false);
        static_cast<void>(emit(changed));
        return {};
    }

    void ProjectScenes::requestClose() noexcept
    {
        impl_->closing = true;
        for (const auto& [id, entry] : impl_->documents)
        {
            static_cast<void>(close(id));
        }
    }

    exec::task<FrameworkResult<void>> ProjectScenes::closeAsync() noexcept
    {
        requestClose();
        co_return co_await stdexec::continues_on(
            Impl::CloseCompletionSender{*impl_, Impl::ECloseCondition::ALL},
            execution::ExecutionRuntime::instance().mainScheduler()
        );
    }

    exec::task<FrameworkResult<void>> ProjectScenes::recoverClose() noexcept
    {
        auto& state = *impl_;
        (void)co_await stdexec::continues_on(
            Impl::CloseCompletionSender{state, Impl::ECloseCondition::OPERATIONS},
            execution::ExecutionRuntime::instance().mainScheduler()
        );
        std::vector<scene::SceneCloseReceipt> receipts;
        for (auto& [id, entry] : state.documents)
        {
            const auto retired = state.context.retireScene(entry->document->instance());
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
            if (retired)
            {
                receipts.push_back(*retired);
                if (retired->failure().type != 0)
                {
                    auto retried = state.context.engine().sceneRuntime().retryClose(entry->document->instance());
                    if (!retried)
                    {
                        co_return cxx::unexpected(scene::toError(retried.error()));
                    }
                }
            }
            if (entry->play && entry->play->instance.valid())
            {
                const auto play = state.context.retireScene(entry->play->instance);
                if (!play)
                {
                    co_return cxx::unexpected(play.error());
                }
                if (play)
                {
                    receipts.push_back(*play);
                    if (play->failure().type != 0)
                    {
                        auto retried = state.context.engine().sceneRuntime().retryClose(entry->play->instance);
                        if (!retried)
                        {
                            co_return cxx::unexpected(scene::toError(retried.error()));
                        }
                    }
                }
            }
        }
        for (const auto& receipt : receipts)
        {
            auto finished = co_await stdexec::continues_on(
                receipt.whenComplete(),
                execution::ExecutionRuntime::instance().mainScheduler()
            );
            if (!finished)
            {
                co_return cxx::unexpected(finished.error());
            }
        }
        state.close_failure = {};
        state.documents.clear();
        state.completeClosures();
        co_return FrameworkResult<void>{};
    }

    bool ProjectScenes::settled() const noexcept
    {
        return impl_->operations == 0 && impl_->documents.empty();
    }

    const std::optional<VProjectSceneFailure>& ProjectScenes::failure() const noexcept
    {
        return impl_->failure;
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

} // namespace lux::editor
