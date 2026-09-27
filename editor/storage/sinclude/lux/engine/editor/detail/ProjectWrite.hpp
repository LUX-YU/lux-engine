#pragma once

#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/log/Log.hpp>

namespace lux::editor::detail
{
    // Owns one reserved project publication until disk effects and Main adoption settle.
    // Both document saves and imports use this protocol; history tickets stay with the document.
    class ProjectWrite final
    {
        struct Publish final
        {
            const ProjectPublication* publication;
            std::stop_token stop;
            EditorResult<ProjectPublicationReceipt> operator()() const noexcept
            {
                return publishProjectFiles(*publication, stop);
            }
        };
        struct Recover final
        {
            std::filesystem::path root;
            EditorResult<void> operator()() const noexcept
            {
                return recoverProjectFiles(root);
            }
        };
        struct Idle final
        {};

    public:
        ProjectWrite(
            ProjectStorage& project,
            process::ExecutionRuntime& runtime,
            ProjectPublication publication,
            process::CompletionWork::Request completed = {}
        )
            : project_(project), runtime_(runtime), completed_(std::move(completed)),
              adoption_(
                  runtime,
                  this,
                  [](void* owner) noexcept {
                      auto& write = *static_cast<ProjectWrite*>(owner);
                      write.adoptCompleted();
                      write.completed_.request();
                  }
              ),
              publication_(std::move(publication)), tasks_(runtime)
        {
            startPublishing();
        }
        ProjectWrite(const ProjectWrite&) = delete;
        ProjectWrite(ProjectWrite&&) = delete;
        ~ProjectWrite()
        {
            // Accepted disk work reaches a result, even when its UI owner has gone away.
            // A retained failure is not an invitation to retry forever during destruction.
            if (!runtime_.waitUntil([this]() noexcept { return !pending_; }))
                std::terminate();
            adoption_.cancel();
            if (const auto* failure = std::get_if<EditorFailure>(&status_))
                log::error(
                    "project.write",
                    "{}: {}; publication/recovery state retained on disk",
                    failure->domain,
                    failure->message
                );
        }

        const VPublicationStatus& status() const noexcept
        {
            return status_;
        }
        bool terminal() const noexcept
        {
            return status_.index() >= 2;
        }
        bool abandoning() const noexcept
        {
            return abandoning_;
        }

        EditorResult<void> retry()
        {
            if (!std::holds_alternative<EditorFailure>(status_) || pending_ || finishing_)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.write.retry"});
            }
            status_ = PublicationPending{};
            adoption_.request();
            if (receipt_.index() != 0)
            {
                adopt();
            }
            else if (abandoning_)
            {
                startRecovery();
            }
            else
            {
                startPublishing();
            }
            return {};
        }

        void abandon()
        {
            if (terminal() || abandoning_)
            {
                return;
            }
            abandoning_ = true;
            adoption_.request();
            stop_.request_stop();
            if (!pending_ && !finishing_)
            {
                if (receipt_.index() != 0)
                {
                    adopt();
                }
                else
                {
                    startRecovery();
                }
            }
        }

    private:
        void adoptCompleted()
        {
            if (finishing_)
            {
                return;
            }
            if (auto* completed = std::get_if<EditorResult<ProjectPublicationReceipt>>(&result_))
            {
                auto result = std::move(*completed);
                result_.emplace<Idle>();
                pending_ = false;
                if (result)
                {
                    receipt_.emplace<ProjectPublicationReceipt>(std::move(*result));
                    adopt();
                }
                else if (abandoning_)
                {
                    startRecovery();
                }
                else
                {
                    status_ = std::move(result.error());
                }
            }
            if (auto* completed = std::get_if<EditorResult<void>>(&result_))
            {
                auto result = std::move(*completed);
                result_.emplace<Idle>();
                pending_ = false;
                if (result)
                {
                    publication_ = {};
                    status_ = PublicationAbandoned{};
                }
                else
                {
                    status_ = std::move(result.error());
                }
            }
        }

    private:
        void startPublishing()
        {
            submit("Publish project", Publish{&publication_, stop_.get_token()});
        }
        void startRecovery()
        {
            submit("Recover project", Recover{project_.root()});
        }
        template <class Work> void submit(std::string name, Work work) noexcept
        {
            status_ = PublicationPending{};
            pending_ = true;
            auto started = tasks_.submit(
                {std::move(name), "Storage"},
                [scheduler = *runtime_.blocking(), work = std::move(work)](process::TaskReporter) mutable noexcept {
                    return stdexec::then(stdexec::schedule(scheduler), std::move(work));
                },
                [this](auto&& result) noexcept {
                    result_ = taskResult(std::move(result));
                    adoption_.request();
                }
            );
            if (!started)
            {
                pending_ = false;
                status_ =
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "project.write.submit", 0, {}, started.error()};
                completed_.request();
            }
        }
        void adopt()
        {
            if (finishing_)
            {
                return;
            }
            finishing_ = true;
            auto& receipt = std::get<ProjectPublicationReceipt>(receipt_);
            auto adopted = project_.adoptPublication(publication_, receipt);
            if (adopted)
            {
                auto cleanup = std::move(receipt.cleanup);
                receipt_.emplace<Idle>();
                publication_ = {};
                status_ = PublicationSucceeded{std::move(cleanup)};
            }
            else
            {
                status_ = std::move(adopted.error());
            }
            finishing_ = false;
        }

        ProjectStorage& project_;
        process::ExecutionRuntime& runtime_;
        process::CompletionWork::Request completed_;
        process::CompletionWork adoption_;
        ProjectPublication publication_;
        std::stop_source stop_;
        bool abandoning_{};
        bool finishing_{};
        bool pending_{};
        VPublicationStatus status_;
        std::variant<Idle, ProjectPublicationReceipt> receipt_;
        std::variant<Idle, EditorResult<ProjectPublicationReceipt>, EditorResult<void>> result_;
        process::TaskScope tasks_;
    };
} // namespace lux::editor::detail
