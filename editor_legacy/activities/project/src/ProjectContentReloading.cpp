#include <algorithm>
#include <lux/engine/editor/sessions/ReloadSessionOperation.hpp>
#include <lux/engine/editor/sessions/SessionOpening.hpp>
#include <lux/engine/editor/storage/ProjectContentReloading.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

namespace lux::editor
{
    namespace
    {
        bool isBusy(sessions::ESessionError error) noexcept
        {
            return error == sessions::ESessionError::BUSY;
        }
        bool isBusy(const sessions::SessionFactoryFailure& error) noexcept
        {
            return error.code == sessions::ESessionFactoryError::BUSY;
        }
        bool isBusy(const persistence::PersistenceFailure& error) noexcept
        {
            return error.code == persistence::EPersistenceError::BUSY;
        }
        bool isBusy(persistence::EPersistenceError error) noexcept
        {
            return error == persistence::EPersistenceError::BUSY;
        }
        template <class Error> auto failure(std::string domain, const Error& error)
        {
            return cxx::unexpected(EditorFailure{
                isBusy(error) ? EEditorError::BUSY : EEditorError::SOURCE_FAILURE,
                std::move(domain),
                0,
                {},
                error
            });
        }
        auto rejected(EEditorError code, std::string domain)
        {
            return cxx::unexpected(EditorFailure{code, std::move(domain)});
        }
    } // namespace
    struct ProjectContentReloading::Impl final
    {
        struct Record final
        {
            ProjectReloadReport report;
            std::unique_ptr<sessions::ReloadSessionOperation> operation;
        };
        std::shared_ptr<sessions::SessionStore> sessions_;
        std::shared_ptr<sessions::SessionOpening> opening_;
        ProjectStorage& project_;
        std::shared_ptr<persistence::WriteCoordinator> writes_;
        std::shared_ptr<persistence::IArtifactStore> files_;
        process::ExecutionRuntime& execution_;
        services::ServiceRegistry& services_;
        services::ServiceScope& scope_;
        const std::thread::id owner_{std::this_thread::get_id()};
        mutable bool dispatching_{};
        bool closing_{};
        std::vector<Record> records_;
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
        Impl(
            std::shared_ptr<sessions::SessionStore> sessions,
            std::shared_ptr<sessions::SessionOpening> opening,
            ProjectStorage& project,
            std::shared_ptr<persistence::WriteCoordinator> writes,
            std::shared_ptr<persistence::IArtifactStore> files,
            process::ExecutionRuntime& execution,
            services::ServiceRegistry& services,
            services::ServiceScope& scope
        )
            : sessions_(std::move(sessions)), opening_(std::move(opening)), project_(project),
              writes_(std::move(writes)), files_(std::move(files)), execution_(execution), services_(services),
              scope_(scope)
        {
            records_.reserve(64);
        }
        ~Impl()
        {
            dispatching_ = true;
        }
        EditorResult<void> admission() const noexcept
        {
            if (owner_ != std::this_thread::get_id())
            {
                return rejected(EEditorError::INVALID_STATE, "reload.owner-thread");
            }
            if (dispatching_)
            {
                return rejected(EEditorError::BUSY, "reload.dispatch");
            }
            return {};
        }
        EditorResult<void> request(sessions::ContentStamp source)
        {
            if (auto checked = admission(); !checked)
            {
                return checked;
            }
            Dispatch dispatch{dispatching_};
            if (closing_)
            {
                return rejected(EEditorError::CLOSING, "reload.admission");
            }
            if (records_.size() >= 64)
            {
                return rejected(EEditorError::CAPACITY, "reload.results");
            }
            auto current = sessions_->describe(source.session);
            if (!current)
            {
                return failure("reload.session", current.error());
            }
            if (current->current != source)
            {
                return failure("reload.source", sessions::ESessionError::STALE_CONTENT);
            }
            if (!current->binding)
            {
                return failure("reload.unbound", persistence::EPersistenceError::UNBOUND);
            }
            const bool has_active_source = std::ranges::any_of(
                records_,
                [&](const auto& other)
                {
                    const auto& report = other.report;
                    return report.source == source || (report.source.session == source.session && !report.result);
                }
            );
            if (has_active_source)
            {
                return rejected(EEditorError::BUSY, "reload.active");
            }
            const auto* found = project_.asset(current->binding->asset);
            if (!found)
            {
                return rejected(EEditorError::SOURCE_FAILURE, "reload.asset");
            }
            const auto asset = *found; // File/backend callbacks cannot invalidate this catalog observation.
            auto destination = files_->resolve(asset.source_path);
            if (!destination)
            {
                return failure("reload.target", destination.error());
            }
            auto bytes = project_.captureSource(asset.id, 64 * 1024 * 1024, destination->expected_version);
            if (!bytes)
            {
                return cxx::unexpected(bytes.error());
            }
            // Preserve the factory/code actually installed with the source, not today's replacement catalog.
            auto factory = opening_->factory(source.session);
            if (!factory)
            {
                return failure("reload.factory", factory.error());
            }
            const auto& format = (*factory)->descriptor().source;
            const bool is_same_format =
                format && format->canonical_name == asset.source_type && format->version == asset.source_version;
            if (!is_same_format)
            {
                return rejected(EEditorError::SOURCE_FAILURE, "reload.format");
            }
            sessions::SessionLoadInput
                input{std::move(*bytes), asset.id, current->binding, std::move(*destination), 64 * 1024 * 1024, source};
            auto operation = sessions::ReloadSessionOperation::start(
                execution_,
                *sessions_,
                *writes_,
                services_,
                scope_,
                std::move(*factory),
                std::move(input)
            );
            if (!operation)
            {
                return failure("reload.admission", operation.error());
            }
            records_.push_back({{source}, std::move(*operation)});
            return {};
        }
        EditorResult<void> update()
        {
            if (auto checked = admission(); !checked)
            {
                return checked;
            }
            Dispatch dispatch{dispatching_};
            for (auto& entry : records_)
            {
                if (!entry.operation)
                {
                    continue;
                }
                if (closing_)
                {
                    entry.operation->cancel();
                }
                entry.operation->update(opening_->find(entry.report.source.session));
                if (entry.operation->outcome())
                {
                    entry.report.result = *entry.operation->outcome();
                    // Foreign candidate/result cleanup still runs inside the same dispatch protection.
                    entry.operation.reset();
                }
            }
            return {};
        }
        EditorResult<void> acknowledge(sessions::ContentStamp source)
        {
            if (auto checked = admission(); !checked)
            {
                return checked;
            }
            Dispatch dispatch{dispatching_};
            const auto found =
                std::ranges::find_if(records_, [&](const auto& entry) { return entry.report.source == source; });
            if (found == records_.end())
            {
                return rejected(EEditorError::STALE_REQUEST, "reload.result");
            }
            if (!found->report.result)
            {
                return rejected(EEditorError::BUSY, "reload.result");
            }
            records_.erase(found);
            return {};
        }
        EditorResult<void> requestClose() noexcept
        {
            if (auto checked = admission(); !checked)
            {
                return checked;
            }
            Dispatch dispatch{dispatching_};
            closing_ = true;
            for (auto& entry : records_)
            {
                if (entry.operation)
                {
                    entry.operation->cancel();
                }
            }
            return {};
        }
        bool settled() const noexcept
        {
            return std::ranges::none_of(records_, [](const auto& entry) { return bool(entry.operation); });
        }
        EditorResult<std::vector<ProjectReloadReport>> reports() const
        {
            if (auto checked = admission(); !checked)
            {
                return cxx::unexpected(checked.error());
            }
            Dispatch dispatch{dispatching_};
            std::vector<ProjectReloadReport> result;
            result.reserve(records_.size());
            for (const auto& entry : records_)
            {
                result.push_back(entry.report);
            }
            return result;
        }
    };
    ProjectContentReloading::ProjectContentReloading(
        std::shared_ptr<sessions::SessionStore> sessions,
        std::shared_ptr<sessions::SessionOpening> opening,
        ProjectStorage& project,
        std::shared_ptr<persistence::WriteCoordinator> writes,
        std::shared_ptr<persistence::IArtifactStore> files,
        process::ExecutionRuntime& execution,
        services::ServiceRegistry& services,
        services::ServiceScope& scope
    )
        : impl_(std::make_unique<Impl>(
              std::move(sessions),
              std::move(opening),
              project,
              std::move(writes),
              std::move(files),
              execution,
              services,
              scope
          ))
    {
    }
    ProjectContentReloading::~ProjectContentReloading() = default;
    EditorResult<void> ProjectContentReloading::request(sessions::ContentStamp source)
    {
        return impl_->request(source);
    }
    EditorResult<void> ProjectContentReloading::update()
    {
        return impl_->update();
    }
    EditorResult<void> ProjectContentReloading::acknowledge(sessions::ContentStamp source)
    {
        return impl_->acknowledge(source);
    }
    EditorResult<void> ProjectContentReloading::requestClose() noexcept
    {
        return impl_->requestClose();
    }
    bool ProjectContentReloading::settled() const noexcept
    {
        return impl_->settled();
    }
    EditorResult<std::vector<ProjectReloadReport>> ProjectContentReloading::reports() const
    {
        return impl_->reports();
    }
} // namespace lux::editor

namespace lux::editor
{
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ProjectContentReloading, ProjectContentReloading>(
                services::ServiceNameView{"lux.editor.project.content-reloading"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.editor.sessions"},
             1,
             cxx::typeToken<sessions::SessionStore>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.sessions.opening"},
             1,
             cxx::typeToken<sessions::SessionOpening>(),
             services::EDependencyKind::SHARED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.storage"},
             1,
             cxx::typeToken<ProjectStorage>(),
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
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.registry"},
             1,
             cxx::typeToken<services::ServiceRegistry>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.services.scope"},
             1,
             cxx::typeToken<services::ServiceScope>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ProjectContentReloading>>
        createReloading(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto sessions = resolver.get<sessions::SessionStore>(0);
            if (!sessions)
            {
                return cxx::unexpected(std::move(sessions.error()));
            }
            auto opening = resolver.get<sessions::SessionOpening>(1);
            if (!opening)
            {
                return cxx::unexpected(std::move(opening.error()));
            }
            auto project = resolver.require<ProjectStorage>(2);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
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
            auto execution = resolver.require<process::ExecutionRuntime>(5);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            auto services = resolver.require<services::ServiceRegistry>(6);
            if (!services)
            {
                return cxx::unexpected(std::move(services.error()));
            }
            auto scope = resolver.require<services::ServiceScope>(7);
            if (!scope)
            {
                return cxx::unexpected(std::move(scope.error()));
            }
            return std::make_unique<ProjectContentReloading>(
                std::move(*sessions),
                std::move(*opening),
                project->get(),
                std::move(*writes),
                std::move(*files),
                execution->get(),
                services->get(),
                scope->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kProjectContentReloadingService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ProjectContentReloading, createReloading>(
            services::ServiceNameView{"lux.editor.project.content-reloading"},
            contracts,
            dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* instance) noexcept -> services::ServiceResult<bool>
        { return static_cast<const ProjectContentReloading*>(instance)->settled(); };
        descriptor.maintain = [](void* instance) noexcept -> services::ServiceResult<void>
        {
            auto result = static_cast<ProjectContentReloading*>(instance)->update();
            if (!result)
            {
                const auto& error = result.error();
                const auto code = error.code == EEditorError::BUSY ? services::EServiceError::BUSY
                                                                   : services::EServiceError::FACTORY_FAILURE;
                return cxx::unexpected(
                    services::ServiceFailure{code, error.message, error.domain, static_cast<std::uint64_t>(error.code)}
                );
            }
            return {};
        };
        return descriptor;
    }();
} // namespace lux::editor
