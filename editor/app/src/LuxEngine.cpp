#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUiFactories.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/editor/detail/ProjectPreparation.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        error::Error executionError(process::EExecutionError value) noexcept
        {
            constexpr error::ErrorId ids[]{
                Errors::ProcessExecution0,
                Errors::ProcessExecution1,
                Errors::ProcessExecution2,
                Errors::ProcessExecution3,
                Errors::ProcessExecution4,
                Errors::ProcessExecution5,
                Errors::ProcessExecution6,
                Errors::ProcessExecution7,
                Errors::ProcessExecution8,
                Errors::ProcessExecution9,
                Errors::ProcessExecution10
            };
            static_assert(
                std::size(ids) == static_cast<std::size_t>(process::EExecutionError::CAPABILITY_UNAVAILABLE) + 1
            );
            const auto code = static_cast<std::size_t>(value);
            constexpr auto unknown = Errors::ProcessExecutionUnknown;
            return {code < std::size(ids) ? ids[code] : unknown, {code}};
        }
        error::Error creationError(const engine::EngineContext::VCreateFailure& failure) noexcept
        {
            return std::visit(
                [](const auto& value) noexcept -> error::Error
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, process::EExecutionError>)
                    {
                        return executionError(value);
                    }
                    else
                    {
                        return scene::toError(value);
                    }
                },
                failure
            );
        }
        error::Error renderingError(const engine::RenderContext::VFailure& failure) noexcept
        {
            return std::visit(
                [](const auto& value) noexcept -> error::Error
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, process::EExecutionError>)
                    {
                        return executionError(value);
                    }
                    else
                    {
                        return render::toError(value);
                    }
                },
                failure
            );
        }
        struct Operation final
        {
            bool& active;
            explicit Operation(bool& flag) noexcept : active(flag)
            {
                active = true;
            }
            ~Operation()
            {
                active = false;
            }
        };
    } // namespace
    struct LuxEngine::Impl final
    {
        explicit Impl(EditorConfig config, EditorAssembly assembly)
            : config_(std::move(config)), assembly_(std::move(assembly))
        {
        }
        ~Impl() noexcept
        {
            if (transition_)
            {
                transition_->task.requestStop();
                if (!transition_->task.join())
                {
                    std::terminate();
                }
                if (transition_->candidate)
                {
                    transition_->candidate->beginClose();
                    if (!transition_->candidate->tasks().join())
                    {
                        std::terminate();
                    }
                }
            }
            if (project_)
            {
                project_->beginClose();
                if (!project_->tasks().join())
                {
                    std::terminate();
                }
            }
            // Retiring panes still borrow the old Context; accepted completions settle before their destruction.
            transition_.reset();
            // Only mechanical project teardown here. The original Runtime drains retirement during its destruction.
            // Engine (and native output users) dies before EditorWindow by member declaration order.
            if (window_ && !window_->uiRoot().clearPanes())
            {
                std::terminate();
            }
        }
        FrameworkResult<void> initialize() noexcept
        {
            auto window = EditorWindow::create({config_.width, config_.height, config_.title});
            if (!window)
            {
                return cxx::unexpected(std::move(window.error()));
            }
            window_ = std::move(*window);
            auto engine = engine::EngineContext::create(
                {2, 512, 512, {256}, process::BlockingSchedulerConfig{2, 128}},
                {0, 2048}
            );
            if (!engine)
            {
                return cxx::unexpected(creationError(engine.error()));
            }
            engine_ = std::move(*engine);
            auto rendering = engine::initializeRendering(
                *engine_,
                window::LuxWindow::requiredVulkanInstanceExtensions(),
                render::RendererConfig{.enable_vsync = config_.enable_vsync}
            );
            if (!rendering)
            {
                return cxx::unexpected(renderingError(rendering.error()));
            }
            auto& context = *engine_->renderContext();
            auto features = context.registerFeatures({render::kUiRenderRenderFeatureRegistration});
            if (!features)
            {
                return cxx::unexpected(renderingError(features.error()));
            }
            auto configuration = ui::makeRenderConfiguration(window_->uiRoot());
            if (!configuration)
            {
                return cxx::unexpected(
                    error::Error{Errors::UiRenderConfiguration, {static_cast<std::uint64_t>(configuration.error())}}
                );
            }
            std::uint32_t width{}, height{};
            window_->framebufferSize(width, height);
#if defined(_WIN32)
            const auto native = reinterpret_cast<std::uintptr_t>(window_->nativeHandle());
            if (!native)
            {
                return cxx::unexpected(error::Error{Errors::EditorWindowHasNoNativeOutput, {}});
            }
            scene::ViewConfig output{.extent = {width, height}, .output = scene::NativeSurfaceOutput{native}};
#else
            return cxx::unexpected(error::Error{Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatform, {}});
            scene::ViewConfig output;
#endif
            auto scene = EditorUiScene::create(*engine_, std::move(*configuration), output);
            if (!scene)
            {
                return cxx::unexpected(std::move(scene.error()));
            }
            ui_scene_ = std::move(*scene);
            object::ObjectRuntime::instance().setWake(&window::LuxWindow::wakeEvents);
            engine_->execution().setWake(&window::LuxWindow::wakeEvents);
            return {};
        }
        struct ProjectTransition final
        {
            explicit ProjectTransition(process::ExecutionRuntime& execution) : task(execution) {}
            // Completion never borrows a Context; this record remains stable until task.settled().
            std::optional<process::TTaskResult<detail::ProjectPreparation, error::Error>> completed;
            process::TaskScope task;
            std::unique_ptr<EditorContext> candidate;
            std::vector<std::unique_ptr<ui::Pane>> panes;
            std::vector<std::unique_ptr<ui::Pane>> retiring_panes;
            bool cancel_requested{};
            bool close_only{};
            bool candidate_prepared{};
            bool current_detached{};
            bool cleaning_failed_candidate{};
            error::Error failure;
        };
        FrameworkResult<void> admitProject(std::filesystem::path file, std::optional<ProjectManifest> create) noexcept
        {
            if (!object::ObjectRuntime::instance().isCurrent())
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
            }
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectChangeInsideAHostOperation});
            }
            if (transition_ || window_->shouldClose())
            {
                return cxx::unexpected(error::Error{Errors::ProjectTransitionBusy});
            }
            if (!file.is_absolute() || file.filename().empty())
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
            auto scheduler = engine_->execution().blocking();
            if (!scheduler)
            {
                return cxx::unexpected(executionError(scheduler.error()));
            }
            Operation guard{operating_};
            auto next = std::make_unique<ProjectTransition>(engine_->execution());
            auto* record = next.get();
            auto submitted = record->task.submit(
                {"Prepare project", "editor.project"},
                [scheduler = *scheduler, file, create = std::move(create), locations = config_.plugin_locations](
                    process::TaskReporter reporter
                ) mutable noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [file = std::move(file),
                         create = std::move(create),
                         locations = std::move(locations),
                         reporter]() noexcept -> FrameworkResult<detail::ProjectPreparation>
                        { return detail::prepareProject(file, create, locations, reporter); }
                    );
                },
                [record](process::TTaskResult<detail::ProjectPreparation, error::Error>&& result) noexcept
                { record->completed.emplace(std::move(result)); }
            );
            if (!submitted)
            {
                return cxx::unexpected(executionError(submitted.error()));
            }
            status_ = {EProjectTransition::PREPARING, {}, std::move(file)};
            transition_ = std::move(next);
            return {};
        }
        FrameworkResult<void> closeProject() noexcept
        {
            if (!object::ObjectRuntime::instance().isCurrent())
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
            }
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectChangeInsideAHostOperation});
            }
            if (transition_)
            {
                return cxx::unexpected(error::Error{Errors::ProjectTransitionBusy});
            }
            transition_ = std::make_unique<ProjectTransition>(engine_->execution());
            transition_->close_only = true;
            status_ = {EProjectTransition::CLOSING_CURRENT};
            return {};
        }
        FrameworkResult<void> cancelProjectTransition() noexcept
        {
            if (!object::ObjectRuntime::instance().isCurrent())
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
            }
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectChangeInsideAHostOperation});
            }
            if (!transition_ || status_.state != EProjectTransition::PREPARING)
            {
                return cxx::unexpected(error::Error{Errors::ProjectTransitionBusy});
            }
            transition_->cancel_requested = true;
            transition_->task.requestStop();
            return {};
        }
        void failTransition(error::Error error) noexcept
        {
            transition_->failure = error;
            transition_->cleaning_failed_candidate = true;
            if (transition_->candidate)
            {
                transition_->candidate->beginClose();
            }
            transition_->panes.clear();
        }
        void finishTransition(EProjectTransition state, error::Error error = {}) noexcept
        {
            status_.state = state;
            status_.failure = error;
            transition_.reset();
        }
        FrameworkResult<void> advanceProject() noexcept
        {
            if (!transition_)
            {
                return {};
            }
            auto& next = *transition_;
            if (!next.task.settled())
            {
                return {};
            }
            if (next.completed)
            {
                auto completed = std::move(*next.completed);
                next.completed.reset();
                if (!completed)
                {
                    const auto& error = completed.error();
                    const auto failure = error.domainFailure()      ? *error.domainFailure()
                                         : error.executionFailure() ? executionError(*error.executionFailure())
                                                                    : error::Error{Errors::ProjectCancelled};
                    failTransition(failure);
                }
                else
                {
                    status_.manifest_published = completed->published;
                    if (next.cancel_requested)
                    {
                        failTransition({Errors::ProjectCancelled});
                    }
                    else if (!completed->result)
                    {
                        failTransition(completed->result.error());
                    }
                    else
                    {
                        auto assemble = [&](EditorContext& context) noexcept -> FrameworkResult<void>
                        { return assembly_ ? assembly_(context) : FrameworkResult<void>{}; };
                        auto context = EditorContext::create(*engine_, std::move(*completed->result), assemble);
                        if (!context)
                        {
                            failTransition(context.error());
                        }
                        else
                        {
                            next.candidate = std::move(*context);
                            next.panes.reserve(config_.layout.size());
                            for (const auto& item : config_.layout)
                            {
                                auto pane = createPane(*next.candidate, item);
                                if (!pane)
                                {
                                    failTransition(pane.error());
                                    break;
                                }
                                if (!*pane || (*pane)->attachedRoot() || (*pane)->parent())
                                {
                                    failTransition({Errors::EditorUiFactoryReturnedAnAttachedOrNullPane});
                                    break;
                                }
                                next.panes.push_back(std::move(*pane));
                            }
                            next.candidate_prepared = !next.cleaning_failed_candidate;
                        }
                    }
                }
            }
            if (next.cleaning_failed_candidate)
            {
                if (next.candidate && !next.candidate->closed())
                {
                    return {};
                }
                const auto state = next.failure.type == Errors::ProjectCancelled || next.cancel_requested
                                       ? EProjectTransition::CANCELLED
                                       : EProjectTransition::FAILED;
                finishTransition(state, next.failure);
                return {};
            }
            if (!next.close_only && !next.candidate_prepared)
            {
                return {};
            }
            // From here closure is committed intent; cancellation is only allowed during preparation.
            // Root owns both application and project UI. Only this mount's handles belong to the project.
            if (!next.current_detached)
            {
                next.retiring_panes.reserve(project_panes_.size());
                auto& root = window_->uiRoot();
                while (!project_panes_.empty())
                {
                    if (auto* pane = root.resolvePane(project_panes_.back()))
                    {
                        auto removed = root.removePane(*pane);
                        if (!removed)
                        {
                            if (removed.error() == ui::EPaneError::BUSY)
                            {
                                return {};
                            }
                            return cxx::unexpected(
                                error::Error{Errors::EditorUiClear, {static_cast<std::uint64_t>(removed.error())}}
                            );
                        }
                        next.retiring_panes.push_back(std::move(*removed));
                    }
                    // A prior external removal already invalidated this registration. Never follow a reused slot.
                    project_panes_.pop_back();
                }
                next.current_detached = true;
                if (project_)
                {
                    project_->beginClose();
                }
            }
            status_.state = EProjectTransition::CLOSING_CURRENT;
            if (project_ && !project_->closed())
            {
                return {};
            }
            // Detached UI cannot update or receive input, but its destructors still borrow the settled Context.
            next.retiring_panes.clear();
            project_.reset();
            if (!next.close_only)
            {
                std::vector<ui::Pane*> candidates;
                candidates.reserve(next.panes.size());
                project_panes_.reserve(next.panes.size());
                for (const auto& pane : next.panes)
                {
                    candidates.push_back(pane.get());
                }
                auto mounted = window_->uiRoot().addPanes(next.panes);
                if (!mounted)
                {
                    failTransition(
                        {Errors::EditorCannotMountProjectWindows, {static_cast<std::uint64_t>(mounted.error())}}
                    );
                    return {};
                }
                // addPanes freezes structure through notification; no callback intervenes before these borrows resolve.
                for (const auto* pane : candidates)
                {
                    project_panes_.push_back(window_->uiRoot().paneHandle(*pane));
                }
                project_ = std::move(next.candidate);
            }
            finishTransition(EProjectTransition::SUCCEEDED);
            return {};
        }
        FrameworkResult<EFrameStatus> frame() noexcept
        {
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorRecursiveHostFrame, {}});
            }
            Operation guard{operating_};
            const auto frame_started = std::chrono::steady_clock::now();
            ++statistics_.iterations;
            window::LuxWindow::pollEvents();
            auto phase_started = std::chrono::steady_clock::now();
            auto collected = engine_->execution().collectCompletions();
            if (!collected)
            {
                return cxx::unexpected(executionError(collected.error()));
            }
            statistics_.execution_collect += std::chrono::steady_clock::now() - phase_started;
            phase_started = std::chrono::steady_clock::now();
            auto dispatched = engine_->execution().dispatchTaskEvents();
            if (!dispatched)
            {
                return cxx::unexpected(executionError(dispatched.error()));
            }
            statistics_.task_dispatch += std::chrono::steady_clock::now() - phase_started;
            phase_started = std::chrono::steady_clock::now();
            statistics_.object_messages += object::ObjectRuntime::instance().dispatchPending();
            statistics_.object_dispatch += std::chrono::steady_clock::now() - phase_started;
            const bool closing = window_->shouldClose();
            if (closing)
            {
                if (transition_ && status_.state == EProjectTransition::PREPARING)
                {
                    transition_->cancel_requested = true;
                    transition_->task.requestStop();
                }
                if (!transition_ && project_)
                {
                    transition_ = std::make_unique<ProjectTransition>(engine_->execution());
                    transition_->close_only = true;
                }
            }
            if (auto advanced = advanceProject(); !advanced)
            {
                return cxx::unexpected(advanced.error());
            }
            auto& root = window_->uiRoot();
            if (closing)
            {
                ui_scene_->stopFrames();
            }
            if (!closing)
            {
                auto input = window_->sampleInput();
                if (!input)
                {
                    return cxx::unexpected(std::move(input.error()));
                }
            }
            std::uint32_t width{}, height{}, pixels_x{}, pixels_y{};
            window_->size(width, height);
            window_->framebufferSize(pixels_x, pixels_y);
            ui_scene_->setExtent({pixels_x, pixels_y});
            const auto now = std::chrono::steady_clock::now();
            const bool has_extent = width && height && pixels_x && pixels_y;
            auto output = ui_scene_->outputReady();
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            const bool can_draw = !closing && has_extent && !window_->minimized() && *output;
            auto* ui_draw_data = can_draw ? ui_scene_->acquireDrawData() : nullptr;
            statistics_.backpressured_iterations += can_draw && !ui_draw_data;
            const float elapsed =
                std::max(std::chrono::duration<float>(now - last_frame_).count(), std::numeric_limits<float>::min());
            ui::FrameInfo info{
                {float(width), float(height)},
                elapsed,
                {width ? float(pixels_x) / width : 1.F, height ? float(pixels_y) / height : 1.F}
            };
            auto capture = [&](const ui::DrawData& data) noexcept { return ui_scene_->captureDrawData(data); };
            auto drawn = ui_draw_data ? root.update(info, *ui_draw_data, ui::Root::Capture{capture}) : root.update();
            if (!drawn)
            {
                return cxx::unexpected(error::Error{Errors::UiCapture, {static_cast<std::uint64_t>(drawn.error())}});
            }
            statistics_.ui = root.statistics();
            statistics_.ui_maintenance += statistics_.ui.maintenance;
            statistics_.ui_draw += statistics_.ui.draw;
            statistics_.ui_capture += statistics_.ui.capture;
            statistics_.captured_frames += statistics_.ui.captured;
            if (ui_draw_data)
            {
                last_frame_ = now;
            }
            const auto consumed = root.inputSnapshot();
            window_->input().evaluate(
                elapsed,
                ui_draw_data && !consumed.keyboard_captured,
                ui_draw_data && !consumed.pointer_captured
            );
            phase_started = std::chrono::steady_clock::now();
            auto published = ui_scene_->publishFrame();
            if (!published)
            {
                return cxx::unexpected(std::move(published.error()));
            }
            statistics_.ui_publish += std::chrono::steady_clock::now() - phase_started;
            phase_started = std::chrono::steady_clock::now();
            auto driven = engine_->sceneRuntime().driveFrame();
            statistics_.scene_drive += std::chrono::steady_clock::now() - phase_started;
            statistics_.scenes = engine_->sceneRuntime().instanceCount();
            if (!driven)
            {
                return cxx::unexpected(scene::toError(driven.error()));
            }
            if (!driven->empty())
            {
                return cxx::unexpected(scene::toError(driven->front()));
            }
            (void)object::ObjectRuntime::instance().collectRetired();
            statistics_.total += std::chrono::steady_clock::now() - frame_started;
            return closing && !transition_ && !project_ ? EFrameStatus::EXIT_REQUESTED : EFrameStatus::RUNNING;
        }
        FrameworkResult<void> exec() noexcept
        {
            for (;;)
            {
                auto running = frame();
                if (!running)
                {
                    return cxx::unexpected(std::move(running.error()));
                }
                if (*running == EFrameStatus::EXIT_REQUESTED)
                {
                    return {};
                }
                // A reusable slot is immediate work. Every other progress source already wakes
                // GLFW: native events, Process completions, Object messages and Scene timers.
                // GLFW retains a posted event across the predicate -> wait race.
                std::uint32_t width{}, height{}, pixels_x{}, pixels_y{};
                window_->size(width, height);
                window_->framebufferSize(pixels_x, pixels_y);
                const bool visible_output = width && height && pixels_x && pixels_y && !window_->minimized();
                auto output = ui_scene_->outputReady();
                if (!output)
                {
                    return cxx::unexpected(output.error());
                }
                const bool backpressured = visible_output && *output && !ui_scene_->hasWritableFrame();
                const bool can_produce = !window_->shouldClose() && visible_output && *output && !backpressured;
                const bool candidate_settled =
                    transition_ && (!transition_->cleaning_failed_candidate || !transition_->candidate ||
                                    transition_->candidate->closed());
                const bool current_settled = !project_ || !project_->closing() || project_->closed();
                const bool transition_ready =
                    transition_ && transition_->task.settled() && candidate_settled && current_settled;
                const bool begin_close = window_->shouldClose() && !transition_;
                const bool ready = begin_close || transition_ready || can_produce ||
                                   engine_->execution().hasPendingWork() ||
                                   object::ObjectRuntime::instance().statistics().pending != 0;
                if (!ready)
                {
                    const auto started = std::chrono::steady_clock::now();
                    ++statistics_.waits;
                    statistics_.backpressure_waits += backpressured;
                    window::LuxWindow::waitEvents();
                    statistics_.wait += std::chrono::steady_clock::now() - started;
                }
            }
        }
        FrameStatistics statistics_;
        EditorConfig config_;
        EditorAssembly assembly_;
        ProjectTransitionStatus status_;
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::unique_ptr<EditorUiScene> ui_scene_;
        std::unique_ptr<EditorContext> project_;
        std::vector<ui::PaneHandle> project_panes_;
        std::unique_ptr<ProjectTransition> transition_;
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()};
        bool operating_{};
    };
    LuxEngine::LuxEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    LuxEngine::~LuxEngine() noexcept = default;
    FrameworkResult<std::unique_ptr<LuxEngine>> LuxEngine::create(EditorConfig config, EditorAssembly assembly) noexcept
    {
        if (auto registered = registerFrameworkErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        const bool is_invalid_window = config.width <= 0 || config.height <= 0;
        if (is_invalid_window)
        {
            return cxx::unexpected(error::Error{Errors::EditorInvalidWindowExtent, {}});
        }
        auto& objects = object::ObjectRuntime::instance();
        if (!objects.isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorEditorRequiresObjectThread, {}});
        }
        if (config.layout.size() > ui::RootConfig{}.pane_capacity)
        {
            return cxx::unexpected(error::Error{
                Errors::EditorCannotMountProjectWindows,
                {static_cast<std::uint64_t>(ui::EPaneError::CAPACITY)}
            });
        }
        std::unordered_set<std::string> names;
        for (const auto& item : config.layout)
        {
            const bool invalid = item.type.empty() || item.name.empty() || item.type.find('\0') != item.type.npos ||
                                 item.name.find('\0') != item.name.npos;
            if (invalid)
            {
                return cxx::unexpected(error::Error{Errors::EditorInvalidLayoutItem});
            }
            if (!names.insert(item.name).second)
            {
                return cxx::unexpected(error::Error{Errors::EditorDuplicateUiInstanceName});
            }
        }
        auto impl = std::make_unique<Impl>(std::move(config), std::move(assembly));
        auto initialized = impl->initialize();
        if (!initialized)
        {
            return cxx::unexpected(std::move(initialized.error()));
        }
        return std::unique_ptr<LuxEngine>{new LuxEngine(std::move(impl))};
    }
    FrameworkResult<void> LuxEngine::openProject(std::filesystem::path file) noexcept
    {
        return impl_->admitProject(std::move(file), {});
    }
    FrameworkResult<void> LuxEngine::createProject(ProjectCreateRequest request) noexcept
    {
        return impl_->admitProject(request.root / "Project.luxproj", std::move(request.manifest));
    }
    FrameworkResult<void> LuxEngine::cancelProjectTransition() noexcept
    {
        return impl_->cancelProjectTransition();
    }
    ProjectTransitionStatus LuxEngine::projectStatus() const noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        return impl_->status_;
    }
    FrameworkResult<void> LuxEngine::closeProject() noexcept
    {
        return impl_->closeProject();
    }
    FrameworkResult<void> LuxEngine::exec() noexcept
    {
        return impl_->exec();
    }
    FrameworkResult<EFrameStatus> LuxEngine::frame() noexcept
    {
        return impl_->frame();
    }
    FrameStatistics LuxEngine::statistics() const noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        return impl_->statistics_;
    }
    EditorWindow& LuxEngine::window() noexcept
    {
        return *impl_->window_;
    }
    engine::EngineContext& LuxEngine::engine() noexcept
    {
        return *impl_->engine_;
    }
    const engine::EngineContext& LuxEngine::engine() const noexcept
    {
        return *impl_->engine_;
    }
    EditorContext* LuxEngine::context() noexcept
    {
        return impl_->project_.get();
    }
} // namespace lux::editor
