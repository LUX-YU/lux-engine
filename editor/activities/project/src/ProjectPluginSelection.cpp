#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <algorithm>
#include <thread>

namespace lux::editor
{
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
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
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
            std::span<const ProjectPluginEntry> based_on, std::vector<ProjectPluginEntry> desired
        )
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            if (publication_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "plugins.publication"});
            if (!std::ranges::equal(based_on, project_.manifest().plugins))
                return cxx::unexpected(EditorFailure{
                    EEditorError::STALE_REQUEST, "plugins.source", 0,
                    "Project selection changed. Revert the draft before trying again."
                });
            ProjectUpdate input;
            input.plugins = std::move(desired);
            auto prepared = project_.preparePublication(input);
            if (!prepared)
                return cxx::unexpected(prepared.error());
            publication_ = std::make_unique<ProjectPublicationOperation>(
                project_, runtime_, writes_, files_, execution_, std::move(*prepared)
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
        ProjectStorage& project, process::ExecutionRuntime& runtime, persistence::WriteCoordinator& writes,
        persistence::IArtifactStore& files, persistence::SaveExecution& execution
    ) : impl_(std::make_unique<Impl>(project, runtime, writes, files, execution))
    {}
    ProjectPluginSelection::~ProjectPluginSelection() = default;
    EditorResult<void> ProjectPluginSelection::request(
        std::span<const ProjectPluginEntry> based_on, std::vector<ProjectPluginEntry> desired
    )
    {
        return impl_->request(based_on, std::move(desired));
    }
    EditorResult<void> ProjectPluginSelection::retry() { return impl_->retry(); }
    EditorResult<void> ProjectPluginSelection::abandon() { return impl_->abandon(); }
    EditorResult<void> ProjectPluginSelection::acknowledge() { return impl_->acknowledge(); }
    EditorResult<void> ProjectPluginSelection::update() { return impl_->update(); }
    const VPublicationStatus* ProjectPluginSelection::status() const noexcept
    {
        return impl_->publication_ ? &impl_->publication_->status() : nullptr;
    }
    bool ProjectPluginSelection::settled() const noexcept
    {
        return !impl_->publication_ || impl_->publication_->terminal();
    }
}
