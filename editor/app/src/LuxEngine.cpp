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
        explicit Impl(EditorConfig config) : config_(std::move(config)) {}
        ~Impl() noexcept
        {
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
        FrameworkResult<void> openProject(
            ProjectDescription project,
            const EditorLayout& layout,
            Assembly assembly
        ) noexcept
        {
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectChangeInsideAHostOperation, {}});
            }
            auto description = validateProject(project);
            if (!description)
            {
                return description;
            }
            std::unordered_set<std::string> instances;
            for (const auto& ui : layout)
            {
                const bool is_invalid_ui = ui.type.empty() || ui.name.empty() || ui.type.find('\0') != ui.type.npos ||
                                           ui.name.find('\0') != ui.name.npos;
                if (is_invalid_ui)
                {
                    return cxx::unexpected(error::Error{Errors::EditorInvalidLayoutItem, {}});
                }
                if (!instances.insert(ui.name).second)
                {
                    return cxx::unexpected(error::Error{Errors::EditorDuplicateUiInstanceName, {}});
                }
            }
            Operation guard{operating_};
            auto cleared = window_->uiRoot().clearPanes();
            if (!cleared)
            {
                return cxx::unexpected(error::Error{
                    cleared.error() == ui::EPaneError::BUSY ? Errors::EditorUiClearBusy : Errors::EditorUiClear,
                    {static_cast<std::uint64_t>(cleared.error())}
                });
            }
            project_.reset();
            // Local order matters on every error: candidates die before the Context they borrow.
            auto created_context = EditorContext::create(*engine_, std::move(project), assembly);
            if (!created_context)
            {
                return cxx::unexpected(created_context.error());
            }
            auto candidate = std::move(*created_context);
            std::vector<std::unique_ptr<ui::Pane>> panes;
            panes.reserve(layout.size());
            for (const auto& item : layout)
            {
                auto pane = createPane(*candidate, item);
                if (!pane)
                {
                    return cxx::unexpected(std::move(pane.error()));
                }
                if (!*pane || (*pane)->attachedRoot() || (*pane)->parent())
                {
                    return cxx::unexpected(error::Error{Errors::EditorUiFactoryReturnedAnAttachedOrNullPane, {}});
                }
                panes.push_back(std::move(*pane));
            }
            auto mounted = window_->uiRoot().addPanes(panes);
            if (!mounted)
            {
                return cxx::unexpected(
                    error::Error{Errors::EditorCannotMountProjectWindows, {static_cast<std::uint64_t>(mounted.error())}}
                );
            }
            project_ = std::move(candidate);
            return {};
        }
        FrameworkResult<void> closeProject() noexcept
        {
            if (operating_)
            {
                return cxx::unexpected(error::Error{Errors::EditorProjectChangeInsideAHostOperation, {}});
            }
            Operation guard{operating_};
            auto cleared = window_->uiRoot().clearPanes();
            if (!cleared)
            {
                return cxx::unexpected(error::Error{
                    cleared.error() == ui::EPaneError::BUSY ? Errors::EditorUiClearBusy : Errors::EditorUiClear,
                    {static_cast<std::uint64_t>(cleared.error())}
                });
            }
            project_.reset();
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
            auto& root = window_->uiRoot();
            const bool closing = window_->shouldClose();
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
                const bool can_produce = visible_output && *output && !backpressured;
                const bool ready = window_->shouldClose() || can_produce || engine_->execution().hasPendingWork() ||
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
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::unique_ptr<EditorUiScene> ui_scene_;
        std::unique_ptr<EditorContext> project_;
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()};
        bool operating_{};
    };
    LuxEngine::LuxEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    LuxEngine::~LuxEngine() noexcept = default;
    FrameworkResult<std::unique_ptr<LuxEngine>> LuxEngine::create(EditorConfig config) noexcept
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
        auto impl = std::make_unique<Impl>(std::move(config));
        auto initialized = impl->initialize();
        if (!initialized)
        {
            return cxx::unexpected(std::move(initialized.error()));
        }
        return std::unique_ptr<LuxEngine>{new LuxEngine(std::move(impl))};
    }
    FrameworkResult<void> LuxEngine::openProject(
        ProjectDescription project,
        const EditorLayout& layout,
        Assembly assembly
    ) noexcept
    {
        return impl_->openProject(std::move(project), layout, assembly);
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
