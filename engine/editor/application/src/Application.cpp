#include <lux/engine/editor/application/Application.hpp>
#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
namespace lux::editor::application
{
    namespace
    {
        auto fail(EApplicationError code) noexcept
        {
            return lux::cxx::unexpected(ApplicationFailure{code});
        }
        auto windowFailure(ui::WindowFailure source) noexcept
        {
            ApplicationFailure error{EApplicationError::WINDOW_FAILURE};
            error.window = source;
            return lux::cxx::unexpected(error);
        }
        auto sceneFailure(sessions::SceneFailure source) noexcept
        {
            ApplicationFailure error{EApplicationError::SCENE_FAILURE};
            error.scene = source;
            return lux::cxx::unexpected(error);
        }
        auto renderFailure(rendering::RendererFailure source) noexcept
        {
            ApplicationFailure error{EApplicationError::RENDERER_FAILURE};
            error.renderer = source;
            return lux::cxx::unexpected(error);
        }
        auto workspaceFailure(const ui::SceneWorkspaceFailure &source) noexcept
        {
            if (source.scene)
                return sceneFailure(*source.scene);
            if (source.window)
                return windowFailure(*source.window);
            return fail(EApplicationError::INVALID_STATE);
        }
        auto processFailure(lux::process::EExecutionError source) noexcept
        {
            ApplicationFailure error{EApplicationError::PROCESS_FAILURE};
            error.execution = source;
            return lux::cxx::unexpected(error);
        }
        auto assetFailure(lux::process::asset_loading::EVfsAssetReadEndpointError source) noexcept
        {
            ApplicationFailure error{EApplicationError::PROCESS_FAILURE};
            error.assets = source;
            return lux::cxx::unexpected(error);
        }
        struct Gate final
        {
            bool &busy;
            ~Gate() noexcept
            {
                busy = false;
            }
        };
    } // namespace
    struct EditorApplication::Impl final
    {
        explicit Impl(const EditorApplicationCreateInfo &input) : config(input)
        {
        }
        const std::thread::id owner{std::this_thread::get_id()};
        EditorApplicationCreateInfo config;
        std::optional<Toolset> tools{std::in_place};
        EApplicationState state{EApplicationState::COMPOSING};
        bool busy{}, close_requested{};
        std::uint64_t cycle{}, next_session{1};
        std::optional<lux::object::ObjectMessageQueue> messages;
        std::unique_ptr<lux::window::GlfwRuntime> platform;
        std::optional<lux::process::ExecutionRuntime> execution;
        lux::asset::AssetVfs vfs;
        std::shared_ptr<lux::process::asset_loading::VfsAssetReadEndpoint> assets;
        std::unique_ptr<ui::EditorWindow> window;
        std::unique_ptr<rendering::EditorRenderer> renderer;
        std::optional<sessions::SceneOpenInfo> source;
        std::unique_ptr<sessions::SceneSession> session;
        std::unique_ptr<sessions::SceneView> unattached_view;
        std::unique_ptr<ui::SceneWorkspace> workspace;
        rendering::EditorFramePacket pending_frame;
        rendering::RendererStatistics final_statistics;
        ApplicationResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return fail(EApplicationError::WRONG_THREAD);
            if (busy)
                return fail(EApplicationError::BUSY);
            return {};
        }
    };
    EditorApplication::EditorApplication(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }
    EditorApplication::~EditorApplication() noexcept
    {
        if (impl_->state != EApplicationState::STOPPED && impl_->state != EApplicationState::COMPOSING)
            std::terminate();
    }
    ApplicationResult<std::unique_ptr<EditorApplication>> EditorApplication::create(
        EditorApplicationCreateInfo &input) noexcept
    {
        if (!input.source || !input.metadata)
            return fail(EApplicationError::INVALID_ARGUMENT);
        try
        {
            auto impl = std::make_unique<Impl>(input);
            return std::unique_ptr<EditorApplication>(new EditorApplication(std::move(impl)));
        }
        catch (const std::bad_alloc &)
        {
            return fail(EApplicationError::ALLOCATION_FAILURE);
        }
    }
    EApplicationState EditorApplication::state() const noexcept
    {
        return impl_->state;
    }
    ApplicationResult<std::reference_wrapper<Toolset>> EditorApplication::tooling() noexcept
    {
        if (auto checked = impl_->check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (impl_->state != EApplicationState::COMPOSING || impl_->close_requested)
            return fail(EApplicationError::INVALID_STATE);
        return std::ref(*impl_->tools);
    }
    ApplicationResult<void> EditorApplication::start() noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->state != EApplicationState::COMPOSING || impl_->close_requested)
            return fail(EApplicationError::INVALID_STATE);
        impl_->busy = true;
        Gate gate{impl_->busy};
        impl_->tools->freeze();
        impl_->state = EApplicationState::STARTING;
        struct StartState final
        {
            EApplicationState &state;
            ~StartState() noexcept
            {
                if (state == EApplicationState::STARTING)
                    state = EApplicationState::START_FAILED;
            }
        } start_state{impl_->state};
        try
        {
            impl_->messages.emplace();
            impl_->platform = std::make_unique<lux::window::GlfwRuntime>();
            if (!impl_->platform->valid())
                return fail(EApplicationError::WINDOW_FAILURE);
            auto execution = lux::process::ExecutionRuntime::create(impl_->config.execution);
            if (!execution)
                return processFailure(execution.error());
            impl_->execution.emplace(std::move(*execution));
            for (const auto &mount : impl_->config.mounts)
                if (impl_->vfs.mount(mount) == lux::asset::kInvalidMountId)
                    return fail(EApplicationError::START_FAILURE);
            auto blocking = impl_->execution->blocking();
            if (!blocking)
                return processFailure(blocking.error());
            auto assets = lux::process::asset_loading::VfsAssetReadEndpoint::create(impl_->vfs.view(), *blocking,
                                                                                    impl_->config.asset_read);
            if (!assets)
                return assetFailure(assets.error());
            impl_->assets = std::move(*assets);
            auto window = ui::EditorWindow::create(impl_->messages->dispatcherRef(), impl_->config.window);
            if (!window)
                return windowFailure(window.error());
            impl_->window = std::move(*window);
            auto renderer = rendering::EditorRenderer::create(impl_->window->nativeWindow(), impl_->window->uiSession(),
                                                              impl_->config.renderer);
            if (!renderer)
                return renderFailure(renderer.error());
            impl_->renderer = std::move(*renderer);
            auto source = impl_->config.source(sessions::SessionId{impl_->next_session++},
                                               impl_->window->uiSession().dispatcherRef(), *impl_->renderer,
                                               impl_->assets->port(), impl_->config.metadata);
            if (!source)
                return sceneFailure(source.error());
            impl_->source.emplace(std::move(*source));
            auto session = sessions::SceneSession::openInspection(*impl_->source);
            if (!session)
                return sceneFailure(session.error());
            impl_->session = std::move(*session);
            impl_->source.reset();
            auto view = sessions::SceneView::create(impl_->window->uiSession().dispatcherRef(), *impl_->session,
                                                    *impl_->renderer);
            if (!view)
                return sceneFailure(view.error());
            impl_->unattached_view = std::move(*view);
            auto workspace = ui::SceneWorkspace::create(*impl_->window, {1}, *impl_->session, impl_->unattached_view);
            if (!workspace)
                return windowFailure(workspace.error());
            impl_->workspace = std::move(*workspace);
            auto activated = impl_->workspace->activate();
            if (!activated)
                return windowFailure(activated.error());
            impl_->state = EApplicationState::RUNNING;
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return fail(EApplicationError::ALLOCATION_FAILURE);
        }
    }
    ApplicationResult<void> EditorApplication::requestClose() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(EApplicationError::WRONG_THREAD);
        if (impl_->state == EApplicationState::STOPPED)
            return {};
        impl_->close_requested = true;
        return {};
    }
    ApplicationResult<std::size_t> EditorApplication::run(std::size_t max_frames) noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        if (impl_->state != EApplicationState::RUNNING)
            return fail(EApplicationError::INVALID_STATE);
        impl_->busy = true;
        Gate gate{impl_->busy};
        std::size_t frames{};
        auto previous = std::chrono::steady_clock::now();
        while (!impl_->close_requested)
        {
            auto input = impl_->window->collectInput();
            if (!input)
                return windowFailure(input.error());
            if (impl_->window->closeRequested() || (max_frames && frames >= max_frames))
            {
                impl_->close_requested = true;
                break;
            }
            auto drained = impl_->execution->drainMain(64);
            if (!drained)
                return processFailure(drained.error());
            auto polled = impl_->renderer->poll(64);
            if (!polled)
                return renderFailure(polled.error());
            auto diagnostic = impl_->renderer->takeDiagnostic();
            if (!diagnostic)
                return renderFailure(diagnostic.error());
            if (*diagnostic)
            {
                ApplicationFailure error{EApplicationError::RENDERER_FAILURE};
                error.renderer = (**diagnostic).failure;
                error.renderer_diagnostic = **diagnostic;
                return lux::cxx::unexpected(error);
            }
            if (impl_->pending_frame.valid())
            {
                auto submitted = impl_->renderer->trySubmitFrame(impl_->pending_frame);
                if (!submitted)
                    return renderFailure(submitted.error());
            }
            if (impl_->cycle == (std::numeric_limits<std::uint64_t>::max)())
                return fail(EApplicationError::INVALID_STATE);
            const auto now = std::chrono::steady_clock::now();
            const auto delta = std::clamp(std::chrono::duration<double>(now - previous).count(), 0.000001, 0.1);
            previous = now;
            const sessions::SceneOwnerUpdate update{++impl_->cycle, delta};
            auto updated = impl_->session->updateAtOwnerSafePoint(update);
            if (!updated)
                return sceneFailure(updated.error());
            auto synchronized = impl_->workspace->updateBeforeFrame();
            if (!synchronized)
                return workspaceFailure(synchronized.error());
            std::uint32_t width{}, height{}, pixel_width{}, pixel_height{};
            impl_->window->nativeWindow().size(width, height);
            impl_->window->nativeWindow().framebufferSize(pixel_width, pixel_height);
            const bool minimized = !pixel_width || !pixel_height;
            const lux::ui::Vec2 scale{width && pixel_width ? static_cast<float>(pixel_width) / width : 1,
                                      height && pixel_height ? static_cast<float>(pixel_height) / height : 1};
            auto began = impl_->window->beginFrame(
                {{minimized ? 0.0F : static_cast<float>(width), minimized ? 0.0F : static_cast<float>(height)},
                 static_cast<float>(delta),
                 scale});
            if (!began)
                return windowFailure(began.error());
            auto drawn = impl_->window->drawPanes();
            if (!drawn)
                return windowFailure(drawn.error());
            auto applied = impl_->workspace->afterDraw(delta, scale);
            if (!applied)
                return windowFailure(applied.error());
            auto advanced = impl_->session->advanceScene(update);
            if (!advanced)
                return sceneFailure(advanced.error());
            auto snapshot = impl_->window->finishFrame();
            if (!snapshot)
                return windowFailure(snapshot.error());
            if (!impl_->session->presentationPending())
            {
                auto packet = impl_->renderer->sealFrame(*snapshot, impl_->workspace->frameImages());
                if (!packet)
                    return renderFailure(packet.error());
                // Explicitly cancel the previous unaccepted image, without replaying any UI or Scene operation.
                impl_->pending_frame = std::move(*packet);
                auto submitted = impl_->renderer->trySubmitFrame(impl_->pending_frame);
                if (!submitted)
                    return renderFailure(submitted.error());
            }
            impl_->workspace->releaseFrameImages();
            ++frames;
            // Pace owner/UI work independently of GPU completion; never waitIdle to regulate frames.
            // Overruns do not accumulate a catch-up queue. Minimized owners still advance pending close/resource work.
            std::this_thread::sleep_until(now + std::chrono::milliseconds{minimized ? 16 : 8});
        }
        impl_->state = EApplicationState::CLOSE_REQUESTED;
        return frames;
    }
    ApplicationResult<bool> EditorApplication::advanceShutdown(std::size_t budget) noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        if (impl_->state == EApplicationState::STOPPED)
            return true;
        if (!impl_->close_requested)
            return fail(EApplicationError::INVALID_STATE);
        if (!budget)
            return false;
        impl_->busy = true;
        Gate gate{impl_->busy};
        if (impl_->execution)
        {
            auto result = impl_->execution->drainMain(budget);
            if (!result)
                return processFailure(result.error());
        }
        if (impl_->renderer)
        {
            auto result = impl_->renderer->poll(budget);
            if (!result)
                return renderFailure(result.error());
        }
        impl_->pending_frame = {};
        if (impl_->window && impl_->window->frameOpen())
        {
            auto result = impl_->window->discardFrame();
            if (!result)
                return windowFailure(result.error());
        }
        if (impl_->workspace)
        {
            impl_->state = EApplicationState::DETACHING_UI;
            auto began = impl_->workspace->beginClose();
            if (!began)
                return windowFailure(began.error());
            auto closed = impl_->workspace->advanceClose();
            if (!closed)
                return workspaceFailure(closed.error());
            if (*closed == sessions::ECloseProgress::PENDING)
            {
                impl_->state = EApplicationState::CLOSING_VIEWS;
                return false;
            }
            impl_->workspace.reset();
        }
        if (impl_->unattached_view)
        {
            impl_->state = EApplicationState::CLOSING_VIEWS;
            auto began = impl_->unattached_view->beginClose();
            if (!began)
                return sceneFailure(began.error());
            auto closed = impl_->unattached_view->advanceClose();
            if (!closed)
                return sceneFailure(closed.error());
            if (*closed == sessions::ECloseProgress::PENDING)
                return false;
            impl_->unattached_view.reset();
        }
        if (impl_->session)
        {
            impl_->state = EApplicationState::CLOSING_SESSIONS;
            auto began = impl_->session->beginClose();
            if (!began)
                return sceneFailure(began.error());
            auto closed = impl_->session->advanceClose();
            if (!closed)
                return sceneFailure(closed.error());
            if (*closed == sessions::ECloseProgress::PENDING)
                return false;
            impl_->session.reset();
        }
        impl_->source.reset();
        if (impl_->renderer)
        {
            impl_->state = EApplicationState::DRAINING_RENDERER;
            auto began = impl_->renderer->beginClose();
            if (!began)
                return renderFailure(began.error());
            auto closed = impl_->renderer->advanceClose();
            if (!closed)
                return renderFailure(closed.error());
            if (*closed == rendering::ERenderClose::PENDING)
                return false;
            auto joined = impl_->renderer->joinStopped();
            if (!joined)
                return renderFailure(joined.error());
            impl_->final_statistics = impl_->renderer->statistics();
            impl_->renderer.reset();
        }
        if (impl_->window)
        {
            auto result = impl_->window->closeAfterRendererStopped();
            if (!result)
                return windowFailure(result.error());
            impl_->window.reset();
        }
        if (impl_->tools)
        {
            impl_->tools->requestStop();
            impl_->tools.reset();
        }
        if (impl_->assets)
        {
            // Every Session child scope has completed before these terminal joins.
            impl_->assets->requestStop();
            auto joined = impl_->assets->join();
            if (!joined)
                return assetFailure(joined.error());
            impl_->assets.reset();
        }
        if (impl_->execution)
        {
            impl_->execution->requestStop();
            auto joined = impl_->execution->join();
            if (!joined)
                return processFailure(joined.error());
            impl_->execution.reset();
        }
        impl_->messages.reset();
        impl_->platform.reset();
        impl_->config.metadata.reset();
        impl_->state = EApplicationState::STOPPED;
        return true;
    }
    rendering::RendererStatistics EditorApplication::rendererStatistics() const noexcept
    {
        return impl_->renderer ? impl_->renderer->statistics() : impl_->final_statistics;
    }
} // namespace lux::editor::application
