#include <algorithm>
#include <chrono>
#include <exception>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorUIRoot.hpp>
#include <lux/engine/editor/EditorUiScene.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        template <class E> FrameworkFailure engineFailure(const char* operation, E error)
        {
            return {EFrameworkError::ENGINE, operation, 0, std::move(error)};
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
        explicit Impl(EditorConfig config, object::ObjectMessageQueue messages)
            : config_(std::move(config)), messages_(std::move(messages))
        {
        }
        ~Impl() noexcept
        {
            // Only mechanical project teardown here. The original Runtime drains retirement during its destruction.
            // Engine (and native output users) dies before EditorWindow by member declaration order.
            if (window_ && !window_->uiRoot().clearProjectUi())
            {
                std::terminate();
            }
        }
        FrameworkResult<void> initialize() noexcept
        {
            auto window =
                EditorWindow::create({config_.width, config_.height, config_.title}, messages_.dispatcherRef());
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
                return cxx::unexpected(engineFailure("engine.create", engine.error()));
            }
            engine_ = std::move(*engine);
            auto rendering =
                engine::initializeRendering(*engine_, window::LuxWindow::requiredVulkanInstanceExtensions());
            if (!rendering)
            {
                return cxx::unexpected(engineFailure("render.initialize", rendering.error()));
            }
            auto& context = *engine_->renderContext();
            auto features = context.registerFeatures({render::kUiRenderRenderFeatureRegistration});
            if (!features)
            {
                return cxx::unexpected(engineFailure("render.ui_feature", features.error()));
            }
            auto configuration = ui::makeRenderConfiguration(window_->uiRoot());
            if (!configuration)
            {
                return cxx::unexpected(engineFailure("ui.configuration", configuration.error()));
            }
            std::uint32_t width{}, height{};
            window_->framebufferSize(width, height);
#if defined(_WIN32)
            const auto native = reinterpret_cast<std::uintptr_t>(window_->nativeHandle());
            if (!native)
            {
                return cxx::unexpected(FrameworkFailure{EFrameworkError::WINDOW, "Window has no native output"});
            }
            scene::ViewConfig output{.extent = {width, height}, .output = scene::NativeSurfaceOutput{native}};
#else
            return cxx::unexpected(
                FrameworkFailure{EFrameworkError::WINDOW, "Native UI output is not implemented on this platform"}
            );
            scene::ViewConfig output;
#endif
            auto scene = EditorUiScene::create(
                engine_->execution(),
                engine_->sceneRuntime(),
                context.runtime(),
                context.resources(),
                std::move(*configuration),
                output
            );
            if (!scene)
            {
                return cxx::unexpected(std::move(scene.error()));
            }
            ui_scene_ = std::move(*scene);
            messages_.setWake(&window::LuxWindow::wakeEvents);
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
                return cxx::unexpected(FrameworkFailure{EFrameworkError::BUSY, "Project change inside a host operation"}
                );
            }
            auto description = validateProject(project);
            if (!description)
            {
                return description;
            }
            std::unordered_set<std::string> instances;
            for (const auto& ui : layout)
            {
                const bool is_invalid_ui = !ui.type.isValid() || ui.instance.empty();
                if (is_invalid_ui)
                {
                    return cxx::unexpected(FrameworkFailure{EFrameworkError::INVALID_DESCRIPTION, "Invalid layout item"}
                    );
                }
                if (!instances.insert(ui.instance).second)
                {
                    return cxx::unexpected(FrameworkFailure{EFrameworkError::DUPLICATE, "Duplicate UI instance name"});
                }
            }
            Operation guard{operating_};
            auto cleared = window_->uiRoot().clearProjectUi();
            if (!cleared)
            {
                return cleared;
            }
            project_.reset();
            // Local order matters on every error: candidates die before the Context they borrow.
            auto candidate = std::make_unique<EditorContext>(*engine_, messages_.dispatcherRef(), std::move(project));
            auto registered = assembly(*candidate);
            if (!registered)
            {
                return registered;
            }
            candidate->freeze();
            std::vector<std::unique_ptr<ui::Pane>> panes;
            panes.reserve(layout.size());
            for (const auto& item : layout)
            {
                auto pane = candidate->ui().create(*candidate, item);
                if (!pane)
                {
                    return cxx::unexpected(std::move(pane.error()));
                }
                const bool is_null = !*pane;
                const bool is_wrong_identity =
                    !is_null && ((*pane)->id().name() != item.instance || (*pane)->type().name() != item.type.name());
                if (is_null || is_wrong_identity)
                {
                    return cxx::unexpected(
                        FrameworkFailure{EFrameworkError::FACTORY_FAILED, "UI factory returned a wrong instance"}
                    );
                }
                panes.push_back(std::move(*pane));
            }
            auto mounted = window_->uiRoot().mountProjectUi(panes);
            if (!mounted)
            {
                return mounted;
            }
            project_ = std::move(candidate);
            return {};
        }
        FrameworkResult<void> closeProject() noexcept
        {
            if (operating_)
            {
                return cxx::unexpected(FrameworkFailure{EFrameworkError::BUSY, "Project change inside a host operation"}
                );
            }
            Operation guard{operating_};
            auto cleared = window_->uiRoot().clearProjectUi();
            if (!cleared)
            {
                return cleared;
            }
            project_.reset();
            return {};
        }
        FrameworkResult<bool> frame() noexcept
        {
            if (operating_)
            {
                return cxx::unexpected(FrameworkFailure{EFrameworkError::BUSY, "Recursive host frame"});
            }
            Operation guard{operating_};
            window::LuxWindow::pollEvents();
            auto collected = engine_->execution().collectCompletions();
            if (!collected)
            {
                return cxx::unexpected(engineFailure("process.collect", collected.error()));
            }
            auto dispatched = engine_->execution().dispatchTaskEvents();
            if (!dispatched)
            {
                return cxx::unexpected(engineFailure("process.dispatch", dispatched.error()));
            }
            (void)messages_.dispatchPending();
            auto& root = window_->uiRoot();
            root.applyPendingChanges();
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
            auto drawn = root.frame(info, ui_draw_data, capture);
            if (!drawn)
            {
                return cxx::unexpected(engineFailure("ui.frame", drawn.error()));
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
            auto published = ui_scene_->publishInput();
            if (!published)
            {
                return cxx::unexpected(std::move(published.error()));
            }
            auto driven = engine_->sceneRuntime().driveFrame();
            if (!driven)
            {
                return cxx::unexpected(engineFailure("scene.drive", driven.error()));
            }
            if (!driven->empty())
            {
                return cxx::unexpected(engineFailure("scene.failure", driven->front()));
            }
            (void)messages_.collectRetired();
            return !closing;
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
                if (!*running)
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
        object::ObjectMessageQueue messages_;
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
        const bool is_invalid_window = config.width <= 0 || config.height <= 0;
        if (is_invalid_window)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::INVALID_DESCRIPTION, "Invalid window extent"});
        }
        auto messages = object::ObjectMessageQueue::create(4096);
        if (!messages)
        {
            return cxx::unexpected(engineFailure("objects.create", messages.error()));
        }
        auto impl = std::make_unique<Impl>(std::move(config), std::move(*messages));
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
    FrameworkResult<bool> LuxEngine::frame() noexcept
    {
        return impl_->frame();
    }
    EditorWindow& LuxEngine::window() noexcept
    {
        return *impl_->window_;
    }
    EditorUIRoot& LuxEngine::uiRoot() noexcept
    {
        return impl_->window_->uiRoot();
    }
    engine::EngineContext& LuxEngine::engine() noexcept
    {
        return *impl_->engine_;
    }
    EditorContext* LuxEngine::context() noexcept
    {
        return impl_->project_.get();
    }
    std::uint64_t LuxEngine::capturedFrames() const noexcept
    {
        return impl_->ui_scene_->capturedFrames();
    }
} // namespace lux::editor
