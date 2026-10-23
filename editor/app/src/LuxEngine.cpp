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
        struct ApplicationResources final
        {
            std::unique_ptr<EditorWindow> window;
            std::unique_ptr<engine::EngineContext> engine;
            std::unique_ptr<rendering::RenderExecution> rendering;
            std::optional<execution::TaskScope> asset_read_tasks;
            std::unique_ptr<process::asset_loading::VfsAssetReadEndpoint> source;
            std::unique_ptr<rendering::RenderAssets> assets;
            std::unique_ptr<EditorUiScene> ui_scene;
            std::vector<std::unique_ptr<EditorContext>> closing_projects;
            std::size_t frames_pending = 0;
            bool closed = false;

            void retire(std::unique_ptr<EditorContext> context) noexcept
            {
                if (!context)
                {
                    return;
                }
                context->close();
                if (!context->finishClose())
                {
                    closing_projects.push_back(std::move(context));
                }
            }

            void collectProjects() noexcept
            {
                // Stop callbacks and service destructors may enqueue another retirement. Keep no
                // vector iterator across them, and remove lookup ownership before running a destructor.
                for (auto remaining = closing_projects.size(); remaining != 0;)
                {
                    const auto index = --remaining;
                    auto* context = closing_projects[index].get();
                    if (!context->finishClose())
                    {
                        continue;
                    }
                    auto retired = std::move(closing_projects[index]);
                    if (index + 1 != closing_projects.size())
                    {
                        closing_projects[index] = std::move(closing_projects.back());
                    }
                    closing_projects.pop_back();
                }
            }

            ~ApplicationResources() noexcept
            {
                if (engine && !closed)
                {
                    close();
                }
            }

            void consume() noexcept
            {
                auto& execution = engine->execution();
                if (rendering)
                {
                    rendering->consumeResponses();
                }
                if (!execution.dispatchMainCompletions() || !execution.dispatchTaskEvents())
                {
                    std::terminate();
                }
            }

            FrameworkResult<void> submit(rendering::FrameInput& frame) noexcept
            {
                ++frames_pending;
                auto accepted = engine->execution().submit(
                    {.name = "Editor frame", .category = "editor.render"},
                    [this, &frame](execution::TaskReporter) noexcept
                    {
                        return stdexec::then(
                            rendering->mainClient().submitFrame(frame),
                            [this, &frame](render::FrameCompletion result) noexcept
                            {
                                if (result.error.type != 0)
                                {
                                    rendering::reportRenderFailure(result.error, "Editor frame");
                                }
                                if (!rendering->mainClient().releaseFrame(frame))
                                {
                                    std::terminate();
                                }
                                --frames_pending;
                                return result.error.type == 0 ? execution::ETaskState::SUCCEEDED
                                                              : execution::ETaskState::FAILED;
                            }
                        );
                    }
                );
                if (!accepted)
                {
                    --frames_pending;
                    static_cast<void>(rendering->mainClient().releaseFrame(frame));
                    return cxx::unexpected(execution::toError(accepted.error()));
                }
                return {};
            }

            void maintenanceFrame() noexcept
            {
                auto& client = rendering->mainClient();
                if (!client.hasQueuedFrameCommands())
                {
                    return;
                }
                auto input = client.beginFrame();
                if (input)
                {
                    if (!submit(**input))
                    {
                        std::terminate();
                    }
                }
                else if (input.error().type != render::kRuntimeCapacity && !client.closed())
                {
                    std::terminate();
                }
            }

            void close() noexcept
            {
                // Keep CPU/IO, the Main protocol and the window alive while accepted work settles.
                // Project Scene owners are retired by Impl before reaching this application boundary.
                auto& execution = engine->execution();
                auto& objects = object::ObjectRuntime::instance();
                if (ui_scene)
                {
                    ui_scene->stopFrames();
                }
                bool assets_closing = false;
                for (;;)
                {
                    const auto epoch = execution.wakeEpoch();
                    consume();
                    if (!closing_projects.empty())
                    {
                        const auto driven = engine->sceneRuntime().driveFrame();
                        if (!driven || !driven->empty())
                        {
                            std::terminate();
                        }
                        collectProjects();
                    }
                    if (assets && closing_projects.empty() && !assets_closing)
                    {
                        assets->close();
                        assets_closing = true;
                    }
                    if (ui_scene && !ui_scene->maintain())
                    {
                        std::terminate();
                    }
                    if (assets)
                    {
                        assets->maintain();
                    }
                    if (rendering)
                    {
                        maintenanceFrame();
                    }
                    static_cast<void>(objects.dispatchPending());
                    static_cast<void>(objects.collectRetired());
                    const bool ui_closed = !ui_scene || ui_scene->settled();
                    const bool assets_closed = !assets || assets->settled();
                    if (ui_closed && assets_closed && frames_pending == 0 && closing_projects.empty())
                    {
                        break;
                    }
                    if (!execution.hasReadyWork() && execution.wakeEpoch() == epoch)
                    {
                        window::LuxWindow::waitEvents();
                    }
                }
                if (rendering)
                {
                    const auto stopped = rendering->shutdown();
                    if (!stopped)
                    {
                        rendering::reportRenderFailure(stopped.error(), "Render shutdown");
                    }
                }
                ui_scene.reset();
                assets.reset();
                source.reset();
                asset_read_tasks.reset();
                execution.requestStop();
                for (;;)
                {
                    const auto epoch = execution.wakeEpoch();
                    consume();
                    static_cast<void>(objects.dispatchPending());
                    static_cast<void>(objects.collectRetired());
                    if (execution.settled() && objects.statistics().pending == 0)
                    {
                        break;
                    }
                    if (!execution.hasReadyWork() && execution.wakeEpoch() == epoch)
                    {
                        window::LuxWindow::waitEvents();
                    }
                }
                closed = true;
            }
        };

        Impl(EditorConfig config, EditorAssembly assembly) noexcept
            : config_(std::move(config)), assembly_(std::move(assembly))
        {
        }

        ~Impl() noexcept
        {
            if (application_.window && !application_.window->uiRoot().clearPanes())
            {
                std::terminate();
            }
            if (project_)
            {
                application_.retire(std::move(project_->context));
            }
            project_.reset();
            if (pending_project_change_)
            {
                if (auto* candidate = std::get_if<AdoptProject>(&*pending_project_change_))
                {
                    candidate->panes.clear();
                    application_.retire(std::move(candidate->context));
                }
                pending_project_change_.reset();
            }
            project_tasks_.reset();
            // ApplicationResources performs the final synchronous barrier after project revocation.
        }

        static FrameworkResult<std::unique_ptr<Impl>> create(EditorConfig config, EditorAssembly assembly) noexcept
        {
            auto impl = std::make_unique<Impl>(std::move(config), std::move(assembly));
            auto& application = impl->application_;
            auto window = EditorWindow::create({impl->config_.width, impl->config_.height, impl->config_.title});
            if (!window)
            {
                return cxx::unexpected(window.error());
            }
            application.window = std::move(*window);
            auto engine =
                engine::EngineContext::create({2, 512, {256}, execution::BlockingSchedulerConfig{2, 128}}, 2048);
            if (!engine)
            {
                return cxx::unexpected(creationError(engine.error()));
            }
            application.engine = std::move(*engine);
            auto& execution = application.engine->execution();
            object::ObjectRuntime::instance().setWake(&window::LuxWindow::wakeEvents);
            execution.setWake(&window::LuxWindow::wakeEvents);
            render::SurfaceDescription surface;
#if defined(_WIN32)
            surface.window.windows.hwnd = application.window->nativeHandle();
#else
            return cxx::unexpected(error::Error{Errors::EditorNativeUiOutputIsNotImplementedOnThisPlatform});
#endif
            surface.vertical_sync = impl->config_.enable_vsync;
            rendering::RenderExecutionConfig render_config;
            render_config.render.channel_name = "lux.editor.render";
            render_config.render.validation = impl->config_.enable_validation;
            render_config.render.frames_in_flight = 3;
            render_config.render.max_frames_ahead = 1;
            render_config.render.uploads.emplace();
            render_config.device.texture_descriptors = true;
            auto rendering = rendering::RenderExecution::create(std::move(render_config), execution);
            if (!rendering)
            {
                return cxx::unexpected(rendering.error());
            }
            application.rendering = std::move(*rendering);
            impl->project_tasks_.emplace(execution);
            application.asset_read_tasks.emplace(execution);
            auto blocking = execution.blocking();
            if (!blocking)
            {
                return cxx::unexpected(execution::toError(blocking.error()));
            }
            auto source = process::asset_loading::VfsAssetReadEndpoint::create(
                application.engine->assets().view(),
                *blocking,
                *application.asset_read_tasks,
                {16}
            );
            if (!source)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            application.source = std::move(*source);
            auto assets = rendering::RenderAssets::create(*application.rendering, application.source->port());
            if (!assets)
            {
                return cxx::unexpected(assets.error());
            }
            application.assets = std::move(*assets);
            auto atlas = application.window->uiRoot().fontAtlas();
            if (!atlas)
            {
                return cxx::unexpected(
                    error::Error{Errors::UiRenderConfiguration, {static_cast<std::uint64_t>(atlas.error())}}
                );
            }
            const auto& metrics = application.window->metrics();
            render::TargetDesc output{
                metrics.framebuffer_width,
                metrics.framebuffer_height,
                rdesc::ETextureFormat::RGBA8_UNORM,
                1,
                render::ETargetKind::SURFACE
            };
            auto scene = EditorUiScene::create(*application.rendering, output, surface, std::move(*atlas));
            if (!scene)
            {
                return cxx::unexpected(scene.error());
            }
            application.ui_scene = std::move(*scene);
            return impl;
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
                static_cast<void>(application_.engine->execution().requestStop(*project_prepare_));
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
                    application_.retire(std::move(candidate->context));
                }
            }
        }

        FrameworkResult<void> admitProject(
            LuxEngine& owner,
            std::filesystem::path file,
            std::optional<ProjectManifest> create
        ) noexcept
        {
            if (application_.window->shouldClose())
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
            auto scheduler = application_.engine->execution().blocking();
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
            auto submitted = project_tasks_->submit(
                {"Prepare project", "editor.project"},
                [scheduler = *scheduler,
                 destination = process::objectScheduler(owner),
                 owner = &owner,
                 serial,
                 file = std::move(file),
                 create = std::move(create),
                 locations = config_.plugin_locations](execution::TaskReporter reporter) mutable noexcept
                {
                    auto work =
                        stdexec::schedule(scheduler) |
                        stdexec::then([file, create = std::move(create), locations = std::move(locations), reporter](
                                      ) noexcept { return detail::prepareProject(file, create, locations, reporter); }
                        ) |
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
                    return stdexec::continues_on(std::move(work), destination) |
                           stdexec::then(
                               [owner, serial, task = reporter.id(), file = std::move(file)](
                                   detail::ProjectPreparation result
                               ) mutable noexcept
                               {
                                   const auto outcome =
                                       result.result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                                   detail::ProjectPrepared completion{serial, task, std::move(file), std::move(result)};
                                   static_cast<void>(object::sendEvent(*owner, completion));
                                   return outcome;
                               }
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

        void receivePrepared(LuxEngine& owner, detail::ProjectPrepared& event) noexcept
        {
            const auto serial = event.request_serial;
            if (serial != project_request_serial_ || application_.window->shouldClose())
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
            if (!composition.bindExtension(*application_.rendering) || !composition.bindExtension(*application_.assets))
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
            auto context = detail::createEditorContext(
                *application_.engine,
                std::move(*event.result.result),
                std::move(composition)
            );
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
                    application_.retire(std::move(candidate.context));
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

        void requestClose() noexcept
        {
            const auto serial = project_request_serial_ + 1;
            invalidatePreparation();
            if (serial == project_request_serial_)
            {
                pending_project_change_.emplace(CloseCurrentProject{serial});
            }
        }

        FrameworkResult<void> applyPendingProjectChanges(LuxEngine& owner) noexcept
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
                        application_.retire(std::move(candidate->context));
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
            auto& root = application_.window->uiRoot();
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
            if (old)
            {
                application_.retire(std::move(old->context));
            }
            old.reset();
            static_cast<void>(owner.emit(owner.projectChanged));
            return {};
        }

        FrameworkResult<ELoopState> pumpOnce(LuxEngine& owner) noexcept
        {
            if (pumping_)
            {
                return cxx::unexpected(error::Error{Errors::EditorRecursiveHostFrame, {}});
            }
            pumping_ = true;
            cxx::scope_exit finish([this]() noexcept { pumping_ = false; });
            window::LuxWindow::pollEvents();
            const auto& metrics = application_.window->metrics();
            if (metrics.revision != applied_metrics_revision_)
            {
                application_.ui_scene
                    ->setExtent(metrics.framebuffer_width, metrics.framebuffer_height, !metrics.minimized);
                applied_metrics_revision_ = metrics.revision;
            }
            application_.rendering->consumeResponses();
            if (application_.rendering->mainClient().closed())
            {
                return cxx::unexpected(render::RenderError{render::kOutcomeUnknown});
            }
            auto collected = application_.engine->execution().dispatchMainCompletions();
            if (!collected)
            {
                return cxx::unexpected(execution::toError(collected.error()));
            }
            auto dispatched = application_.engine->execution().dispatchTaskEvents();
            if (!dispatched)
            {
                return cxx::unexpected(execution::toError(dispatched.error()));
            }
            static_cast<void>(object::ObjectRuntime::instance().dispatchPending());
            const bool closing = application_.window->shouldClose();
            if (closing)
            {
                requestClose();
            }
            if (auto applied = applyPendingProjectChanges(owner); !applied)
            {
                return cxx::unexpected(applied.error());
            }
            auto& root = application_.window->uiRoot();
            if (closing)
            {
                application_.ui_scene->stopFrames();
            }
            if (!closing)
            {
                auto input = application_.window->sampleInput();
                if (!input)
                {
                    return cxx::unexpected(std::move(input.error()));
                }
            }
            const auto width = metrics.width, height = metrics.height;
            const auto pixels_x = metrics.framebuffer_width, pixels_y = metrics.framebuffer_height;
            const auto now = std::chrono::steady_clock::now();
            const bool has_extent = width && height && pixels_x && pixels_y;
            if (auto maintained = application_.ui_scene->maintain(); !maintained)
            {
                return cxx::unexpected(maintained.error());
            }
            auto output = application_.ui_scene->outputReady();
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            const bool can_draw = !closing && has_extent && !metrics.minimized && *output;
            application_.assets->maintain();
            auto& client = application_.rendering->mainClient();
            rendering::FrameInput* frame = nullptr;
            cxx::scope_exit release_unsent(
                [&]() noexcept
                {
                    if (frame && client.buildingFrame() == frame)
                    {
                        static_cast<void>(client.releaseFrame(*frame));
                    }
                }
            );
            const bool needs_frame = can_draw || client.hasQueuedFrameCommands();
            if (needs_frame && client.hasFrameCapacity())
            {
                auto input = client.beginFrame();
                if (!input)
                {
                    return cxx::unexpected(input.error());
                }
                frame = *input;
            }
            auto* ui_draw_data = can_draw && frame ? &application_.ui_scene->captureBuffer(*frame) : nullptr;
            const float elapsed =
                std::max(std::chrono::duration<float>(now - last_frame_).count(), std::numeric_limits<float>::min());
            ui::FrameInfo info{
                {float(width), float(height)},
                elapsed,
                {width ? float(pixels_x) / width : 1.F, height ? float(pixels_y) / height : 1.F}
            };
            auto drawn = ui_draw_data ? root.update(info, *ui_draw_data) : root.update();
            if (!drawn)
            {
                return cxx::unexpected(error::Error{Errors::UiCapture, {static_cast<std::uint64_t>(drawn.error())}});
            }
            if (ui_draw_data)
            {
                last_frame_ = now;
            }
            const auto consumed = root.inputSnapshot();
            application_.window->input().evaluate(
                elapsed,
                ui_draw_data && !consumed.keyboard_captured,
                ui_draw_data && !consumed.pointer_captured
            );
            auto driven = application_.engine->sceneRuntime().driveFrame();
            if (!driven)
            {
                return cxx::unexpected(scene::toError(driven.error()));
            }
            if (!driven->empty())
            {
                return cxx::unexpected(scene::toError(driven->front()));
            }
            application_.collectProjects();
            if (ui_draw_data)
            {
                auto appended = application_.ui_scene->appendFrameCommands(*frame);
                if (!appended)
                {
                    static_cast<void>(client.releaseFrame(*frame));
                    return cxx::unexpected(appended.error());
                }
            }
            if (frame)
            {
                if (auto sent = application_.submit(*frame); !sent)
                {
                    return cxx::unexpected(sent.error());
                }
            }
            (void)object::ObjectRuntime::instance().collectRetired();
            return closing ? ELoopState::EXIT_REQUESTED : ELoopState::RUNNING;
        }

        [[nodiscard]] FrameworkResult<bool> hasImmediateWork() noexcept
        {
            const auto& metrics = application_.window->metrics();
            const bool has_extent =
                metrics.width && metrics.height && metrics.framebuffer_width && metrics.framebuffer_height;
            auto output = application_.ui_scene->outputReady();
            if (!output)
            {
                return cxx::unexpected(output.error());
            }
            const bool can_produce = !application_.window->shouldClose() && has_extent && !metrics.minimized &&
                                     *output && application_.rendering->mainClient().hasFrameCapacity();
            return application_.window->shouldClose() || pending_project_change_.has_value() || can_produce ||
                   (application_.rendering->mainClient().hasQueuedFrameCommands() &&
                    application_.rendering->mainClient().hasFrameCapacity()) ||
                   application_.engine->execution().hasReadyWork() ||
                   object::ObjectRuntime::instance().statistics().pending != 0;
        }

        ApplicationResources application_;
        EditorConfig config_;
        EditorAssembly assembly_;
        std::optional<execution::TaskScope> project_tasks_;
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
        impl_->application_.window->uiRoot().setCommandFallback(this);
    }

    LuxEngine::~LuxEngine() noexcept
    {
        if (isDispatching()) // Final application barrier cannot run inside a completion callback.
        {
            std::terminate();
        }
        impl_->application_.window->uiRoot().setCommandFallback(nullptr);
        beginDestruction();
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

    FrameworkResult<void> LuxEngine::run() noexcept
    {
        for (;;)
        {
            auto state = pumpOnce();
            if (!state)
            {
                return cxx::unexpected(state.error());
            }
            if (*state == ELoopState::EXIT_REQUESTED)
            {
                impl_->application_.close();
                return {};
            }
            auto ready = impl_->hasImmediateWork();
            if (!ready)
            {
                return cxx::unexpected(ready.error());
            }
            if (!*ready)
            {
                window::LuxWindow::waitEvents();
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
        return *state == LuxEngine::ELoopState::RUNNING;
    }

    EditorWindow& LuxEngine::window() noexcept
    {
        return *impl_->application_.window;
    }

    engine::EngineContext& LuxEngine::engine() noexcept
    {
        return *impl_->application_.engine;
    }

    const engine::EngineContext& LuxEngine::engine() const noexcept
    {
        return *impl_->application_.engine;
    }

    rendering::RenderExecution& LuxEngine::rendering() noexcept
    {
        return *impl_->application_.rendering;
    }

    const rendering::RenderExecution& LuxEngine::rendering() const noexcept
    {
        return *impl_->application_.rendering;
    }

    rendering::RenderAssets& LuxEngine::renderAssets() noexcept
    {
        return *impl_->application_.assets;
    }

    FrameworkResult<void> LuxEngine::setUiImages(std::vector<UiTargetImage> images) noexcept
    {
        return impl_->application_.ui_scene->setTargetImages(std::move(images));
    }

    bool LuxEngine::uiImagesApplied() const noexcept
    {
        return impl_->application_.ui_scene->imagesApplied();
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
