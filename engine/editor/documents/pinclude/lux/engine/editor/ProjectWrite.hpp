#pragma once

#include <lux/engine/editor/DocumentTask.hpp>
#include <lux/engine/editor/project/Project.hpp>

namespace lux::editor::detail
{
    struct PublicationPending final
    {
    };
    struct PublicationSucceeded final
    {
        EditorResult<void> cleanup;
    };
    struct PublicationAbandoned final
    {
    };
    using PublicationStatus =
        std::variant<PublicationPending, EditorFailure, PublicationSucceeded, PublicationAbandoned>;

    // Owns one reserved project publication until disk effects and Main adoption settle.
    // Both document saves and imports use this protocol; history tickets stay with the document.
    class ProjectWrite final
    {
        struct Publish final
        {
            const ProjectPublication *publication;
            std::stop_token stop;
            EditorResult<ProjectPublicationReceipt> operator()() const noexcept
            {
                return publishProjectFiles(*publication, stop);
            }
        };
        struct Recover final
        {
            std::filesystem::path root;
            EditorResult<void> operator()() const noexcept { return recoverProjectFiles(root); }
        };
        using Publishing = ScheduledDocumentTask<process::BlockingScheduler, Publish>;
        using Recovering = ScheduledDocumentTask<process::BlockingScheduler, Recover>;
        struct Idle final
        {
        };

      public:
        ProjectWrite(Project &project, process::ExecutionRuntime &runtime, ProjectPublication publication)
            : project_(project), runtime_(runtime), publication_(std::move(publication))
        {
            startPublishing();
        }
        ProjectWrite(const ProjectWrite &) = delete;
        ProjectWrite(ProjectWrite &&) = delete;
        ~ProjectWrite()
        {
            if (!terminal())
            {
                std::terminate();
            }
        }

        const PublicationStatus &status() const noexcept { return status_; }
        bool terminal() const noexcept { return status_.index() >= 2; }
        bool abandoning() const noexcept { return abandoning_; }

        EditorResult<void> retry()
        {
            if (!std::holds_alternative<EditorFailure>(status_) || work_.index() != 0 || finishing_)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.write.retry"});
            }
            status_ = PublicationPending{};
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
            stop_.request_stop();
            if (work_.index() == 0 && !finishing_)
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

        void poll()
        {
            if (finishing_)
            {
                return;
            }
            if (auto *work = std::get_if<Publishing>(&work_); work && work->ready())
            {
                auto result = work->take();
                work_.emplace<Idle>();
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
            if (auto *work = std::get_if<Recovering>(&work_); work && work->ready())
            {
                auto result = work->take();
                work_.emplace<Idle>();
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
            status_ = PublicationPending{};
            work_
                .emplace<Publishing>(runtime_, stdexec::then(stdexec::schedule(*runtime_.blocking()),
                                                             Publish{&publication_, stop_.get_token()}))
                .start();
        }
        void startRecovery()
        {
            status_ = PublicationPending{};
            work_
                .emplace<Recovering>(runtime_,
                                     stdexec::then(stdexec::schedule(*runtime_.blocking()), Recover{project_.root()}))
                .start();
        }
        void adopt()
        {
            if (finishing_)
            {
                return;
            }
            finishing_ = true;
            auto &receipt = std::get<ProjectPublicationReceipt>(receipt_);
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

        Project &project_;
        process::ExecutionRuntime &runtime_;
        ProjectPublication publication_;
        std::stop_source stop_;
        bool abandoning_{};
        bool finishing_{};
        PublicationStatus status_;
        std::variant<Idle, ProjectPublicationReceipt> receipt_;
        std::variant<Idle, Publishing, Recovering> work_;
    };
} // namespace lux::editor::detail
