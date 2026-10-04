from pathlib import Path
s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
original = (s / 'editor/application/src/EditorProjectTools.cpp').read_text()
start = original.index('    void EditorApplication::Impl::maintainRecentProjects()')
end = original.index('    void EditorApplication::Impl::installRecentProjects', start)
body = original[start:end]
body = body.replace('void EditorApplication::Impl::maintainRecentProjects()', 'void updateCore(bool allow_new_work)')
body = body.replace('phase_ != EApplicationPhase::RUNNING', '!allow_new_work')
body = body.replace('applicationFailure(', 'recentFailure(')
body = body.replace('*config_.user_directory', 'directory_').replace('config_.project_file', 'project_')
body = body.replace('*engine_->execution().blocking()', '*tasks_.execution().blocking()')
body = body.replace('project_tasks_.submit', 'tasks_.submit')
body = body.replace('EditorResult<RecentProjects>', 'EditorResult<Prepared>')
body = body.replace('return RecentProjects{', 'return Prepared{').replace('TTaskResult<RecentProjects,', 'TTaskResult<Prepared,')
body = body.replace('        if (recent_ticket_)\n        {\n            if (std::exchange(recent_reconcile_, false))\n                if (auto result = writes_.reconcile(*recent_ticket_, files_); !result)\n                    recent_failure_ = recentFailure("recent.reconcile", result.error()).value();', '        if (recent_ticket_)\n        {')
assert 'config_' not in body and 'engine_' not in body and 'phase_' not in body
prefix = '''#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
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
    }
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

        Impl(std::filesystem::path directory, std::filesystem::path project, process::ExecutionRuntime& runtime,
             persistence::WriteCoordinator& writes, persistence::IArtifactStore& files,
             persistence::SaveExecution& execution)
            : directory_(std::move(directory)), project_(std::move(project)), writes_(writes), files_(files),
              execution_(execution), tasks_(runtime)
        {}
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
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
            auto drained = tasks_.execution().waitUntil([this]() noexcept {
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
            });
            if (!drained)
                std::terminate();
        }
'''
suffix = '''    };
    RecentProjects::RecentProjects(
        std::filesystem::path directory, std::filesystem::path project, process::ExecutionRuntime& runtime,
        persistence::WriteCoordinator& writes, persistence::IArtifactStore& files, persistence::SaveExecution& execution
    ) : impl_(std::make_unique<Impl>(std::move(directory), std::move(project), runtime, writes, files, execution))
    {}
    RecentProjects::~RecentProjects() = default;
    EditorResult<void> RecentProjects::refresh() { return impl_->refresh(); }
    EditorResult<void> RecentProjects::reconcile() { return impl_->reconcile(); }
    EditorResult<void> RecentProjects::update(bool allow_new_work) { return impl_->update(allow_new_work); }
    bool RecentProjects::settled() const noexcept { return impl_->settled(); }
    std::span<const std::filesystem::path> RecentProjects::entries() const noexcept { return impl_->recent_projects_; }
    std::optional<persistence::WriteTicket> RecentProjects::ticket() const noexcept { return impl_->recent_ticket_; }
    const persistence::VPublicationOutcome* RecentProjects::publication() const noexcept
    {
        return impl_->recent_publication_ ? &*impl_->recent_publication_ : nullptr;
    }
    const EditorFailure* RecentProjects::failure() const noexcept
    {
        return impl_->recent_failure_ ? &*impl_->recent_failure_ : nullptr;
    }
}
'''
target = s / 'editor/activities/project/src/RecentProjects.cpp'
assert not target.exists()
target.write_text(prefix + '\n'.join('    ' + line if line else '' for line in body.splitlines()) + '\n' + suffix)
