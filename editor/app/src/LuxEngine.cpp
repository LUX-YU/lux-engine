#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUiErrors.hpp>
#include <lux/engine/editor/EditorUiFactories.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/detail/EditorUiScene.hpp>
#include <lux/engine/editor/detail/LuxEngineTestAccess.hpp>
#include <lux/engine/editor/detail/ProjectPrepared.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/process/ObjectScheduler.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/rendering/WindowSurface.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <lux/engine/ui/Command.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        error::Error creationError(const engine::EngineContext::VCreateFailure& failure) noexcept
        {
            return std::visit(
                [](const auto& value) noexcept -> error::Error
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, execution::EExecutionError>)
                    {
                        return execution::toError(value);
                    }
                    else
                    {
                        return scene::toError(value);
                    }
                },
                failure
            );
        }

    } // namespace

    struct LuxEngine::Impl final
    {
        struct OpenProject final
        {
            std::unique_ptr<EditorContext> context;
            ProjectScenes* scenes{};
            detail::ProjectUiMount ui;

            void beginClose() noexcept
            {
                ui = {};
                scenes = context->services().constructed<ProjectScenes>();
                if (scenes)
                {
                    scenes->closeAll();
                }
            }

            bool advanceClose() noexcept
            {
                if (scenes)
                {
                    scenes->maintain();
                    if (!scenes->settled())
                    {
                        return false;
                    }
                }
                context->close();
                return context->finishClose();
            }
        };

        enum class EClosePhase : std::uint8_t
        {
            OPEN,
            PROJECTS,
            ASSETS,
            RENDER,
            EXECUTION,
            CLOSED
        };

        Impl(
            EditorConfig config,
            EditorAssembly assembly,
            std::unique_ptr<EditorWindow> window,
            std::unique_ptr<engine::EngineContext> engine,
            std::unique_ptr<rendering::RenderExecution> rendering,
            std::unique_ptr<process::asset_loading::VfsAssetReadEndpoint> source,
            std::unique_ptr<rendering::RenderAssets> assets,
            std::unique_ptr<EditorUiScene> ui_scene
        ) noexcept
            : window_(std::move(window)), engine_(std::move(engine)), rendering_(std::move(rendering)),
              source_(std::move(source)), assets_(std::move(assets)), ui_scene_(std::move(ui_scene)),
              config_(std::move(config)), assembly_(std::move(assembly)), project_tasks_(engine_->execution())
        {
        }

        ~Impl() noexcept
        {
            if (close_phase_ != EClosePhase::CLOSED)
            {
                std::terminate(); // The facade drains while all dependencies and its callback identity still exist.
            }
        }

        static FrameworkResult<std::unique_ptr<Impl>> create(EditorConfig config, EditorAssembly assembly) noexcept
        {
            auto window = EditorWindow::create({config.width, config.height, config.title});
            if (!window)
            {
                return cxx::unexpected(window.error());
            }
            auto engine = engine::EngineContext::create(config.execution, 2048);
            if (!engine)
            {
                return cxx::unexpected(creationError(engine.error()));
            }
            auto& execution = (*engine)->execution();
            object::ObjectRuntime::instance().setWake(&window::LuxWindow::wakeEvents);
            execution.setWake(&window::LuxWindow::wakeEvents);
            render::SurfaceDescription surface;
            if (!rendering::makeWindowSurface((*window)->nativeWindow(), config.enable_vsync, surface))
            {
                return cxx::unexpected(error::Error{Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatform});
            }
            rendering::RenderExecutionConfig render_config;
            render_config.render.channel_name = "lux.editor.render";
            render_config.render.validation = config.enable_validation;
            render_config.render.frames_in_flight = 3;
            render_config.render.max_frames_ahead = 1;
            render_config.render.uploads.emplace();
            render_config.device.texture_descriptors = true;
            render_config.device.texture_compression_bc = true;
            auto rendering = rendering::RenderExecution::create(std::move(render_config), execution);
            if (!rendering)
            {
                return cxx::unexpected(rendering.error());
            }
            // Only an unpublished startup failure uses this rollback. Later members have no accepted business work.
            cxx::scope_exit stop_render(
                [&]() noexcept
                {
                    if (auto stopped = (*rendering)->shutdown(); !stopped)
                    {
                        rendering::reportRenderFailure(stopped.error(), "Rollback Editor startup");
                    }
                }
            );
            auto blocking = execution.blocking();
            if (!blocking)
            {
                return cxx::unexpected(execution::toError(blocking.error()));
            }
            auto source = process::asset_loading::VfsAssetReadEndpoint::create(
                (*engine)->assets().view(),
                *blocking,
                execution,
                {16}
            );
            if (!source)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            auto assets = rendering::RenderAssets::create(**rendering, (*source)->port());
            if (!assets)
            {
                return cxx::unexpected(assets.error());
            }
            auto atlas = (*window)->uiRoot().fontAtlas();
            if (!atlas)
            {
                return cxx::unexpected(
                    error::Error{Errors::UiRenderConfiguration, {static_cast<std::uint64_t>(atlas.error())}}
                );
            }
            const auto& metrics = (*window)->metrics();
            render::TargetDesc output{
                metrics.framebuffer_width,
                metrics.framebuffer_height,
                rdesc::ETextureFormat::RGBA8_UNORM,
                1,
                render::ETargetKind::SURFACE
            };
            auto ui_scene = EditorUiScene::create(**rendering, output, surface, std::move(*atlas));
            if (!ui_scene)
            {
                return cxx::unexpected(ui_scene.error());
            }
            auto impl = std::make_unique<Impl>(
                std::move(config),
                std::move(assembly),
                std::move(*window),
                std::move(*engine),
                std::move(*rendering),
                std::move(*source),
                std::move(*assets),
                std::move(*ui_scene)
            );
            stop_render.release();
            return impl;
        }

        void retire(std::unique_ptr<OpenProject> project) noexcept
        {
            project->beginClose();
            if (!project->advanceClose())
            {
                closing_projects_.push_back(std::move(project));
            }
        }

        void retire(std::unique_ptr<EditorContext> context) noexcept
        {
            if (context)
            {
                auto project = std::make_unique<OpenProject>();
                project->context = std::move(context);
                retire(std::move(project));
            }
        }

        void collectProjects() noexcept
        {
            // Destructors can enqueue a new retirement. Keep objects stable, and remove lookup before destruction.
            for (auto remaining = closing_projects_.size(); remaining != 0;)
            {
                const auto index = --remaining;
                if (!closing_projects_[index]->advanceClose())
                {
                    continue;
                }
                auto retired = std::move(closing_projects_[index]);
                if (index + 1 != closing_projects_.size())
                {
                    closing_projects_[index] = std::move(closing_projects_.back());
                }
                closing_projects_.pop_back();
            }
        }

        FrameworkResult<void> dispatchCompletions() noexcept
        {
            if (close_phase_ < EClosePhase::RENDER)
            {
                if (auto received = rendering_->consumeResponses(); !received)
                {
                    return cxx::unexpected(received.error());
                }
            }
            auto& execution = engine_->execution();
            if (auto completed = execution.dispatchMainCompletions(); !completed)
            {
                return cxx::unexpected(execution::toError(completed.error()));
            }
            if (auto monitored = execution.dispatchTaskEvents(); !monitored)
            {
                return cxx::unexpected(execution::toError(monitored.error()));
            }
            static_cast<void>(object::ObjectRuntime::instance().dispatchPending());
            return {};
        }

        FrameworkResult<void> maintenanceFrame() noexcept
        {
            if (!rendering_->mainClient().hasQueuedFrameCommands())
            {
                return {};
            }
            auto input = rendering_->beginFrame();
            if (!input)
            {
                return cxx::unexpected(input.error());
            }
            return *input ? rendering_->submitFrame(**input) : FrameworkResult<void>{};
        }

        void beginClose() noexcept
        {
            if (close_phase_ != EClosePhase::OPEN)
            {
                return;
            }
            close_phase_ = EClosePhase::PROJECTS;
            invalidatePreparation();
            project_tasks_.requestStop();
            if (!window_->uiRoot().clearPanes())
            {
                std::terminate(); // Called outside Root dispatch, before any Context application is retired.
            }
            if (project_)
            {
                project_->ui.disarm();
                retire(std::move(project_));
            }
        }

        FrameworkResult<ELoopState> advanceClose() noexcept
        {
            if (auto dispatched = dispatchCompletions(); !dispatched)
            {
                return cxx::unexpected(dispatched.error());
            }
            if (close_phase_ == EClosePhase::PROJECTS)
            {
                auto driven = engine_->sceneRuntime().driveFrame(scene::ESceneDriveMode::MAINTENANCE);
                if (!driven)
                {
                    return cxx::unexpected(scene::toError(driven.error()));
                }
                if (!driven->empty())
                {
                    return cxx::unexpected(scene::toError(driven->front()));
                }
                collectProjects();
                if (closing_projects_.empty() && project_tasks_.settled())
                {
                    ui_scene_->stopFrames();
                    assets_->close();
                    close_phase_ = EClosePhase::ASSETS;
                }
            }
            if (close_phase_ == EClosePhase::PROJECTS || close_phase_ == EClosePhase::ASSETS)
            {
                if (auto ui = ui_scene_->maintain(); !ui)
                {
                    return cxx::unexpected(ui.error());
                }
                assets_->maintain();
                if (auto frame = maintenanceFrame(); !frame)
                {
                    return cxx::unexpected(frame.error());
                }
                const bool settled = close_phase_ == EClosePhase::ASSETS && ui_scene_->settled() &&
                                     assets_->settled() && rendering_->mainClient().pendingRequests() == 0;
                if (settled)
                {
                    rendering_->beginClose();
                    close_phase_ = EClosePhase::RENDER;
                }
            }
            if (close_phase_ == EClosePhase::RENDER)
            {
                auto stopped = rendering_->advanceClose();
                if (!stopped)
                {
                    return cxx::unexpected(stopped.error());
                }
                if (*stopped)
                {
                    engine_->execution().requestStop();
                    close_phase_ = EClosePhase::EXECUTION;
                }
            }
            auto& objects = object::ObjectRuntime::instance();
            static_cast<void>(objects.collectRetired());
            if (close_phase_ == EClosePhase::EXECUTION && engine_->execution().settled() &&
                objects.statistics().pending == 0)
            {
                return ELoopState::STOPPED;
            }
            const bool pending_frame = close_phase_ < EClosePhase::RENDER &&
                                       rendering_->mainClient().hasQueuedFrameCommands() &&
                                       rendering_->mainClient().hasFrameCapacity();
            return pending_frame ? ELoopState::RUNNING : ELoopState::WAITING;
        }

        FrameworkResult<void> finishClose() noexcept
        {
            if (close_phase_ == EClosePhase::CLOSED)
            {
                return {};
            }
            // This barrier is outside pumpOnce's callback borrow and outside every counted Sender.
            if (auto joined = engine_->execution().join(); !joined)
            {
                return cxx::unexpected(execution::toError(joined.error()));
            }
            close_phase_ = EClosePhase::CLOSED;
            return {};
        }

        void waitIfIdle(std::uint64_t observed_epoch) noexcept
        {
            auto& execution = engine_->execution();
            const bool notified = execution.wakeEpoch() != observed_epoch || execution.hasReadyWork() ||
                                  object::ObjectRuntime::instance().statistics().pending != 0;
            if (!notified)
            {
                window::LuxWindow::waitEvents();
            }
        }

        FrameworkResult<void> close() noexcept
        {
            if (close_phase_ == EClosePhase::CLOSED)
            {
                return {};
            }
            beginClose();
            for (;;)
            {
                const auto epoch = engine_->execution().wakeEpoch();
                auto progress = advanceClose();
                if (!progress)
                {
                    return cxx::unexpected(progress.error());
                }
                if (*progress == ELoopState::STOPPED)
                {
                    return finishClose();
                }
                if (*progress == ELoopState::WAITING)
                {
                    waitIfIdle(epoch);
                }
            }
        }

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
            if (discarded)
            {
                if (auto* candidate = std::get_if<AdoptProject>(&*discarded))
                {
                    candidate->panes.clear();
                    retire(std::move(candidate->context));
                }
            }
        }

        static auto prepareProjectTask(
            LuxEngine& application,
            execution::BlockingScheduler scheduler,
            std::uint64_t serial,
            std::filesystem::path file,
            std::optional<ProjectManifest> create,
            std::vector<ProjectPluginLocation> locations,
            execution::TaskReporter reporter
        ) noexcept
        {
            auto work =
                stdexec::schedule(scheduler) |
                stdexec::then([file, create = std::move(create), locations = std::move(locations), reporter]() noexcept
                              { return detail::prepareProject(file, create, locations, reporter); }) |
                stdexec::upon_error(
                    [](execution::EExecutionError error) noexcept
                    { return detail::ProjectPreparation{cxx::unexpected(execution::toError(error)), false}; }
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
            return stdexec::continues_on(std::move(work), process::objectScheduler(application)) |
                   stdexec::then(
                       [&application, serial, task = reporter.id(), file = std::move(file)](
                           detail::ProjectPreparation result
                       ) mutable noexcept
                       {
                           const auto outcome =
                               result.result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                           detail::ProjectPrepared completion{serial, task, std::move(file), std::move(result)};
                           static_cast<void>(object::sendEvent(application, completion));
                           return outcome;
                       }
                   );
        }

        FrameworkResult<void> admitProject(
            LuxEngine& application,
            std::filesystem::path file,
            std::optional<ProjectManifest> create
        ) noexcept
        {
            if (close_phase_ != EClosePhase::OPEN || window_->shouldClose())
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
                return cxx::unexpected(execution::toError(scheduler.error()));
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
                [this,
                 &application,
                 scheduler = *scheduler,
                 serial,
                 file = std::move(file),
                 create = std::move(create)](execution::TaskReporter reporter) mutable noexcept
                {
                    return prepareProjectTask(
                        application,
                        scheduler,
                        serial,
                        std::move(file),
                        std::move(create),
                        config_.plugin_locations,
                        reporter
                    );
                }
            );
            if (!submitted)
            {
                return cxx::unexpected(execution::toError(submitted.error()));
            }
            project_prepare_ = *submitted;
            return {};
        }

        void receivePrepared(LuxEngine& application, detail::ProjectPrepared& event) noexcept
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
                    static_cast<void>(application.emit(
                        application.projectOpenFailed,
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
            if (!composition.bindExtension(*rendering_) || !composition.bindExtension(*assets_) ||
                !composition.bindExtension(*ui_scene_))
            {
                std::terminate();
            }
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
            cxx::scope_exit retire_candidate(
                [&]() noexcept
                {
                    candidate.panes.clear();
                    retire(std::move(candidate.context));
                }
            );
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

        void requestProjectClose() noexcept
        {
            const auto serial = project_request_serial_ + 1;
            invalidatePreparation();
            if (serial == project_request_serial_)
            {
                pending_project_change_.emplace(CloseCurrentProject{serial});
            }
        }

        FrameworkResult<void> applyPendingProjectChanges(LuxEngine& application) noexcept
        {
            if (!pending_project_change_)
            {
                return {};
            }
            // Never clear an intent submitted by a later notification or destructor.
            auto pending = std::move(*pending_project_change_);
            pending_project_change_.reset();
            cxx::scope_exit retire_candidate(
                [&]() noexcept
                {
                    if (auto* candidate = std::get_if<AdoptProject>(&pending))
                    {
                        candidate->panes.clear();
                        retire(std::move(candidate->context));
                    }
                }
            );
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
                next->scenes = next->context->services().constructed<ProjectScenes>();
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
                    static_cast<void>(application.emit(
                        application.projectOpenFailed,
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
            if (project_ && !config_.layout.empty())
            {
                ui::DockTree tree;
                tree.nodes.resize(7);
                tree.nodes[0] = {ui::EDockSplit::HORIZONTAL, 1, 2, 0.22F};
                tree.nodes[2] = {ui::EDockSplit::VERTICAL, 3, 4, 0.74F};
                tree.nodes[3] = {ui::EDockSplit::HORIZONTAL, 5, 6, 0.72F};
                const auto handles = project_->ui.handles();
                for (std::size_t i = 0; i < handles.size(); ++i)
                {
                    const auto region = config_.layout[i].region;
                    const auto node = region == EPaneRegion::LEFT     ? 1
                                      : region == EPaneRegion::RIGHT  ? 6
                                      : region == EPaneRegion::BOTTOM ? 4
                                                                      : 5;
                    tree.nodes[node].panes.push_back(handles[i]);
                }
                // Application panes, such as project creation, remain Root-owned and tab with project tools.
                for (const auto& node : root.captureDockTree().nodes)
                {
                    for (auto handle : node.panes)
                    {
                        if (std::ranges::find(handles, handle) == handles.end())
                        {
                            tree.nodes[1].panes.push_back(handle);
                        }
                    }
                }
                const auto& metrics = window_->metrics();
                tree.surfaces.push_back(
                    {0, {{0, 0}, {static_cast<float>(metrics.width), static_cast<float>(metrics.height)}}, false}
                );
                if (!root.setDockTree(std::move(tree)))
                {
                    std::terminate();
                }
            }
            if (old)
            {
                retire(std::move(old));
            }
            old.reset();
            static_cast<void>(application.emit(application.projectChanged));
            return {};
        }

        void updateSurfaceExtent() noexcept
        {
            window::LuxWindow::pollEvents();
            const auto& metrics = window_->metrics();
            if (metrics.revision != applied_metrics_revision_)
            {
                ui_scene_->setExtent(metrics.framebuffer_width, metrics.framebuffer_height, !metrics.minimized);
                applied_metrics_revision_ = metrics.revision;
            }
        }

        FrameworkResult<bool> prepareUiOutput() noexcept
        {
            if (auto maintained = ui_scene_->maintain(); !maintained)
            {
                return cxx::unexpected(maintained.error());
            }
            auto output = ui_scene_->outputReady();
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            const auto& metrics = window_->metrics();
            const bool has_extent =
                metrics.width && metrics.height && metrics.framebuffer_width && metrics.framebuffer_height;
            return has_extent && !metrics.minimized && *output;
        }

        FrameworkResult<bool> captureUi(rendering::FrameInput* frame, bool draw) noexcept
        {
            auto& root = window_->uiRoot();
            const auto& metrics = window_->metrics();
            const auto now = std::chrono::steady_clock::now();
            const float elapsed =
                std::max(std::chrono::duration<float>(now - last_frame_).count(), std::numeric_limits<float>::min());
            auto* data = draw && frame ? &ui_scene_->captureBuffer(*frame) : nullptr;
            ui::FrameInfo info{
                {float(metrics.width), float(metrics.height)},
                elapsed,
                {metrics.width ? float(metrics.framebuffer_width) / metrics.width : 1.F,
                 metrics.height ? float(metrics.framebuffer_height) / metrics.height : 1.F}
            };
            auto captured = data ? root.update(info, *data) : root.update();
            if (!captured)
            {
                return cxx::unexpected(error::Error{Errors::UiCapture, {static_cast<std::uint64_t>(captured.error())}});
            }
            if (data)
            {
                last_frame_ = now;
            }
            const auto consumed = root.inputSnapshot();
            window_->input().evaluate(elapsed, data && !consumed.keyboard_captured, data && !consumed.pointer_captured);
            return data != nullptr;
        }

        FrameworkResult<ELoopState> pumpOnce(LuxEngine& application) noexcept
        {
            if (pumping_)
            {
                return cxx::unexpected(error::Error{Errors::EditorRecursiveHostFrame, {}});
            }
            pumping_ = true;
            cxx::scope_exit finish([this]() noexcept { pumping_ = false; });
            if (window_->shouldClose() || close_phase_ != EClosePhase::OPEN)
            {
                beginClose();
                return advanceClose();
            }
            updateSurfaceExtent();
            if (auto dispatched = dispatchCompletions(); !dispatched)
            {
                return cxx::unexpected(dispatched.error());
            }
            if (auto applied = applyPendingProjectChanges(application); !applied)
            {
                return cxx::unexpected(applied.error());
            }
            if (auto input = window_->sampleInput(); !input)
            {
                return cxx::unexpected(std::move(input.error()));
            }
            if (project_ && project_->scenes)
            {
                project_->scenes->maintain();
            }
            assets_->maintain();
            auto presentation = prepareUiOutput();
            if (!presentation)
            {
                return cxx::unexpected(presentation.error());
            }
            const bool can_draw = *presentation;
            auto& client = rendering_->mainClient();
            auto admitted = can_draw || client.hasQueuedFrameCommands()
                                ? rendering_->beginFrame()
                                : FrameworkResult<rendering::FrameInput*>{nullptr};
            if (!admitted)
            {
                return cxx::unexpected(admitted.error());
            }
            auto* frame = *admitted;
            cxx::scope_exit release_unsent(
                [&]() noexcept
                {
                    if (frame)
                    {
                        static_cast<void>(client.releaseFrame(*frame));
                    }
                }
            );
            auto captured = captureUi(frame, can_draw);
            if (!captured)
            {
                return cxx::unexpected(captured.error());
            }
            const auto mode = frame && can_draw ? scene::ESceneDriveMode::ADVANCE : scene::ESceneDriveMode::MAINTENANCE;
            auto driven = engine_->sceneRuntime().driveFrame(mode);
            if (!driven)
            {
                return cxx::unexpected(scene::toError(driven.error()));
            }
            if (!driven->empty())
            {
                return cxx::unexpected(scene::toError(driven->front()));
            }
            collectProjects();
            if (*captured)
            {
                auto appended = ui_scene_->appendFrameCommands(*frame);
                if (!appended)
                {
                    return cxx::unexpected(appended.error());
                }
            }
            if (frame)
            {
                auto* submitted = std::exchange(frame, nullptr);
                if (auto sent = rendering_->submitFrame(*submitted); !sent)
                {
                    return cxx::unexpected(sent.error());
                }
            }
            (void)object::ObjectRuntime::instance().collectRetired();
            const bool another_frame = client.hasFrameCapacity() && (can_draw || client.hasQueuedFrameCommands());
            return another_frame || pending_project_change_ ? ELoopState::RUNNING : ELoopState::WAITING;
        }

        // Reverse destruction: UI/assets/source, RenderExecution, Engine ExecutionRuntime, then native window.
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::unique_ptr<rendering::RenderExecution> rendering_;
        std::unique_ptr<process::asset_loading::VfsAssetReadEndpoint> source_;
        std::unique_ptr<rendering::RenderAssets> assets_;
        std::unique_ptr<EditorUiScene> ui_scene_;
        EditorConfig config_;
        EditorAssembly assembly_;
        execution::TaskScope project_tasks_;
        std::vector<std::unique_ptr<OpenProject>> closing_projects_;
        EClosePhase close_phase_ = EClosePhase::OPEN;
        std::unique_ptr<OpenProject> project_;
        std::optional<VPendingProjectChange> pending_project_change_;
        std::optional<execution::TaskId> project_prepare_;
        std::uint64_t project_request_serial_{};
        std::uint64_t applied_metrics_revision_{};
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()};
        bool pumping_{};
    };

    LuxEngine::LuxEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
        impl_->window_->uiRoot().setCommandFallback(this);
    }

    LuxEngine::~LuxEngine() noexcept
    {
        if (isDispatching()) // Final application barrier cannot run inside a completion callback.
        {
            std::terminate();
        }
        impl_->window_->uiRoot().setCommandFallback(nullptr);
        beginDestruction();
        if (auto closed = impl_->close(); !closed)
        {
            rendering::reportRenderFailure(closed.error(), "Close Editor");
            std::terminate();
        }
    }

    FrameworkResult<std::unique_ptr<LuxEngine>> LuxEngine::create(EditorConfig config, EditorAssembly assembly) noexcept
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
        if (auto registered = execution::registerExecutionErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerAppErrors(); !registered)
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
            impl_->requestProjectClose();
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
                if (impl_->project_ && impl_->project_->scenes)
                {
                    if (object::sendEvent(*impl_->project_->scenes, *command))
                    {
                        event.accept();
                    }
                }
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
                    impl_->requestProjectClose();
                }
                else
                {
                    impl_->invalidatePreparation();
                }
                command->result = ui::ECommandDispatchResult::EXECUTED;
            }
        }
    }

    FrameworkResult<void> LuxEngine::run() noexcept
    {
        for (;;)
        {
            const auto epoch = impl_->engine_->execution().wakeEpoch();
            auto state = pumpOnce();
            if (!state)
            {
                return cxx::unexpected(state.error());
            }
            if (*state == ELoopState::STOPPED)
            {
                return impl_->finishClose();
            }
            if (*state == ELoopState::WAITING)
            {
                impl_->waitIfIdle(epoch);
            }
        }
    }

    FrameworkResult<LuxEngine::ELoopState> LuxEngine::pumpOnce() noexcept
    {
        if (isDispatching())
        {
            return cxx::unexpected(error::Error{Errors::EditorRecursiveHostFrame});
        }
        beginCallbackBorrow(*this);
        auto result = impl_->pumpOnce(*this);
        endCallbackBorrow(*this);
        return result;
    }

    FrameworkResult<bool> detail::LuxEngineTestAccess::pumpOnce(LuxEngine& host) noexcept
    {
        auto state = host.pumpOnce();
        if (!state)
        {
            return cxx::unexpected(state.error());
        }
        return *state != LuxEngine::ELoopState::STOPPED;
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

    rendering::RenderExecution& LuxEngine::rendering() noexcept
    {
        return *impl_->rendering_;
    }

    const rendering::RenderExecution& LuxEngine::rendering() const noexcept
    {
        return *impl_->rendering_;
    }

    rendering::RenderAssets& LuxEngine::renderAssets() noexcept
    {
        return *impl_->assets_;
    }

    FrameworkResult<void> LuxEngine::setUiImages(std::vector<UiTargetImage> images) noexcept
    {
        return impl_->ui_scene_->setTargetImages(std::move(images));
    }

    bool LuxEngine::uiImagesApplied() const noexcept
    {
        return impl_->ui_scene_->imagesApplied();
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
