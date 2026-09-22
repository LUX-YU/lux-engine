#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <optional>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <imm.h>
#endif

namespace lux::editor
{
namespace
{
TextInputPlatformStatus applyTextInput(lux::window::LuxWindow &window, lux::ui::UiTextInputAnchor anchor,
                                       lux::ui::Size logical_size) noexcept
{
    TextInputPlatformStatus result;
    if (!anchor.valid || !anchor.want_visible || !glfwGetWindowAttrib(window.handle(), GLFW_FOCUSED) ||
        !glfwGetWindowAttrib(window.handle(), GLFW_VISIBLE) || glfwGetWindowAttrib(window.handle(), GLFW_ICONIFIED))
    {
        return result;
    }
    result.frame = anchor.frame;
#if defined(_WIN32)
    const auto handle = static_cast<HWND>(window.win32Handle());
    RECT client{};
    if (!GetClientRect(handle, &client))
    {
        result.state = ETextInputPlatformState::PLATFORM_FAILURE;
        return result;
    }
    // Both IMM coordinates and GetClientRect use this owner's current Win32 client space.
    // FrameInfo may use logical units; convert from its actual extent exactly once.
    // The render framebuffer scale is NOT an additional DPI multiplier.
    const bool valid_input = logical_size.width > 0 && logical_size.height > 0 && std::isfinite(anchor.caret.x) &&
                             std::isfinite(anchor.caret.y) && std::isfinite(anchor.line_height) &&
                             anchor.line_height > 0;
    if (!valid_input || client.right <= 0 || client.bottom <= 0)
    {
        result.state = ETextInputPlatformState::INVALID_COORDINATES;
        return result;
    }
    const auto scale_x = double(client.right) / logical_size.width;
    const auto scale_y = double(client.bottom) / logical_size.height;
    const auto x = double(anchor.caret.x) * scale_x;
    const auto y = double(anchor.caret.y) * scale_y;
    const auto bottom = y + double(anchor.line_height) * scale_y;
    const bool valid_rectangle =
        x >= 0 && x <= client.right && y >= 0 && y <= client.bottom && bottom >= y && bottom <= client.bottom;
    if (!valid_rectangle)
    {
        result.state = ETextInputPlatformState::INVALID_COORDINATES;
        return result;
    }
    const auto ime = ImmGetContext(handle);
    if (!ime)
    {
        result.state = ETextInputPlatformState::UNAVAILABLE;
        return result;
    }

    struct ReleaseContext final
    {
        HWND window;
        HIMC context;

        ~ReleaseContext()
        {
            ImmReleaseContext(window, context);
        }
    } release{handle, ime};

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = composition.ptCurrentPos;
    candidate.rcArea = {composition.ptCurrentPos.x, composition.ptCurrentPos.y, composition.ptCurrentPos.x + 1,
                        static_cast<LONG>(std::ceil(bottom))};
    result.composition_positioned = ImmSetCompositionWindow(ime, &composition) != FALSE;
    result.candidate_positioned = ImmSetCandidateWindow(ime, &candidate) != FALSE;
    result.state = result.composition_positioned && result.candidate_positioned
                       ? ETextInputPlatformState::APPLIED
                       : ETextInputPlatformState::PLATFORM_FAILURE;
#else
    result.state = ETextInputPlatformState::UNAVAILABLE;
#endif
    return result;
}

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

} // namespace

EditorResult<bool> Editor::selectExistingFile(std::filesystem::path &selection)
{
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const auto &initial = selection.native();
    if (initial.size() >= buffer.size())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "file.dialog"});
    }
    std::copy(initial.begin(), initial.end(), buffer.begin());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(impl_->window->win32Handle());
    dialog.lpstrTitle = L"Choose source file";
    dialog.lpstrFilter = L"All files\0*.*\0\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&dialog))
    {
        const auto error = CommDlgExtendedError();
        if (error)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "file.dialog", error});
        }
        return false;
    }
    std::filesystem::path chosen(buffer.data());
    selection.swap(chosen);
    return true;
#else
    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "file.dialog"});
#endif
}

void Editor::collectInput()
{
    if (!impl_ || !impl_->window || !impl_->ui || impl_->desktop_stopping)
    {
        return;
    }
    lux::window::LuxWindow::pollEvents();
    for (const auto &event : impl_->window->drainInputEvents())
    {
        std::visit(
            [this](const auto &value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, window::WindowKeyEvent>)
                {
                    const auto key = uiKey(value.key);
                    if (key != lux::ui::EKey::NONE)
                    {
                        impl_->ui->feedInput(lux::ui::UiKey{key, value.action != GLFW_RELEASE});
                    }
                }
                else if constexpr (std::same_as<Value, window::WindowMouseButtonEvent>)
                {
                    const auto button = pointerButton(value.button);
                    if (button)
                    {
                        impl_->ui->feedInput(lux::ui::UiPointerButton{*button, value.action != GLFW_RELEASE});
                    }
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
    if (!impl_->native_close && impl_->window->shouldClose())
    {
        std::fprintf(stderr, "[editor.exit] event=native-close-flag\n");
        impl_->native_close = true;
        if (impl_->project_pane)
        {
            impl_->project_pane->nativeClose();
        }
    }
    const bool inactive = impl_->native_close || !glfwGetWindowAttrib(impl_->window->handle(), GLFW_FOCUSED) ||
                          !glfwGetWindowAttrib(impl_->window->handle(), GLFW_VISIBLE);
    if (inactive)
    {
        impl_->text_input_status = {};
    }
}

void Editor::bindPlatformInput()
{
    auto *input = impl_->ui;
    impl_->window->on_cursor_move = [input](const window::CursorMoveEvent &event) {
        input->feedInput(lux::ui::UiPointerMove{{float(event.x), float(event.y)}});
    };
    impl_->window->on_focus = [input](const window::WindowFocusEvent &) {
        input->feedInput(lux::ui::UiWindowFocus{true});
    };
    impl_->window->on_lost_focus = [input](const window::WindowLostFocusEvent &) {
        input->feedInput(lux::ui::UiWindowFocus{false});
    };
}
void Editor::clearPlatformInput() noexcept
{
    if (impl_ && impl_->window)
    {
        impl_->window->on_cursor_move = {};
        impl_->window->on_focus = {};
        impl_->window->on_lost_focus = {};
        impl_->text_input_status = {};
    }
}
void Editor::cancelNativeClose() noexcept
{
    impl_->native_close = false;
    glfwSetWindowShouldClose(impl_->window->handle(), GLFW_FALSE);
}
void Editor::positionTextInput(lux::ui::Size size) noexcept
{
    impl_->text_input_status = impl_->native_close ? TextInputPlatformStatus{}
                                                   : applyTextInput(*impl_->window, impl_->ui->textInputAnchor(), size);
}
} // namespace lux::editor
