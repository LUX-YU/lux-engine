#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/ui/UiInputEvent.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

namespace lux::editor::ui
{
    namespace
    {
        [[nodiscard]] lux::ui::EKey uiKey(int key) noexcept
        {
            switch (key)
            {
            case GLFW_KEY_A:
                return lux::ui::EKey::A;
            case GLFW_KEY_B:
                return lux::ui::EKey::B;
            case GLFW_KEY_C:
                return lux::ui::EKey::C;
            case GLFW_KEY_D:
                return lux::ui::EKey::D;
            case GLFW_KEY_E:
                return lux::ui::EKey::E;
            case GLFW_KEY_F:
                return lux::ui::EKey::F;
            case GLFW_KEY_G:
                return lux::ui::EKey::G;
            case GLFW_KEY_H:
                return lux::ui::EKey::H;
            case GLFW_KEY_I:
                return lux::ui::EKey::I;
            case GLFW_KEY_J:
                return lux::ui::EKey::J;
            case GLFW_KEY_K:
                return lux::ui::EKey::K;
            case GLFW_KEY_L:
                return lux::ui::EKey::L;
            case GLFW_KEY_M:
                return lux::ui::EKey::M;
            case GLFW_KEY_N:
                return lux::ui::EKey::N;
            case GLFW_KEY_O:
                return lux::ui::EKey::O;
            case GLFW_KEY_P:
                return lux::ui::EKey::P;
            case GLFW_KEY_Q:
                return lux::ui::EKey::Q;
            case GLFW_KEY_R:
                return lux::ui::EKey::R;
            case GLFW_KEY_S:
                return lux::ui::EKey::S;
            case GLFW_KEY_T:
                return lux::ui::EKey::T;
            case GLFW_KEY_U:
                return lux::ui::EKey::U;
            case GLFW_KEY_V:
                return lux::ui::EKey::V;
            case GLFW_KEY_W:
                return lux::ui::EKey::W;
            case GLFW_KEY_X:
                return lux::ui::EKey::X;
            case GLFW_KEY_Y:
                return lux::ui::EKey::Y;
            case GLFW_KEY_Z:
                return lux::ui::EKey::Z;
            case GLFW_KEY_LEFT_SHIFT:
                return lux::ui::EKey::LEFT_SHIFT;
            case GLFW_KEY_RIGHT_SHIFT:
                return lux::ui::EKey::RIGHT_SHIFT;
            case GLFW_KEY_LEFT_CONTROL:
                return lux::ui::EKey::LEFT_CONTROL;
            case GLFW_KEY_RIGHT_CONTROL:
                return lux::ui::EKey::RIGHT_CONTROL;
            case GLFW_KEY_LEFT_ALT:
                return lux::ui::EKey::LEFT_ALT;
            case GLFW_KEY_RIGHT_ALT:
                return lux::ui::EKey::RIGHT_ALT;
            case GLFW_KEY_TAB:
                return lux::ui::EKey::TAB;
            case GLFW_KEY_ENTER:
                return lux::ui::EKey::ENTER;
            case GLFW_KEY_ESCAPE:
                return lux::ui::EKey::ESCAPE;
            case GLFW_KEY_SPACE:
                return lux::ui::EKey::SPACE;
            case GLFW_KEY_BACKSPACE:
                return lux::ui::EKey::BACKSPACE;
            case GLFW_KEY_DELETE:
                return lux::ui::EKey::DELETE_KEY;
            case GLFW_KEY_LEFT:
                return lux::ui::EKey::LEFT;
            case GLFW_KEY_RIGHT:
                return lux::ui::EKey::RIGHT;
            case GLFW_KEY_UP:
                return lux::ui::EKey::UP;
            case GLFW_KEY_DOWN:
                return lux::ui::EKey::DOWN;
            case GLFW_KEY_HOME:
                return lux::ui::EKey::HOME;
            case GLFW_KEY_END:
                return lux::ui::EKey::END;
            default:
                return lux::ui::EKey::NONE;
            }
        }

        [[nodiscard]] std::optional<lux::ui::EPointerButton> pointerButton(int button) noexcept
        {
            switch (button)
            {
            case GLFW_MOUSE_BUTTON_LEFT:
                return lux::ui::EPointerButton::LEFT;
            case GLFW_MOUSE_BUTTON_MIDDLE:
                return lux::ui::EPointerButton::MIDDLE;
            case GLFW_MOUSE_BUTTON_RIGHT:
                return lux::ui::EPointerButton::RIGHT;
            default:
                return std::nullopt;
            }
        }

        auto fail(EWindowError code) noexcept
        {
            return lux::cxx::unexpected(WindowFailure{code});
        }
    } // namespace

    struct EditorWindow::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        std::unique_ptr<lux::window::LuxWindow> window;
        std::unique_ptr<lux::ui::UISession> ui;
        std::unique_ptr<ActiveEditHistory> histories;
        std::optional<lux::ui::Frame> frame;
        bool drawing{}, entering{}, close_requested{}, closed{};

        WindowResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return fail(EWindowError::WRONG_THREAD);
            if (closed)
                return fail(EWindowError::CLOSED);
            if (entering)
                return fail(EWindowError::BUSY);
            return {};
        }
    };

    EditorWindow::EditorWindow(lux::object::ObjectDispatcherRef dispatcher, std::unique_ptr<Impl> impl)
        : Object(std::move(dispatcher)), impl_(std::move(impl))
    {
    }
    EditorWindow::~EditorWindow() noexcept
    {
        if (!impl_->closed)
            std::terminate();
    }

    WindowResult<std::unique_ptr<EditorWindow>> EditorWindow::create(lux::object::ObjectDispatcherRef dispatcher,
                                                                     const WindowSpec &spec) noexcept
    {
        if (!dispatcher || !dispatcher.isCurrent())
            return fail(EWindowError::WRONG_THREAD);
        const bool valid_size = spec.width && spec.height && spec.width <= INT_MAX && spec.height <= INT_MAX;
        if (!valid_size || spec.title.empty())
            return fail(EWindowError::INVALID_ARGUMENT);
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->ui = std::make_unique<lux::ui::UISession>();
            auto &commands = impl->ui->commandRouter();
            if (!commands.defineCommand({lux::ui::UiCommandId{"lux.edit.undo"}, "Undo"}) ||
                !commands.defineCommand({lux::ui::UiCommandId{"lux.edit.redo"}, "Redo"}))
                return fail(EWindowError::UI_FAILURE);
            impl->window = std::make_unique<lux::window::LuxWindow>(static_cast<int>(spec.width),
                                                                    static_cast<int>(spec.height), spec.title);
            if (!impl->window->isInitialized())
                return fail(EWindowError::PLATFORM_FAILURE);
            auto histories = ActiveEditHistory::create(32);
            if (!histories)
                return fail(EWindowError::ALLOCATION_FAILURE);
            impl->histories = std::move(*histories);
            impl->window->hide(!spec.visible);
            // The callback receiver is this Window's stable, exclusively owned input state.
            auto *input = impl->ui.get();
            impl->window->on_cursor_move = [input](const lux::window::CursorMoveEvent &event)
            { input->feedInput(lux::ui::UiPointerMove{{static_cast<float>(event.x), static_cast<float>(event.y)}}); };
            impl->window->on_focus = [input](const lux::window::WindowFocusEvent &)
            { input->feedInput(lux::ui::UiWindowFocus{true}); };
            impl->window->on_lost_focus = [input](const lux::window::WindowLostFocusEvent &)
            { input->feedInput(lux::ui::UiWindowFocus{false}); };
            return std::unique_ptr<EditorWindow>(new EditorWindow(std::move(dispatcher), std::move(impl)));
        }
        catch (const std::bad_alloc &)
        {
            return fail(EWindowError::ALLOCATION_FAILURE);
        }
    }

    lux::ui::UISession &EditorWindow::uiSession() noexcept
    {
        return *impl_->ui;
    }
    lux::window::LuxWindow &EditorWindow::nativeWindow() noexcept
    {
        return *impl_->window;
    }
    ActiveEditHistory &EditorWindow::activeHistory() noexcept
    {
        return *impl_->histories;
    }
    bool EditorWindow::frameOpen() const noexcept
    {
        return impl_->frame.has_value();
    }
    bool EditorWindow::closeRequested() const noexcept
    {
        return impl_->close_requested;
    }

    WindowResult<void> EditorWindow::collectInput() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (impl_->frame)
            return fail(EWindowError::BUSY);
        lux::window::LuxWindow::pollEvents();
        for (const auto &event : impl_->window->drainInputEvents())
        {
            std::visit(
                [this](const auto &value)
                {
                    using Value = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Value, window::WindowKeyEvent>)
                    {
                        const auto key = uiKey(value.key);
                        if (key != lux::ui::EKey::NONE)
                            impl_->ui->feedInput(lux::ui::UiKey{key, value.action != GLFW_RELEASE});
                    }
                    else if constexpr (std::same_as<Value, window::WindowMouseButtonEvent>)
                    {
                        const auto button = pointerButton(value.button);
                        if (button)
                            impl_->ui->feedInput(lux::ui::UiPointerButton{*button, value.action != GLFW_RELEASE});
                    }
                    else if constexpr (std::same_as<Value, window::WindowScrollEvent>)
                    {
                        impl_->ui->feedInput(
                            lux::ui::UiPointerWheel{{static_cast<float>(value.x), static_cast<float>(value.y)}});
                    }
                    else if constexpr (std::same_as<Value, window::WindowTextEvent>)
                    {
                        impl_->ui->feedInput(lux::ui::UiText{static_cast<char32_t>(value.codepoint)});
                    }
                },
                event);
        }
        impl_->close_requested |= impl_->window->shouldClose();
        return {};
    }

    WindowResult<void> EditorWindow::beginFrame(const lux::ui::FrameInfo &info) noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (impl_->frame)
            return fail(EWindowError::BUSY);
        const bool valid_size = std::isfinite(info.display_size.width) && info.display_size.width >= 0 &&
                                std::isfinite(info.display_size.height) && info.display_size.height >= 0;
        const bool valid_scale = std::isfinite(info.framebuffer_scale.x) && info.framebuffer_scale.x > 0 &&
                                 std::isfinite(info.framebuffer_scale.y) && info.framebuffer_scale.y > 0;
        if (!valid_size || !valid_scale || !std::isfinite(info.delta_seconds) || info.delta_seconds <= 0)
            return fail(EWindowError::INVALID_ARGUMENT);
        struct EntryGate final
        {
            bool &entering;
            ~EntryGate() noexcept
            {
                entering = false;
            }
        } gate{impl_->entering};
        impl_->entering = true;
        try
        {
            impl_->frame.emplace(impl_->ui->beginFrame(info));
        }
        catch (const std::bad_alloc &)
        {
            return fail(EWindowError::ALLOCATION_FAILURE);
        }
        return {};
    }
    WindowResult<void> EditorWindow::drawPanes() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (!impl_->frame || impl_->drawing)
            return fail(EWindowError::BUSY);
        impl_->drawing = true;
        try
        {
            impl_->frame->drawPanes();
            // Route once after current-frame focus and text ownership have been established.
            const auto input = impl_->ui->inputSnapshot();
            const bool ctrl = input.held[static_cast<std::size_t>(lux::ui::EKey::LEFT_CONTROL)] ||
                              input.held[static_cast<std::size_t>(lux::ui::EKey::RIGHT_CONTROL)];
            const bool accepts_shortcut = input.window_focused && !input.keyboard_blocked && !input.modal_open && ctrl;
            if (accepts_shortcut)
            {
                auto &router = impl_->ui->commandRouter();
                const auto invoke = [&](lux::ui::EKey key, lux::ui::UiCommandIdView id)
                {
                    if (input.pressed[static_cast<std::size_t>(key)])
                        if (const auto command = router.findCommand(id))
                            static_cast<void>(router.invoke(*command));
                };
                invoke(lux::ui::EKey::Z, lux::ui::UiCommandIdView{"lux.edit.undo"});
                invoke(lux::ui::EKey::Y, lux::ui::UiCommandIdView{"lux.edit.redo"});
            }
        }
        catch (const std::bad_alloc &)
        {
            impl_->drawing = false;
            static_cast<void>(discardFrame());
            return fail(EWindowError::ALLOCATION_FAILURE);
        }
        impl_->drawing = false;
        return {};
    }
    WindowResult<lux::ui::UiFrameSnapshot> EditorWindow::finishFrame() noexcept
    {
        if (auto check = impl_->check(); !check)
            return lux::cxx::unexpected(check.error());
        if (!impl_->frame || impl_->drawing)
            return fail(EWindowError::BUSY);
        impl_->frame->finish();
        impl_->frame.reset();
        auto snapshot = impl_->ui->captureFrame();
        if (!snapshot)
            return fail(EWindowError::UI_FAILURE);
        return std::move(*snapshot);
    }
    WindowResult<void> EditorWindow::discardFrame() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (impl_->drawing)
            return fail(EWindowError::BUSY);
        impl_->frame.reset();
        return {};
    }
    WindowResult<void> EditorWindow::installLayout(const WorkspaceLayout &layout) noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        if (impl_->frame)
            return fail(EWindowError::BUSY);
        if (!layout.workspace.value || layout.version != 1 || !impl_->ui->validateSplitLayout(layout.spec))
            return fail(EWindowError::LAYOUT_FAILURE);
        try
        {
            impl_->ui->setSplitLayout(layout.spec);
        }
        catch (const std::bad_alloc &)
        {
            return fail(EWindowError::ALLOCATION_FAILURE);
        }
        return {};
    }
    WindowResult<void> EditorWindow::requestClose() noexcept
    {
        if (auto check = impl_->check(); !check)
            return check;
        impl_->close_requested = true;
        return {};
    }
    WindowResult<void> EditorWindow::closeAfterRendererStopped() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closed)
            return {};
        if (impl_->frame || impl_->drawing || impl_->entering)
            return fail(EWindowError::BUSY);
        if (!impl_->histories->close())
            return fail(EWindowError::BUSY);
        impl_->window->on_cursor_move = {};
        impl_->window->on_focus = {};
        impl_->window->on_lost_focus = {};
        impl_->ui.reset();
        impl_->window.reset();
        impl_->closed = true;
        return {};
    }
} // namespace lux::editor::ui
