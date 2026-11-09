#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/Engine.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorApplication.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUiErrors.hpp>
#include <lux/engine/editor/EditorUiFactories.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/EditorProject.hpp>
#include <lux/engine/editor/detail/ProjectPreparation.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/ObjectScheduler.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <unordered_set>

namespace lux::editor
{
    struct EditorApplication::Impl final
    {
        enum class EEvent : std::uint8_t
        {
            START,
            LOAD,
            LOAD_FINISHED,
            CLOSE,
            CLOSE_FAILED,
            CLOSED
        };

        Impl(
            EditorConfig config,
            EditorAssembly assembly,
            std::unique_ptr<EditorWindow> window,
            std::unique_ptr<engine::Engine> engine
        ) noexcept
            : window_(std::move(window)), engine_(std::move(engine)), config_(std::move(config)),
              assembly_(std::move(assembly)), project_tasks_(execution::ExecutionRuntime::instance())
        {
        }

        ~Impl() noexcept
        {
            execution::ExecutionRuntime::instance().setWake(nullptr);
            window_->on_close = {};
            if (!engine_->closed())
            {
                std::terminate(); // Accepted borrowers must have ended before ordinary destruction.
            }
        }

        static FrameworkResult<std::unique_ptr<Impl>> create(EditorConfig config, EditorAssembly assembly) noexcept
        {
            auto window = EditorWindow::create({config.width, config.height, config.title});
            if (!window)
            {
                return cxx::unexpected(window.error());
            }
            auto engine = std::make_unique<engine::Engine>();
            cxx::scope_exit close_engine([&]() noexcept { static_cast<void>(engine->close()); });
            auto& execution = execution::ExecutionRuntime::instance();
            execution.setWake(&window::LuxWindow::wakeEvents);
            cxx::scope_exit disconnect_wake([]() noexcept
            {
                execution::ExecutionRuntime::instance().setWake(nullptr);
            });
            auto impl = std::make_unique<Impl>(
                std::move(config),
                std::move(assembly),
                std::move(*window),
                std::move(engine)
            );
            close_engine.release();
            auto event_loop = impl->engine_->setEventLoop(
                +[]() noexcept { window::LuxWindow::pollEvents(); },
                +[]() noexcept { window::LuxWindow::waitEvents(); },
                +[]() noexcept { window::LuxWindow::wakeEvents(); }
            );
            impl->window_->on_close = [engine = impl->engine_.get()](const window::WindowCloseEvent&) noexcept
            { engine->requestClose(); };
            auto failure_connection = object::LuxObject::connect(
                impl->engine_.get(),
                &engine::Engine::failureReported,
                [application = impl.get()](const error::Error& failure) noexcept { application->showFailure(failure); }
            );
            if (failure_connection)
            {
                impl->failure_connection_ = std::move(*failure_connection);
            }
            auto input =
                impl->engine_->registerOperation<engine::EEngineOperationPhase::POLL, &EditorWindow::sampleInput>(
                    *impl->window_
                );
            auto close_project =
                impl->engine_
                    ->registerClose<&Impl::closeProjectSession>(*impl, "lux.editor.project", {}, {"lux.engine.scenes"});
            auto clear_ui =
                impl->engine_->registerClose<&Impl::clearUi>(*impl, "lux.editor.panes", {"lux.engine.assets"});
            const bool has_loop = event_loop && failure_connection && input;
            const bool has_close = close_project && clear_ui;
            const bool installed = has_loop && has_close;
            if (!installed)
            {
                // Objects are already accepting asynchronous startup work. Preserve them for ordinary close.
                impl->engine_->reportFailure(execution::toError(execution::EExecutionError::INVALID_STATE));
                impl->engine_->requestClose();
            }
            disconnect_wake.release();
            return impl;
        }

        void transition(EEvent event) noexcept
        {
            auto next = state_;
            switch (event)
            {
            case EEvent::START:
                next = EApplicationState::SELECTING_PROJECT;
                break;
            case EEvent::LOAD:
                next = EApplicationState::LOADING_PROJECT;
                break;
            case EEvent::LOAD_FINISHED:
                if (state_ == EApplicationState::CLOSING || state_ == EApplicationState::CLOSE_FAILED)
                {
                    return;
                }
                if (engine_->closing())
                {
                    next = EApplicationState::CLOSING;
                    break;
                }
                next = project_change_ && project_change_->cleanup_failure.type
                           ? EApplicationState::PROJECT_CLEANUP_FAILED
                       : project_ ? EApplicationState::RUNNING
                                  : EApplicationState::SELECTING_PROJECT;
                break;
            case EEvent::CLOSE:
                next = EApplicationState::CLOSING;
                break;
            case EEvent::CLOSE_FAILED:
                next = EApplicationState::CLOSE_FAILED;
                break;
            case EEvent::CLOSED:
                next = EApplicationState::CLOSED;
                break;
            }
            if (next == state_)
            {
                return;
            }
            state_ = next;
            if (project_pane_)
            {
                project_pane_->setApplicationState(state_);
            }
        }

        static void openFromPane(void* receiver, OpenProjectRequest& request) noexcept
        {
            static_cast<void>(object::sendEvent(*static_cast<EditorApplication*>(receiver), request));
        }

        static void createFromPane(void* receiver, CreateProjectRequest& request) noexcept
        {
            static_cast<void>(object::sendEvent(*static_cast<EditorApplication*>(receiver), request));
        }

        static void actionFromPane(void* receiver) noexcept
        {
            auto& app = *static_cast<EditorApplication*>(receiver);
            if (app.state() == EApplicationState::LOADING_PROJECT)
            {
                CancelProjectLoadRequest request;
                static_cast<void>(object::sendEvent(app, request));
            }
            else if (app.state() == EApplicationState::PROJECT_CLEANUP_FAILED)
            {
                RetryProjectCleanupRequest request;
                static_cast<void>(object::sendEvent(app, request));
            }
            else if (app.state() == EApplicationState::CLOSE_FAILED)
            {
                RetryApplicationCloseRequest request;
                static_cast<void>(object::sendEvent(app, request));
            }
        }

        FrameworkResult<void> initialize(EditorApplication& application, ProjectStartup startup) noexcept
        {
            application_ = &application;
            auto pane = std::make_unique<ProjectCreationPane>(
                &application,
                &openFromPane,
                &createFromPane,
                std::move(startup.available_plugins),
                &actionFromPane
            );
            auto* input = pane.get();
            auto attached = window_->uiRoot().addPane(std::move(pane));
            if (!attached)
            {
                return cxx::unexpected(error::Error{
                    Errors::EditorCannotMountProjectWindows,
                    {static_cast<std::uint64_t>(attached.error())}
                });
            }
            project_pane_ = input;
            transition(EEvent::START);
            if (!config_.project_file.empty())
            {
                return requestProjectLoad(config_.project_file, std::move(startup.create));
            }
            return {};
        }

        FrameworkResult<void>
        requestProjectLoad(std::filesystem::path file, std::optional<ProjectManifest> create) noexcept
        {
            const bool may_load =
                state_ == EApplicationState::RUNNING || state_ == EApplicationState::SELECTING_PROJECT;
            if (!may_load || project_change_)
            {
                return cxx::unexpected(error::Error{Errors::ProjectLoadBusy});
            }
            if (engine_->closing())
            {
                return cxx::unexpected(error::Error{Errors::ProjectClosing});
            }
            const bool invalid_path = !file.is_absolute() || file.filename().empty();
            if (invalid_path)
            {
                return cxx::unexpected(
                    error::Error{Errors::ProjectManifest, {static_cast<std::uint64_t>(EProjectError::INVALID_PATH)}}
                );
            }
            if (create)
            {
                if (auto valid = validateProjectManifest(*create); !valid)
                {
                    return cxx::unexpected(error::Error{
                        Errors::ProjectManifest,
                        {static_cast<std::uint64_t>(valid.error().code), valid.error().ordinal}
                    });
                }
            }
            if (request_serial_ == UINT64_MAX)
            {
                std::terminate(); // Identity exhaustion, never wrap a delayed completion.
            }
            project_change_.emplace();
            project_change_->request = ++request_serial_;
            project_change_->manifest_file = std::move(file);
            project_change_->create = std::move(create);
            transition(EEvent::LOAD);
            project_pane_->setStatus("Preparing project...");
            auto accepted = project_tasks_.submit(
                {"Load project", "editor.project"},
                [this](execution::TaskReporter reporter) noexcept
                {
                    auto prepared = execution::asResult(
                        stdexec::starts_on(
                            execution::ExecutionRuntime::instance().mainScheduler(),
                            prepareProject(reporter)
                        )
                    );
                    return stdexec::let_value(
                        std::move(prepared),
                        [this](auto& result) noexcept
                        {
                            error::Error error;
                            if (!result)
                            {
                                if (const auto* failure = result.error().domainFailure())
                                {
                                    error = *failure;
                                }
                                else if (const auto* failure = result.error().executionFailure())
                                {
                                    error = execution::toError(*failure);
                                }
                                else
                                {
                                    error = {Errors::ProjectCancelled};
                                }
                            }
                            return stdexec::write_env(
                                stdexec::starts_on(
                                    execution::ExecutionRuntime::instance().mainScheduler(),
                                    finishProjectLoad(error)
                                ),
                                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                            );
                        }
                    );
                }
            );
            if (!accepted)
            {
                project_change_.reset();
                transition(EEvent::LOAD_FINISHED);
                return cxx::unexpected(execution::toError(accepted.error()));
            }
            project_task_ = *accepted;
            return {};
        }

        FrameworkResult<void> cancelProjectLoad() noexcept
        {
            if (state_ != EApplicationState::LOADING_PROJECT || !project_task_)
            {
                return cxx::unexpected(error::Error{Errors::ProjectLoadBusy});
            }
            if (project_change_->project_published)
            {
                return cxx::unexpected(error::Error{Errors::ProjectAlreadyPublished});
            }
            auto stopped = execution::ExecutionRuntime::instance().requestStop(*project_task_);
            return stopped ? FrameworkResult<void>{} : cxx::unexpected(error::Error{Errors::ProjectLoadBusy});
        }

        exec::task<FrameworkResult<void>> prepareProject(execution::TaskReporter reporter) noexcept
        {
            auto& change = *project_change_;
            auto blocking = execution::ExecutionRuntime::instance().blockingIoScheduler();
            if (!blocking)
            {
                co_return cxx::unexpected(execution::toError(blocking.error()));
            }
            const auto prepare_files = [this, &change, reporter]() noexcept
            {
                auto prepared =
                    detail::prepareProject(change.manifest_file, change.create, config_.plugin_locations, reporter);
                // Preserve the disk fact before cancellation can suppress the next Main continuation.
                change.manifest_publication = prepared.manifest_publication;
                return prepared;
            };
            auto loaded = co_await stdexec::then(stdexec::schedule(*blocking), prepare_files);
            if (!loaded.prepared_project)
            {
                co_return cxx::unexpected(loaded.prepared_project.error());
            }
            co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
            const bool cancelled_before_construction = reporter.stopToken().stop_requested() || engine_->closing();
            if (cancelled_before_construction)
            {
                // IO publication is already recorded, but cancelled input must not start candidate factories.
                co_return cxx::unexpected(error::Error{Errors::ProjectCancelled});
            }
            auto prepared = std::move(*loaded.prepared_project);
            auto layout = prepared.layout ? std::move(*prepared.layout) : config_.layout;
            auto placement = detail::ProjectUiMount::placement(layout);
            if (!placement)
            {
                co_return cxx::unexpected(error::Error{Errors::EditorInvalidLayoutItem});
            }
            if (layout.panes.size() > ui::RootConfig{}.pane_capacity)
            {
                co_return cxx::unexpected(error::Error{
                    Errors::EditorCannotMountProjectWindows,
                    {static_cast<std::uint64_t>(ui::EPaneError::CAPACITY)}
                });
            }
            EditorComposition composition;
            if (assembly_)
            {
                if (auto assembled = assembly_(composition); !assembled)
                {
                    co_return assembled;
                }
            }
            auto context = detail::createEditorContext(*engine_, std::move(prepared), std::move(composition));
            if (!context)
            {
                co_return cxx::unexpected(context.error());
            }
            change.candidate = std::make_unique<detail::EditorProject>(std::move(*context));
            auto initialized =
                co_await change.candidate->context->services().initializeProject(*change.candidate->context);
            if (!initialized)
            {
                co_return initialized;
            }
            change.placement = std::move(*placement);
            change.panes.reserve(layout.panes.size());
            for (const auto& item : layout.panes)
            {
                auto pane = createPane(*change.candidate->context, item);
                if (!pane)
                {
                    co_return cxx::unexpected(pane.error());
                }
                const bool invalid = !*pane || (*pane)->attachedRoot() || (*pane)->parent();
                if (invalid)
                {
                    co_return cxx::unexpected(error::Error{Errors::EditorUiFactoryReturnedAnAttachedOrNullPane});
                }
                change.panes.push_back(std::move(*pane));
            }
            for (;;)
            {
                const bool cancelled = reporter.stopToken().stop_requested() || engine_->closing() ||
                                       state_ != EApplicationState::LOADING_PROJECT;
                if (cancelled)
                {
                    co_return cxx::unexpected(error::Error{Errors::ProjectCancelled});
                }
                auto published = publishProject(change);
                if (published)
                {
                    co_return FrameworkResult<void>{};
                }
                if (published.error() != ui::EPaneError::BUSY)
                {
                    co_return cxx::unexpected(error::Error{
                        Errors::EditorCannotMountProjectWindows,
                        {static_cast<std::uint64_t>(published.error())}
                    });
                }
                // Root rejected before publishing. Retain the candidate and leave the current callback batch.
                co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
                auto replacement = detail::ProjectUiMount::placement(layout);
                if (!replacement)
                {
                    co_return cxx::unexpected(error::Error{Errors::EditorInvalidLayoutItem});
                }
                change.placement = std::move(*replacement);
            }
        }

        ui::PaneResult<void> publishProject(detail::ProjectChange& change) noexcept
        {
            auto& root = window_->uiRoot();
            change.candidate->ui.prepare(root, change.panes.size());
            auto commit = [&](std::span<const ui::PaneHandle> handles) noexcept
            {
                change.candidate->ui.arm(handles);
                change.previous = std::exchange(project_, std::move(change.candidate));
                if (change.previous)
                {
                    change.previous->ui.disarm();
                }
                change.project_published = true;
            };
            auto removed = root.replacePanes(
                project_ ? project_->ui.handles() : std::span<const ui::PaneHandle>{},
                change.panes,
                commit,
                std::move(change.placement)
            );
            if (!removed)
            {
                return cxx::unexpected(removed.error());
            }
            removed->clear();
            static_cast<void>(application_->emit(application_->projectChanged));
            return {};
        }

        exec::task<FrameworkResult<void>> closeProject(std::unique_ptr<detail::EditorProject>& project) noexcept
        {
            if (!project)
            {
                co_return FrameworkResult<void>{};
            }
            auto closed = co_await project->close();
            if (!closed)
            {
                co_return closed;
            }
            // Leave producer/plugin completion stacks before ending their enclosing module/Context borrow.
            co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
            project.reset();
            co_return FrameworkResult<void>{};
        }

        exec::task<execution::ETaskState> finishProjectLoad(error::Error error) noexcept
        {
            auto& change = *project_change_;
            change.preparation_failure = error;
            change.panes.clear();
            auto& discarded = change.project_published ? change.previous : change.candidate;
            auto closed = co_await closeProject(discarded);
            if (!closed)
            {
                change.cleanup_failure = closed.error();
            }
            co_await stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler());
            ProjectLoadOutcome outcome{
                change.request,
                change.manifest_file,
                change.cleanup_failure.type ? change.cleanup_failure : change.preparation_failure,
                change.manifest_publication,
                change.project_published
            };
            project_task_.reset();
            transition(EEvent::LOAD_FINISHED);
            if (project_pane_)
            {
                project_pane_->setStatus(
                    outcome.error.type ? error::format(outcome.error) : "Project: " + project_->context->project().name
                );
            }
            if (!change.cleanup_failure.type)
            {
                project_change_.reset();
            }
            static_cast<void>(application_->emit(application_->projectLoadCompleted, outcome));
            co_return outcome.error.type == Errors::ProjectCancelled ? execution::ETaskState::CANCELLED
            : outcome.error.type                                     ? execution::ETaskState::FAILED
                                                                     : execution::ETaskState::SUCCEEDED;
        }

        exec::task<FrameworkResult<void>> recoverProjectChange() noexcept
        {
            auto& change = *project_change_;
            auto& project = change.project_published ? change.previous : change.candidate;
            auto recovered = co_await project->recoverClose();
            if (!recovered)
            {
                co_return recovered;
            }
            auto closed = co_await closeProject(project);
            if (!closed)
            {
                co_return closed;
            }
            change.cleanup_failure = {};
            project_change_.reset();
            transition(EEvent::LOAD_FINISHED);
            co_return FrameworkResult<void>{};
        }

        FrameworkResult<void> retryProjectCleanup() noexcept
        {
            if (state_ != EApplicationState::PROJECT_CLEANUP_FAILED || project_task_)
            {
                return cxx::unexpected(error::Error{Errors::ProjectLoadBusy});
            }
            auto accepted = project_tasks_.submit(
                {"Retry project cleanup", "editor.project"},
                [this](execution::TaskReporter) noexcept
                {
                    auto recovered = execution::asResult(
                        stdexec::starts_on(
                            execution::ExecutionRuntime::instance().mainScheduler(),
                            recoverProjectChange()
                        )
                    );
                    return stdexec::then(
                        std::move(recovered),
                        [this](auto result) noexcept
                        {
                            project_task_.reset();
                            if (!result)
                            {
                                const auto* failure = result.error().domainFailure();
                                if (failure && project_pane_)
                                {
                                    project_pane_->setStatus(error::format(*failure));
                                }
                            }
                            return result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                        }
                    );
                }
            );
            if (!accepted)
            {
                return cxx::unexpected(execution::toError(accepted.error()));
            }
            project_task_ = *accepted;
            return {};
        }

        exec::task<FrameworkResult<void>> closeProjectSession() noexcept
        {
            const bool retrying = state_ == EApplicationState::CLOSE_FAILED;
            transition(EEvent::CLOSE);
            project_tasks_.requestStop();
            co_await stdexec::continues_on(
                project_tasks_.whenSettled(),
                execution::ExecutionRuntime::instance().mainScheduler()
            );
            if (project_change_)
            {
                project_change_->panes.clear();
                auto& discarded =
                    project_change_->project_published ? project_change_->previous : project_change_->candidate;
                if (discarded && project_change_->cleanup_failure.type)
                {
                    auto recovered = co_await discarded->recoverClose();
                    if (!recovered)
                    {
                        co_return recovered;
                    }
                }
                auto discarded_result = co_await closeProject(discarded);
                if (!discarded_result)
                {
                    co_return discarded_result;
                }
                project_change_.reset();
            }
            if (retrying && project_)
            {
                auto recovered = co_await project_->recoverClose();
                if (!recovered)
                {
                    co_return recovered;
                }
            }
            auto closed = co_await closeProject(project_);
            co_return closed;
        }

        FrameworkResult<void> clearUi() noexcept
        {
            // Panes can borrow product data, but all Scene and rendering work is already settled.
            if (auto cleared = window_->uiRoot().clearPanes(); !cleared)
            {
                return cxx::unexpected(
                    error::Error{Errors::EditorCannotMountProjectWindows, {static_cast<std::uint64_t>(cleared.error())}}
                );
            }
            project_pane_ = nullptr;
            return {};
        }

        void showFailure(error::Error error) noexcept
        {
            if (project_pane_)
            {
                project_pane_->setStatus(error::format(error));
            }
            if (engine_->closeFailed())
            {
                transition(EEvent::CLOSE_FAILED);
            }
        }

        // Reverse destruction follows the actual facilities borrowed by project/Scene work.
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::Engine> engine_;
        EditorConfig config_;
        EditorAssembly assembly_;
        execution::TaskScope project_tasks_;
        std::unique_ptr<detail::EditorProject> project_;
        std::optional<detail::ProjectChange> project_change_;
        std::optional<execution::TaskId> project_task_;
        std::uint64_t request_serial_{};
        EApplicationState state_ = EApplicationState::STARTING;
        EditorApplication* application_ = nullptr;
        ProjectCreationPane* project_pane_ = nullptr; // Root owns it until the pane teardown operation completes.
        object::Connection failure_connection_;
    };

    EditorApplication::EditorApplication(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
        impl_->window_->uiRoot().setCommandFallback(this);
    }

    EditorApplication::~EditorApplication() noexcept
    {
        if (impl_->engine_->isDriving() || isDispatching())
        {
            std::terminate();
        }
        if (!impl_->engine_->closed())
        {
            std::terminate(); // run/Engine::close must settle accepted borrows before destruction.
        }
        impl_->transition(Impl::EEvent::CLOSED);
        impl_->window_->uiRoot().setCommandFallback(nullptr);
        impl_->project_pane_ = nullptr;
        beginDestruction();
    }

    FrameworkResult<std::unique_ptr<EditorApplication>>
    EditorApplication::create(EditorConfig config, EditorAssembly assembly, ProjectStartup startup) noexcept
    {
        if (auto registered = registerProjectErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerContextErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerEditorUiErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerAppErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        const bool invalid_window = config.width <= 0 || config.height <= 0;
        if (invalid_window)
        {
            return cxx::unexpected(error::Error{Errors::EditorInvalidWindowExtent});
        }
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorEditorRequiresObjectThread});
        }
        if (auto valid = validateProjectLayout(config.layout); !valid)
        {
            return cxx::unexpected(error::Error{Errors::EditorInvalidLayoutItem});
        }
        auto impl = Impl::create(std::move(config), std::move(assembly));
        if (!impl)
        {
            return cxx::unexpected(impl.error());
        }
        auto application = std::unique_ptr<EditorApplication>{new EditorApplication(std::move(*impl))};
        auto initialized = application->impl_->initialize(*application, std::move(startup));
        if (!initialized)
        {
            // The unpublished shell has no project work. Drive the same shutdown transaction explicitly.
            application->impl_->engine_->requestClose();
            static_cast<void>(application->run());
            return cxx::unexpected(initialized.error());
        }
        return application;
    }

    EApplicationState EditorApplication::state() const noexcept
    {
        return impl_->state_;
    }

    void EditorApplication::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<OpenProjectRequest>())
        {
            event.accept();
            auto accepted = impl_->requestProjectLoad(request->manifest_file, {});
            request->rejection = accepted ? error::Error{} : accepted.error();
        }
        else if (auto* request = event.getIf<CreateProjectRequest>())
        {
            event.accept();
            auto accepted = impl_->requestProjectLoad(request->root / "Project.luxproj", std::move(request->manifest));
            request->rejection = accepted ? error::Error{} : accepted.error();
        }
        else if (event.getIf<CloseProjectRequest>())
        {
            event.accept();
            impl_->engine_->requestClose();
        }
        else if (auto* request = event.getIf<CancelProjectLoadRequest>())
        {
            event.accept();
            auto cancelled = impl_->cancelProjectLoad();
            request->rejection = cancelled ? error::Error{} : cancelled.error();
        }
        else if (auto* request = event.getIf<RetryProjectCleanupRequest>())
        {
            event.accept();
            auto retried = impl_->retryProjectCleanup();
            request->rejection = retried ? error::Error{} : retried.error();
        }
        else if (auto* request = event.getIf<RetryApplicationCloseRequest>())
        {
            event.accept();
            if (state() != EApplicationState::CLOSE_FAILED)
            {
                request->rejection = {Errors::ProjectClosing};
            }
            else
            {
                impl_->engine_->requestClose();
            }
        }
        else if (auto* command = event.getIf<ui::Command>())
        {
            const bool close = command->id == ui::CommandIdView{"lux.project.close"};
            const bool cancel = command->id == ui::CommandIdView{"lux.project.cancel_open"};
            if (close || cancel)
            {
                event.accept();
                command->enabled =
                    close ? state() != EApplicationState::CLOSED
                          : state() == EApplicationState::LOADING_PROJECT && !impl_->project_change_->project_published;
                command->result =
                    command->enabled ? ui::ECommandDispatchResult::NOT_FOUND : ui::ECommandDispatchResult::DISABLED;
                if (command->enabled && command->phase == ui::ECommandPhase::EXECUTE)
                {
                    if (close)
                    {
                        impl_->engine_->requestClose();
                    }
                    else
                    {
                        static_cast<void>(impl_->cancelProjectLoad());
                    }
                    command->result = ui::ECommandDispatchResult::EXECUTED;
                }
                return;
            }
            if (!ready())
            {
                event.accept();
                command->enabled = false;
                command->result = ui::ECommandDispatchResult::DISABLED;
                return;
            }
            object::EventView forwarded{*command};
            if (impl_->project_->context->services().dispatch(forwarded))
            {
                event.accept();
            }
        }
    }

    FrameworkResult<void> EditorApplication::run() noexcept
    {
        if (impl_->engine_->isDriving() || isDispatching())
        {
            return cxx::unexpected(error::Error{Errors::EditorRecursiveHostFrame});
        }
        beginCallbackBorrow(*this);
        cxx::scope_exit release([&]() noexcept { endCallbackBorrow(*this); });
        auto result = impl_->engine_->run();
        if (impl_->engine_->closed())
        {
            impl_->transition(Impl::EEvent::CLOSED);
        }
        return result;
    }

    EditorWindow& EditorApplication::window() noexcept
    {
        return *impl_->window_;
    }

    engine::Engine& EditorApplication::engine() noexcept
    {
        return *impl_->engine_;
    }

    const engine::Engine& EditorApplication::engine() const noexcept
    {
        return *impl_->engine_;
    }

    EditorContext& EditorApplication::project() noexcept
    {
        return *impl_->project_->context;
    }

    const EditorContext& EditorApplication::project() const noexcept
    {
        return *impl_->project_->context;
    }

} // namespace lux::editor
