#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor
{
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ProjectPluginSelection, ProjectPluginSelection>(
                services::ServiceNameView{"lux.editor.project.plugin-selection"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
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
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.files"},
             1,
             cxx::typeToken<persistence::IArtifactStore>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.persistence.execution"},
             1,
             cxx::typeToken<persistence::SaveExecution>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ProjectPluginSelection>>
        createSelection(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto project = resolver.require<ProjectStorage>(0);
            if (!project)
            {
                return cxx::unexpected(std::move(project.error()));
            }
            auto runtime = resolver.require<process::ExecutionRuntime>(1);
            if (!runtime)
            {
                return cxx::unexpected(std::move(runtime.error()));
            }
            auto writes = resolver.require<persistence::WriteCoordinator>(2);
            if (!writes)
            {
                return cxx::unexpected(std::move(writes.error()));
            }
            auto files = resolver.require<persistence::IArtifactStore>(3);
            if (!files)
            {
                return cxx::unexpected(std::move(files.error()));
            }
            auto execution = resolver.require<persistence::SaveExecution>(4);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            return std::make_unique<ProjectPluginSelection>(
                project->get(), runtime->get(), writes->get(), files->get(), execution->get()
            );
        }
    } // namespace
    constinit const services::ServiceDescriptor kProjectPluginSelectionService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ProjectPluginSelection, createSelection>(
            services::ServiceNameView{"lux.editor.project.plugin-selection"}, contracts, dependencies
        );
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.settled = [](const void* allocation) noexcept -> services::ServiceResult<bool>
        { return static_cast<const ProjectPluginSelection*>(allocation)->settled(); };
        descriptor.maintain = [](void* allocation) noexcept -> services::ServiceResult<void>
        {
            auto maintained = static_cast<ProjectPluginSelection*>(allocation)->update();
            if (!maintained)
            {
                const auto& error = maintained.error();
                return cxx::unexpected(services::ServiceFailure{
                    error.code == EEditorError::BUSY ? services::EServiceError::BUSY
                                                     : services::EServiceError::FACTORY_FAILURE,
                    error.message,
                    error.domain,
                    static_cast<std::uint64_t>(error.code)
                });
            }
            return {};
        };
        return descriptor;
    }();

    struct ProjectPluginSelection::Impl final
    {
        ProjectStorage& project_;
        process::ExecutionRuntime& runtime_;
        persistence::WriteCoordinator& writes_;
        persistence::IArtifactStore& files_;
        persistence::SaveExecution& execution_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        std::unique_ptr<ProjectPublicationOperation> publication_;

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

        ~Impl()
        {
            dispatching_ = true;
            publication_.reset(); // Its accepted completions drain before the borrowed providers are released.
        }

        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "plugins.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugins.dispatch"});
            return {};
        }
        EditorResult<void> request(
            std::span<const ProjectPluginEntry> based_on,
            std::vector<ProjectPluginEntry> desired
        )
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (publication_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugins.publication"});
            if (!std::ranges::equal(based_on, project_.manifest().plugins))
                return cxx::unexpected(EditorFailure{
                    EEditorError::STALE_REQUEST,
                    "plugins.source",
                    0,
                    "Project selection changed. Revert the draft before trying again."
                });
            ProjectUpdate input;
            input.plugins = std::move(desired);
            auto prepared = project_.preparePublication(input);
            if (!prepared)
                return cxx::unexpected(prepared.error());
            publication_ = std::make_unique<ProjectPublicationOperation>(
                project_,
                runtime_,
                writes_,
                files_,
                execution_,
                std::move(*prepared)
            );
            return {};
        }
        EditorResult<void> retry()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (!publication_)
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "plugins.publication"});
            return publication_->retry();
        }
        EditorResult<void> abandon()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (publication_)
                publication_->abandon();
            return {};
        }
        EditorResult<void> acknowledge()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (publication_ && !publication_->terminal())
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugins.acknowledge"});
            publication_.reset();
            return {};
        }
        EditorResult<void> update()
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (publication_)
                publication_->update();
            return {};
        }
    };

    ProjectPluginSelection::ProjectPluginSelection(
        ProjectStorage& project,
        process::ExecutionRuntime& runtime,
        persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files,
        persistence::SaveExecution& execution
    )
        : impl_(std::make_unique<Impl>(project, runtime, writes, files, execution))
    {
    }
    ProjectPluginSelection::~ProjectPluginSelection() = default;
    EditorResult<void> ProjectPluginSelection::request(
        std::span<const ProjectPluginEntry> based_on,
        std::vector<ProjectPluginEntry> desired
    )
    {
        return impl_->request(based_on, std::move(desired));
    }
    EditorResult<void> ProjectPluginSelection::retry()
    {
        return impl_->retry();
    }
    EditorResult<void> ProjectPluginSelection::abandon()
    {
        return impl_->abandon();
    }
    EditorResult<void> ProjectPluginSelection::acknowledge()
    {
        return impl_->acknowledge();
    }
    EditorResult<void> ProjectPluginSelection::update()
    {
        return impl_->update();
    }
    const VPublicationStatus* ProjectPluginSelection::status() const noexcept
    {
        return impl_->publication_ ? &impl_->publication_->status() : nullptr;
    }
    bool ProjectPluginSelection::settled() const noexcept
    {
        return !impl_->publication_ || impl_->publication_->terminal();
    }
} // namespace lux::editor
