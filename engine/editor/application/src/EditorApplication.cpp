#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/application/UiVulkanPresentation.hpp>

#include <lux/engine/ui/UiInputEvent.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <GLFW/glfw3.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace lux::editor
{
    namespace
    {
        [[nodiscard]] ui::EKey uiKey(int key) noexcept
        {
            switch (key)
            {
            case GLFW_KEY_TAB: return ui::EKey::TAB;
            case GLFW_KEY_ENTER: return ui::EKey::ENTER;
            case GLFW_KEY_ESCAPE: return ui::EKey::ESCAPE;
            case GLFW_KEY_SPACE: return ui::EKey::SPACE;
            case GLFW_KEY_BACKSPACE: return ui::EKey::BACKSPACE;
            case GLFW_KEY_DELETE: return ui::EKey::DELETE_KEY;
            case GLFW_KEY_LEFT: return ui::EKey::LEFT;
            case GLFW_KEY_RIGHT: return ui::EKey::RIGHT;
            case GLFW_KEY_UP: return ui::EKey::UP;
            case GLFW_KEY_DOWN: return ui::EKey::DOWN;
            case GLFW_KEY_HOME: return ui::EKey::HOME;
            case GLFW_KEY_END: return ui::EKey::END;
            default: return ui::EKey::NONE;
            }
        }

        [[nodiscard]] std::optional<ui::EPointerButton> pointerButton(int button) noexcept
        {
            switch (button)
            {
            case GLFW_MOUSE_BUTTON_LEFT: return ui::EPointerButton::LEFT;
            case GLFW_MOUSE_BUTTON_MIDDLE: return ui::EPointerButton::MIDDLE;
            case GLFW_MOUSE_BUTTON_RIGHT: return ui::EPointerButton::RIGHT;
            default: return std::nullopt;
            }
        }

        struct CloseResult final
        {
            std::atomic_bool complete{};
        };

        struct CloseReceiver final
        {
            using receiver_concept = stdexec::receiver_t;

            void set_value() && noexcept
            {
                result->complete.store(true, std::memory_order_release);
                result->complete.notify_all();
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept { return {}; }

            CloseResult* result{};
        };
    } // namespace

    struct EditorApplication::PresentationOwners final
    {
        std::unique_ptr<window::GlfwRuntime> window_runtime;
        std::unique_ptr<window::LuxWindow> window;
        std::unique_ptr<application::detail::UiVulkanPresentation> presenter;
    };

    EditorApplication::EditorApplication(
        process::ExecutionRuntime runtime,
        scene::SceneMetaManager scene_meta,
        std::optional<EditorPresentationConfig> presentation
    )
        : execution_(std::move(runtime)),
          scene_meta_(std::move(scene_meta)),
          presentation_config_(std::move(presentation))
    {
        ui_.emplace();
        selection_.emplace(ui_->dispatcherRef());
        toolset_.emplace();
        tasks_.emplace();
    }

    EditorApplication::CreateResult EditorApplication::create(EditorApplicationCreateInfo info) noexcept
    {
        auto runtime = process::ExecutionRuntime::create(info.execution);
        if (!runtime)
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_CREATE_FAILURE);

        std::unique_ptr<EditorApplication> application;
        try
        {
            application.reset(new EditorApplication(
                std::move(*runtime),
                std::move(info.scene_meta),
                std::move(info.presentation)
            ));
            for (auto& mount : info.mounts)
            {
                if (application->vfs_.mount(std::move(mount)) == asset::kInvalidMountId)
                {
                    static_cast<void>(application->shutdown());
                    return lux::cxx::unexpected(EEditorApplicationError::VFS_MOUNT_FAILURE);
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            if (application)
                static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::ALLOCATION_FAILURE);
        }

        auto blocking = application->execution_.blocking();
        if (!blocking)
        {
            static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_CREATE_FAILURE);
        }
        auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(
            application->vfs_.view(),
            *blocking,
            info.asset_read
        );
        if (!endpoint)
        {
            static_cast<void>(application->shutdown());
            return lux::cxx::unexpected(EEditorApplicationError::ASSET_READ_CREATE_FAILURE);
        }
        application->asset_read_endpoint_ = std::move(*endpoint);
        return application;
    }

    EditorApplication::~EditorApplication() noexcept
    {
        if (state_ != EState::JOINED)
            std::terminate();
    }

    lux::cxx::expected<void, EEditorApplicationError> EditorApplication::start() noexcept
    {
        if (state_ != EState::COMPOSING || !asset_read_endpoint_)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        if (presentation_config_)
        {
            const auto& config = *presentation_config_;
            const bool invalid_size = config.width == 0U || config.height == 0U ||
                config.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                config.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            const bool invalid_capacity = config.frame_capacity == 0U || config.control_capacity == 0U ||
                config.upload_capacity == 0U || config.upload_byte_capacity == 0U;
            if (invalid_size || invalid_capacity || config.title.empty())
                return lux::cxx::unexpected(EEditorApplicationError::PRESENTATION_CREATE_FAILURE);
            try
            {
                auto owners = std::make_unique<PresentationOwners>();
                owners->window_runtime = std::make_unique<window::GlfwRuntime>();
                if (!owners->window_runtime->valid())
                    return lux::cxx::unexpected(EEditorApplicationError::WINDOW_RUNTIME_CREATE_FAILURE);
                owners->window = std::make_unique<window::LuxWindow>(
                    static_cast<int>(config.width),
                    static_cast<int>(config.height),
                    config.title
                );
                if (!owners->window->isInitialized())
                    return lux::cxx::unexpected(EEditorApplicationError::WINDOW_CREATE_FAILURE);
                owners->window->hide(!config.visible);
                auto presenter = application::detail::UiVulkanPresentation::create(
                    *owners->window,
                    *ui_,
                    application::detail::UiVulkanPresentationConfig{
                        config.frame_capacity,
                        config.control_capacity,
                        config.upload_capacity,
                        config.upload_byte_capacity,
                        config.program_memory,
                        config.enable_validation
                    }
                );
                if (!presenter)
                    return lux::cxx::unexpected(EEditorApplicationError::PRESENTATION_CREATE_FAILURE);
                owners->presenter = std::move(*presenter);
                owners->window->on_cursor_move = [this](const window::CursorMoveEvent& event) {
                    ui_->feedInput(ui::UiPointerMove{{static_cast<float>(event.x), static_cast<float>(event.y)}});
                };
                owners->window->on_focus = [this](const window::WindowFocusEvent&) {
                    ui_->feedInput(ui::UiWindowFocus{true});
                };
                owners->window->on_lost_focus = [this](const window::WindowLostFocusEvent&) {
                    ui_->feedInput(ui::UiWindowFocus{false});
                };
                presentation_owners_ = std::move(owners);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EEditorApplicationError::ALLOCATION_FAILURE);
            }
        }
        toolset_->freeze();
        context_.emplace(EditorContextCreateInfo{
            *toolset_,
            vfs_.view(),
            asset_read_endpoint_->port(),
            execution_,
            *tasks_,
            *selection_,
            *ui_,
            scene_meta_
        });
        state_ = EState::RUNNING;
        return {};
    }

    EditorApplication::ContextResult EditorApplication::context() noexcept
    {
        if (state_ != EState::RUNNING || !context_)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        return std::ref(*context_);
    }

    lux::cxx::expected<std::size_t, EEditorApplicationError>
    EditorApplication::drainMain(std::size_t budget) noexcept
    {
        auto drained = execution_.drainMain(budget);
        if (!drained)
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
        return *drained;
    }

    void EditorApplication::feedWindowInput() noexcept
    {
        if (!presentation_owners_ || !presentation_owners_->window)
            return;
        for (const auto& event : presentation_owners_->window->drainInputEvents())
        {
            std::visit([this](const auto& value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, window::WindowKeyEvent>)
                {
                    const auto key = uiKey(value.key);
                    if (key != ui::EKey::NONE)
                        ui_->feedInput(ui::UiKey{key, value.action != GLFW_RELEASE});
                }
                else if constexpr (std::same_as<Value, window::WindowMouseButtonEvent>)
                {
                    const auto button = pointerButton(value.button);
                    if (button)
                        ui_->feedInput(ui::UiPointerButton{*button, value.action != GLFW_RELEASE});
                }
                else if constexpr (std::same_as<Value, window::WindowScrollEvent>)
                {
                    ui_->feedInput(ui::UiPointerWheel{{static_cast<float>(value.x), static_cast<float>(value.y)}});
                }
                else if constexpr (std::same_as<Value, window::WindowTextEvent>)
                {
                    ui_->feedInput(ui::UiText{static_cast<char32_t>(value.codepoint)});
                }
            }, event);
        }
    }

    lux::cxx::expected<std::size_t, EEditorApplicationError>
    EditorApplication::run(std::size_t max_frames) noexcept
    {
        if (state_ != EState::RUNNING || !presentation_owners_ || !presentation_owners_->window ||
            !presentation_owners_->presenter)
        {
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        }
        std::size_t frames{};
        auto previous = std::chrono::steady_clock::now();
        while (!presentation_owners_->window->shouldClose() && (max_frames == 0U || frames < max_frames))
        {
            window::LuxWindow::pollEvents();
            feedWindowInput();
            if (!drainMain(64U))
                return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);

            std::uint32_t width{};
            std::uint32_t height{};
            std::uint32_t framebuffer_width{};
            std::uint32_t framebuffer_height{};
            presentation_owners_->window->size(width, height);
            presentation_owners_->window->framebufferSize(framebuffer_width, framebuffer_height);
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<float>(now - previous).count();
            previous = now;
            const float delta = std::clamp(elapsed, 1.0F / 1000.0F, 0.1F);
            const float scale_x = width == 0U ? 1.0F : static_cast<float>(framebuffer_width) / width;
            const float scale_y = height == 0U ? 1.0F : static_cast<float>(framebuffer_height) / height;
            try
            {
                auto frame = ui_->beginFrame({
                    {static_cast<float>(width), static_cast<float>(height)},
                    delta,
                    {scale_x, scale_y}
                });
                frame.drawPanes();
                frame.finish();
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EEditorApplicationError::ALLOCATION_FAILURE);
            }
            if (!presentation_owners_->presenter->present(*ui_))
                return lux::cxx::unexpected(EEditorApplicationError::PRESENTATION_FRAME_FAILURE);
            ++frames;
            if (framebuffer_width == 0U || framebuffer_height == 0U)
                std::this_thread::sleep_for(std::chrono::milliseconds{16});
        }
        return frames;
    }

    void EditorApplication::clearWindowCallbacks() noexcept
    {
        if (!presentation_owners_ || !presentation_owners_->window)
            return;
        presentation_owners_->window->on_cursor_move = {};
        presentation_owners_->window->on_focus = {};
        presentation_owners_->window->on_lost_focus = {};
    }

    bool EditorApplication::closeRootTasks() noexcept
    {
        CloseResult result;
        try
        {
            auto operation = stdexec::connect(tasks_->close(), CloseReceiver{&result});
            stdexec::start(operation);
            while (!result.complete.load(std::memory_order_acquire))
            {
                if (!execution_.drainMain(64U))
                    return false;
                std::this_thread::yield();
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    lux::cxx::expected<void, EEditorApplicationError> EditorApplication::shutdown() noexcept
    {
        if (state_ == EState::JOINED)
            return lux::cxx::unexpected(EEditorApplicationError::INVALID_STATE);
        state_ = EState::STOPPING;
        if (!closeRootTasks())
            return lux::cxx::unexpected(EEditorApplicationError::TASK_CLOSE_FAILURE);

        if (presentation_owners_ && presentation_owners_->presenter)
        {
            presentation_owners_->presenter->requestStop();
            if (!presentation_owners_->presenter->join())
                return lux::cxx::unexpected(EEditorApplicationError::PRESENTATION_JOIN_FAILURE);
        }
        clearWindowCallbacks();

        context_.reset();
        tasks_.reset();
        toolset_->requestStop();
        toolset_.reset();
        selection_.reset();
        ui_.reset();
        presentation_owners_.reset();
        if (asset_read_endpoint_)
        {
            asset_read_endpoint_->requestStop();
            if (!asset_read_endpoint_->join())
                return lux::cxx::unexpected(EEditorApplicationError::ASSET_READ_JOIN_FAILURE);
            asset_read_endpoint_.reset();
        }

        execution_.requestStop();
        while (true)
        {
            auto drained = execution_.drainMain(64U);
            if (!drained)
                return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
            if (*drained == 0U)
                break;
        }
        if (!execution_.join())
            return lux::cxx::unexpected(EEditorApplicationError::EXECUTION_JOIN_FAILURE);
        state_ = EState::JOINED;
        return {};
    }
} // namespace lux::editor
