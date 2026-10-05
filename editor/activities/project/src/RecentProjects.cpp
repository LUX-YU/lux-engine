#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <sstream>
#include <thread>

namespace lux::editor
{
    namespace
    {
        template <class Error> auto recentFailure(std::string domain, const Error& cause)
        {
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, cause});
        }
    } // namespace
    struct RecentProjects::Impl final
    {
        struct Prepared final
        {
            std::vector<std::filesystem::path> paths;
            persistence::WriteTarget target;
            persistence::EncodedArtifact encoded;
        };
        const std::filesystem::path directory_, project_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        persistence::SaveExecution& execution_;
        process::TaskScope tasks_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        bool recent_requested_{true};
        std::optional<process::TaskId> recent_task_;
        std::optional<EditorResult<Prepared>> recent_result_;
        std::vector<std::filesystem::path> recent_projects_;
        std::optional<persistence::WriteTicket> recent_ticket_;
        std::optional<persistence::VPublicationOutcome> recent_publication_;
        std::optional<EditorFailure> recent_failure_;

        Impl(
            std::filesystem::path directory,
            std::filesystem::path project,
            process::ExecutionRuntime& runtime,
            persistence::WriteCoordinator& writes,
            persistence::IArtifactStore& files,
            persistence::SaveExecution& execution
        )
            : directory_(directory.lexically_normal()), project_(project.lexically_normal()), writes_(writes),
              files_(files), execution_(execution), tasks_(runtime)
        {
        }
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "recent.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recent.dispatch"});
            return {};
        }
        EditorResult<void> refresh()
        {
            if (auto ready = admission(); !ready)
                return ready;
            recent_requested_ = true;
            return {};
        }
        EditorResult<void> reconcile()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (!recent_ticket_)
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "recent.publication"});
            auto result = writes_.reconcile(*recent_ticket_, files_);
            if (!result)
                return recentFailure("recent.reconcile", result.error());
            return {};
        }
        EditorResult<void> update(bool allow_new_work)
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            updateCore(allow_new_work);
            return {};
        }
        bool settled() const noexcept
        {
            return !recent_task_ && !recent_result_ && !recent_ticket_;
        }
        ~Impl()
        {
            dispatching_ = true;
            tasks_.requestStop();
            if (!tasks_.join())
                std::terminate();
            // RAII uses the original execution/completion path. No accepted disk fact is forgotten
            // when a caller destroys this activity before its ordinary maintenance has settled it.
            auto drained = tasks_.execution().waitUntil(
                [this]() noexcept
                {
                    updateCore(false);
                    if (settled())
                        return true;
                    if (recent_ticket_)
                    {
                        auto status = writes_.status(*recent_ticket_);
                        if (!status)
                            std::terminate();
                        if (status->stage == persistence::EWriteStage::UNKNOWN)
                        {
                            if (!writes_.reconcile(*recent_ticket_, files_))
                                std::terminate();
                            status = writes_.status(*recent_ticket_);
                            if (!status || status->stage == persistence::EWriteStage::UNKNOWN)
                                std::terminate();
                            updateCore(false);
                            if (settled())
                                return true;
                        }
                    }
                    if (!execution_.submitReady())
                        std::terminate();
                    return false;
                }
            );
            if (!drained)
                std::terminate();
        }
        void updateCore(bool allow_new_work)
        {
            if (recent_ticket_)
            {
                const auto status = writes_.status(*recent_ticket_);
                if (!status)
                    recent_failure_ = recentFailure("recent.status", status.error()).value();
                if (status && status->stage == persistence::EWriteStage::TERMINAL)
                {
                    recent_publication_ = status->outcome;
                    if (auto acknowledged = writes_.acknowledge(*recent_ticket_); acknowledged)
                        recent_ticket_.reset();
                    else
                        recent_failure_ = recentFailure("recent.acknowledge", acknowledged.error()).value();
                }
            }
            if (recent_result_)
            {
                if (!*recent_result_)
                {
                    recent_failure_ = recent_result_->error();
                    recent_result_.reset();
                }
                else if (!allow_new_work)
                    recent_result_.reset(); // Reading is complete; no new write is admitted while closing.
                else
                {
                    auto& value = **recent_result_;
                    auto ticket = persistence::publishEncodedArtifact(writes_, value.target, value.encoded);
                    if (ticket)
                    {
                        recent_projects_ = std::move(value.paths);
                        recent_ticket_ = *ticket;
                        recent_publication_.reset();
                        recent_failure_.reset();
                        recent_result_.reset();
                    }
                    else
                    {
                        const bool is_retryable = ticket.error().code == persistence::EPersistenceError::BUSY ||
                                                  ticket.error().code == persistence::EPersistenceError::CAPACITY;
                        if (!is_retryable)
                        {
                            recent_failure_ = recentFailure("recent.publish", ticket.error()).value();
                            recent_result_.reset();
                        }
                    }
                }
            }
            const bool has_pending = recent_task_ || recent_result_ || recent_ticket_;
            const bool can_start = recent_requested_ && allow_new_work && !has_pending;
            if (!can_start)
                return;
            const bool is_absolute = directory_.is_absolute() && project_.is_absolute();
            if (!is_absolute)
            {
                recent_requested_ = false;
                recent_failure_ = EditorFailure{EEditorError::INVALID_ARGUMENT, "recent.root"};
                return;
            }
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
            {
                recent_requested_ = false;
                recent_failure_ = recentFailure("recent.scheduler", blocking.error()).value();
                return;
            }
            const auto path = directory_ / "lux/editor/recent-projects.toml";
            auto accepted = tasks_.submit(
                {"Read recent projects", "Preferences"},
                [path, directory = directory_, project = project_, scheduler = *blocking](process::TaskReporter
                ) noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [path, directory, project]() -> EditorResult<Prepared>
                        {
                            const auto utf8 = [](const std::filesystem::path& value)
                            {
                                const auto bytes = value.generic_u8string();
                                return std::string{bytes.begin(), bytes.end()};
                            };
                            storage::FileArtifactStore store(directory);
                            auto target = store.resolve(utf8(path));
                            if (!target)
                                return recentFailure("recent.read", target.error());
                            std::vector<std::filesystem::path> paths{project};
                            if (target->expected_version != "missing")
                            {
                                auto bytes = storage::readPublicationFile(path, 64 * 1024);
                                if (!bytes)
                                    return recentFailure("recent.read", bytes.error());
                                if (storage::publicationDigest(*bytes) != target->expected_version)
                                    return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.changed"}
                                    );
                                const std::string_view text{
                                    reinterpret_cast<const char*>(bytes->data()),
                                    bytes->size()
                                };
                                auto parsed = toml::parse(text);
                                const bool is_valid =
                                    parsed && parsed["version"].value_or(0) == 1 && parsed["projects"].is_array();
                                if (!is_valid)
                                    return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "recent.format"}
                                    );
                                for (const auto& row : *parsed["projects"].as_array())
                                {
                                    auto value = row.value<std::string>();
                                    const bool is_invalid = !value || value->empty() || value->size() > 4096;
                                    if (is_invalid)
                                        return cxx::unexpected(
                                            EditorFailure{EEditorError::SOURCE_FAILURE, "recent.entry"}
                                        );
                                    auto candidate = std::filesystem::u8path(*value).lexically_normal();
                                    if (!candidate.is_absolute())
                                        return cxx::unexpected(
                                            EditorFailure{EEditorError::SOURCE_FAILURE, "recent.path"}
                                        );
                                    const bool has_capacity = paths.size() < 20;
                                    const bool is_unique = std::ranges::find(paths, candidate) == paths.end();
                                    if (has_capacity && is_unique)
                                        paths.push_back(std::move(candidate));
                                }
                            }
                            toml::array rows;
                            for (const auto& value : paths)
                                rows.push_back(utf8(value));
                            std::ostringstream output;
                            output << toml::table{{"version", 1}, {"projects", std::move(rows)}};
                            const auto encoded = output.str();
                            if (encoded.size() > 64 * 1024)
                                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "recent.bytes"});
                            const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
                            return Prepared{
                                std::move(paths),
                                std::move(*target),
                                persistence::EncodedArtifact{{bytes.begin(), bytes.end()}}
                            };
                        }
                    );
                },
                [this](process::TTaskResult<Prepared, EditorFailure>&& result) noexcept
                {
                    recent_task_.reset();
                    if (result)
                        recent_result_.emplace(std::move(*result));
                    else if (auto* error = result.error().domainFailure())
                        recent_result_.emplace(cxx::unexpected(std::move(*error)));
                    else
                        recent_result_.emplace(recentFailure("recent.task", result.error()));
                }
            );
            if (accepted)
            {
                recent_task_ = *accepted;
                recent_requested_ = false;
            }
            else
                recent_failure_ = recentFailure("recent.submit", accepted.error()).value();
        }
    };
    RecentProjects::RecentProjects(
        std::filesystem::path directory,
        std::filesystem::path project,
        process::ExecutionRuntime& runtime,
        persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files,
        persistence::SaveExecution& execution
    )
        : impl_(std::make_unique<Impl>(std::move(directory), std::move(project), runtime, writes, files, execution))
    {
    }
    RecentProjects::RecentProjects(
        std::filesystem::path directory,
        std::filesystem::path project,
        process::ExecutionRuntime& runtime,
        std::shared_ptr<persistence::WriteCoordinator> writes,
        std::shared_ptr<persistence::IArtifactStore> files,
        std::shared_ptr<persistence::SaveExecution> execution
    )
        : writes_owner_(std::move(writes)), files_owner_(std::move(files)), execution_owner_(std::move(execution)),
          impl_(std::make_unique<Impl>(
              std::move(directory), std::move(project), runtime, *writes_owner_, *files_owner_, *execution_owner_
          ))
    {
    }
    RecentProjects::~RecentProjects() = default;
    EditorResult<void> RecentProjects::refresh()
    {
        return impl_->refresh();
    }
    EditorResult<void> RecentProjects::reconcile()
    {
        return impl_->reconcile();
    }
    EditorResult<void> RecentProjects::update(bool allow_new_work)
    {
        return impl_->update(allow_new_work);
    }
    bool RecentProjects::settled() const noexcept
    {
        return impl_->settled();
    }
    std::span<const std::filesystem::path> RecentProjects::entries() const noexcept
    {
        return impl_->recent_projects_;
    }
    std::optional<persistence::WriteTicket> RecentProjects::ticket() const noexcept
    {
        return impl_->recent_ticket_;
    }
    const persistence::VPublicationOutcome* RecentProjects::publication() const noexcept
    {
        return impl_->recent_publication_ ? &*impl_->recent_publication_ : nullptr;
    }
    const EditorFailure* RecentProjects::failure() const noexcept
    {
        return impl_->recent_failure_ ? &*impl_->recent_failure_ : nullptr;
    }

    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<RecentProjects, RecentProjects>(
                services::ServiceNameView{"lux.editor.project.recent"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.user-directory"},
             1,
             cxx::typeToken<std::filesystem::path>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.writes"},
             1,
             cxx::typeToken<persistence::WriteCoordinator>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.files"},
             1,
             cxx::typeToken<persistence::IArtifactStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.execution"},
             1,
             cxx::typeToken<persistence::SaveExecution>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<RecentProjects>>
        createRecent(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto directory = resolver.require<std::filesystem::path>(0);
            if (!directory)
            {
                return cxx::unexpected(std::move(directory.error()));
            }
            if (!directory->get().is_absolute())
            {
                return cxx::unexpected(services::ServiceFailure{
                    services::EServiceError::INVALID_CONFIGURATION, "Recent projects require an absolute user directory"
                });
            }
            auto project = resolver.require<ProjectStorage>(1);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
            }
            auto execution = resolver.require<process::ExecutionRuntime>(2);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            auto writes = resolver.get<persistence::WriteCoordinator>(3);
            if (!writes)
            {
                return cxx::unexpected(std::move(writes.error()));
            }
            auto files = resolver.get<persistence::IArtifactStore>(4);
            if (!files)
            {
                return cxx::unexpected(std::move(files.error()));
            }
            auto publishing = resolver.get<persistence::SaveExecution>(5);
            if (!publishing)
            {
                return cxx::unexpected(std::move(publishing.error()));
            }
            return std::make_unique<RecentProjects>(
                directory->get(),
                project->get().projectFile(),
                execution->get(),
                std::move(*writes),
                std::move(*files),
                std::move(*publishing)
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kRecentProjectsService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<RecentProjects, createRecent>(
            services::ServiceNameView{"lux.editor.project.recent"}, contracts, dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* instance) noexcept -> services::ServiceResult<bool>
        { return static_cast<const RecentProjects*>(instance)->settled(); };
        // The close use case still supplies permission for new writes to update(bool). Registering
        // an unconditional maintain callback here would admit a write during a reversible review.
        return descriptor;
    }();
} // namespace lux::editor
