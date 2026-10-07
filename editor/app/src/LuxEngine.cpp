#include <algorithm>
#include <chrono>
#include <exception>
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
            auto rendering =
                engine::initializeRendering(*engine_, window::LuxWindow::requiredVulkanInstanceExtensions());
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
            window::LuxWindow::pollEvents();
            auto collected = engine_->execution().collectCompletions();
            if (!collected)
            {
                return cxx::unexpected(executionError(collected.error()));
            }
            auto dispatched = engine_->execution().dispatchTaskEvents();
            if (!dispatched)
            {
                return cxx::unexpected(executionError(dispatched.error()));
            }
            (void)object::ObjectRuntime::instance().dispatchPending();
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
            const bool can_draw =
                !closing && has_extent && !window_->minimized() && now >= next_frame_ && ui_scene_->outputReady();
            auto* ui_draw_data = can_draw ? ui_scene_->acquireDrawData() : nullptr;
            const float elapsed = std::clamp(std::chrono::duration<float>(now - last_frame_).count(), 0.001F, 0.1F);
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
            if (ui_draw_data)
            {
                last_frame_ = now;
                next_frame_ = now + std::chrono::milliseconds(config_.frame_interval_ms);
            }
            const auto consumed = root.inputSnapshot();
            window_->input().evaluate(
                elapsed,
                ui_draw_data && !consumed.keyboard_captured,
                ui_draw_data && !consumed.pointer_captured
            );
            auto published = ui_scene_->publishFrame();
            if (!published)
            {
                return cxx::unexpected(std::move(published.error()));
            }
            auto driven = engine_->sceneRuntime().driveFrame();
            if (!driven)
            {
                return cxx::unexpected(scene::toError(driven.error()));
            }
            if (!driven->empty())
            {
                return cxx::unexpected(scene::toError(driven->front()));
            }
            (void)object::ObjectRuntime::instance().collectRetired();
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
                // Native input and Process/renderer completions wake this same wait. No worker polling loop.
                const auto now = std::chrono::steady_clock::now();
                const auto remaining = std::chrono::duration<double>(next_frame_ - now).count();
                const double delay = std::clamp(remaining, 0.001, 0.05);
                if (!engine_->execution().hasPendingWork())
                {
                    window::LuxWindow::waitEvents(delay);
                }
            }
        }
        EditorConfig config_;
        std::unique_ptr<EditorWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        std::unique_ptr<EditorUiScene> ui_scene_;
        std::unique_ptr<EditorContext> project_;
        std::chrono::steady_clock::time_point last_frame_{std::chrono::steady_clock::now()}, next_frame_{};
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
