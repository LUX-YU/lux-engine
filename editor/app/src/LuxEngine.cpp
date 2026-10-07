#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUiFactories.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/editor/detail/ProjectPrepared.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/process/ObjectScheduler.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/ui/Command.hpp>
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
        Impl(
            EditorConfig config,
            EditorAssembly assembly,
            std::unique_ptr<EditorWindow> window,
            std::unique_ptr<engine::EngineContext> engine,
            std::unique_ptr<EditorUiScene> scene
        ) noexcept
            : window_(std::move(window)), engine_(std::move(engine)), ui_scene_(std::move(scene)),
              config_(std::move(config)), assembly_(std::move(assembly)), project_tasks_(engine_->execution())
        {
        }
        ~Impl() noexcept
        {
            // Global UI also belongs to this host. Every Pane dies while its Context/Engine is valid.
            if (!window_->uiRoot().clearPanes())
            {
                std::terminate();
            }
        }
        static FrameworkResult<std::unique_ptr<Impl>> create(EditorConfig config, EditorAssembly assembly) noexcept
        {
            auto window = EditorWindow::create({config.width, config.height, config.title});
            if (!window)
            {
                return cxx::unexpected(std::move(window.error()));
            }

            auto engine = engine::EngineContext::create(
                {2, 512, 512, {256}, process::BlockingSchedulerConfig{2, 128}},
                {0, 2048}
            );
            if (!engine)
            {
                return cxx::unexpected(creationError(engine.error()));
            }

            auto rendering = engine::initializeRendering(
                **engine,
                window::LuxWindow::requiredVulkanInstanceExtensions(),
                render::RendererConfig{.enable_vsync = config.enable_vsync}
            );
            if (!rendering)
            {
                return cxx::unexpected(renderingError(rendering.error()));
            }
            auto& context = *(*engine)->renderContext();
            auto features = context.registerFeatures({render::kUiRenderRenderFeatureRegistration});
            if (!features)
            {
                return cxx::unexpected(renderingError(features.error()));
            }
            auto configuration = ui::makeRenderConfiguration((*window)->uiRoot());
            if (!configuration)
            {
                return cxx::unexpected(
                    error::Error{Errors::UiRenderConfiguration, {static_cast<std::uint64_t>(configuration.error())}}
                );
            }
            std::uint32_t width{}, height{};
            (*window)->framebufferSize(width, height);
#if defined(_WIN32)
            const auto native = reinterpret_cast<std::uintptr_t>((*window)->nativeHandle());
            if (!native)
            {
                return cxx::unexpected(error::Error{Errors::EditorWindowHasNoNativeOutput, {}});
            }
            scene::ViewConfig output{.extent = {width, height}, .output = scene::NativeSurfaceOutput{native}};
#else
            return cxx::unexpected(error::Error{Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatform, {}});
            scene::ViewConfig output;
#endif
            auto scene = EditorUiScene::create(**engine, std::move(*configuration), output);
            if (!scene)
            {
                return cxx::unexpected(std::move(scene.error()));
            }

            object::ObjectRuntime::instance().setWake(&window::LuxWindow::wakeEvents);
            (*engine)->execution().setWake(&window::LuxWindow::wakeEvents);
            return std::make_unique<Impl>(
                std::move(config),
                std::move(assembly),
                std::move(*window),
                std::move(*engine),
                std::move(*scene)
            );
        }
        struct OpenProject final
        {
            std::unique_ptr<EditorContext> context;
            detail::ProjectUiMount ui;
        };
        struct AdoptProject final
        {
            std::uint64_t request_serial{};
            std::filesystem::path manifest_file;
            bool manifest_published{};
            std::unique_ptr<EditorContext> context;
            std::vector<std::unique_ptr<ui::Pane>> panes;
        };
        struct CloseCurrentProject final
        {
            std::uint64_t request_serial{};
        };
        using VPendingProjectChange = std::variant<AdoptProject, CloseCurrentProject>;

        void invalidatePreparation() noexcept
        {
            if (project_request_serial_ == std::numeric_limits<std::uint64_t>::max())
            {
                std::terminate();
            }
            ++project_request_serial_;
            if (project_prepare_)
            {
                static_cast<void>(engine_->execution().requestStop(*project_prepare_));
                project_prepare_.reset();
            }
            // Move before cleanup: extension destructors may synchronously submit a newer intent.
            auto discarded = std::move(pending_project_change_);
            pending_project_change_.reset();
        }
        FrameworkResult<void> admitProject(
            LuxEngine& owner,
            std::filesystem::path file,
            std::optional<ProjectManifest> create
        ) noexcept
        {
            if (window_->shouldClose())
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
            auto scheduler = engine_->execution().blocking();
            if (!scheduler)
            {
                return cxx::unexpected(executionError(scheduler.error()));
            }
            // An intent triggered by candidate cleanup is later than this request.
            const auto serial = project_request_serial_ + 1;
            invalidatePreparation();
            if (serial != project_request_serial_)
            {
                return {};
            }
            auto submitted = project_tasks_.submit(
                {"Prepare project", "editor.project"},
                [scheduler = *scheduler,
                 destination = process::objectScheduler(owner),
                 owner = &owner,
                 serial,
                 file = std::move(file),
                 create = std::move(create),
                 locations = config_.plugin_locations](process::TaskReporter reporter) mutable noexcept
                {
                    auto work =
                        stdexec::schedule(scheduler) |
                        stdexec::then([file, create = std::move(create), locations = std::move(locations), reporter](
                                      ) noexcept { return detail::prepareProject(file, create, locations, reporter); }
                        ) |
                        stdexec::upon_error(
                            [](process::EExecutionError error) noexcept
                            { return detail::ProjectPreparation{cxx::unexpected(executionError(error)), false}; }
                        ) |
                        stdexec::upon_stopped(
                            []() noexcept
                            {
                                return detail::ProjectPreparation{
                                    cxx::unexpected(error::Error{Errors::ProjectCancelled}),
                                    false
                                };
                            }
                        );
                    return stdexec::continues_on(std::move(work), destination) |
                           stdexec::then(
                               [owner, serial, task = reporter.id(), file = std::move(file)](
                                   detail::ProjectPreparation result
                               ) mutable noexcept
                               {
                                   detail::ProjectPrepared completion{serial, task, std::move(file), std::move(result)};
                                   static_cast<void>(object::sendEvent(*owner, completion));
                               }
                           );
                }
            );
            if (!submitted)
            {
                return cxx::unexpected(executionError(submitted.error()));
            }
            project_prepare_ = *submitted;
            return {};
        }
        void receivePrepared(LuxEngine& owner, detail::ProjectPrepared& event) noexcept
        {
            const auto serial = event.request_serial;
            if (serial != project_request_serial_ || window_->shouldClose())
            {
                return;
            }
            if (project_prepare_ == event.task)
            {
                project_prepare_.reset();
            }
            auto fail = [&](error::Error error) noexcept
            {
                if (serial == project_request_serial_ && error.type != Errors::ProjectCancelled)
                {
                    static_cast<void>(owner.emit(
                        owner.projectOpenFailed,
                        ProjectOpenFailure{event.manifest_file, error, event.result.published}
                    ));
                }
            };
            if (!event.result.result)
            {
                fail(event.result.result.error());
                return;
            }
            EditorComposition composition;
            if (assembly_)
            {
                auto assembled = assembly_(composition);
                if (!assembled)
                {
                    fail(assembled.error());
                    return;
                }
            }
            if (serial != project_request_serial_)
            {
                return;
            }
            auto context =
                detail::createEditorContext(*engine_, std::move(*event.result.result), std::move(composition));
            if (!context)
            {
                fail(context.error());
                return;
            }
            AdoptProject candidate{serial, event.manifest_file, event.result.published, std::move(*context), {}};
            candidate.panes.reserve(config_.layout.size());
            for (const auto& item : config_.layout)
            {
                auto pane = createPane(*candidate.context, item);
                if (serial != project_request_serial_)
                {
                    return;
                }
                if (!pane)
                {
                    fail(pane.error());
                    return;
                }
                const bool invalid_pane = !*pane || (*pane)->attachedRoot() || (*pane)->parent();
                if (invalid_pane)
                {
                    fail({Errors::EditorUiFactoryReturnedAnAttachedOrNullPane});
                    return;
                }
                candidate.panes.push_back(std::move(*pane));
            }
            pending_project_change_.emplace(std::move(candidate));
        }
        void requestClose() noexcept
        {
            const auto serial = project_request_serial_ + 1;
            invalidatePreparation();
            if (serial == project_request_serial_)
            {
                pending_project_change_.emplace(CloseCurrentProject{serial});
            }
        }
        FrameworkResult<void> applyPendingHostChanges(LuxEngine& owner) noexcept
        {
            if (!pending_project_change_)
            {
                return {};
            }
            // Never clear an intent submitted by a later notification or destructor.
            auto pending = std::move(*pending_project_change_);
            pending_project_change_.reset();
            const auto serial = std::visit([](const auto& value) noexcept { return value.request_serial; }, pending);
            if (serial != project_request_serial_)
            {
                return {};
            }
            auto* adopt = std::get_if<AdoptProject>(&pending);
            if (!adopt && !project_)
            {
                return {};
            }
            auto& root = window_->uiRoot();
            std::unique_ptr<OpenProject> next;
            if (adopt)
            {
                next = std::make_unique<OpenProject>();
                next->ui.prepare(root, adopt->panes.size());
                next->context = std::move(adopt->context);
            }
            auto old = std::move(project_);
            auto commit = [&](std::span<const ui::PaneHandle> added) noexcept
            {
                if (old)
                {
                    old->ui.disarm();
                }
                if (next)
                {
                    next->ui.arm(added);
                    project_ = std::move(next);
                }
            };
            auto removed = root.replacePanes(
                old ? old->ui.handles() : std::span<const ui::PaneHandle>{},
                adopt ? std::span{adopt->panes} : std::span<std::unique_ptr<ui::Pane>>{},
                commit
            );
            if (!removed)
            {
                project_ = std::move(old);
                // Restore candidate lifetime order before user cleanup or failure notification.
                if (adopt)
                {
                    adopt->context = std::move(next->context);
                }
                if (removed.error() == ui::EPaneError::BUSY)
                {
                    return cxx::unexpected(
                        error::Error{Errors::EditorUiClearBusy, {static_cast<std::uint64_t>(removed.error())}}
                    );
                }
                if (adopt && serial == project_request_serial_)
                {
                    static_cast<void>(owner.emit(
                        owner.projectOpenFailed,
                        ProjectOpenFailure{
                            adopt->manifest_file,
                            {Errors::EditorCannotMountProjectWindows, {static_cast<std::uint64_t>(removed.error())}},
                            adopt->manifest_published
                        }
                    ));
                }
                return {};
            }
            removed->clear();
            old.reset();
            static_cast<void>(owner.emit(owner.projectChanged));
            return {};
        }
        FrameworkResult<EFrameStatus> frame(LuxEngine& owner) noexcept
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
                requestClose();
            }
            if (auto applied = applyPendingHostChanges(owner); !applied)
            {
                return cxx::unexpected(applied.error());
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
            return closing ? EFrameStatus::EXIT_REQUESTED : EFrameStatus::RUNNING;
        }
        FrameworkResult<void> exec(LuxEngine& owner) noexcept
        {
            for (;;)
            {
                auto running = owner.frame();
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
                const bool ready = window_->shouldClose() || pending_project_change_.has_value() || can_produce ||
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
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::unique_ptr<EditorUiScene> ui_scene_;
        EditorConfig config_;
        EditorAssembly assembly_;
        process::TaskScope project_tasks_;
        std::unique_ptr<OpenProject> project_;
        std::optional<VPendingProjectChange> pending_project_change_;
        std::optional<process::TaskId> project_prepare_;
        std::uint64_t project_request_serial_{};
        FrameStatistics statistics_;
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()};
        bool operating_{};
    };
    LuxEngine::LuxEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
        impl_->window_->uiRoot().setCommandFallback(this);
    }
    LuxEngine::~LuxEngine() noexcept
    {
        impl_->window_->uiRoot().setCommandFallback(nullptr);
        beginDestruction();
    }
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
        auto impl = Impl::create(std::move(config), std::move(assembly));
        if (!impl)
        {
            return cxx::unexpected(impl.error());
        }
        return std::unique_ptr<LuxEngine>{new LuxEngine(std::move(*impl))};
    }
    void LuxEngine::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<OpenProjectRequest>())
        {
            event.accept();
            auto result = impl_->admitProject(*this, request->manifest_file, {});
            request->rejection = result ? error::Error{} : result.error();
        }
        else if (auto* request = event.getIf<CreateProjectRequest>())
        {
            event.accept();
            auto result = impl_->admitProject(*this, request->root / "Project.luxproj", std::move(request->manifest));
            request->rejection = result ? error::Error{} : result.error();
        }
        else if (event.getIf<CloseProjectRequest>())
        {
            event.accept();
            impl_->requestClose();
        }
        else if (auto* prepared = event.getIf<detail::ProjectPrepared>())
        {
            event.accept();
            impl_->receivePrepared(*this, *prepared);
        }
        else if (auto* command = event.getIf<ui::Command>())
        {
            const bool close = command->id == ui::CommandIdView{"lux.project.close"};
            const bool cancel = command->id == ui::CommandIdView{"lux.project.cancel_open"};
            if (!close && !cancel)
            {
                return;
            }
            event.accept();
            command->enabled = close ? impl_->project_ != nullptr : impl_->project_prepare_.has_value();
            command->result =
                command->enabled ? ui::ECommandDispatchResult::NOT_FOUND : ui::ECommandDispatchResult::DISABLED;
            if (command->phase == ui::ECommandPhase::EXECUTE && command->enabled)
            {
                if (close)
                {
                    impl_->requestClose();
                }
                else
                {
                    impl_->invalidatePreparation();
                }
                command->result = ui::ECommandDispatchResult::EXECUTED;
            }
        }
    }
    FrameworkResult<void> LuxEngine::exec() noexcept
    {
        return impl_->exec(*this);
    }
    FrameworkResult<EFrameStatus> LuxEngine::frame() noexcept
    {
        beginCallbackBorrow(*this);
        auto result = impl_->frame(*this);
        endCallbackBorrow(*this);
        return result;
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
    EditorContext* LuxEngine::project() noexcept
    {
        return impl_->project_ ? impl_->project_->context.get() : nullptr;
    }
    const EditorContext* LuxEngine::project() const noexcept
    {
        return impl_->project_ ? impl_->project_->context.get() : nullptr;
    }
} // namespace lux::editor
