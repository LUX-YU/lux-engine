#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>

namespace lux::editor
{
    namespace
    {
        auto failed(EEditorError code)
        {
            return lux::cxx::unexpected(EditorFailure{code, "editor"});
        }

        void report(const EditorFailure &error)
        {
            std::fprintf(stderr, "%s:%llu %s\n", error.domain.c_str(), static_cast<unsigned long long>(error.reason),
                         error.message.c_str());
        }

        struct ScopeClosed final
        {
            using receiver_concept = stdexec::receiver_t;
            std::atomic<bool> *done;

            stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

            void set_value() && noexcept
            {
                done->store(true, std::memory_order_release);
            }
        };
    } // namespace

    struct Editor::Opening final
    {
        DocumentKey key;
        std::unique_ptr<DocumentOpening> work;
        std::vector<OpenRequestId> waiters;
    };

    struct Editor::Request final
    {
        OpenRequestId id;
        OpenRequestStatus status{OpenPending{}};
    };

    Editor::Editor(EditorConfig config) : config_(std::move(config))
    {
        requests_.reserve(config_.limits.open_requests);
        openings_.reserve(config_.limits.documents);
        documents_.reserve(config_.limits.documents);
    }

    Editor::~Editor() = default;

    EditorResult<void> Editor::registerDocument(DocumentRegistration registration)
    {
        if (state_ == EState::CLOSING || state_ == EState::FINISHED)
        {
            return failed(EEditorError::CLOSING);
        }
        if (registration.type.empty() || !registration.open)
        {
            return failed(EEditorError::INVALID_ARGUMENT);
        }
        if (std::ranges::find(registrations_, registration.type, &DocumentRegistration::type) != registrations_.end())
        {
            return failed(EEditorError::INVALID_ARGUMENT);
        }
        registrations_.push_back(std::move(registration));
        return {};
    }

    EditorResult<OpenRequestId> Editor::requestOpen(const OpenDocumentRequest &request)
    {
        if (state_ != EState::RUNNING || !project_ || exit_requested_)
        {
            return failed(EEditorError::CLOSING);
        }
        if (request.key.project != project_->manifest().id || request.key.source.isNull())
        {
            return failed(EEditorError::INVALID_ARGUMENT);
        }
        if (requests_.size() == config_.limits.open_requests || next_request_ == UINT64_MAX)
        {
            return failed(EEditorError::CAPACITY);
        }

        const OpenRequestId id{next_request_};
        for (const auto &document : documents_.values())
        {
            if (document->summary().key == request.key)
            {
                if (document->closeStatus().state != ECloseState::OPEN)
                {
                    return failed(EEditorError::CLOSING);
                }
                requests_.push_back({id, document->handle()});
                ++next_request_;
                return id;
            }
        }
        for (const auto &opening : openings_)
        {
            if (opening->key == request.key)
            {
                if (opening->waiters.empty())
                {
                    return failed(EEditorError::CLOSING);
                }
                opening->waiters.push_back(id);
                requests_.push_back({id});
                ++next_request_;
                return id;
            }
        }

        if (documents_.size() + openings_.size() >= config_.limits.documents)
        {
            return failed(EEditorError::CAPACITY);
        }
        const auto registration = std::ranges::find(registrations_, request.key.type, &DocumentRegistration::type);
        if (registration == registrations_.end())
        {
            return failed(EEditorError::MISSING_PROVIDER);
        }
        const auto factory = registration->open;
        auto opening = std::make_unique<Opening>(Opening{request.key, {}, {id}});
        auto *pending = opening.get();
        openings_.push_back(std::move(opening));
        requests_.push_back({id});
        ++next_request_;

        // The key is occupied before provider code can complete inline or reenter requestOpen.
        auto work = factory(*project_, request);
        if (work && *work)
        {
            pending->work = std::move(*work);
            if (pending->waiters.empty() || exit_requested_)
            {
                pending->work->cancel();
            }
        }
        else
        {
            const auto failure = work ? EditorFailure{EEditorError::INVALID_STATE, "document.factory"} : work.error();
            for (const auto waiter : pending->waiters)
            {
                std::ranges::find(requests_, waiter, &Request::id)->status = failure;
            }
            std::erase_if(openings_, [pending](const auto &value) { return value.get() == pending; });
        }
        return id;
    }

    EditorResult<void> Editor::cancelOpen(OpenRequestId id)
    {
        const auto found = std::ranges::find(requests_, id, &Request::id);
        if (found == requests_.end())
        {
            return failed(EEditorError::STALE_REQUEST);
        }
        if (!std::holds_alternative<OpenPending>(found->status))
        {
            return failed(EEditorError::INVALID_STATE);
        }
        found->status = OpenCancelled{};
        for (const auto &opening : openings_)
        {
            const auto removed = std::erase(opening->waiters, id);
            if (removed && opening->waiters.empty() && opening->work)
            {
                opening->work->cancel();
            }
        }
        return {};
    }

    EditorResult<OpenRequestStatus> Editor::openStatus(OpenRequestId id) const
    {
        const auto found = std::ranges::find(requests_, id, &Request::id);
        if (found == requests_.end())
        {
            return failed(EEditorError::STALE_REQUEST);
        }
        return found->status;
    }

    EditorResult<void> Editor::acknowledgeOpen(OpenRequestId id)
    {
        const auto found = std::ranges::find(requests_, id, &Request::id);
        if (found == requests_.end())
        {
            return failed(EEditorError::STALE_REQUEST);
        }
        if (std::holds_alternative<OpenPending>(found->status))
        {
            return failed(EEditorError::BUSY);
        }
        requests_.erase(found);
        return {};
    }

    EditorResult<std::reference_wrapper<DocumentEditor>> Editor::document(DocumentHandle handle)
    {
        const auto *value = documents_.find(handle);
        if (!value)
        {
            return failed(EEditorError::STALE_DOCUMENT);
        }
        return std::ref(**value);
    }

    std::vector<DocumentSummary> Editor::documents() const
    {
        std::vector<DocumentSummary> result;
        result.reserve(documents_.size());
        for (const auto &document : documents_.values())
        {
            result.push_back(document->summary());
        }
        return result;
    }

    void Editor::requestExit() noexcept
    {
        exit_requested_ = true;
    }

    void Editor::fail(EditorFailure failure)
    {
        if (outcome_)
        {
            report(failure);
            outcome_ = lux::cxx::unexpected(std::move(failure));
        }
        requestExit();
    }

    void Editor::acceptOpenings()
    {
        for (std::size_t index{}; index < openings_.size();)
        {
            auto &opening = *openings_[index];
            opening.work->poll();
            if (!opening.work->settled())
            {
                ++index;
                continue;
            }
            auto result = opening.work->take();
            OpenRequestStatus status;
            if (result && *result)
            {
                auto *document = result->get();
                const auto handle = documents_.insert(std::move(*result));
                document->handle_ = handle;
                status = handle;
                if (opening.waiters.empty() || exit_requested_)
                {
                    document->requestClose();
                }
            }
            else
            {
                const auto failure =
                    result ? EditorFailure{EEditorError::INVALID_STATE, "document.take"} : result.error();
                status = failure;
                report(failure);
            }
            for (const auto waiter : opening.waiters)
            {
                const auto found = std::ranges::find(requests_, waiter, &Request::id);
                if (found != requests_.end())
                {
                    found->status = status;
                }
            }
            openings_.erase(openings_.begin() + index);
        }
    }

    void Editor::pollDocuments(PollBudget &budget)
    {
        const auto &values = documents_.values();
        if (values.empty())
        {
            return;
        }
        poll_cursor_ %= values.size();
        for (std::size_t index{}; index < values.size(); ++index)
        {
            values[(poll_cursor_ + index) % values.size()]->poll(budget);
        }
        poll_cursor_ = (poll_cursor_ + 1) % values.size();
    }

    void Editor::collectClosed()
    {
        std::vector<DocumentHandle> retired;
        for (const auto &document : documents_.values())
        {
            if (document->closeStatus().state == ECloseState::CLOSED)
            {
                retired.push_back(document->handle());
            }
        }
        for (const auto handle : retired)
        {
            documents_.erase(handle);
        }
    }

    int Editor::exec()
    {
        if (state_ != EState::COLD || !config_.frontend || config_.project_file.empty() || !config_.limits.documents ||
            !config_.limits.open_requests || !config_.limits.turn.document_steps ||
            !config_.limits.turn.main_completions || !config_.limits.turn.render_replies ||
            !config_.limits.turn.object_messages)
        {
            return 2;
        }
        state_ = EState::RUNNING;
        auto runtime = process::ExecutionRuntime::create(config_.execution);
        if (!runtime)
        {
            state_ = EState::FINISHED;
            return 3;
        }
        const auto blocking = runtime->blocking();
        if (!blocking)
        {
            runtime->requestStop();
            static_cast<void>(runtime->join());
            state_ = EState::FINISHED;
            return 3;
        }

        auto frontend = config_.frontend();
        if (!frontend)
        {
            runtime->requestStop();
            static_cast<void>(runtime->join());
            state_ = EState::FINISHED;
            return 5;
        }
        const auto started = frontend->beginStartup(*this, *runtime, messages_.dispatcherRef());
        if (!started)
        {
            fail(started.error());
        }

        EditorResult<ProjectSource> source = failed(EEditorError::INVALID_STATE);
        std::atomic<bool> ready{}, scope_closed{};
        process::TaskScope load;
        const bool skip_source = exit_requested_;
        auto work = stdexec::then(stdexec::schedule(*blocking),
                                  [&, skip_source]() noexcept
                                  {
                                      if (skip_source)
                                      {
                                          source = failed(EEditorError::CANCELLED);
                                          ready.store(true, std::memory_order_release);
                                          return;
                                      }
                                      source = readProjectSource(config_.project_file);
                                      ready.store(true, std::memory_order_release);
                                  });
        auto errors = stdexec::upon_error(std::move(work),
                                          [&](process::EExecutionError error) noexcept
                                          {
                                              source = lux::cxx::unexpected(
                                                  EditorFailure{EEditorError::EXECUTION_FAILURE, "process.schedule",
                                                                static_cast<std::uint64_t>(error)});
                                              ready.store(true, std::memory_order_release);
                                          });
        auto stopped = stdexec::upon_stopped(std::move(errors),
                                             [&]() noexcept
                                             {
                                                 source = failed(EEditorError::CANCELLED);
                                                 ready.store(true, std::memory_order_release);
                                             });
        const auto admitted = load.start(std::move(stopped));
        if (!admitted)
        {
            source = failed(EEditorError::EXECUTION_FAILURE);
            ready.store(true, std::memory_order_release);
        }
        while (!ready.load(std::memory_order_acquire))
        {
            auto budget = config_.limits.turn;
            frontend->collectInput(*this);
            if (exit_requested_)
            {
                load.requestStop();
            }
            static_cast<void>(runtime->drainMain(budget.main_completions));
            frontend->poll(budget);
            budget.object_messages -= messages_.dispatchPending(budget.object_messages);
            if (!exit_requested_)
            {
                frontend->draw(*this, budget);
            }
            frontend->wait();
        }
        auto close_load = stdexec::connect(load.close(), ScopeClosed{&scope_closed});
        stdexec::start(close_load);
        while (!scope_closed.load(std::memory_order_acquire))
        {
            static_cast<void>(runtime->drainMain(config_.limits.turn.main_completions));
            std::this_thread::yield();
        }

        int exit_code{};
        if (!started || exit_requested_)
        {
            exit_code = started ? 0 : 5;
        }
        else if (!source)
        {
            fail(source.error());
            exit_code = 4;
        }
        else
        {
            auto project = Project::open(*source, *blocking, messages_.dispatcherRef());
            if (!project)
            {
                fail(project.error());
                exit_code = 4;
            }
            else
            {
                project_ = project->get();
                const auto entered = frontend->enterProject(*this, **project, *runtime, messages_.dispatcherRef());
                if (!entered)
                {
                    fail(entered.error());
                    exit_code = 5;
                    requestExit();
                }

                bool frontend_closing{};
                while (!frontend_closing || frontend->closeStatus().state != ECloseState::CLOSED)
                {
                    auto budget = config_.limits.turn;
                    frontend->collectInput(*this);
                    const auto main = runtime->drainMain(budget.main_completions);
                    if (main)
                    {
                        budget.main_completions -= *main;
                    }
                    frontend->poll(budget);
                    pollDocuments(budget);
                    acceptOpenings();
                    collectClosed();
                    budget.object_messages -= messages_.dispatchPending(budget.object_messages);

                    if (exit_requested_)
                    {
                        if (state_ != EState::CLOSING)
                        {
                            frontend->stopPresenting();
                        }
                        state_ = EState::CLOSING;
                        for (const auto &opening : openings_)
                        {
                            opening->waiters.clear();
                            opening->work->cancel();
                        }
                        for (const auto &document : documents_.values())
                        {
                            document->requestClose();
                        }
                        if (!frontend_closing && documents_.empty() && openings_.empty())
                        {
                            frontend->requestClose();
                            frontend_closing = true;
                        }
                    }
                    if (!frontend_closing)
                    {
                        frontend->draw(*this, budget);
                    }
                    frontend->wait();
                }
                registrations_.clear();
                (*project)->requestClose();
                while (true)
                {
                    const auto closed = (*project)->advanceClose();
                    if (closed && *closed)
                    {
                        break;
                    }
                    if (!closed && outcome_)
                    {
                        fail(closed.error());
                        exit_code = 6;
                    }
                    auto budget = config_.limits.turn;
                    static_cast<void>(runtime->drainMain(budget.main_completions));
                    frontend->poll(budget);
                    static_cast<void>(messages_.dispatchPending(budget.object_messages));
                    frontend->wait();
                }
                project_ = nullptr;
            }
        }
        if (frontend->closeStatus().state != ECloseState::CLOSED)
        {
            frontend->stopPresenting();
            frontend->requestClose();
            while (frontend->closeStatus().state != ECloseState::CLOSED)
            {
                auto budget = config_.limits.turn;
                static_cast<void>(runtime->drainMain(budget.main_completions));
                frontend->poll(budget);
                frontend->wait();
            }
        }
        frontend.reset();
        messages_.close();
        runtime->requestStop();
        while (true)
        {
            const auto drained = runtime->drainMain(config_.limits.turn.main_completions);
            if (!drained || *drained == 0)
            {
                break;
            }
        }
        const auto joined = runtime->join();
        state_ = EState::FINISHED;
        return joined ? (outcome_ ? exit_code : (exit_code ? exit_code : 5)) : 7;
    }
} // namespace lux::editor
